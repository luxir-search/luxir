// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <boost/unordered/unordered_flat_map.hpp>

#include "luxir/query/Query.h"
#include "luxir/reader/BKDReader.h"
#include "luxir/reader/IntColReader.h"
#include "luxir/reader/SkipStats.h"
#include "luxir/search/DocSet.h"
#include "luxir/util/geo.h"

namespace luxir {

/// The k documents nearest a point of a GEO_POINT field, by the same
/// great-circle distance geo_distance queries use.
///
/// Like kNN, the result is globally bounded, so it is computed once in
/// Weight::prepare over every segment's domain and replayed from per-segment
/// hit arrays. One heap of the k best (distance, segment, doc) entries is
/// shared by all segments, so each segment starts from the k-th distance the
/// segments before it reached. Per segment:
///
///   - With a points index, a best-first walk (BKDReader::nearest) visits
///     cells in ascending lower-bound distance (geo::boxSortKeyLowerBound),
///     decodes the leaves it reaches, and stops at the first cell farther
///     than the current k-th distance. Docs outside the domain are skipped
///     point by point.
///   - A domain sparse enough that reading its docs' column values costs
///     less than the walk is expected to (see sparseDomainWins) is iterated
///     instead, as is the domain or the whole column when the segment has no
///     points index.
///
/// A multi-valued doc counts once, at its nearest point. Ties keep the
/// earlier segment and doc. Hits score boost / (1 + meters), so the nearest
/// ranks first; sort by geo_distance(field, lat, lon) for the exact meters.
class GeoNearestQuery final : public Query {
  std::string_view field;
  double centerLatitude;
  double centerLongitude;
  int32_t k;
  // Inclusive limit; infinity when unset.
  double maxDistanceMeters;
  bool multiValued;

public:
  static constexpr std::string_view QUERY_NAME = "GeoNearestQuery";

  // Relative cost of one sparse domain doc (a column seek and a distance)
  // against one point the walk decodes and measures.
  static constexpr int64_t SPARSE_DOC_COST = 2;

  struct Hit {
    int32_t docId;
    float score;
  };

  GeoNearestQuery(std::string_view field, double centerLatitude,
                  double centerLongitude, int32_t k,
                  double maxDistanceMeters, bool multiValued)
      : Query(QueryKind::GEO_NEAREST), field(field),
        centerLatitude(centerLatitude), centerLongitude(centerLongitude),
        k(k), maxDistanceMeters(maxDistanceMeters), multiValued(multiValued) {
    geo::checkLatitude(centerLatitude);
    geo::checkLongitude(centerLongitude);
    if (k <= 0) throw std::invalid_argument("k must be positive");
    if (std::isnan(maxDistanceMeters) || maxDistanceMeters < 0.0) {
      throw std::invalid_argument(
          "max_distance_meters must be non-negative");
    }
  }

  bool recoverableAsPredicate() const noexcept override { return false; }

  bool equalsSameKind(const Query& other) const override {
    const auto& rhs = static_cast<const GeoNearestQuery&>(other);
    return field == rhs.field && k == rhs.k
        && multiValued == rhs.multiValued
        && std::bit_cast<uint64_t>(centerLatitude)
            == std::bit_cast<uint64_t>(rhs.centerLatitude)
        && std::bit_cast<uint64_t>(centerLongitude)
            == std::bit_cast<uint64_t>(rhs.centerLongitude)
        && std::bit_cast<uint64_t>(maxDistanceMeters)
            == std::bit_cast<uint64_t>(rhs.maxDistanceMeters);
  }

  uint64_t hashImpl() const override {
    uint64_t value = mixHash(Query::hashImpl(), field);
    value = mixHash(value, k);
    value = mixHash(value, std::bit_cast<uint64_t>(centerLatitude));
    value = mixHash(value, std::bit_cast<uint64_t>(centerLongitude));
    return mixHash(value, std::bit_cast<uint64_t>(maxDistanceMeters));
  }

  std::string_view getField() const noexcept { return field; }
  double getCenterLatitude() const noexcept { return centerLatitude; }
  double getCenterLongitude() const noexcept { return centerLongitude; }
  int32_t getK() const noexcept { return k; }
  double getMaxDistanceMeters() const noexcept { return maxDistanceMeters; }

  void validateLogicalImpl(
      PlanningContext& context, float multiplier = 1.0f) const override {
    unused(context);
    checkedBoostProduct(multiplier, 1.0f);
  }

  FilterKeyScope appendFilterKey(FilterKeyBuilder& out,
                                 const FilterKeyContext& ctx) const override {
    out.appendKind(kind);
    unused(ctx);
    out.appendString(field);
    out.appendDouble(centerLatitude);
    out.appendDouble(centerLongitude);
    out.appendInt32(k);
    out.appendDouble(maxDistanceMeters);
    out.appendBool(multiValued);
    return FilterKeyScope::READER_STABLE;
  }

  // The haversinSortKey at or below which a point is within the limit.
  double maxSortKey() const {
    return std::isinf(maxDistanceMeters)
        ? std::numeric_limits<double>::infinity()
        : geo::distanceQuerySortKey(maxDistanceMeters);
  }

  // Whether iterating `domainDocs` docs costs less than the best-first walk
  // over `points` points is expected to. Under a domain independent of
  // location, the walk decodes about k * points / domainDocs points before
  // it holds k domain docs, and at least one leaf.
  static bool sparseDomainWins(int64_t domainDocs, int64_t points,
                               int64_t k, int64_t leafSize) {
    if (domainDocs <= 0) return true;
    double walked = std::max((double)leafSize,
        std::min((double)points, (double)k * (double)points
                                     / (double)domainDocs));
    return (double)domainDocs * (double)SPARSE_DOC_COST < walked;
  }

  // Bounded heap of the k best (key, segment, doc) entries, worst on top.
  // Multi-valued fields keep one entry per doc: a closer point of a held
  // doc lowers its key in place.
  class NearestHeap {
    struct Entry {
      double key;
      int32_t segment;
      int32_t docId;
    };

    std::vector<Entry> entries;
    boost::unordered_flat_map<uint64_t, uint32_t> slots;
    size_t k;
    double limit;
    bool multiValued;

    static bool worse(const Entry& a, const Entry& b) {
      if (a.key != b.key) return a.key > b.key;
      if (a.segment != b.segment) return a.segment > b.segment;
      return a.docId > b.docId;
    }

    static uint64_t id(int32_t segment, int32_t docId) {
      return ((uint64_t)(uint32_t)segment << 32) | (uint32_t)docId;
    }

    void place(size_t index, const Entry& entry) {
      entries[index] = entry;
      if (multiValued) slots[id(entry.segment, entry.docId)] = (uint32_t)index;
    }

    void siftUp(size_t index) {
      Entry entry = entries[index];
      while (index > 0) {
        size_t parent = (index - 1) / 2;
        if (!worse(entry, entries[parent])) break;
        place(index, entries[parent]);
        index = parent;
      }
      place(index, entry);
    }

    void siftDown(size_t index) {
      Entry entry = entries[index];
      size_t size = entries.size();
      for (;;) {
        size_t child = index * 2 + 1;
        if (child >= size) break;
        if (child + 1 < size && worse(entries[child + 1], entries[child])) {
          child++;
        }
        if (!worse(entries[child], entry)) break;
        place(index, entries[child]);
        index = child;
      }
      place(index, entry);
    }

  public:
    NearestHeap(size_t k, double limit, bool multiValued)
        : k(k), limit(limit), multiValued(multiValued) {
      entries.reserve(std::min<size_t>(k, 1 << 16));
    }

    // The key at or below which an entry can still enter or improve.
    double bound() const {
      return entries.size() < k ? limit : entries[0].key;
    }

    void offer(double key, int32_t segment, int32_t docId) {
      if (key > bound()) return;
      Entry entry{key, segment, docId};
      if (multiValued) {
        auto found = slots.find(id(segment, docId));
        if (found != slots.end()) {
          size_t index = found->second;
          if (key < entries[index].key) {
            entries[index].key = key;
            siftDown(index);
          }
          return;
        }
      }
      if (entries.size() < k) {
        entries.push_back(entry);
        siftUp(entries.size() - 1);
        return;
      }
      if (!worse(entries[0], entry)) return;
      if (multiValued) slots.erase(id(entries[0].segment, entries[0].docId));
      entries[0] = entry;
      siftDown(0);
    }

    size_t size() const { return entries.size(); }

    template <class F>
    void forEach(F&& f) const {
      for (const Entry& entry : entries) f(entry.key, entry.segment, entry.docId);
    }
  };

  class Scorer final : public Query::Scorer {
    std::span<const Hit> hits;
    int32_t cur = -1;
    float maxScore = 0.0f;

  public:
    explicit Scorer(std::span<const Hit> hits) noexcept : hits(hits) {
      for (const Hit& hit : hits) maxScore = std::max(maxScore, hit.score);
    }

    int32_t next() override {
      cur++;
      return cur < (int32_t)hits.size() ? hits[cur].docId : PostingsReader::END;
    }

    int32_t advance(int32_t docid) override {
      assert(docId() < docid);
      int32_t doc;
      while ((doc = next()) < docid) {}
      return doc;
    }

    int32_t docId() override {
      if (cur < 0) return -1;
      return cur < (int32_t)hits.size() ? hits[cur].docId : PostingsReader::END;
    }

    float score() override {
      assert(cur >= 0 && cur < (int32_t)hits.size());
      return hits[cur].score;
    }

    float getMaxScore(int32_t upTo) override {
      unused(upTo);
      return maxScore;
    }

    float getMaxScoreForSetup(int32_t upTo) override {
      unused(upTo);
      return maxScore;
    }
  };

  class Weight final : public Query::Weight {
    GeoNearestQuery& query;
    float boost;

    class PreparedHits final : public Query::Weight::PreparedWeight {
      std::vector<std::vector<Hit>> perSegHits;

      class Supplier final : public Query::ScorerSupplier {
        std::span<const Hit> hits;

        class Plan final : public Query::ScorerPlan {
          std::span<const Hit> hits;

        protected:
          Query::Scorer* buildScorer(MemPool& targetPool) override {
            return targetPool.make<GeoNearestQuery::Scorer>(hits);
          }

        public:
          Plan(const Query::PlanContext& planContext,
               const Query::ScorerShape& shape, int64_t cost,
               std::span<const Hit> hits)
            : Query::ScorerPlan(planContext, shape, cost), hits(hits) {}
        };

      public:
        explicit Supplier(std::span<const Hit> hits) : hits(hits) {
          assert(!hits.empty());
        }

        int64_t cost() override { return (int64_t)hits.size(); }

        Query::ScorerShape describeScorer(
            const Query::PlanContext& buildContext) const override {
          unused(buildContext);
          return {
            .matchState = Query::MatchState::NONEMPTY,
            .directKind = Query::DirectScorerKind::OTHER,
            .reportedTwoPhase = Query::ReportedTwoPhase::NO,
            .windowFillClause = Query::ClauseShape::NONE,
            .termDisjunctionClause = Query::ClauseShape::NONE,
            .termConjunctionClause = Query::ClauseShape::NONE,
            .independentTerm = Query::IndependentTermAccess::UNSUPPORTED,
            .docsOnly = Query::DocsOnlyAccess::UNSUPPORTED,
            .directDocSet = Query::DirectDocSetAccess::UNSUPPORTED,
          };
        }

        Query::ScorerPlan* resolve(
            MemPool& planPool,
            const Query::PlanContext& planContext) override {
          return planPool.make<Plan>(
              planContext, describeScorer(planContext), cost(), hits);
        }
      };

      std::span<const Hit> segmentHits(const IndexReader::Segment& segment) const {
        if ((size_t)segment.ord >= perSegHits.size()) return {};
        const auto& hits = perSegHits[(size_t)segment.ord];
        return {hits.data(), hits.size()};
      }

    public:
      explicit PreparedHits(std::vector<std::vector<Hit>>&& perSegHits)
        : perSegHits(std::move(perSegHits)) {}

      Query::Scorer* createScorer(MemPool& target,
                                  IndexReader::Segment& segment) override {
        auto hits = segmentHits(segment);
        if (hits.empty()) return nullptr;
        return target.make<GeoNearestQuery::Scorer>(hits);
      }

      Query::ScorerSupplier* scorerSupplierImpl(
          MemPool& target, IndexReader::Segment& segment,
          Query::SupplierExecutionMode executionMode) override {
        unused(executionMode);
        auto hits = segmentHits(segment);
        if (hits.empty()) return nullptr;
        return target.make<Supplier>(hits);
      }

      size_t retainedBytes() const noexcept override {
        size_t bytes = sizeof(*this) + perSegHits.capacity() * sizeof(perSegHits[0]);
        for (const auto& hits : perSegHits) bytes += hits.capacity() * sizeof(Hit);
        return bytes;
      }

      bool outputIsSubsetOfDomain() const noexcept override { return true; }
    };

    template <class F>
    static void forEachDomainDoc(DocSet& domain, F&& f) {
      if (domain.type == DocSet::ARRAY) {
        for (int32_t doc : static_cast<ArrDocSet&>(domain).docs()) f(doc);
        return;
      }
      const FixedBitSet& bits = static_cast<BitDocSet&>(domain).bits();
      size_t words = FixedBitSet::sizeInWords(bits.size());
      for (size_t word = 0; word < words; word++) {
        uint64_t value = bits.words[word];
        while (value != 0) {
          f((int32_t)(word * 64 + (uint32_t)std::countr_zero(value)));
          value &= value - 1;
        }
      }
    }

    // Offers the points of one column doc the iterator is positioned on.
    template <class Iter, class Offer>
    static void offerColumnDoc(IntColReader& column, Iter& iter, Offer&& offer) {
      if (!column.multiValued()) {
        offer(iter.value());
        return;
      }
      auto [start, end] = column.getStartEndValueRank(iter.rank());
      for (int64_t rank = start; rank < end; rank++) {
        offer(iter.values().valueAt(rank));
      }
    }

  public:
    Weight(Query::Context& context, GeoNearestQuery& query, int32_t flags,
           float multiplier)
      : Query::Weight(context, query, flags), query(query),
        boost(constantWhenScored(flags, multiplier)) {
      traits |= NEEDS_PREPARE;
    }

    std::unique_ptr<Query::Weight::PreparedWeight> prepare(
        Query::Weight::PrepareContext& ctx) override {
      IndexReader& reader = ctx.reader;
      auto segInfos = reader.field(query.field);
      size_t numSegs = reader.segments().size();
      double lat = query.centerLatitude;
      double lon = query.centerLongitude;
      NearestHeap heap((size_t)query.k, query.maxSortKey(), query.multiValued);
      std::vector<uint32_t> docScratch;
      std::vector<uint32_t> latScratch;
      std::vector<uint32_t> lonScratch;
      for (size_t i = 0; i < numSegs; i++) {
        const SegFieldInfo* info = segInfos.at(i);
        if (info == nullptr || info->columnLoc.offset() <= 0) continue;
        if (info->type != FieldType::GEO_POINT) {
          throw std::runtime_error(std::string(QUERY_NAME)
                                   + " requires a GEO_POINT field");
        }
        IndexReader::Segment& segment = reader.segments()[i];
        IntColReader column(segment.postingsReader(), *info);
        if (column.numValues() == 0) continue;
        DocSet* domain = ctx.domainPerSeg.empty() ? nullptr : ctx.domainPerSeg[i];
        // Domain contract (Query.h): a present domain is live-filtered.
        assert(domain != nullptr || segment.liveDocs() == nullptr);
        int32_t segOrd = (int32_t)i;
        auto offerPacked = [&](int32_t doc, int64_t packed) {
          heap.offer(geo::haversinSortKey(
                         lat, lon,
                         geo::decodeLatitude(geo::unpackLatitude(packed)),
                         geo::decodeLongitude(geo::unpackLongitude(packed))),
                     segOrd, doc);
        };

        bool hasPoints = info->pointsMetaOff != 0;
        std::optional<BKDReader> bkd;
        if (hasPoints) bkd.emplace(segment.postingsReader(), *info);
        if (domain != nullptr) {
          int64_t domainDocs = domain->card();
          if (domainDocs == 0) continue;
          if (!hasPoints || sparseDomainWins(domainDocs,
                                             (int64_t)bkd->pointCount(),
                                             query.k, bkd->maxPointsPerLeaf())) {
            skipCount(SkipStats::geoNearestSparse);
            IntColReader::SparseIterator iter(column);
            int32_t found = -1;
            forEachDomainDoc(*domain, [&](int32_t doc) {
              if (found == PostingsReader::END) return;
              if (found < doc) found = iter.advance(doc);
              if (found != doc) return;
              offerColumnDoc(column, iter, [&](int64_t packed) {
                offerPacked(doc, packed);
              });
            });
            continue;
          }
        } else if (!hasPoints) {
          skipCount(SkipStats::geoNearestScans);
          IntColReader::Iterator iter(column);
          for (int32_t doc = iter.next(); doc != PostingsReader::END;
               doc = iter.next()) {
            offerColumnDoc(column, iter, [&](int64_t packed) {
              offerPacked(doc, packed);
            });
          }
          continue;
        }

        skipCount(SkipStats::geoNearestWalks);
        size_t leafSize = bkd->maxPointsPerLeaf();
        docScratch.resize(leafSize);
        latScratch.resize(leafSize);
        lonScratch.resize(leafSize);
        DocSetProbe eligible(domain);
        uint32_t leaves = bkd->nearest(
            {docScratch, latScratch, lonScratch},
            [&](int32_t latMin, int32_t latMax, int32_t lonMin, int32_t lonMax) {
              return geo::boxSortKeyLowerBound(
                  lat, lon, geo::decodeLatitude(latMin),
                  geo::decodeLatitude(latMax), geo::decodeLongitude(lonMin),
                  geo::decodeLongitude(lonMax));
            },
            [&]() { return heap.bound(); },
            [&](int32_t doc, int32_t pointLat, int32_t pointLon) {
              if (!eligible.get(doc)) return;
              heap.offer(geo::haversinSortKey(lat, lon,
                                              geo::decodeLatitude(pointLat),
                                              geo::decodeLongitude(pointLon)),
                         segOrd, doc);
            });
        if (SkipStats::enabled) SkipStats::geoNearestLeaves += leaves;
      }

      std::vector<std::vector<Hit>> perSegHits(numSegs);
      heap.forEach([&](double key, int32_t segment, int32_t docId) {
        float score = boost / (float)(1.0 + geo::haversinMeters(key));
        perSegHits[(size_t)segment].push_back({docId, score});
      });
      for (auto& hits : perSegHits) {
        std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
          return a.docId < b.docId;
        });
      }
      return std::make_unique<PreparedHits>(std::move(perSegHits));
    }

    Query::Scorer* createScorer(MemPool& target,
                                IndexReader::Segment& segment) override {
      unused(target, segment);
      return nullptr;
    }

    Query::ScorerSupplier* scorerSupplierImpl(
        MemPool& target, IndexReader::Segment& segment,
        Query::SupplierExecutionMode executionMode) override {
      unused(target, segment, executionMode);
      return nullptr;
    }
  };

  Query::Weight* createWeight(Context& context, int32_t flags,
                              float multiplier = 1.0f) override;
};

} // namespace luxir
