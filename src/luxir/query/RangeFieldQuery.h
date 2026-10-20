// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string_view>

#include <boost/sort/spreadsort/integer_sort.hpp>

#include "luxir/query/PointsMaterialize.h"
#include "luxir/query/Query.h"
#include "luxir/reader/IntColReader.h"
#include "luxir/reader/RangeBKDReader.h"

namespace luxir {

// Relates the [min, max] intervals of a RANGE field to an inclusive encoded
// query interval [lo, hi]. Every relation is one box over (min, max):
//   intersects: min <= hi AND max >= lo
//   contains:   min <= lo AND max >= hi
//   within:     min >= lo AND max <= hi
// Execution mirrors the geo queries: the 2-D BKD when present and the lead
// is not much sparser, else a two-phase column verify or scan.
class RangeFieldQuery final : public Query {
public:
  enum class Relation : uint8_t { INTERSECTS, CONTAINS, WITHIN };

private:
  std::string_view field;
  Relation relation;
  RangeBoxRelation box;
  // REQUEST_STABLE when a DATE bound came from unrounded NOW math, as for
  // NumericPredicateQuery. equals()/hash() ignore it.
  FilterKeyScope filterKeyScope = FilterKeyScope::SEGMENT_STABLE;

  static RangeBoxRelation makeBox(Relation relation, int64_t lo, int64_t hi) {
    constexpr int64_t MIN = std::numeric_limits<int64_t>::min();
    constexpr int64_t MAX = std::numeric_limits<int64_t>::max();
    switch (relation) {
      case Relation::INTERSECTS: return {{MIN, lo}, {hi, MAX}};
      case Relation::CONTAINS: return {{MIN, hi}, {lo, MAX}};
      case Relation::WITHIN: return {{lo, MIN}, {MAX, hi}};
    }
    std::unreachable();
  }

public:
  static constexpr std::string_view QUERY_NAME = "RangeFieldQuery";
  class Weight;

  bool recoverableAsPredicate() const noexcept override { return true; }

  // lo <= hi: an empty query interval is the builder's match-nothing.
  RangeFieldQuery(std::string_view field, Relation relation, int64_t lo, int64_t hi,
                  FilterKeyScope filterKeyScope = FilterKeyScope::SEGMENT_STABLE)
    : Query(QueryKind::RANGE_FIELD), field(field), relation(relation),
      box(makeBox(relation, lo, hi)), filterKeyScope(filterKeyScope) {
    assert(lo <= hi);
  }

  std::string_view getField() const { return field; }
  Relation getRelation() const { return relation; }
  const RangeBoxRelation& getBox() const { return box; }

  bool equalsSameKind(const Query& other) const override {
    const auto& rhs = static_cast<const RangeFieldQuery&>(other);
    return field == rhs.field && relation == rhs.relation
        && std::ranges::equal(box.lo, rhs.box.lo) && std::ranges::equal(box.hi, rhs.box.hi);
  }

  uint64_t hashImpl() const override {
    uint64_t value = mixHash(Query::hashImpl(), field);
    value = mixHash(value, (uint8_t)relation);
    for (int d = 0; d < 2; d++) {
      value = mixHash(value, box.lo[d]);
      value = mixHash(value, box.hi[d]);
    }
    return value;
  }

  FilterKeyScope appendFilterKey(FilterKeyBuilder& out,
                                 const FilterKeyContext& ctx) const override {
    unused(ctx);
    out.appendKind(kind);
    out.appendString(field);
    out.appendByte((uint8_t)relation);
    for (int d = 0; d < 2; d++) {
      out.appendInt64(box.lo[d]);
      out.appendInt64(box.hi[d]);
    }
    return filterKeyScope;
  }

  Query::Weight* createWeight(Context& context, int32_t flags,
                              float multiplier = 1.0f) override;
};

// Verifies each doc's (min, max) pairs against the box, one column lookup
// per doc. The column always has the multi-valued layout (see RangeFieldType).
template <class ColIter>
class RangeFieldScorer final : public Query::ConstantScorer {
  IntColReader& reader;
  ColIter iter;
  RangeBoxRelation box;
  int32_t docid = -1;

  bool valueMatches() {
    if (docid == PostingsReader::END) return false;
    auto [start, end] = reader.getStartEndValueRank(iter.rank());
    for (int64_t rank = start; rank + 1 < end; rank += 2) {
      if (box.matches(iter.values().valueAt(rank), iter.values().valueAt(rank + 1))) return true;
    }
    return false;
  }

public:
  RangeFieldScorer(IntColReader& reader, const RangeBoxRelation& box, float constantScore)
    : Query::ConstantScorer(constantScore), reader(reader), iter(reader), box(box) {}

  int32_t approximationNext() override {
    docid = iter.next();
    return docid;
  }
  int32_t approximationAdvance(int32_t target) override {
    docid = iter.advance(target);
    return docid;
  }
  int32_t approximationDocId() override { return docid; }
  bool matches() override { return valueMatches(); }
  float matchCost() override { return 4.0f; }

  int32_t next() override {
    for (;;) {
      docid = iter.next();
      if (docid == PostingsReader::END || valueMatches()) return docid;
    }
  }
  int32_t advance(int32_t target) override {
    assert(docid < target);
    docid = iter.advance(target);
    while (docid != PostingsReader::END && !valueMatches()) {
      docid = iter.next();
    }
    return docid;
  }
  int32_t docId() override { return docid; }
};

class RangeFieldQuery::Weight final : public Query::Weight {
  RangeFieldQuery& query;
  IndexReader::FieldSegments segInfos;
  float constantScore;

  bool segmentInfo(IndexReader::Segment& segment, const SegFieldInfo*& info) const {
    info = segInfos.at((size_t)segment.ord);
    return info != nullptr && info->columnLoc.offset() > 0;
  }

  // One point per doc: a BKD count is a doc count.
  static bool onePairPerDoc(const SegFieldInfo& info) {
    return info.numValues == 2 * (int64_t)info.docsWithField;
  }

  static IntColReader* openColumn(MemPool& pool, IndexReader::Segment& segment,
                                  const SegFieldInfo& info) {
    if (info.type != FieldType::RANGE) {
      throw std::runtime_error(std::string(QUERY_NAME) + " requires a range field");
    }
    auto* reader = pool.make<IntColReader>(segment.postingsReader(), info);
    if (!reader->multiValued() || reader->numValues() % 2 != 0) {
      throw std::runtime_error(std::string(QUERY_NAME) + ": malformed range column");
    }
    return reader;
  }

public:
  Weight(Query::Context& context, RangeFieldQuery& query, int32_t flags, float constantScore)
    : Query::Weight(context, query, flags), query(query), constantScore(constantScore) {
    traits |= IS_CONSTANT_SCORING;
    segInfos = context.topReader.field(query.getField());
  }

  // A BKD count can decode crossing leaves, so probes only report that level;
  // the walk belongs to execution after the supplier has been selected.
  Query::CountShortcut countShortcut(IndexReader::Segment& segment, DocSet* domain) override {
    const SegFieldInfo* info = nullptr;
    if (!segmentInfo(segment, info) || info->numValues == 0) {
      return {0, Query::CountHow::METADATA};
    }
    auto how = info->pointsMetaOff != 0 && onePairPerDoc(*info)
        ? Query::CountHow::INDEX_WALK : Query::CountHow::ITERATE;
    auto shortcut = Query::countBound(segment, domain, info->docsWithField, how);
    if (shortcut.how == Query::CountHow::INDEX_WALK) shortcut.resolver = this;
    return shortcut;
  }

  class Supplier final : public Query::ScorerSupplier {
    enum class ScorerArm : uint8_t { SPARSE_VERIFY, BKD, SCAN };

    Weight& weight;
    MemPool& planPool;
    IndexReader::Segment& segment;
    IntColReader& reader;
    RangeBKDReader* bkd;
    int64_t estimatedCost;
    uint64_t materializeUpperBound;

    PointsMaterialize::Materialized materialize(MemPool& pool) const {
      assert(bkd != nullptr);
      const RangeBoxRelation& box = weight.query.getBox();
      RangeBKDReader::Scratch scratch = bkd->scratch(pool);
      if (PointsMaterialize::useBitset((uint64_t)estimatedCost, segment.maxDoc())) {
        auto words = pool.make_span<uint64_t>(FixedBitSet::sizeInWords(segment.maxDoc()));
        std::fill(words.begin(), words.end(), 0);
        FixedBitSet bits(words.data(), segment.maxDoc());
        bkd->intersect(box, scratch,
            [&bits](int32_t doc) { bits.set(doc); },
            [&bits](int32_t begin, int32_t end) { PointsMaterialize::setRun(bits, begin, end); });
        return {{}, words.data()};
      }

      // The estimate half-charges crossing leaves and cannot size the array;
      // the upper bound charges every crossing point.
      auto docs = pool.make_span<int32_t>((size_t)materializeUpperBound);
      size_t size = 0;
      auto addDoc = [&docs, &size](int32_t doc) { docs[size++] = doc; };
      bkd->intersect(box, scratch, addDoc,
          [&addDoc](int32_t begin, int32_t end) {
            for (int32_t doc = begin; doc < end; doc++) addDoc(doc);
          });
      assert(size <= materializeUpperBound);
      auto used = docs.first(size);
      // BKD output is sorted only within leaves.
      boost::sort::spreadsort::integer_sort(used.begin(), used.end());
      size = (size_t)(std::unique(used.begin(), used.end()) - used.begin());
      return {docs.first(size), nullptr};
    }

    ScorerArm selectScorerArm(int64_t candidateCost) const {
      if (candidateCost < estimatedCost) return ScorerArm::SPARSE_VERIFY;
      return bkd != nullptr ? ScorerArm::BKD : ScorerArm::SCAN;
    }

    static Query::ScorerShape shapeFor(ScorerArm arm) {
      bool bkdArm = arm == ScorerArm::BKD;
      return {
        .matchState = Query::MatchState::NONEMPTY,
        .directKind = Query::DirectScorerKind::OTHER,
        .reportedTwoPhase = bkdArm ? Query::ReportedTwoPhase::NO : Query::ReportedTwoPhase::YES,
        .windowFillClause = bkdArm ? Query::ClauseShape::DIRECT : Query::ClauseShape::NONE,
        .termDisjunctionClause = Query::ClauseShape::NONE,
        .termConjunctionClause = Query::ClauseShape::NONE,
        .independentTerm = Query::IndependentTermAccess::UNSUPPORTED,
        .docsOnly = Query::DocsOnlyAccess::UNSUPPORTED,
        .directDocSet = Query::DirectDocSetAccess::UNSUPPORTED,
      };
    }

    Query::Scorer* buildScorer(MemPool& targetPool, ScorerArm arm) const {
      const RangeBoxRelation& box = weight.query.getBox();
      switch (arm) {
        case ScorerArm::SPARSE_VERIFY:
          skipCount(SkipStats::rangeFieldSparseVerifyArms);
          return targetPool.make<RangeFieldScorer<IntColReader::SparseIterator>>(
              reader, box, weight.constantScore);
        case ScorerArm::BKD:
          skipCount(SkipStats::rangeFieldBKDArms);
          return PointsMaterialize::scorerFor(targetPool, materialize(targetPool), segment.maxDoc(),
                                              weight.constantScore);
        case ScorerArm::SCAN:
          skipCount(SkipStats::rangeFieldScanArms);
          return targetPool.make<RangeFieldScorer<IntColReader::Iterator>>(
              reader, box, weight.constantScore);
      }
      std::unreachable();
    }

    class Plan final : public Query::ScorerPlan {
      Supplier& supplier;
      ScorerArm arm;

    protected:
      Query::Scorer* buildScorer(MemPool& targetPool) override {
        return supplier.buildScorer(targetPool, arm);
      }

    public:
      Plan(Supplier& supplier, const Query::PlanContext& planContext,
           const Query::ScorerShape& shape, int64_t cost, ScorerArm arm)
        : Query::ScorerPlan(planContext, shape, cost), supplier(supplier), arm(arm) {}
    };

  public:
    Supplier(Weight& weight, MemPool& planPool, IndexReader::Segment& segment,
             IntColReader& reader, RangeBKDReader* bkd, BKDReader::EstimateResult estimate)
      : weight(weight), planPool(planPool), segment(segment), reader(reader), bkd(bkd),
        estimatedCost((int64_t)std::min<uint64_t>(estimate.estimatedCount,
                                                  (uint64_t)reader.docsWithValue())),
        materializeUpperBound(estimate.upperBound) {}

    int64_t cost() override { return estimatedCost; }

    Query::ScorerShape describeScorer(const Query::PlanContext& buildContext) const override {
      return shapeFor(selectScorerArm(buildContext.demand.candidates));
    }

    Query::UnresolvedSupplierCause unresolvedScorerCause(
        const Query::PlanContext& buildContext) const override {
      unused(buildContext);
      return Query::UnresolvedSupplierCause::NONE;
    }

    Query::ScorerPlan* resolve(MemPool& planPool, const Query::PlanContext& planContext) override {
      ScorerArm arm = selectScorerArm(planContext.demand.candidates);
      return planPool.make<Plan>(*this, planContext, shapeFor(arm), cost(), arm);
    }

    BulkPlan planBulk(BulkUse use, const BulkScorerContext& bulkContext) override {
      unused(use);
      if (bulkContext.requireConstantCount) {
        auto shortcut = weight.countShortcut(segment, nullptr);
        if (shortcut.how == Query::CountHow::METADATA) {
          return {BulkAnswer::YES, BulkAnswer::NO, BulkAnswer::NO, BulkAnswer::NO,
                  nullptr, shortcut.maxMatches};
        }
        if (shortcut.how != Query::CountHow::INDEX_WALK) {
          return {BulkAnswer::NO, BulkAnswer::NO, BulkAnswer::NO, BulkAnswer::NO};
        }
        assert(bkd != nullptr && segment.liveDocs() == nullptr);
        int64_t exact = (int64_t)bkd->countIntersect(weight.query.getBox(), bkd->scratch(planPool));
        return {BulkAnswer::YES, BulkAnswer::NO, BulkAnswer::NO, BulkAnswer::NO, nullptr, exact};
      }
      bool available = bkd != nullptr && !bulkContext.requireFilterConsumption;
      return {
        available ? BulkAnswer::YES : BulkAnswer::NO,
        available ? BulkAnswer::YES : BulkAnswer::NO,
        BulkAnswer::NO,
        BulkAnswer::NO,
      };
    }

    BulkScorer* buildBulk(MemPool& targetPool, const BulkPlan& plan) override {
      assert(plan.available == BulkAnswer::YES);
      assert(!plan.hasConstantCount());
      unused(plan);
      if (bkd == nullptr) return nullptr;
      skipCount(SkipStats::rangeFieldBKDArms);
      return PointsMaterialize::bulkFor(targetPool, materialize(targetPool), segment.maxDoc(),
                                        weight.constantScore);
    }
  };

  Query::ScorerSupplier* scorerSupplierImpl(MemPool& targetPool, IndexReader::Segment& segment,
                                            Query::SupplierExecutionMode executionMode) override {
    unused(executionMode);
    const SegFieldInfo* info = nullptr;
    if (!segmentInfo(segment, info)) return nullptr;
    auto* reader = openColumn(targetPool, segment, *info);
    if (reader->numValues() == 0) return nullptr;
    RangeBKDReader* bkd = nullptr;
    BKDReader::EstimateResult estimate{(uint64_t)reader->docsWithValue(),
                                       (uint64_t)reader->numValues() / 2};
    if (info->pointsMetaOff != 0) {
      bkd = targetPool.make<RangeBKDReader>(segment.postingsReader(), *info);
      if (bkd->pointCount() * 2 != (uint64_t)reader->numValues()) {
        throw std::runtime_error(std::string(QUERY_NAME) + ": points/column value count mismatch");
      }
      estimate = bkd->estimateIntersect(query.getBox());
      if (estimate.upperBound == 0) return nullptr;
    }
    return targetPool.make<Supplier>(*this, targetPool, segment, *reader, bkd, estimate);
  }

  Query::Scorer* createScorer(MemPool& targetPool, IndexReader::Segment& segment) override {
    auto* supplier = scorerSupplier(targetPool, segment);
    if (supplier == nullptr) return nullptr;
    Query::Demand demand = Query::Demand::fromLeadCost(std::numeric_limits<int64_t>::max());
    return supplier->resolve(targetPool, supplier->makePlanContext(demand))->build(targetPool);
  }
};

} // namespace luxir
