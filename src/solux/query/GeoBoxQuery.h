#pragma once

#include <cassert>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string_view>

#include <boost/sort/spreadsort/integer_sort.hpp>

#include "solux/query/PointsMaterialize.h"
#include "solux/query/Query.h"
#include "solux/reader/BKDReader.h"
#include "solux/reader/IntColReader.h"
#include "solux/util/geo.h"

namespace solux {

// Inclusive latitude/longitude box over packed GEO_POINT column values.
// Bounds are supplied in degrees and quantized exactly like Lucene's
// LatLonPoint.newBoxQuery.
class GeoBoxQuery final : public Query {
  std::string_view field;
  int32_t minLatitude;
  int32_t maxLatitude;
  int32_t minLongitude;
  int32_t maxLongitude;
  bool empty = false;

  static bool pointInBox(int64_t packed, int32_t minLatitude,
                         int32_t maxLatitude, int32_t minLongitude,
                         int32_t maxLongitude) {
    int32_t latitude = geo::unpackLatitude(packed);
    if (latitude < minLatitude || latitude > maxLatitude) return false;
    return geo::longitudeInRange(geo::unpackLongitude(packed), minLongitude,
                                 maxLongitude);
  }

public:
  GeoBoxQuery(std::string_view field, double minLat, double maxLat,
              double minLon, double maxLon) : field(field) {
    geo::checkLatitude(minLat);
    geo::checkLatitude(maxLat);
    geo::checkLongitude(minLon);
    geo::checkLongitude(maxLon);
    if (minLat > maxLat) {
      throw std::invalid_argument("minimum latitude exceeds maximum latitude");
    }

    // +90 and the non-wrapping singleton +180 are not representable after
    // quantization. Lucene returns MatchNoDocs for the same cases.
    if (minLat == 90.0 || (minLon == 180.0 && maxLon == 180.0)) {
      empty = true;
    }
    // A wrapped interval starting at +180 is equivalent to starting at -180.
    if (minLon == 180.0 && maxLon < minLon) minLon = -180.0;

    minLatitude = geo::encodeLatitudeCeil(minLat);
    maxLatitude = geo::encodeLatitude(maxLat);
    minLongitude = geo::encodeLongitudeCeil(minLon);
    maxLongitude = geo::encodeLongitude(maxLon);

    if (maxLatitude < minLatitude) empty = true;
    if (minLon <= maxLon && maxLongitude < minLongitude) empty = true;
  }

  std::string_view getField() const { return field; }
  int32_t getMinLatitude() const { return minLatitude; }
  int32_t getMaxLatitude() const { return maxLatitude; }
  int32_t getMinLongitude() const { return minLongitude; }
  int32_t getMaxLongitude() const { return maxLongitude; }
  bool isEmpty() const { return empty; }

  Query::Weight* createWeight(Context& context, int32_t flags) override {
    return context.pool.make<Weight>(context, *this, flags);
  }

  template <class ColIter>
  class BoxScorer final : public Query::Scorer {
    IntColReader& reader;
    ColIter iter;
    int32_t minLatitude;
    int32_t maxLatitude;
    int32_t minLongitude;
    int32_t maxLongitude;
    bool multi;
    int32_t docid = -1;

    bool pointMatches(int64_t packed) const {
      return pointInBox(packed, minLatitude, maxLatitude, minLongitude,
                        maxLongitude);
    }

    bool valueInBox() {
      if (docid == PostingsReader::END) return false;
      if (!multi) return pointMatches(iter.value());
      auto [start, end] = reader.getStartEndValueRank(iter.rank());
      for (int64_t rank = start; rank < end; rank++) {
        if (pointMatches(iter.values().valueAt(rank))) return true;
      }
      return false;
    }

  public:
    BoxScorer(IntColReader& reader, int32_t minLatitude,
              int32_t maxLatitude, int32_t minLongitude,
              int32_t maxLongitude)
      : reader(reader), iter(reader), minLatitude(minLatitude),
        maxLatitude(maxLatitude), minLongitude(minLongitude),
        maxLongitude(maxLongitude), multi(reader.multiValued()) {}

    bool hasTwoPhase() const override { return true; }
    int32_t approximationNext() override {
      docid = iter.next();
      return docid;
    }
    int32_t approximationAdvance(int32_t target) override {
      docid = iter.advance(target);
      return docid;
    }
    int32_t approximationDocId() override { return docid; }
    bool matches() override { return valueInBox(); }
    float matchCost() override {
      if (!multi) return 2.0f;
      int64_t docs = reader.docsWithValue();
      return docs > 0 ? 2.0f * (float)reader.numValues() / (float)docs : 2.0f;
    }

    int32_t next() override {
      for (;;) {
        docid = iter.next();
        if (docid == PostingsReader::END || valueInBox()) return docid;
      }
    }
    int32_t advance(int32_t target) override {
      assert(docid < target);
      docid = iter.advance(target);
      while (docid != PostingsReader::END && !valueInBox()) {
        docid = iter.next();
      }
      return docid;
    }
    int32_t docId() override { return docid; }
    float score() override { return 0.0f; }
  };

  class Weight final : public Query::Weight {
    GeoBoxQuery& query;
    std::span<SegFieldInfo*> segInfos;

    bool segmentInfo(IndexReader::Segment& segment, SegFieldInfo*& info) const {
      if (segInfos.empty()) return false;
      info = segInfos[(size_t)segment.ord];
      return info != nullptr && info->columnLoc.offset() > 0;
    }

  public:
    Weight(Context& context, GeoBoxQuery& query, int32_t flags)
      : Query::Weight(context, flags), query(query) {
      traits |= IS_CONSTANT_SCORING;
      segInfos = context.readSegInfos(query.getField());
    }

    class Supplier final : public Query::ScorerSupplier {
      GeoBoxQuery::Weight& weight;
      IndexReader::Segment& segment;
      IntColReader& reader;
      BKDReader* bkd;
      BKDBoxRelation relation;
      int64_t estimatedCost;
      uint64_t materializeUpperBound;

      BKDReader::Scratch scratch(MemPool& pool) const {
        size_t size = bkd->maxPointsPerLeaf();
        return {pool.make_span<uint32_t>(size),
                pool.make_span<uint32_t>(size),
                pool.make_span<uint32_t>(size)};
      }

      PointsMaterialize::Materialized materialize(MemPool& pool) const {
        assert(bkd != nullptr);
        BKDReader::Scratch leafScratch = scratch(pool);
        if (PointsMaterialize::useBitset((uint64_t)estimatedCost,
                                         segment.maxDoc())) {
          auto words = pool.make_span<uint64_t>(
              FixedBitSet::sizeInWords(segment.maxDoc()));
          std::fill(words.begin(), words.end(), 0);
          FixedBitSet bits(words.data(), segment.maxDoc());
          bkd->intersect(relation, leafScratch,
              [&bits](int32_t doc) { bits.set(doc); },
              [&bits](int32_t begin, int32_t end) {
                PointsMaterialize::setRun(bits, begin, end);
              },
              [&bits](int32_t wordIndex, uint64_t word) {
                bits.words[wordIndex] |= word;
              });
          return {{}, words.data()};
        }

        // The selectivity estimate charges crossing leaves at half their
        // points, so it cannot size the array safely. The same bounds-only
        // traversal also returns the full count of every crossing leaf; that
        // is a fixed upper bound on emissions and avoids a heap-backed grow.
        auto docs = pool.make_span<int32_t>((size_t)materializeUpperBound);
        size_t size = 0;
        auto addDoc = [&docs, &size](int32_t doc) { docs[size++] = doc; };
        bkd->intersect(relation, leafScratch, addDoc,
            [&addDoc](int32_t begin, int32_t end) {
              for (int32_t doc = begin; doc < end; doc++) addDoc(doc);
            },
            [&addDoc](int32_t wordIndex, uint64_t word) {
              while (word != 0) {
                addDoc(wordIndex * 64 + (int32_t)std::countr_zero(word));
                word &= word - 1;
              }
            });
        assert(size <= materializeUpperBound);
        auto used = docs.first(size);
        // BKD docids are ascending only within each leaf. Always sort across
        // leaves; only multi-valued fields need duplicate docids removed.
        boost::sort::spreadsort::integer_sort(used.begin(), used.end());
        if (reader.multiValued()) {
          size = (size_t)(std::unique(used.begin(), used.end()) - used.begin());
        }
        return {docs.first(size), nullptr};
      }

    public:
      Supplier(GeoBoxQuery::Weight& weight, IndexReader::Segment& segment,
               IntColReader& reader, BKDReader* bkd,
               BKDReader::EstimateResult estimate)
        : weight(weight), segment(segment), reader(reader), bkd(bkd),
          relation(weight.query.getMinLatitude(), weight.query.getMaxLatitude(),
                   weight.query.getMinLongitude(), weight.query.getMaxLongitude()),
          estimatedCost((int64_t)std::min<uint64_t>(
              estimate.estimatedCount, (uint64_t)reader.docsWithValue())),
          materializeUpperBound(estimate.upperBound) {}

      int64_t cost() override { return estimatedCost; }

      Query::Scorer* get(MemPool& targetPool, int64_t leadCost) override {
        if (leadCost < cost()) {
          skipCount(SkipStats::geoSparseVerifyArms);
          return targetPool.make<BoxScorer<IntColReader::SparseIterator>>(
              reader, weight.query.getMinLatitude(),
              weight.query.getMaxLatitude(), weight.query.getMinLongitude(),
              weight.query.getMaxLongitude());
        }
        if (bkd != nullptr) {
          skipCount(SkipStats::geoBKDArms);
          return PointsMaterialize::scorerFor(
              targetPool, materialize(targetPool), segment.maxDoc());
        }
        skipCount(SkipStats::geoScanArms);
        return targetPool.make<BoxScorer<IntColReader::Iterator>>(
            reader, weight.query.getMinLatitude(),
            weight.query.getMaxLatitude(), weight.query.getMinLongitude(),
            weight.query.getMaxLongitude());
      }

      BulkScorer* bulkScorer(MemPool& targetPool) override {
        if (bkd == nullptr) return nullptr;
        skipCount(SkipStats::geoBKDArms);
        return PointsMaterialize::bulkFor(
            targetPool, materialize(targetPool), segment.maxDoc());
      }
    };

    Query::ScorerSupplier* scorerSupplier(MemPool& targetPool,
                                           IndexReader::Segment& segment) override {
      if (query.isEmpty()) return nullptr;
      SegFieldInfo* info = nullptr;
      if (!segmentInfo(segment, info)) return nullptr;
      if (info->type != FieldType::GEO_POINT) {
        throw std::runtime_error("GeoBoxQuery requires a GEO_POINT field");
      }
      auto* reader = targetPool.make<IntColReader>(segment.postingsReader(), *info);
      if (reader->numValues() == 0) return nullptr;
      BKDReader* bkd = nullptr;
      BKDReader::EstimateResult estimate{
          (uint64_t)reader->docsWithValue(), (uint64_t)reader->numValues()};
      if (info->pointsMetaOff != 0) {
        bkd = targetPool.make<BKDReader>(segment.postingsReader(), *info);
        if (bkd->pointCount() != (uint64_t)reader->numValues()) {
          throw std::runtime_error("GeoBoxQuery: points/column value count mismatch");
        }
        BKDBoxRelation relation(query.getMinLatitude(), query.getMaxLatitude(),
                                query.getMinLongitude(), query.getMaxLongitude());
        estimate = bkd->estimateIntersect(relation);
        if (estimate.upperBound == 0) return nullptr;
      }
      return targetPool.make<Supplier>(*this, segment, *reader, bkd, estimate);
    }

    Query::Scorer* createScorer(MemPool& targetPool,
                                IndexReader::Segment& segment) override {
      auto* supplier = scorerSupplier(targetPool, segment);
      return supplier == nullptr ? nullptr
          : supplier->get(targetPool, std::numeric_limits<int64_t>::max());
    }

    // Test/benchmark baseline: force the dense column scan and bypass BKD.
    Query::Scorer* createScanScorerForTests(MemPool& targetPool,
                                            IndexReader::Segment& segment) {
      if (query.isEmpty()) return nullptr;
      SegFieldInfo* info = nullptr;
      if (!segmentInfo(segment, info)) return nullptr;
      if (info->type != FieldType::GEO_POINT) {
        throw std::runtime_error("GeoBoxQuery requires a GEO_POINT field");
      }
      auto* reader = targetPool.make<IntColReader>(segment.postingsReader(),
                                                   *info);
      if (reader->numValues() == 0) return nullptr;
      return targetPool.make<BoxScorer<IntColReader::Iterator>>(
          *reader, query.getMinLatitude(), query.getMaxLatitude(),
          query.getMinLongitude(), query.getMaxLongitude());
    }

    int64_t count(IndexReader::Segment& segment) override {
      if (query.isEmpty()) return 0;
      if (segment.liveDocs() != nullptr) return -1;
      SegFieldInfo* info = nullptr;
      if (!segmentInfo(segment, info)) return 0;
      if (info->type != FieldType::GEO_POINT) {
        throw std::runtime_error("GeoBoxQuery requires a GEO_POINT field");
      }
      IntColReader reader(segment.postingsReader(), *info);
      if (reader.numValues() == 0) return 0;
      if (reader.multiValued() || info->pointsMetaOff == 0) return -1;
      BKDReader bkd(segment.postingsReader(), *info);
      if (bkd.pointCount() != (uint64_t)reader.numValues()) {
        throw std::runtime_error("GeoBoxQuery: points/column value count mismatch");
      }
      auto guard = MemPool::threadLocalPoolGuard();
      size_t size = bkd.maxPointsPerLeaf();
      BKDReader::Scratch scratch{{}, guard.pool().make_span<uint32_t>(size),
                                 guard.pool().make_span<uint32_t>(size)};
      BKDBoxRelation relation(query.getMinLatitude(), query.getMaxLatitude(),
                              query.getMinLongitude(), query.getMaxLongitude());
      return (int64_t)bkd.countIntersect(relation, scratch).exactCount;
    }
  };
};

} // namespace solux
