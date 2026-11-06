// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#include <numbers>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

#include "luxir/query/Query.h"
#include "luxir/reader/BKDReader.h"
#include "luxir/reader/IntColReader.h"
#include "luxir/reader/SkipStats.h"
#include "luxir/util/geo.h"
#include "luxir/util/screaming.h"

namespace luxir {

/// Elasticsearch's distance_feature over a GEO_POINT field: every document
/// with a point matches and scores boost * pivot / (pivot + d), where d is
/// the great-circle distance in meters from the origin to its nearest
/// point. The score is boost at the origin, boost / 2 at the pivot distance,
/// and falls toward zero.
///
/// The scorer honors setMinCompetitiveScore the way Lucene's
/// LatLonPointDistanceFeatureQuery does: a floor theta means a doc can only
/// compete within d_max = pivot * (boost / theta - 1), so the scorer looks
/// up the docs inside that circle in the points index and iterates only
/// them. The lookup repeats each time d_max halves, and only while the
/// index estimates the circle at under half of the docs left to iterate.
/// A floor above boost ends iteration.
class DistanceFeatureQuery final : public Query {
  std::string_view field;
  double originLatitude;
  double originLongitude;
  double pivotMeters;

public:
  static constexpr std::string_view QUERY_NAME = "DistanceFeatureQuery";
  // Test and benchmark seam: ignore minimum competitive scores.
  static inline bool disableSkippingForTests = false;

  DistanceFeatureQuery(std::string_view field, double originLatitude,
                       double originLongitude, double pivotMeters)
      : Query(QueryKind::DISTANCE_FEATURE), field(field),
        originLatitude(originLatitude), originLongitude(originLongitude),
        pivotMeters(pivotMeters) {
    geo::checkLatitude(originLatitude);
    geo::checkLongitude(originLongitude);
    if (!std::isfinite(pivotMeters) || pivotMeters <= 0.0) {
      throw std::invalid_argument("pivot_meters must be finite and positive");
    }
  }

  bool recoverableAsPredicate() const noexcept override { return false; }

  bool equalsSameKind(const Query& other) const override {
    const auto& rhs = static_cast<const DistanceFeatureQuery&>(other);
    return field == rhs.field
        && std::bit_cast<uint64_t>(originLatitude)
            == std::bit_cast<uint64_t>(rhs.originLatitude)
        && std::bit_cast<uint64_t>(originLongitude)
            == std::bit_cast<uint64_t>(rhs.originLongitude)
        && std::bit_cast<uint64_t>(pivotMeters)
            == std::bit_cast<uint64_t>(rhs.pivotMeters);
  }

  uint64_t hashImpl() const override {
    uint64_t value = mixHash(Query::hashImpl(), field);
    value = mixHash(value, std::bit_cast<uint64_t>(originLatitude));
    value = mixHash(value, std::bit_cast<uint64_t>(originLongitude));
    return mixHash(value, std::bit_cast<uint64_t>(pivotMeters));
  }

  std::string_view getField() const noexcept { return field; }
  double getOriginLatitude() const noexcept { return originLatitude; }
  double getOriginLongitude() const noexcept { return originLongitude; }
  double getPivotMeters() const noexcept { return pivotMeters; }

  void validateLogicalImpl(
      PlanningContext& context, float multiplier = 1.0f) const override {
    unused(context);
    checkedBoostProduct(multiplier, 1.0f);
  }

  // As a filter it matches every doc with a point, whatever the origin.
  FilterKeyScope appendFilterKey(FilterKeyBuilder& out,
                                 const FilterKeyContext& ctx) const override {
    out.appendKind(kind);
    unused(ctx);
    out.appendString(field);
    out.appendDouble(originLatitude);
    out.appendDouble(originLongitude);
    out.appendDouble(pivotMeters);
    return FilterKeyScope::SEGMENT_STABLE;
  }

  // The score of a doc `meters` away, as the scorer computes it.
  static float scoreAt(float boost, double pivotMeters, double meters) {
    return (float)(boost * (pivotMeters / (pivotMeters + meters)));
  }

  class Scorer final : public Query::Scorer {
    MemPool& pool;
    IntColReader& column;
    IntColReader::Iterator iter;
    const BKDReader* bkd;
    double lat;
    double lon;
    double pivot;
    float boost;
    int32_t maxDoc;
    int32_t doc = -1;
    int32_t scoredDoc = -1;
    float docScore = 0.0f;
    // Once restricted, iteration visits only the docs set in candidates:
    // those after the doc current at the build with a point inside the last
    // circle built. A bitset, so a wide circle needs no sort.
    std::span<uint64_t> candidates;
    int64_t candidateCount = 0;
    int32_t candidateStart = -1;
    bool restricted = false;
    bool exhausted = false;
    BKDReader::Scratch scratch;
    float minCompetitiveScore = -std::numeric_limits<float>::infinity();
    double attemptedMeters = std::numeric_limits<double>::infinity();

    int32_t land(int32_t target) {
      // Every candidate has a point, so the column lands on it exactly.
      if (iter.docId() < target) iter.advance(target);
      assert(iter.docId() == target);
      return doc = target;
    }

    int32_t nextCandidate(int32_t target) {
      if (exhausted || target >= maxDoc) return doc = PostingsReader::END;
      size_t word = (size_t)target >> 6;
      uint64_t bits = candidates[word] & (~UINT64_C(0) << (target & 63));
      while (bits == 0) {
        if (++word == candidates.size()) return doc = PostingsReader::END;
        bits = candidates[word];
      }
      return land((int32_t)(word * 64 + (size_t)std::countr_zero(bits)));
    }

    double nearestMeters() {
      auto key = [&](int64_t packed) {
        return geo::haversinSortKey(
            lat, lon, geo::decodeLatitude(geo::unpackLatitude(packed)),
            geo::decodeLongitude(geo::unpackLongitude(packed)));
      };
      if (!column.multiValued()) return geo::haversinMeters(key(iter.value()));
      auto [start, end] = column.getStartEndValueRank(iter.rank());
      double best = std::numeric_limits<double>::infinity();
      for (int64_t rank = start; rank < end; rank++) {
        best = std::min(best, key(iter.values().valueAt(rank)));
      }
      return geo::haversinMeters(best);
    }

    // Docs the current iteration has left, for pricing a narrower one.
    int64_t remaining() const {
      if (restricted) {
        int64_t span = (int64_t)maxDoc - candidateStart;
        return span > 0 ? candidateCount * ((int64_t)maxDoc - doc) / span : 0;
      }
      int64_t docs = column.docsWithValue();
      int32_t at = std::max(doc, 0);
      return maxDoc > 0 ? docs * (int64_t)(maxDoc - at) / maxDoc : docs;
    }

    void restrict(double meters) {
      BKDDistanceRelation relation(lat, lon, meters);
      if (scratch.docs.empty()) {
        size_t leafSize = bkd->maxPointsPerLeaf();
        scratch = {pool.make_span<uint32_t>(leafSize),
                   pool.make_span<uint32_t>(leafSize),
                   pool.make_span<uint32_t>(leafSize)};
      }
      if (candidates.empty()) {
        candidates = pool.make_span<uint64_t>(FixedBitSet::sizeInWords(maxDoc));
      }
      std::fill(candidates.begin(), candidates.end(), 0);
      auto add = [&](int32_t candidate) {
        candidates[(size_t)candidate >> 6] |= UINT64_C(1) << (candidate & 63);
      };
      bkd->intersect(relation, scratch, add,
          [&](int32_t first, int32_t last) {
            for (int32_t candidate = first; candidate < last; candidate++) add(candidate);
          },
          [&](int32_t word, uint64_t bits) {
            while (bits != 0) {
              add(word * 64 + std::countr_zero(bits));
              bits &= bits - 1;
            }
          });
      // The current doc and those before it are consumed.
      for (size_t word = 0; doc >= 0 && word <= ((size_t)doc >> 6); word++) {
        uint64_t keep = word < ((size_t)doc >> 6) ? 0
            : (doc & 63) == 63 ? 0 : ~UINT64_C(0) << ((doc & 63) + 1);
        candidates[word] &= keep;
      }
      candidateCount = 0;
      for (uint64_t word : candidates) candidateCount += std::popcount(word);
      candidateStart = std::max(doc, 0);
      restricted = true;
      skipCount(SkipStats::distanceFeatureCircleBuilds);
    }

  public:
    Scorer(MemPool& pool, IntColReader& column, const BKDReader* bkd,
           double lat, double lon, double pivot, float boost, int32_t maxDoc)
        : pool(pool), column(column), iter(column), bkd(bkd), lat(lat),
          lon(lon), pivot(pivot), boost(boost), maxDoc(maxDoc) {}

    int32_t next() override {
      if (restricted) return nextCandidate(doc + 1);
      return doc = iter.next();
    }

    int32_t advance(int32_t target) override {
      assert(doc < target);
      if (restricted) return nextCandidate(target);
      return doc = iter.advance(target);
    }

    int32_t docId() override { return doc; }

    float score() override {
      if (scoredDoc != doc) {
        scoredDoc = doc;
        docScore = scoreAt(boost, pivot, nearestMeters());
      }
      return docScore;
    }

    float matchCost() override {
      int64_t docs = column.docsWithValue();
      return docs > 0 ? 2.0f * (float)column.numValues() / (float)docs : 2.0f;
    }

    void setMinCompetitiveScore(float minScore) override {
      if (!(minScore > minCompetitiveScore) || disableSkippingForTests) return;
      minCompetitiveScore = minScore;
      if (doc == PostingsReader::END) return;
      if (minScore > boost) {
        // No doc scores above boost; ties at the floor still compete.
        exhausted = true;
        restricted = true;
        return;
      }
      if (minScore <= 0.0f || bkd == nullptr) return;
      // The distance at which the float score falls below the floor: the
      // floor is lowered by a few float ulps, since a doc slightly beyond the
      // exact distance can still round up to it, and the radius gets slack
      // on top. The scores themselves are computed exactly per doc.
      double floor = (double)minScore * (1.0 - 0x1p-22);
      double meters = pivot * ((double)boost / floor - 1.0);
      // A circle reaching the antipode covers the globe.
      if (!(meters < std::numbers::pi * geo::EARTH_MEAN_RADIUS_METERS)) return;
      if (meters > attemptedMeters * 0.5) return;
      attemptedMeters = meters;
      double radius = meters * (1.0 + 1e-6) + (pivot + meters) * 0x1p-22 + 0.01;
      uint64_t estimate = bkd->estimateIntersect(
          BKDDistanceRelation(lat, lon, radius)).upperBound;
      if (estimate * 2 <= (uint64_t)remaining()) restrict(radius);
    }

    bool prunesOnMinCompetitiveScore() const override { return true; }

    float getMaxScore(int32_t upTo) override {
      unused(upTo);
      return boost;
    }

    float getMaxScoreForSetup(int32_t upTo) override {
      unused(upTo);
      return boost;
    }

    ScoreBounds getScoreBounds(int32_t upTo) override {
      unused(upTo);
      return ScoreBounds::nonNegative(boost);
    }
  };

  class Weight final : public Query::Weight {
    DistanceFeatureQuery& query;
    IndexReader::FieldSegments segInfos;
    float boost;

    class Supplier final : public Query::ScorerSupplier {
      Weight& weight;
      IndexReader::Segment& segment;
      IntColReader& column;
      const BKDReader* bkd;

      class Plan final : public Query::ScorerPlan {
        Supplier& supplier;

      protected:
        Query::Scorer* buildScorer(MemPool& targetPool) override {
          return supplier.build(targetPool);
        }

      public:
        Plan(Supplier& supplier, const Query::PlanContext& planContext,
             const Query::ScorerShape& shape, int64_t cost)
          : Query::ScorerPlan(planContext, shape, cost), supplier(supplier) {}
      };

    public:
      Supplier(Weight& weight, IndexReader::Segment& segment,
               IntColReader& column, const BKDReader* bkd)
        : weight(weight), segment(segment), column(column), bkd(bkd) {}

      Query::Scorer* build(MemPool& targetPool) {
        const DistanceFeatureQuery& query = weight.query;
        return targetPool.make<Scorer>(
            targetPool, column, bkd, query.originLatitude,
            query.originLongitude, query.pivotMeters, weight.boost,
            segment.maxDoc());
      }

      int64_t cost() override { return column.docsWithValue(); }

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
        return planPool.make<Plan>(*this, planContext,
                                   describeScorer(planContext), cost());
      }
    };

  public:
    Weight(Query::Context& context, DistanceFeatureQuery& query, int32_t flags,
           float multiplier)
      : Query::Weight(context, query, flags), query(query),
        boost(constantWhenScored(flags, multiplier)) {
      segInfos = context.topReader.field(query.field);
    }

    Query::ScorerSupplier* scorerSupplierImpl(
        MemPool& targetPool, IndexReader::Segment& segment,
        Query::SupplierExecutionMode executionMode) override {
      unused(executionMode);
      const SegFieldInfo* info = segInfos.at((size_t)segment.ord);
      if (info == nullptr || info->columnLoc.offset() <= 0) return nullptr;
      if (info->type != FieldType::GEO_POINT) {
        throw std::runtime_error(std::string(QUERY_NAME)
                                 + " requires a GEO_POINT field");
      }
      auto* column = targetPool.make<IntColReader>(segment.postingsReader(), *info);
      if (column->numValues() == 0) return nullptr;
      const BKDReader* bkd = info->pointsMetaOff != 0
          ? targetPool.make<BKDReader>(segment.postingsReader(), *info) : nullptr;
      return targetPool.make<Supplier>(*this, segment, *column, bkd);
    }

    Query::Scorer* createScorer(MemPool& targetPool,
                                IndexReader::Segment& segment) override {
      auto* supplier = scorerSupplier(targetPool, segment);
      if (supplier == nullptr) return nullptr;
      Query::Demand demand = Query::Demand::fromLeadCost(
          std::numeric_limits<int64_t>::max());
      return supplier->resolve(
          targetPool, supplier->makePlanContext(demand))->build(targetPool);
    }
  };

  Query::Weight* createWeight(Context& context, int32_t flags,
                              float multiplier = 1.0f) override;
};

} // namespace luxir
