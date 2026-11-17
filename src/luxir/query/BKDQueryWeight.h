// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>

#include <boost/sort/spreadsort/integer_sort.hpp>

#include "luxir/query/PointsMaterialize.h"
#include "luxir/query/Query.h"
#include "luxir/reader/IntColReader.h"
#include "luxir/reader/SkipStats.h"

namespace luxir {

template <class PointSource, class Relation, class ColIter>
class BKDQueryScorer final : public Query::ConstantScorer {
  IntColReader& reader;
  ColIter iter;
  Relation relation;
  bool singlePoint;
  int32_t docid = -1;

  bool pointMatches(int64_t rank) {
    auto [v0, v1] = PointSource::pointAt(iter.values(), rank);
    return relation.matches(v0, v1);
  }

  bool valueMatches() {
    if (docid == PostingsReader::END) return false;
    // With one point per populated document, point rank is document rank.
    if (singlePoint) return pointMatches(iter.rank());
    auto [start, end] = PointSource::pointRanks(reader, iter.rank());
    for (int64_t rank = start; rank < end; rank++) {
      if (pointMatches(rank)) return true;
    }
    return false;
  }

public:
  BKDQueryScorer(IntColReader& reader, const Relation& relation,
                 float constantScore)
      : Query::ConstantScorer(constantScore), reader(reader), iter(reader),
        relation(relation), singlePoint(PointSource::onePointPerDoc(reader)) {}

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
  // A box test is two value compares; other relations scale it by their
  // average test. Multiple points increase the scorer's verification work.
  float matchCost() override {
    float point = (float)(PointSource::BOX_MATCH_COST
        * (1.0 + relation.pointCost().average / PointSource::BOX_TEST_POINTS));
    if (PointSource::onePointPerDoc(reader)) return point;
    int64_t docs = reader.docsWithValue();
    return docs > 0 ? point * (float)PointSource::pointCount(reader) / (float)docs : point;
  }

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

template <class QueryType, class Relation>
class BKDQueryWeight final : public Query::Weight {
  using PointSource = typename QueryType::PointSource;
  using PointTraits = typename PointSource::PointTraits;
  using Reader = typename PointSource::Reader;
  using Unsigned = typename PointTraits::Unsigned;

  QueryType& query;
  IndexReader::FieldSegments segInfos;
  float constantScore;

  bool segmentInfo(IndexReader::Segment& segment, const SegFieldInfo*& info) const {
    info = segInfos.at((size_t)segment.ord);
    return info != nullptr && info->columnLoc.offset() > 0;
  }

public:
  BKDQueryWeight(Query::Context& context, QueryType& query, int32_t flags,
                 float constantScore)
      : Query::Weight(context, query, flags), query(query),
        constantScore(constantScore) {
    traits |= IS_CONSTANT_SCORING;
    segInfos = context.topReader.field(query.getField());
  }

  // A BKD count can decode crossing leaves, so probes only report that level;
  // the walk belongs to execution after the supplier has been selected.
  Query::CountShortcut countShortcut(IndexReader::Segment& segment, DocSet* domain) override {
    const SegFieldInfo* info = nullptr;
    if (query.isEmpty() || !segmentInfo(segment, info) || PointSource::pointCount(*info) == 0) {
      return {0, Query::CountHow::METADATA};
    }
    auto how = info->pointsMetaOff != 0 && PointSource::onePointPerDoc(*info)
        ? Query::CountHow::INDEX_WALK : Query::CountHow::ITERATE;
    auto shortcut = Query::countBound(segment, domain, info->docsWithField, how);
    if (shortcut.how == Query::CountHow::INDEX_WALK) shortcut.resolver = this;
    return shortcut;
  }

  class Supplier final : public Query::ScorerSupplier {
    enum class ScorerArm : uint8_t {
      SPARSE_VERIFY,
      BKD,
      SCAN,
    };

    BKDQueryWeight& weight;
    MemPool& planPool;
    IndexReader::Segment& segment;
    IntColReader& reader;
    Reader* bkd;
    Relation relation;
    int64_t estimatedCost;
    uint64_t materializeUpperBound;
    // Points of the crossing leaves, which the traversal tests one by one.
    uint64_t crossingPoints;

    typename Reader::Scratch scratch(MemPool& pool) const {
      size_t size = bkd->maxPointsPerLeaf();
      return {pool.make_span<uint32_t>(size),
              pool.make_span<Unsigned>(size),
              pool.make_span<Unsigned>(size)};
    }

    PointsMaterialize::Materialized materialize(MemPool& pool) const {
      assert(bkd != nullptr);
      typename Reader::Scratch leafScratch = scratch(pool);
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

      // The estimate half-charges crossing leaves and cannot safely size the
      // array. upperBound charges every crossing point and is emission-safe.
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
      // BKD output is sorted only within leaves.
      boost::sort::spreadsort::integer_sort(used.begin(), used.end());
      if (!PointSource::onePointPerDoc(reader)) {
        size = (size_t)(std::unique(used.begin(), used.end()) - used.begin());
      }
      return {docs.first(size), nullptr};
    }

    // Producing the box's BKD membership: its matches into a bitset or a
    // sorted array, plus the point tests of crossing leaves. It charges the
    // estimate capped at the documents with a value; a multivalued box's BKD
    // emits each matching point.
    double bkdPrice() const {
      return PointsMaterialize::price((uint64_t)estimatedCost, segment.maxDoc())
          + (PointSource::CROSSING_POINTS + relation.pointCost().exact) * (double)crossingPoints;
    }

    // Verifying a candidate costs a column check plus the relation's average
    // test extra. The arm comparison uses this flat price for multivalued fields.
    double verifyPoints() const {
      return PointSource::VERIFY_POINTS + relation.pointCost().average;
    }

    // Verify or produce the BKD. A consumer that fills windows over an
    // explicit exposure (its candidates there), or that supplies the
    // candidates it expects to reach the box (a pull conjunction's demands),
    // gets the point emission comparison a numeric range makes: verifying
    // those candidates plus the work producing would spare it, against
    // producing. Under an exposure a verifying box, which cannot fill
    // windows, also turns the consumer away from its window plan. The verify
    // price comes from the source, independently of matchCost(). Elsewhere
    // it verifies for a lead sparser than its matches.
    ScorerArm selectScorerArm(const Query::PlanContext& context) const {
      const Query::Demand& demand = context.demand;
      bool exposure = fillsExposedWindows(context);
      if (bkd != nullptr && (exposure || demand.expectedCandidates >= 0)) {
        double candidates = exposure ? (double)demand.candidates
            : std::min(demand.expectedCandidates, (double)reader.docsWithValue());
        return PointsMaterialize::verifyPrice(candidates, demand, verifyPoints())
                <= bkdPrice()
            ? ScorerArm::SPARSE_VERIFY : ScorerArm::BKD;
      }
      if (demand.candidates < estimatedCost) return ScorerArm::SPARSE_VERIFY;
      return bkd != nullptr ? ScorerArm::BKD : ScorerArm::SCAN;
    }

    static Query::ScorerShape shapeFor(ScorerArm arm,
                                       std::optional<double> checkPrice) {
      bool bkdArm = arm == ScorerArm::BKD;
      return {
        .matchState = Query::MatchState::NONEMPTY,
        .directKind = Query::DirectScorerKind::OTHER,
        .reportedTwoPhase = bkdArm
            ? Query::ReportedTwoPhase::NO
            : Query::ReportedTwoPhase::YES,
        .windowFillClause = bkdArm
            ? Query::ClauseShape::DIRECT
            : Query::ClauseShape::NONE,
        .termDisjunctionClause = Query::ClauseShape::NONE,
        .termConjunctionClause = Query::ClauseShape::NONE,
        .independentTerm = Query::IndependentTermAccess::UNSUPPORTED,
        .docsOnly = Query::DocsOnlyAccess::UNSUPPORTED,
        .directDocSet = Query::DirectDocSetAccess::UNSUPPORTED,
        // Candidate consumption requires a bounded verifier before opting in.
        .membershipConsumption = Query::MembershipConsumption::WINDOWS,
        // The sparse verifier's check of one point; scans do not quote.
        .verificationPoints = arm == ScorerArm::SPARSE_VERIFY
            ? checkPrice : std::nullopt,
      };
    }

    Query::Scorer* buildScorer(
        MemPool& targetPool, ScorerArm arm) const {
      switch (arm) {
        case ScorerArm::SPARSE_VERIFY:
          skipCount(PointSource::sparseVerifyArms);
          return targetPool.make<
              BKDQueryScorer<PointSource, Relation, IntColReader::SparseIterator>>(
                  reader, relation, weight.constantScore);
        case ScorerArm::BKD:
          skipCount(PointSource::bkdArms);
          return PointsMaterialize::scorerFor(
              targetPool, materialize(targetPool), segment.maxDoc(),
              weight.constantScore);
        case ScorerArm::SCAN:
          skipCount(PointSource::scanArms);
          return targetPool.make<
              BKDQueryScorer<PointSource, Relation, IntColReader::Iterator>>(
                  reader, relation, weight.constantScore);
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
        : Query::ScorerPlan(planContext, shape, cost),
          supplier(supplier), arm(arm) {}
    };

  public:
    Supplier(BKDQueryWeight& weight, MemPool& planPool,
             IndexReader::Segment& segment,
             IntColReader& reader, Reader* bkd,
             typename Reader::EstimateResult estimate)
        : weight(weight), planPool(planPool), segment(segment),
          reader(reader), bkd(bkd),
          relation(weight.query.makeRelation()),
          estimatedCost((int64_t)std::min<uint64_t>(
              estimate.estimatedCount, (uint64_t)reader.docsWithValue())),
          materializeUpperBound(estimate.upperBound),
          crossingPoints(estimate.crossingPoints) {}

    int64_t cost() override { return estimatedCost; }

    // The sparse verifier quotes its check when the relation's price is
    // measured and the source guarantees one point per populated document.
    std::optional<double> checkPrice() const {
      if (!PointSource::onePointPerDoc(reader) || !relation.pointCost().quoted) return std::nullopt;
      return verifyPoints();
    }

    Query::ScorerShape describeScorer(
        const Query::PlanContext& buildContext) const override {
      return shapeFor(selectScorerArm(buildContext), checkPrice());
    }

    Query::UnresolvedSupplierCause unresolvedScorerCause(
        const Query::PlanContext& buildContext) const override {
      unused(buildContext);
      return Query::UnresolvedSupplierCause::NONE;
    }

    Query::ScorerPlan* resolve(
        MemPool& planPool,
        const Query::PlanContext& planContext) override {
      ScorerArm arm = selectScorerArm(planContext);
      // Verifying and scanning arms iterate every document with a value.
      int64_t iterated = arm == ScorerArm::BKD ? cost()
          : std::max(cost(), (int64_t)reader.docsWithValue());
      return planPool.make<Plan>(
          *this, planContext, shapeFor(arm, checkPrice()), iterated, arm);
    }

    BulkPlan planBulk(
        BulkUse use, const BulkScorerContext& bulkContext) override {
      unused(use);
      if (bulkContext.requireConstantCount) {
        auto shortcut = weight.countShortcut(segment, nullptr);
        if (shortcut.how == Query::CountHow::METADATA) {
          return {BulkAnswer::YES, BulkAnswer::NO, BulkAnswer::NO,
                  BulkAnswer::NO, nullptr, shortcut.maxMatches};
        }
        if (shortcut.how != Query::CountHow::INDEX_WALK) {
          return {
            BulkAnswer::NO, BulkAnswer::NO, BulkAnswer::NO,
            BulkAnswer::NO,
          };
        }
        assert(bkd != nullptr && PointSource::onePointPerDoc(reader) && !segment.hasDeletes());
        size_t size = bkd->maxPointsPerLeaf();
        typename Reader::Scratch countScratch{
          {}, planPool.make_span<Unsigned>(size),
          planPool.make_span<Unsigned>(size),
        };
        int64_t exact =
            (int64_t)bkd->countIntersect(relation, countScratch).exactCount;
        return {
          BulkAnswer::YES, BulkAnswer::NO, BulkAnswer::NO,
          BulkAnswer::NO, nullptr, exact,
        };
      }
      bool available = bkd != nullptr
          && !bulkContext.requireFilterConsumption;
      return {
        available ? BulkAnswer::YES : BulkAnswer::NO,
        available ? BulkAnswer::YES : BulkAnswer::NO,
        BulkAnswer::NO,
        BulkAnswer::NO,
      };
    }

    BulkScorer* buildBulk(
        MemPool& targetPool, const BulkPlan& plan) override {
      assert(plan.available == BulkAnswer::YES);
      assert(!plan.hasConstantCount());
      assert(bkd != nullptr);
      if (bkd == nullptr) return nullptr;
      skipCount(PointSource::bkdArms);
      return PointsMaterialize::bulkFor(
          targetPool, materialize(targetPool), segment.maxDoc(),
          weight.constantScore);
    }
  };

  Query::ScorerSupplier* scorerSupplierImpl(
      MemPool& targetPool, IndexReader::Segment& segment,
      Query::SupplierExecutionMode executionMode) override {
    unused(executionMode);
    if (query.isEmpty()) return nullptr;
    const SegFieldInfo* info = nullptr;
    if (!segmentInfo(segment, info)) return nullptr;
    auto* reader = PointSource::openColumn(
        targetPool, segment.postingsReader(), *info, QueryType::QUERY_NAME);
    if (PointSource::pointCount(*reader) == 0) return nullptr;
    Reader* bkd = nullptr;
    typename Reader::EstimateResult estimate{
        (uint64_t)reader->docsWithValue(), PointSource::pointCount(*reader)};
    if (info->pointsMetaOff != 0) {
      bkd = targetPool.make<Reader>(segment.postingsReader(), *info);
      if (bkd->pointCount() != PointSource::pointCount(*reader)) {
        throw std::runtime_error(std::string(QueryType::QUERY_NAME)
            + ": points/column value count mismatch");
      }
      Relation relation = query.makeRelation();
      estimate = bkd->estimateIntersect(relation);
      if (estimate.upperBound == 0) return nullptr;
    }
    return targetPool.make<Supplier>(
        *this, targetPool, segment, *reader, bkd, estimate);
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

  // Test and benchmark baseline that bypasses the BKD.
  Query::Scorer* createScanScorerForTests(
      MemPool& targetPool, IndexReader::Segment& segment) {
    if (query.isEmpty()) return nullptr;
    const SegFieldInfo* info = nullptr;
    if (!segmentInfo(segment, info)) return nullptr;
    auto* reader = PointSource::openColumn(
        targetPool, segment.postingsReader(), *info, QueryType::QUERY_NAME);
    if (PointSource::pointCount(*reader) == 0) return nullptr;
    Relation relation = query.makeRelation();
    return targetPool.make<BKDQueryScorer<PointSource, Relation, IntColReader::Iterator>>(
        *reader, relation, constantScore);
  }

};

} // namespace luxir
