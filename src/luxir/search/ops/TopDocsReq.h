#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <limits>
#include <functional>
#include "SearchOp.h"
#include "luxir/query/Query.h"
#include "luxir/query/QueryShape.h"
#include "luxir/search/SearchOverrides.h"
#include "luxir/query/QueryPrep.h"
#include "luxir/reader/SkipStats.h"
#include "luxir/reader/IntColReader.h"
#include "luxir/reader/StoredFieldsReader.h"
#include "luxir/reader/StrColReader.h"
#include "luxir/search/Collector.h"
#include "luxir/search/DomainVariantPlan.h"
#include "luxir/search/EmitDocs.h"
#include "luxir/search/FieldSortCollector.h"
#include "luxir/search/MergeableCollector.h"
#include "luxir/search/SortField.h"
#include "luxir/search/SearchRequest.h"
#include "luxir/util/AtomicMerger.h"

namespace luxir {

struct ParsedFilter {
  Query* query;
  std::span<const std::string_view> exceptOps;
  // Selection ANY keeps one logical routed filter while sourcing its value
  // memberships independently from the cache. Ordinary filters and
  // single-value selections leave this empty and use query directly.
  std::span<Query* const> unionSources{};

  size_t sourceCount() const {
    return unionSources.empty() ? 1 : unionSources.size();
  }

  Query* source(size_t index) const {
    assert(index < sourceCount());
    return unionSources.empty() ? query : unionSources[index];
  }

  bool composesUnion() const { return !unionSources.empty(); }
};


class TopDocsReq : public SearchOp {
protected:

public:
  class Calc;

  // Exact COUNT + pruned TOP_100 over the 5M/301-union cohort crossed between
  // glass (0.758%, 1.008 compose/exhaustive) and philadelphia (0.861%, 0.977).
  // Require an estimated match-candidate cost of at least maxDoc/128
  // (0.781%). The count supplier already caps a filtered conjunction by its
  // cheapest required side, so the gate reflects either a sparse filter or a
  // sparse union body.
  static constexpr int32_t kExactCountTopKMinCandidateDensityInverse = 128;
  // On the same cohort, composed/exhaustive was 0.68 at depth 10, 0.96 at
  // depth 100, and 1.23 at depth 1000 after canonical survivor rescoring.
  static constexpr int32_t kExactCountTopKMaxDepth = 100;
  static inline int32_t exactCountTopKMinCandidateDensityInverseForTests =
      kExactCountTopKMinCandidateDensityInverse;
  // A sparse filter that is cheaper than its union body makes the exhaustive
  // scored batch the natural single pass. Keep this knee separate from the
  // batch admission constant even though both measured at /44.
  static constexpr int32_t
      kExactCountTopKSparseFilterSinglePassDensityInverse = 44;
  static inline int32_t
      exactCountTopKSparseFilterSinglePassDensityInverseForTests =
          kExactCountTopKSparseFilterSinglePassDensityInverse;
  static inline bool
      disableExactCountTopKSparseFilterSinglePassForTests = false;

  // Filtered MUST conjunctions can abandon competitive-score pruning when the
  // filter caps the candidate set tightly enough. The depth-specific knees
  // bracketed on the 5M Wikipedia corpus are:
  //   TOP_10:   0.11% wins; 1.02% has an intersection loss
  //   TOP_100:  2.16% wins; 3.95% has an uncached term loss
  //   TOP_1000: 4.98% wins; 7.96% has an uncached term loss
  // Use the conservative power-of-two boundary inside each bracket.
  static constexpr std::array<int32_t, 3>
      kSparseFilteredTopKDensityInverse{128, 32, 16};
  static inline std::array<int32_t, 3>
      sparseFilteredTopKDensityInverseForTests =
          kSparseFilteredTopKDensityInverse;
  // Direct term unions retain useful competitive pruning at higher filter
  // densities, especially at shallow top-k depths.
  static constexpr std::array<int32_t, 3>
      kSparseFilteredTopKUnionDensityInverse{128, 128, 64};
  static inline std::array<int32_t, 3>
      sparseFilteredTopKUnionDensityInverseForTests =
          kSparseFilteredTopKUnionDensityInverse;
  static inline bool disableSparseFilteredTopKRerouteForTests = false;
  static inline bool disableSparseFilteredTopKUnionForTests = false;
  // Forces independent first-K capture and count-only bulk arrangements for
  // complete constant-scoring top-k requests, and disables the limit-only
  // bounded drain for parity tests.
  static inline bool disableConstantWindowCaptureForTests = false;

  enum class ConstantScoreDisposition : uint8_t {
    NOT_CONSTANT_TOP_K,
    LIMIT_ONLY,
    COMPLETE,
  };

  enum class ExactCountTopKRoute : uint8_t {
    EXACT_CANDIDATE_SCORING,
    CONSTANT_FIRST_K,
  };

  struct FieldSortBestFirstPlan {
    FieldSortCollector::KeyBlockPlan keys;
    int64_t expectedFloor = 0;

    bool available() const noexcept { return keys.batch != nullptr; }
  };

  struct CacheFirstFieldSortPlan {
    FilterCache::Use* acceptedUse = nullptr;
    std::span<uint8_t> routes;
    bool decided = false;

    bool omitsWeight() const noexcept { return acceptedUse != nullptr; }
  };

  static FieldSortBestFirstPlan planFieldSortBestFirst(
      FieldSortCollector& collector, int64_t card, int32_t maxDoc) {
    if (disableFieldSortPruning || disableFieldSortBestFirst || card <= 0) {
      return {};
    }
    auto keys = collector.maskedKeyBlockPlan();
    if (keys.batch == nullptr) return {};
    int64_t expectedFloor = std::min<int64_t>(
        keys.leafCount,
        (collector.topCount * (int64_t)maxDoc + card - 1) / card);
    if (2 * expectedFloor >= keys.leafCount
        && !forceFieldSortBestFirst) {
      return {};
    }
    return {keys, expectedFloor};
  }

  static bool fieldSortCanUseMaskedBestFirst(const SortPlan& sortPlan) {
    if (sortPlan.clauses.size() != 1
        || sortPlan.clauses[0].getKind() != SortClause::COLUMN) {
      return false;
    }
    auto type = const_cast<FieldType&>(
        sortPlan.clauses[0].getSortField().getFieldType()).type();
    return type == FieldType::INT || type == FieldType::DATE
        || type == FieldType::FLOAT || type == FieldType::DOUBLE;
  }

  static int64_t residentFieldSortCard(
      const FilterCache::ExistingCandidate& candidate,
      IndexReader::Segment& segment) {
    const BitDocSet* liveDocs = segment.liveDocs() == nullptr
        ? nullptr : &segment.liveDocs()->docset();
    return candidate.residentCard((size_t)segment.ord, liveDocs);
  }

  // A complete inert candidate supplies exact per-segment cardinalities.
  // Return true only when every segment selects the existing best-first
  // economics. card <= 0 remains a rejection exactly as in
  // planFieldSortBestFirst(); needing no execution work does not authorize
  // cache hit effects for a rejected route.
  static bool planResidentFieldSortBestFirstRoutes(
      const FilterCache::ExistingCandidate& candidate,
      IndexReader& reader, const SortPlan& sortPlan, int64_t topCount,
      std::span<uint8_t> routes) {
    assert(routes.size() == reader.segments().size());
    assert(fieldSortCanUseMaskedBestFirst(sortPlan));
    FieldSortCollector collector(
        topCount, sortPlan.clauses, &reader, false);
    MemPool pool;
    bool allRouted = true;
    for (auto& segment : reader.segments()) {
      auto savepoint = pool.getSavePoint();
      int64_t card = residentFieldSortCard(candidate, segment);
      bool routed = false;
      if (card > 0) {
        collector.setSegment(
            segment.ord, &segment.postingsReader(), &pool, card);
        routed = planFieldSortBestFirst(
            collector, card, segment.maxDoc()).available();
      }
      routes[(size_t)segment.ord] = (uint8_t)routed;
      allRouted &= routed;
      pool.rewind(savepoint);
    }
    return allRouted;
  }

  // Move only the provable Stage 4b decisions ahead of Weight construction.
  // An incomplete lookup or dynamic conjunction returns undecided so the
  // landed Weight-bearing router remains authoritative.
  static CacheFirstFieldSortPlan planCacheFirstFieldSortWholeMembership(
      Query& query, Query::PlanningContext& planning, IndexReader& reader,
      const SortPlan& sortPlan, int64_t topCount,
      bool allowReaderStable) {
    Query::VerificationWork verification =
        membershipVerificationWork(query);
    bool verificationRoute =
        verification == Query::VerificationWork::PRESENT;
    bool numericBestFirst = fieldSortCanUseMaskedBestFirst(sortPlan);
    Query::FieldSortConjunction conjunction = numericBestFirst
        ? query.fieldSortConjunction(planning)
        : Query::FieldSortConjunction::NOT_FLAT;

    bool mayInspectResident = verificationRoute
        || (numericBestFirst
            && conjunction == Query::FieldSortConjunction::NOT_FLAT);
    if (!mayInspectResident) {
      if (numericBestFirst
          && conjunction == Query::FieldSortConjunction::DYNAMIC_TERMS) {
        return {};
      }
      auto routes = planning.pool.make_span<uint8_t>(
          reader.segments().size());
      std::fill(routes.begin(), routes.end(), 0);
      return {nullptr, routes, true};
    }

    auto candidate = planning.lookupExistingFilterUse(
        query, allowReaderStable);
    if (!candidate.has_value()) return {};

    auto routes = planning.pool.make_span<uint8_t>(
        reader.segments().size());
    bool allRouted;
    if (verificationRoute) {
      std::fill(routes.begin(), routes.end(), 1);
      allRouted = true;
    } else {
      allRouted = planResidentFieldSortBestFirstRoutes(
          *candidate, reader, sortPlan, topCount, routes);
    }
    if (!allRouted) return {nullptr, routes, true};

    FilterCache::Use* use = planning.acceptExistingFilterUse(
        std::move(*candidate), FilterCache::AdmissionLane::WHOLE);
    return use == nullptr ? CacheFirstFieldSortPlan{}
                          : CacheFirstFieldSortPlan{use, routes, true};
  }

  // Cached membership pays for FIELD_SORT when it removes verification work
  // from the query-driven ladder, or when its estimated cardinality unlocks
  // the exact-set best-first route. Flat term conjunction cost is only the
  // cheapest posting list, not a useful estimate of intersection membership,
  // so it cannot establish the best-first route by itself. VerificationWork
  // includes nested two-phase children hidden behind a single-phase compound
  // protocol. Plan before getFilterUse(): a request rejected in every segment
  // must not create a Use, record a WHOLE-lane sighting, or admit metadata.
  static bool planFieldSortWholeMembershipRoutes(
      Query& query, Query::Weight& weight, IndexReader& reader,
      const SortPlan& sortPlan, int64_t topCount,
      std::span<uint8_t> routes) {
    assert(routes.size() == reader.segments().size());
    bool canUseBestFirst = fieldSortCanUseMaskedBestFirst(sortPlan);
    if (!canUseBestFirst) {
      bool routed = membershipVerificationWork(query)
          == Query::VerificationWork::PRESENT;
      std::fill(routes.begin(), routes.end(), (uint8_t)routed);
      return routed;
    }
    FieldSortCollector collector(
        topCount, sortPlan.clauses, &reader, false);
    MemPool pool;
    bool anyRouted = false;
    for (auto& segment : reader.segments()) {
      auto savepoint = pool.getSavePoint();
      auto* supplier = weight.scorerSupplier(pool, segment);
      int64_t estimatedCard = supplier == nullptr ? 0 : supplier->cost();
      if (segment.liveDocs() != nullptr) {
        estimatedCard = std::min(
            estimatedCard,
            (int64_t)segment.liveDocs()->docset().card());
      }
      bool routed = false;
      if (supplier != nullptr) {
        auto context = supplier->makePlanContext(
            Query::Demand::fromLeadCost(
                estimatedCard, Query::ExecutionUse::MATCH_WINDOWS));
        routed = supplier->verificationWork(context)
            == Query::VerificationWork::PRESENT;
        if (!routed && canUseBestFirst
            && supplier->describeScorer(context).termConjunctionClause
                != Query::ClauseShape::FLAT_CONJUNCTION) {
          collector.setSegment(
              segment.ord, &segment.postingsReader(), &pool, estimatedCard);
          routed = planFieldSortBestFirst(
              collector, estimatedCard, segment.maxDoc()).available();
        }
      }
      routes[(size_t)segment.ord] = (uint8_t)routed;
      anyRouted |= routed;
      pool.rewind(savepoint);
    }
    return anyRouted;
  }

  static int32_t sparseFilteredTopKDensityInverse(
      Query::Weight::SparseFilteredTopKFamily family, int64_t topCount) {
    if (topCount <= 0) return 0;
    const auto& densityInverses =
        family == Query::Weight::SparseFilteredTopKFamily::UNION
            ? sparseFilteredTopKUnionDensityInverseForTests
            : sparseFilteredTopKDensityInverseForTests;
    if (topCount <= 10) {
      return densityInverses[0];
    }
    if (topCount <= 100) {
      return densityInverses[1];
    }
    if (topCount <= 1000) {
      return densityInverses[2];
    }
    return 0;
  }

  static bool admitSparseFilteredTopK(
      Query::Weight& weight, IndexReader& reader, int64_t topCount) {
    if (disableSparseFilteredTopKRerouteForTests) {
      return false;
    }
    auto family = weight.sparseFilteredTopKFamily();
    if (family == Query::Weight::SparseFilteredTopKFamily::UNION
        && disableSparseFilteredTopKUnionForTests) {
      return false;
    }
    int32_t densityInverse =
        sparseFilteredTopKDensityInverse(family, topCount);
    if (densityInverse <= 0) {
      return false;
    }
    int64_t filterCost = 0;
    MemPool pool;
    for (auto& segment : reader.segments()) {
      auto savepoint = pool.getSavePoint();
      int64_t segmentCost =
          weight.sparseFilteredTopKCost(pool, segment);
      pool.rewind(savepoint);
      if (segmentCost < 0) {
        skipCount(SkipStats::sparseFilteredTopKShapeRejects);
        return false;
      }
      filterCost += segmentCost;
    }
    bool admitted = filterCost <= std::max<int64_t>(
        1, reader.maxDoc() / densityInverse);
    if (!admitted) {
      skipCount(SkipStats::sparseFilteredTopKDensityRejects);
    }
    return admitted;
  }

  const ReqTopDocs& topDocsProto;  // the relevant part of the protobuf request
  Query::PlanningContext& planning;
  Query::Context* weightContext;
  Query* query;
  Query::Weight* weight;
  Query::Weight* variantMembershipWeight;
  Query::Weight* countWeight;
  Query::Weight* rankingWeight;
  Query::Weight* wholeRankingWeight;
  bool wholeConstantRanking;
  Query::ScoreProfile scoreProfile;
  bool requestNeedsScores;
  int64_t topCount; // maximum number of docs to return.
  std::span<ParsedFilter> filters;
  std::span<Query::Weight*> filterWeights;
  std::span<FilterCache::Use*> filterUses;
  DomainVariantPlan domainVariants;
  CollectionRequirements requirements;
  QueryPrep::WholeMembershipPlan wholeMembershipPlan;
  QueryPrep::ExactDomainPlan exactDomainPlan;
  SortPlan sortPlan;

  // Optional sink for the merged top-K collector.  If set, the Calc invokes
  // it instead of self-emitting via fillQueryTopNResponse, letting another
  // op (e.g. FusionOp) consume this TopDocsReq's ranking.  `mc` is null on
  // the empty-index path (no segments, no collector ever obtained); sinks
  // must handle that case.
  std::function<void(Calc&, MergeableCollector*)> rankingSink;


  class Calc : public SearchOp::Calculator {
  public:
    luxir::api::Val* getTargetForSub(SearchResponse* resp, Calculator* sub) override {
      auto* ourVal = parent->getTargetForSub(resp, this);
      // the Val should either be unset, or have a DocList
      assert(
        ourVal != nullptr && (std::holds_alternative<luxir::api::DocList>(ourVal->kind)
          || std::holds_alternative<std::monostate>(ourVal->kind)));
      auto& dl = oneofMut<luxir::api::DocList>(*ourVal);
      return build::opsSlot(dl.ops, op.subOps.size(), sub->getOp().name, resp->mr);
    }

    Calc(TopDocsReq& op, Calculator* parent) : SearchOp::Calculator(op, parent, -1, -1), collectorMerger(nullptr, nullptr) {

      collectorMerger.creator = [&op]() -> MergeableCollector* {
        return new MergeableCollector(
            op.topCount, op.sortPlan, op.req.reader.get(),
            op.requestNeedsScores);
      };
      collectorMerger.destroyer = [](MergeableCollector* data) {
        delete data;
      };

      if (op.subOps.size() > 0) {
        if (op.domainVariants.empty()
            || !op.domainVariants.needsInheritProduction()) {
          output.resize(op.req.reader->segments().size());
        }
        subCalcs.reserve(op.subOps.size());
        if (!op.domainVariants.empty()) {
          subCalcVariants.reserve(op.subOps.size());
        }
        for (auto& [key, subOp] : op.subOps) {
          auto* subCalc = subOp->createCalculator(this, -1);
          subCalcs.emplace_back(subCalc);
          if (!op.domainVariants.empty()) {
            subCalcVariants.push_back(
                op.domainVariants.variantForChild(key));
          }
        }
      }
      requiresWholeIndexPrepare =
          op.weight != nullptr && op.weight->needsPrepare();
      for (auto* weight : op.filterWeights) {
        if (weight->needsPrepare()) {
          requiresWholeIndexPrepare = true;
          break;
        }
      }
      if (!op.domainVariants.empty()
          && op.domainVariants.sourcesNeedPrepare()) {
        requiresWholeIndexPrepare = true;
      }
      if (requiresWholeIndexPrepare) {
        auto numSegs = op.req.reader->segments().size();
        inputDomains.resize(numSegs);
        inputDomainViews.resize(numSegs);
        effectiveDomains.resize(numSegs);
        effectiveDomainViews.resize(numSegs);
      }
    }

    TopDocsReq& thisOp() {
      return static_cast<TopDocsReq&>(op);
    }



    AtomicMerger<MergeableCollector> collectorMerger;
    MaxScoreAccumulator scoreAccumulator;


    // Produced domains for sub-ops. A result either owns an intersection /
    // collected set or borrows request-pinned cache state.
    std::vector<DomainHandle> output;
    std::vector<std::unique_ptr<Calculator>> subCalcs;
    std::vector<size_t> subCalcVariants;

    // Prepared path state. Root delivery already supplies every segment
    // domain at once. A nested streaming parent can still use the per-segment
    // entry point, in which case the last arrival starts preparation.
    // Calculator-level decision: the main weight or at least one filter
    // requires the all-domain preparation phase. This is distinct from the
    // main weight's Weight::needsPrepare() trait because a filter alone can
    // require the phase.
    bool requiresWholeIndexPrepare = false;
    std::atomic<int32_t> gatheredDomainsSeen{0};
    std::vector<DomainHandle> inputDomains;
    std::vector<DocSet*> inputDomainViews;
    std::vector<DomainHandle> effectiveDomains;
    std::vector<DocSet*> effectiveDomainViews;
    std::vector<QueryPrep::PreparedSource> preparedFilterSources;
    std::vector<QueryPrep::PreparedSource> preparedResetFilterSources;
    std::vector<QueryPrep::PreparedSource> preparedDomainSources;
    std::vector<std::vector<DomainHandle>> preparedRoutedFilters;
    std::vector<std::vector<DomainHandle>> preparedResetRoutedFilters;
    std::vector<std::vector<DomainHandle>> preparedDomainExtraSets;
    DomainVariantPlan::PreparedResetVariants preparedDomainVariants;
    std::vector<std::shared_ptr<DomainVariantPlan::Reservation>>
        preparedRoutedReservations;
    std::unique_ptr<Query::Weight::PreparedWeight> preparedWeight;
    std::vector<std::vector<DomainHandle>> preparedVariantInheritBases;

    void calc(oneapi::tbb::task_group* tg, int32_t segnum,
              DomainHandle domain) override {
      assert(domain.isDeliverable());
      if (segnum < 0) {
        assert(segnum == -1);
        completeEmpty(tg);
        return;
      }
      if (!requiresWholeIndexPrepare) {
        task_group_run(tg, [this, tg, segnum, domain]() {
          doCollect(tg, segnum, domain);
        });
        return;
      }

      assert(segnum < (int32_t)inputDomains.size());
      inputDomains[(size_t)segnum] = std::move(domain);
      inputDomainViews[(size_t)segnum] = inputDomains[(size_t)segnum].get();
      auto count =
          gatheredDomainsSeen.fetch_add(1, std::memory_order_acq_rel) + 1;
      if (count != (int32_t)thisOp().req.reader->segments().size()) return;
      startPrepare(tg);
    }

    void calcAll(oneapi::tbb::task_group* tg,
                 std::span<const DomainHandle> domains) override {
      assert(domains.size() == thisOp().req.reader->segments().size());
      if (domains.empty()) {
        completeEmpty(tg);
        return;
      }
      if (!requiresWholeIndexPrepare) {
        dispatch(tg, domains);
        return;
      }
      std::copy(domains.begin(), domains.end(), inputDomains.begin());
      for (size_t i = 0; i < inputDomains.size(); i++) {
        assert(inputDomains[i].isDeliverable());
        inputDomainViews[i] = inputDomains[i].get();
      }
      startPrepare(tg);
    }

    Query::ScorerSupplier* mainScorerSupplier(MemPool& pool, IndexReader::Segment& seg) {
      auto& op = thisOp();
      assert(op.weight != nullptr);
      Query::SegmentSource& source = preparedWeight != nullptr
        ? static_cast<Query::SegmentSource&>(*preparedWeight)
        : static_cast<Query::SegmentSource&>(*op.weight);
      return source.scorerSupplier(pool, seg);
    }

    static Query::ScorerPlan* resolvePullScorer(
        MemPool& pool, Query::ScorerSupplier& supplier,
        int64_t candidates = std::numeric_limits<int64_t>::max()) {
      Query::Demand demand = Query::Demand::fromLeadCost(
          candidates);
      return supplier.resolve(
          pool, supplier.makePlanContext(demand));
    }

    static Query::Scorer* buildPullScorer(
        MemPool& pool, Query::ScorerSupplier& supplier,
        int64_t candidates = std::numeric_limits<int64_t>::max()) {
      Query::ScorerPlan* plan = resolvePullScorer(
          pool, supplier, candidates);
      return plan->build(pool);
    }

    ConstantScoreDisposition constantScoreDisposition(
        const TopDocsCollector& collector,
        DocSetBuilder* builder) noexcept {
      const auto& op = thisOp();
      if (collector.topCount <= 0 || op.weight == nullptr
          || !op.weight->isConstantScoring()) {
        return ConstantScoreDisposition::NOT_CONSTANT_TOP_K;
      }
      if (!TopDocsReq::disableConstantWindowCaptureForTests
          && !op.requirements.needExactCount && builder == nullptr
          && op.filters.empty()) {
        return ConstantScoreDisposition::LIMIT_ONLY;
      }
      return ConstantScoreDisposition::COMPLETE;
    }

    static Query::Scorer* buildBoundedConstantScorer(
        MemPool& pool, Query::ScorerSupplier& supplier,
        DocSet* filter, int64_t topCount) {
      int64_t candidates = filter == nullptr
          ? std::min<int64_t>(supplier.cost(), topCount)
          : supplier.cost();
      return buildPullScorer(pool, supplier, candidates);
    }

    Query::Scorer* createMainScorer(MemPool& pool, IndexReader::Segment& seg) {
      auto* supplier = mainScorerSupplier(pool, seg);
      if (supplier == nullptr) return nullptr;
      return buildPullScorer(pool, *supplier);
    }

    bool admitExactCountTopK(Query::ScorerSupplier& countSupplier,
                             DocSet* collectorFilter, int32_t maxDoc) {
      if (thisOp().topCount > TopDocsReq::kExactCountTopKMaxDepth) {
        return false;
      }
      int32_t densityInverse =
          TopDocsReq::exactCountTopKMinCandidateDensityInverseForTests;
      if (densityInverse <= 0) {
        return false;
      }
      int64_t candidateCost = countSupplier.cost();
      if (collectorFilter != nullptr) {
        candidateCost = std::min<int64_t>(
            candidateCost, collectorFilter->card());
      }
      if (candidateCost < std::max<int64_t>(
              1, (int64_t) maxDoc / densityInverse)) {
        return false;
      }

      int32_t sparseInverse =
          TopDocsReq::
              exactCountTopKSparseFilterSinglePassDensityInverseForTests;
      auto split = countSupplier.exactCountTopKCosts();
      if (!TopDocsReq::
              disableExactCountTopKSparseFilterSinglePassForTests
          && sparseInverse > 0 && split.available()
          && split.filter < split.unionSide
          && split.filter <= (int64_t) maxDoc / sparseInverse) {
        skipCount(
            SkipStats::exactCountTopKSparseFilterSinglePassRejects);
        return false;
      }
      return true;
    }

    void collectKnownCountTopK(
        MemPool& pool, int32_t segnum, int64_t count,
        Query::ScorerSupplier* rankingSupplier, DocSet* collectorFilter,
        TopDocsCollector& collector,
        int32_t maxDoc, bool allowPruning,
        BulkScorer* exactScorer = nullptr) {
      int64_t before = collector.totalHits();
      if (rankingSupplier != nullptr) {
        Query::ScorerSupplier::BulkScorerContext bulkContext;
        auto plan = rankingSupplier->planBulk(
            Query::ScorerSupplier::BulkUse::SCORED_WINDOWS,
            bulkContext);
        auto* rankingBulk =
            plan.available != Query::ScorerSupplier::BulkAnswer::YES
            ? nullptr : rankingSupplier->buildBulk(pool, plan);
        if (plan.available == Query::ScorerSupplier::BulkAnswer::NO) {
          rankingSupplier->recordBulkPlanCommitment(
              Query::ScorerSupplier::BulkUse::SCORED_WINDOWS,
              bulkContext, plan);
        }
        if (rankingBulk != nullptr) {
          collectTopKWindowed(
              segnum, rankingBulk, collectorFilter, collector,
              allowPruning ? &scoreAccumulator : nullptr,
              maxDoc, allowPruning, exactScorer);
        } else {
          auto* rankingScorer = buildPullScorer(pool, *rankingSupplier);
          if (rankingScorer != nullptr) {
            collectTopK(
                segnum, rankingScorer, collectorFilter, nullptr,
                collector, allowPruning,
                allowPruning ? &scoreAccumulator : nullptr);
          }
        }
      }
      int64_t ranked = collector.totalHits() - before;
      assert(count >= ranked);
      collector.hitCount += count - ranked;
    }

    void countThenCollectTopK(
        MemPool& pool, int32_t segnum, BulkScorer* countBulk,
        Query::ScorerSupplier* rankingSupplier, DocSet* collectorFilter,
        DocSetBuilder* builder, TopDocsCollector& collector,
        int32_t maxDoc, bool allowPruning,
        BulkScorer* exactScorer = nullptr) {
      int64_t count = countMatchesWindowed(
          countBulk, collectorFilter, builder, maxDoc);
      collectKnownCountTopK(
          pool, segnum, count, rankingSupplier, collectorFilter,
          collector, maxDoc, allowPruning, exactScorer);
    }

    bool composeVariableScoreExactCountTopK(
        MemPool& pool, int32_t segnum,
        Query::ScorerSupplier& scoringSupplier,
        Query::ScorerSupplier& countSupplier, DocSet* collectorFilter,
        TopDocsCollector& collector, int32_t maxDoc) {
      Query::ScorerSupplier::BulkScorerContext bulkContext;
      auto scoringPlan = scoringSupplier.planBulk(
          Query::ScorerSupplier::BulkUse::EXACT_CANDIDATE_SCORING,
          bulkContext);
      bool scoringDeclined =
          scoringPlan.available
              != Query::ScorerSupplier::BulkAnswer::YES
          || scoringPlan.supportsExactCandidateScoring
              != Query::ScorerSupplier::BulkAnswer::YES;
      if (scoringDeclined) {
        auto countPlan = countSupplier.planBulk(
            Query::ScorerSupplier::BulkUse::COUNT_WINDOWS,
            bulkContext);
        countSupplier.recordBulkPlanCommitment(
            Query::ScorerSupplier::BulkUse::COUNT_WINDOWS,
            bulkContext, countPlan);
        if (countPlan.available
            != Query::ScorerSupplier::BulkAnswer::NO) {
          scoringSupplier.recordBulkPlanCommitment(
              Query::ScorerSupplier::BulkUse::EXACT_CANDIDATE_SCORING,
              bulkContext, scoringPlan);
        }
        return false;
      }

      auto countPlan = countSupplier.planBulk(
          Query::ScorerSupplier::BulkUse::COUNT_WINDOWS,
          bulkContext);
      if (countPlan.available
          != Query::ScorerSupplier::BulkAnswer::YES) {
        countSupplier.recordBulkPlanCommitment(
            Query::ScorerSupplier::BulkUse::COUNT_WINDOWS,
            bulkContext, countPlan);
        return false;
      }
      auto* countBulk = countSupplier.buildBulk(pool, countPlan);
      if (countBulk == nullptr) return false;
      auto* rankingSupplier = thisOp().rankingWeight->scorerSupplier(
          pool, thisOp().planning.topReader.segments()[segnum]);
      auto* exactScorer = scoringSupplier.buildBulk(pool, scoringPlan);
      if (exactScorer == nullptr) return false;
      assert(exactScorer->supportsExactCandidateScoring());
      countThenCollectTopK(
          pool, segnum, countBulk, rankingSupplier, collectorFilter,
          nullptr, collector, maxDoc, true, exactScorer);
      return true;
    }

    bool countThenCollectConstantTopK(
        MemPool& pool, int32_t segnum,
        Query::ScorerSupplier& countSupplier, DocSet* collectorFilter,
        TopDocsCollector& collector, int32_t maxDoc) {
      Query::ScorerSupplier::BulkScorerContext constantContext;
      constantContext.requireConstantCount = true;
      auto constantPlan = countSupplier.planBulk(
          Query::ScorerSupplier::BulkUse::COUNT_WINDOWS,
          constantContext);
      int64_t count = collectorFilter == nullptr
              && constantPlan.hasConstantCount()
          ? constantPlan.constantCount : -1;

      Query::ScorerSupplier::BulkScorerContext bulkContext;
      if (count < 0) {
        auto countPlan = countSupplier.planBulk(
            Query::ScorerSupplier::BulkUse::COUNT_WINDOWS,
            bulkContext);
        if (countPlan.available
            != Query::ScorerSupplier::BulkAnswer::YES) {
          countSupplier.recordBulkPlanCommitment(
              Query::ScorerSupplier::BulkUse::COUNT_WINDOWS,
              bulkContext, countPlan);
          return false;
        }
        auto* countBulk = countSupplier.buildBulk(pool, countPlan);
        if (countBulk == nullptr) return false;
        count = countMatchesWindowed(
            countBulk, collectorFilter, nullptr, maxDoc);
      }

      int64_t captured = 0;
      if (count > 0) {
        auto* rankingSupplier = thisOp().rankingWeight->scorerSupplier(
            pool, thisOp().planning.topReader.segments()[segnum]);
        if (rankingSupplier == nullptr) return false;
        auto* rankingScorer = buildBoundedConstantScorer(
            pool, *rankingSupplier, collectorFilter, collector.topCount);
        if (rankingScorer == nullptr) return false;
        captured = collectFirstKConstant(
            segnum, rankingScorer, collectorFilter, collector,
            collector.topCount);
      }
      assert(count >= captured);
      collector.hitCount += count - captured;
      return true;
    }

    DomainHandle materializeFilter(
        size_t filterIndex, size_t sourceOffset,
        IndexReader::Segment& seg, DocSet* baseDomain,
        const std::vector<QueryPrep::PreparedSource>* prepared,
        const std::shared_ptr<DomainVariantPlan::Reservation>& reservation) {
      auto& op = thisOp();
      const auto& filter = op.filters[filterIndex];
      std::vector<DomainHandle> sources;
      std::vector<DocSet*> sourcePtrs;
      sources.reserve(filter.sourceCount());
      sourcePtrs.reserve(filter.sourceCount());
      for (size_t i = 0; i < filter.sourceCount(); i++) {
        size_t source = sourceOffset + i;
        DomainHandle docs = prepared != nullptr
            ? QueryPrep::materializeEffectiveFilter(
                  (*prepared)[source], *op.req.reader, seg, baseDomain)
            : QueryPrep::materializeEffectiveFilter(
                  *op.filterWeights[source], op.filterUses[source],
                  *op.req.reader, seg, baseDomain);
        if (reservation != nullptr && docs.get() != nullptr
            && docs.isDeliverable()) {
          reservation->grow(docs.get()->ramBytesUsed());
        }
        if (docs.get() == nullptr && filter.composesUnion()) {
          return {};
        }
        sourcePtrs.push_back(docs.get());
        sources.push_back(std::move(docs));
      }
      if (!filter.composesUnion()) {
        assert(sources.size() == 1);
        return std::move(sources[0]);
      }

      assert(sourcePtrs.size() > 1);
      skipCount(SkipStats::selectionUnionCompositions);
      DomainHandle result(DocSet::union_(sourcePtrs));
      if (reservation != nullptr && result.get() != nullptr) {
        reservation->grow(result.get()->ramBytesUsed());
      }
      return result;
    }

    DomainHandle buildEffectiveDomain(
        int32_t segnum, DocSet* baseDomain) {
      auto& op = thisOp();
      if (op.filterWeights.empty()) return {};
      auto& seg = op.req.reader->segments()[segnum];

      std::vector<DomainHandle> filters;
      std::vector<DocSet*> filterPtrs;
      filters.reserve(op.filters.size());
      filterPtrs.reserve(op.filters.size());
      size_t source = 0;
      for (size_t i = 0; i < op.filters.size(); i++) {
        filters.push_back(materializeFilter(
            i, source, seg, baseDomain, &preparedFilterSources, nullptr));
        source += op.filters[i].sourceCount();
        if (filters.back().get() != nullptr) {
          filterPtrs.push_back(filters.back().get());
        }
      }
      assert(source == op.filterWeights.size());
      if (filterPtrs.empty()) return {};
      if (filterPtrs.size() == 1) {
        auto result = std::move(filters[0]);
        return std::move(result).pinnedWith(op.planning.filterUses);
      }
      return DomainHandle(DocSet::intersect(filterPtrs));
    }

    void completeEmpty(oneapi::tbb::task_group* tg) {
      std::span<const DomainHandle> noDomains;
      for (auto& subCalc : subCalcs) {
        subCalc->calcAll(tg, noDomains);
      }
      doneCollecting();
    }

    void startPrepare(oneapi::tbb::task_group* tg) {
      task_group_run(tg, [this, tg]() {
        prepareAndDispatch(tg);
      });
    }

    void prepareMainVariants(oneapi::tbb::task_group* tg) {
      auto& op = thisOp();
      if (preparedWeight == nullptr
          || !op.domainVariants.needsInheritProduction()
          || preparedWeight->domainDependence()
              == PreparedDomainDependence::QUERY_CANONICAL) {
        return;
      }

      auto segments = op.req.reader->segments();
      size_t baseCount = op.domainVariants.inheritBaseCount();
      std::vector<std::vector<DomainHandle>> eligibility(segments.size());
      for (size_t segnum = 0; segnum < segments.size(); segnum++) {
        eligibility[segnum] = op.domainVariants.produceSharedBases(
            inputDomains[segnum], preparedRoutedFilters[segnum]);
        assert(eligibility[segnum].size() == baseCount);
      }

      std::vector<size_t> representatives;
      std::vector<size_t> groupForBase(baseCount);
      for (size_t base = 0; base < baseCount; base++) {
        auto found = std::ranges::find_if(
            representatives, [&](size_t representative) {
              return DomainVariantPlan::sameWholeReaderEffectiveDomain(
                  eligibility, base, representative, segments);
            });
        if (found == representatives.end()) {
          groupForBase[base] = representatives.size();
          representatives.push_back(base);
        } else {
          groupForBase[base] = (size_t) (found - representatives.begin());
        }
      }
      assert(!representatives.empty() && representatives[0] == 0);

      preparedVariantInheritBases.assign(
          segments.size(), std::vector<DomainHandle>(baseCount));
      std::vector<std::vector<DomainHandle>> groupMatches(
          representatives.size(), std::vector<DomainHandle>(segments.size()));
      for (size_t group = 0; group < representatives.size(); group++) {
        size_t representative = representatives[group];
        std::vector<DocSet*> domainViews(segments.size());
        for (size_t segnum = 0; segnum < segments.size(); segnum++) {
          domainViews[segnum] = eligibility[segnum][representative].get();
        }
        Query::Weight::PreparedWeight* variantWeight;
        std::unique_ptr<Query::Weight::PreparedWeight> ownedWeight;
        std::vector<std::shared_ptr<DomainVariantPlan::Reservation>>
            variantReservations;
        if (representative == 0) {
          variantWeight = preparedWeight.get();
        } else {
          size_t variant =
              op.domainVariants.representativeInheritVariant(representative);
          variantReservations.resize(segments.size());
          for (size_t segnum = 0; segnum < segments.size(); segnum++) {
            variantReservations[segnum] =
                op.domainVariants.reserveVariantPreparation(
                    op.req.memoryTracker, segments[segnum].maxDoc(),
                    "top_docs '" + std::string(op.name) + "' variant "
                        + std::to_string(variant) + " segment "
                        + std::to_string(segnum));
          }
          Query::Weight::PrepareContext context{
            *op.req.reader,
            std::span<DocSet* const>(
                domainViews.data(), domainViews.size()),
            tg != nullptr
          };
          ownedWeight = op.weight->prepare(context);
          variantWeight = ownedWeight.get();
        }
        for (size_t segnum = 0; segnum < segments.size(); segnum++) {
          auto reservation = representative == 0
              ? preparedRoutedReservations[segnum]
              : variantReservations[segnum];
          DomainHandle matches(QueryPrep::materialize(
              *op.weight, variantWeight, segments[segnum],
              eligibility[segnum][representative].get()));
          if (matches.get() != nullptr) {
            reservation->grow(matches.get()->ramBytesUsed());
          }
          groupMatches[group][segnum] =
              std::move(matches).retainedWith(reservation);
        }
      }
      for (size_t base = 0; base < baseCount; base++) {
        for (size_t segnum = 0; segnum < segments.size(); segnum++) {
          preparedVariantInheritBases[segnum][base] =
              groupMatches[groupForBase[base]][segnum];
        }
      }
    }

    void prepareDomainQueryVariants(
        std::span<DocSet* const> liveDomains,
        oneapi::tbb::task_group* tg) {
      auto& op = thisOp();
      size_t sourceCount = op.domainVariants.sourceCount();
      size_t segmentCount = op.req.reader->segments().size();
      preparedDomainSources.resize(sourceCount);
      std::vector<size_t> extraSources;
      std::vector<size_t> resetExtraSources;
      std::vector<Query::Weight*> extraWeights;
      std::vector<FilterCache::Use*> extraUses;
      for (size_t source = 0; source < sourceCount; source++) {
        preparedDomainSources[source].weight =
            op.domainVariants.sourceWeight(source);
        preparedDomainSources[source].cacheUse =
            op.domainVariants.sourceUse(source);
        if (op.domainVariants.sourceIsExtra(source)) {
          extraSources.push_back(source);
          extraWeights.push_back(op.domainVariants.sourceWeight(source));
          extraUses.push_back(op.domainVariants.sourceUse(source));
        }
        if (op.domainVariants.sourceIsResetExtra(source)) {
          resetExtraSources.push_back(source);
        }
      }
      Query::Weight::PrepareContext liveContext{
        *op.req.reader, liveDomains, tg != nullptr
      };
      auto preparedExtras = QueryPrep::prepareFilterSources(
          extraWeights, extraUses, liveContext);
      for (size_t i = 0; i < extraSources.size(); i++) {
        preparedDomainSources[extraSources[i]] =
            std::move(preparedExtras[i]);
      }

      preparedDomainExtraSets.assign(
          segmentCount, std::vector<DomainHandle>(sourceCount));
      if (op.domainVariants.needsResetParentFilters()) {
        preparedResetRoutedFilters.resize(segmentCount);
      }
      for (size_t segnum = 0; segnum < segmentCount; segnum++) {
        auto reservation = preparedRoutedReservations[segnum];
        if (op.domainVariants.needsResetParentFilters()) {
          preparedResetRoutedFilters[segnum] =
              materializeResetParentFilters(
                  (int32_t) segnum, reservation);
        }
        auto& segment = op.req.reader->segments()[segnum];
        for (size_t source : resetExtraSources) {
          DomainHandle set = QueryPrep::materializeEffectiveFilter(
              preparedDomainSources[source], *op.req.reader, segment,
              liveDomains[segnum]);
          if (set.get() != nullptr && set.isDeliverable()) {
            reservation->grow(set.get()->ramBytesUsed());
          }
          preparedDomainExtraSets[segnum][source] =
              std::move(set).pinnedWith(op.planning.filterUses);
        }
      }

      size_t variantCount = op.domainVariants.variantCount();
      std::vector<std::vector<DomainHandle>> eligibility(
          segmentCount, std::vector<DomainHandle>(variantCount));
      for (size_t segnum = 0; segnum < segmentCount; segnum++) {
        DomainHandle live = DomainHandle::pinned(
            liveDomains[segnum], op.req.reader);
        for (size_t variant = 1; variant < variantCount; variant++) {
          if (op.domainVariants.frame(variant)
              != DomainVariantPlan::Frame::RESET) {
            continue;
          }
          std::vector<DomainHandle> parts;
          parts.push_back(live);
          if (!preparedResetRoutedFilters.empty()) {
            for (size_t filter = 0;
                 filter < preparedResetRoutedFilters[segnum].size();
                 filter++) {
              if (op.domainVariants.includesFilter(variant, filter)) {
                parts.push_back(preparedResetRoutedFilters[segnum][filter]);
              }
            }
          }
          for (size_t source : op.domainVariants.extraSources(variant)) {
            parts.push_back(preparedDomainExtraSets[segnum][source]);
          }
          eligibility[segnum][variant] = DomainVariantPlan::compose(
              std::move(parts));
        }
      }

      preparedDomainVariants = op.domainVariants.prepareResetVariants(
          preparedDomainSources, eligibility, liveDomains,
          *op.req.reader, tg != nullptr,
          [&](size_t variant, size_t segnum) {
            auto& segment = op.req.reader->segments()[segnum];
            return op.domainVariants.reserveVariantPreparation(
                op.req.memoryTracker, segment.maxDoc(),
                "top_docs '" + std::string(op.name) + "' variant "
                    + std::to_string(variant) + " segment "
                    + std::to_string(segnum));
          });
    }

    void prepareAndDispatch(oneapi::tbb::task_group* tg) {
      auto& op = thisOp();
      auto baseDomains = std::span<DocSet* const>(
          inputDomainViews.data(), inputDomainViews.size());
      Query::Weight::PrepareContext baseCtx{
        *op.req.reader,
        baseDomains,
        tg != nullptr
      };
      bool routed = !op.domainVariants.empty();
      bool mainNeedsPrepare = op.weight != nullptr
          && (op.wholeMembershipPlan.preparesMainWeight(*op.weight)
              || op.weight->needsPrepare());
      if (routed) {
        size_t segmentCount = op.req.reader->segments().size();
        preparedRoutedReservations.resize(segmentCount);
        if (mainNeedsPrepare) {
          preparedRoutedFilters.resize(segmentCount);
        }
        for (size_t i = 0; i < segmentCount; i++) {
          auto& seg = op.req.reader->segments()[i];
          std::string detail = "top_docs '" + std::string(op.name) + "'";
          if (mainNeedsPrepare) detail += " variant 0";
          detail += " segment " + std::to_string(i);
          preparedRoutedReservations[i] =
              op.domainVariants.reserveProduction(
                  op.req.memoryTracker, seg.maxDoc(), detail);
          op.planning.filterUses->enableRoutedAccounting(
              i, op.req.memoryTracker, detail);
        }
      }
      preparedFilterSources = QueryPrep::prepareFilterSources(
          op.filterWeights, op.filterUses, baseCtx);

      if (routed
          && (op.domainVariants.sourceCount() > 0
              || op.domainVariants.needsResetParentFilters())) {
        std::vector<DocSet*> liveDomains;
        liveDomains.reserve(op.req.reader->segments().size());
        for (auto& segment : op.req.reader->segments()) {
          liveDomains.push_back(segment.liveDocs() == nullptr
              ? nullptr : &segment.liveDocs()->docset());
        }
        Query::Weight::PrepareContext liveCtx{
          *op.req.reader,
          std::span<DocSet* const>(
              liveDomains.data(), liveDomains.size()),
          tg != nullptr
        };
        if (op.domainVariants.needsResetParentFilters()) {
          preparedResetFilterSources = QueryPrep::prepareFilterSources(
              op.filterWeights, op.filterUses, liveCtx);
        }
        prepareDomainQueryVariants(
            std::span<DocSet* const>(
                liveDomains.data(), liveDomains.size()), tg);
      }

      if (routed) {
        if (mainNeedsPrepare) {
          for (size_t i = 0; i < op.req.reader->segments().size(); i++) {
            preparedRoutedFilters[i] = materializeRoutedFilters(
                (int32_t) i, baseDomains[i],
                preparedRoutedReservations[i]);
            effectiveDomains[i] = op.domainVariants.defaultDomain(
                inputDomains[i], preparedRoutedFilters[i]);
            assert(effectiveDomains[i].isDeliverable());
            effectiveDomainViews[i] = effectiveDomains[i].get();
          }
          Query::Weight::PrepareContext queryCtx{
            *op.req.reader,
            std::span<DocSet* const>(
                effectiveDomainViews.data(), effectiveDomainViews.size()),
            tg != nullptr
          };
          if (op.wholeMembershipPlan.preparesMainWeight(*op.weight)) {
            preparedWeight =
                op.wholeMembershipPlan.prepareMainWeight(queryCtx);
          } else {
            preparedWeight = op.weight->prepare(queryCtx);
          }
          prepareMainVariants(tg);
          effectiveDomainViews.clear();
          effectiveDomains.clear();
        }
        dispatch(
            tg,
            std::span<const DomainHandle>(
                inputDomains.data(), inputDomains.size()));
        return;
      }

      for (size_t i = 0; i < op.req.reader->segments().size(); i++) {
        if (op.filterWeights.empty()) {
          effectiveDomains[i] = inputDomains[i];
        } else {
          effectiveDomains[i] =
              buildEffectiveDomain((int32_t)i, baseDomains[i]);
        }
        assert(effectiveDomains[i].isDeliverable());
        effectiveDomainViews[i] = effectiveDomains[i].get();
      }

      Query::Weight::PrepareContext queryCtx{
        *op.req.reader,
        std::span<DocSet* const>(effectiveDomainViews.data(), effectiveDomainViews.size()),
        tg != nullptr
      };
      if (op.weight != nullptr
          && op.wholeMembershipPlan.preparesMainWeight(*op.weight)) {
        preparedWeight = op.wholeMembershipPlan.prepareMainWeight(queryCtx);
      } else if (op.weight != nullptr && op.weight->needsPrepare()) {
        preparedWeight = op.weight->prepare(queryCtx);
      }

      dispatch(
          tg,
          std::span<const DomainHandle>(
              effectiveDomains.data(), effectiveDomains.size()));
    }

    void dispatch(
        oneapi::tbb::task_group* tg,
        std::span<const DomainHandle> domains) {
      assert(domains.size() == thisOp().req.reader->segments().size());
      // Keep the same task-stack ordering as Calculator::calcAll.
      for (int32_t segnum = 0; segnum < (int32_t)domains.size(); segnum++) {
        DomainHandle domain = domains[(size_t)segnum];
        assert(domain.isDeliverable());
        task_group_run(tg, [this, tg, segnum, domain]() {
          doCollect(tg, segnum, domain);
        });
      }
    }

    std::vector<DomainHandle> materializeRoutedFilters(
        int32_t segnum, DocSet* baseDomain,
        const std::shared_ptr<DomainVariantPlan::Reservation>& reservation) {
      auto& op = thisOp();
      auto& seg = op.req.reader->segments()[(size_t) segnum];
      std::vector<DomainHandle> filters;
      filters.reserve(op.filters.size());
      size_t source = 0;
      for (size_t i = 0; i < op.filters.size(); i++) {
        DomainHandle filter = materializeFilter(
            i, source, seg, baseDomain,
            requiresWholeIndexPrepare ? &preparedFilterSources : nullptr,
            reservation);
        source += op.filters[i].sourceCount();
        filters.push_back(
            std::move(filter).pinnedWith(op.planning.filterUses));
        assert(filters.back().isDeliverable());
      }
      assert(source == op.filterWeights.size());
      return filters;
    }

    std::vector<DomainHandle> materializeResetParentFilters(
        int32_t segnum,
        const std::shared_ptr<DomainVariantPlan::Reservation>& reservation) {
      auto& op = thisOp();
      auto& seg = op.req.reader->segments()[(size_t) segnum];
      DocSet* live = seg.liveDocs() == nullptr
          ? nullptr : &seg.liveDocs()->docset();
      std::vector<DomainHandle> filters;
      filters.reserve(op.filters.size());
      size_t source = 0;
      for (size_t i = 0; i < op.filters.size(); i++) {
        filters.push_back(std::move(materializeFilter(
            i, source, seg, live,
            requiresWholeIndexPrepare ? &preparedResetFilterSources : nullptr,
            reservation)).pinnedWith(op.planning.filterUses));
        source += op.filters[i].sourceCount();
      }
      assert(source == op.filterWeights.size());
      return filters;
    }

    DomainHandle materializeDomainSource(
        size_t source, int32_t segnum, DocSet* base,
        const std::shared_ptr<DomainVariantPlan::Reservation>& reservation) {
      auto& op = thisOp();
      auto& seg = op.req.reader->segments()[(size_t) segnum];
      DocSet* live = seg.liveDocs() == nullptr
          ? nullptr : &seg.liveDocs()->docset();
      if (!preparedDomainExtraSets.empty()
          && op.domainVariants.sourceIsResetExtra(source)
          && DomainVariantPlan::sameEffectiveDomain(
              base, live, seg.maxDoc())) {
        return preparedDomainExtraSets[(size_t) segnum][source];
      }
      DomainHandle result = preparedDomainSources.empty()
          ? QueryPrep::materializeEffectiveFilter(
                *op.domainVariants.sourceWeight(source),
                op.domainVariants.sourceUse(source),
                *op.req.reader, seg, base)
          : QueryPrep::materializeEffectiveFilter(
                preparedDomainSources[source], *op.req.reader, seg, base);
      if (result.get() != nullptr && result.isDeliverable()) {
        reservation->grow(result.get()->ramBytesUsed());
      }
      return std::move(result).pinnedWith(op.planning.filterUses);
    }

    struct RoutedProduction {
      DomainHandle defaultEligibility;
      std::vector<DomainHandle> filters;
      std::vector<DomainHandle> resetFilters;
      std::vector<DomainHandle> inheritBases;
      std::vector<DomainHandle> domains;
      std::shared_ptr<DomainVariantPlan::Reservation> reservation;
      bool captureSharedRaw = false;
      bool inheritReady = false;

      DomainHandle inheritDefault() const {
        assert(inheritReady);
        return domains[0];
      }
    };

    Query::SegmentSource& variantMembershipSource() {
      auto& op = thisOp();
      if (preparedWeight != nullptr) {
        return *preparedWeight;
      }
      assert(op.variantMembershipWeight != nullptr);
      return *op.variantMembershipWeight;
    }

    void finishResetDomains(RoutedProduction& result, int32_t segnum) {
      auto& op = thisOp();
      auto& seg = op.req.reader->segments()[(size_t) segnum];
      DocSet* live = seg.liveDocs() == nullptr
          ? nullptr : &seg.liveDocs()->docset();
      for (size_t variant = 1;
           variant < op.domainVariants.variantCount(); variant++) {
        if (op.domainVariants.frame(variant)
            != DomainVariantPlan::Frame::RESET) {
          continue;
        }
        if (!preparedDomainVariants.ready.empty()
            && preparedDomainVariants.ready[variant] != 0) {
          result.domains[variant] =
              preparedDomainVariants.domains[(size_t) segnum][variant];
          continue;
        }
        std::vector<DomainHandle> parts;
        parts.push_back(materializeDomainSource(
            op.domainVariants.resetSource(variant), segnum, live,
            result.reservation));
        for (size_t filter = 0; filter < result.resetFilters.size(); filter++) {
          if (op.domainVariants.includesFilter(variant, filter)) {
            parts.push_back(result.resetFilters[filter]);
          }
        }
        for (size_t source : op.domainVariants.extraSources(variant)) {
          parts.push_back(materializeDomainSource(
              source, segnum, live, result.reservation));
        }
        result.domains[variant] = DomainVariantPlan::compose(
            std::move(parts));
        result.domains[variant] =
            std::move(result.domains[variant]).retainedWith(
                result.reservation);
      }
    }

    void finishInheritDomains(RoutedProduction& result, int32_t segnum) {
      auto& op = thisOp();
      assert(!result.inheritBases.empty());
      result.domains[0] = result.inheritBases[0];
      result.domains[0] = std::move(result.domains[0]).retainedWith(
          result.reservation);
      for (size_t variant = 1;
           variant < op.domainVariants.variantCount(); variant++) {
        if (op.domainVariants.frame(variant)
            != DomainVariantPlan::Frame::INHERIT) {
          continue;
        }
        DomainHandle base = result.inheritBases[
            op.domainVariants.inheritBase(variant)];
        std::vector<DomainHandle> parts;
        parts.push_back(base);
        for (size_t source : op.domainVariants.extraSources(variant)) {
          parts.push_back(materializeDomainSource(
              source, segnum, base.get(), result.reservation));
        }
        result.domains[variant] = DomainVariantPlan::compose(
            std::move(parts));
        result.domains[variant] =
            std::move(result.domains[variant]).retainedWith(
                result.reservation);
      }
      result.inheritReady = true;
    }

    RoutedProduction prepareRoutedProduction(
        MemPool& pool, int32_t segnum, DomainHandle baseDomain) {
      auto& op = thisOp();
      assert(!op.domainVariants.empty());
      assert(baseDomain.isDeliverable());
      auto& seg = op.planning.topReader.segments()[(size_t) segnum];
      std::string detail = "top_docs '" + std::string(op.name)
          + "' segment " + std::to_string(segnum);
      RoutedProduction result;
      std::shared_ptr<DomainVariantPlan::Reservation> reservation;
      if (preparedRoutedReservations.empty()) {
        reservation = op.domainVariants.reserveProduction(
            op.req.memoryTracker, seg.maxDoc(), detail);
        op.planning.filterUses->enableRoutedAccounting(
            (size_t) segnum, op.req.memoryTracker, detail);
        result.filters = materializeRoutedFilters(
            segnum, baseDomain.get(), reservation);
      } else {
        assert((size_t) segnum < preparedRoutedReservations.size());
        reservation = std::move(
            preparedRoutedReservations[(size_t) segnum]);
        if (!preparedRoutedFilters.empty()) {
          result.filters = std::move(
              preparedRoutedFilters[(size_t) segnum]);
        }
        assert(reservation != nullptr);
        if (result.filters.empty()) {
          result.filters = materializeRoutedFilters(
              segnum, baseDomain.get(), reservation);
        }
      }
      result.defaultEligibility = op.domainVariants.defaultDomain(
          baseDomain, result.filters);
      assert(result.defaultEligibility.isDeliverable());
      result.reservation = reservation;
      result.domains.resize(op.domainVariants.variantCount());
      if (op.domainVariants.needsResetParentFilters()) {
        if (preparedResetRoutedFilters.empty()) {
          result.resetFilters = materializeResetParentFilters(
              segnum, reservation);
        } else {
          result.resetFilters = std::move(
              preparedResetRoutedFilters[(size_t) segnum]);
        }
      }
      finishResetDomains(result, segnum);

      if (!op.domainVariants.needsInheritProduction()) {
        return result;
      }

      if (!preparedVariantInheritBases.empty()) {
        result.inheritBases = std::move(
            preparedVariantInheritBases[(size_t) segnum]);
        finishInheritDomains(result, segnum);
        return result;
      }

      Query::SegmentSource& source = variantMembershipSource();
      bool implicitMatches = op.weight->matchesAllDocs();
      auto* supplier = implicitMatches
          ? nullptr : source.scorerSupplier(pool, seg);
      DocSet* reusableRaw = preparedWeight == nullptr && supplier != nullptr
          ? supplier->exactDocSet() : nullptr;
      int64_t rawCost = implicitMatches
          ? seg.maxDoc() : supplier == nullptr ? 0 : supplier->cost();
      auto selection = op.domainVariants.selectOffer(
          rawCost, seg.maxDoc(), baseDomain, result.filters,
          implicitMatches, reusableRaw != nullptr,
          op.topCount == 0
              || (op.weight->isConstantScoring()
                  && !op.sortPlan.useFieldSort));
      DomainVariantPlan::CostedOffer selected;
      bool admitted = false;
      for (size_t i = 0; i < selection.count; i++) {
        if (reservation->tryGrow(selection.offers[i].peakBytes)) {
          selected = selection.offers[i];
          admitted = true;
          break;
        }
      }
      if (!admitted) {
        auto leastMemory = std::min_element(
            selection.offers.begin(),
            selection.offers.begin() + (std::ptrdiff_t) selection.count,
            [](const auto& left, const auto& right) {
              return left.peakBytes < right.peakBytes;
            });
        reservation->grow(leastMemory->peakBytes);
        selected = *leastMemory;
      }

      switch (selected.offer) {
        case DomainVariantPlan::Offer::IDENTITY:
          result.inheritBases = op.domainVariants.produceSharedBases(
              baseDomain, result.filters);
          break;
        case DomainVariantPlan::Offer::SHARED_M: {
          if (implicitMatches) {
            result.inheritBases = op.domainVariants.produceSharedBases(
                baseDomain, result.filters);
          } else if (reusableRaw != nullptr) {
            DomainHandle rawMatches = op.domainVariants.constrainRaw(
                DomainHandle::pinned(
                    reusableRaw, op.planning.filterUses),
                baseDomain);
            result.inheritBases = op.domainVariants.produceSharedBases(
                std::move(rawMatches), result.filters);
          } else {
            // Capture M in the existing ranking walk. Bulk paths fall back to
            // scorer-driven collection because their in-window filtering does
            // not expose pre-filter match words yet.
            result.captureSharedRaw = true;
          }
          break;
        }
        case DomainVariantPlan::Offer::INDEPENDENT:
          result.inheritBases = op.domainVariants.produceIndependentBases(
              baseDomain, result.filters, [&](DocSet* eligibility) {
                return DomainHandle(QueryPrep::materialize(
                    source, seg, eligibility,
                    Query::SupplierExecutionMode::ORDINARY,
                    QueryPrep::MaterializeMode::DOMAIN_DRIVEN));
              });
          break;
      }
      if (!result.captureSharedRaw) {
        finishInheritDomains(result, segnum);
      }
      return result;
    }

    void doCollect(
        oneapi::tbb::task_group* tg, int32_t segnum,
        DomainHandle domainHandle) {
      auto& op = thisOp();
      assert(domainHandle.isDeliverable());
      DocSet* domain = domainHandle.get();

      // A constant-scoring match-all is the identity on its domain: every doc
      // in the domain matches, all with the same score.  So the domain IS the
      // result set - its cardinality is the exact hit count, doc order is the
      // ranking, and sub-ops inherit it unchanged.  None of that needs a
      // scorer, so the whole collection ladder below is skipped.  (Scores must
      // be constant for doc order to be the ranking; a rescore over a
      // match-all matches everything but reorders it.)
      bool matchEverything = op.weight != nullptr && op.weight->matchesAllDocs()
        && op.weight->isConstantScoring() && op.filterWeights.empty();
      std::unique_ptr<MergeableCollector> data;
      std::optional<RoutedProduction> routedProduction;
      DocSet* routedBaseDomain = nullptr;
      int64_t numSegs = (int64_t)op.req.reader->segments().size();

      {
        auto poolGuard = MemPool::threadLocalPoolGuard();
        auto& seg = op.planning.topReader.segments()[segnum];
        if (!op.domainVariants.empty()) {
          routedBaseDomain = domain;
          routedProduction.emplace(prepareRoutedProduction(
              poolGuard.pool(), segnum, domainHandle));
          domainHandle = routedProduction->defaultEligibility;
          domain = domainHandle.get();
        }

        // Wait until last moment to obtain collector in hopes of reusing an existing one.
        // Keep ownership until release so a scoring error cannot orphan it.
        data.reset(collectorMerger.obtain());

        // Field-sorted ranking is the one thing a match-all still has to
        // iterate for: it orders by column values the domain says nothing
        // about.  A count-only field sort (limit 0) reads nothing back, so it
        // takes the domain answer like the score-ranked case.
        bool rankFromDocOrder = !data->useFieldSort;
        Query::ScorerSupplier* supplier = nullptr;
        DocSet* borrowedDomain = nullptr;
        bool exactDomain = false;
        DocSet* exactDomainDocs = nullptr;
        if (!requiresWholeIndexPrepare && op.requirements.needExactDomain
            && !op.exactDomainPlan.empty()) {
          auto result = op.exactDomainPlan.produce(
              poolGuard.pool(), *op.req.reader, seg, domain);
          exactDomain = result.available;
          if (exactDomain) {
            if (result.docs.get() == domain) {
              output[(size_t)segnum] = domainHandle;
            } else {
              output[(size_t)segnum] = std::move(result.docs)
                  .pinnedWith(op.planning.filterUses);
            }
            exactDomainDocs = output[(size_t)segnum].get();
            skipCount(SkipStats::exactDomainDocSetCollections);
          } else {
            skipCount(SkipStats::exactDomainStreamFallbacks);
          }
        }
        QueryPrep::WholeMembershipResult wholeMembershipResult;
        if (!exactDomain && !matchEverything
            && !op.wholeMembershipPlan.empty()) {
          wholeMembershipResult = op.wholeMembershipPlan.resolve(
              *op.req.reader, seg, domain, preparedWeight.get());
        }
        bool wholeMembershipAvailable = wholeMembershipResult.available;
        bool wholeTopKCountAvailable = wholeMembershipAvailable
            && op.wholeMembershipPlan.isTopKCount();
        bool wholeFieldSortAvailable = wholeMembershipAvailable
            && op.wholeMembershipPlan.isFieldSort();
        bool wholeFallbackSupplierPending =
            !wholeMembershipAvailable && op.wholeMembershipPlan.hasCacheUse();
        auto obtainMainSupplier = [&]() {
          if (wholeFallbackSupplierPending) {
            op.wholeMembershipPlan.recordFallbackSupplier();
            wholeFallbackSupplierPending = false;
          }
          if (supplier == nullptr) {
            supplier = mainScorerSupplier(poolGuard.pool(), seg);
          }
          return supplier;
        };
        // A root, non-prepared filter-only Boolean can expose its one cached
        // required clause as the exact result. Outer domains remain explicit
        // intersections and must use the collection ladder below.
        if (!exactDomain && !wholeMembershipAvailable
            && op.weight != nullptr && !requiresWholeIndexPrepare
            && domain == nullptr && op.filterWeights.empty()
            && op.weight->isConstantScoring()
            && (rankFromDocOrder || data->topCount() == 0)) {
          supplier = obtainMainSupplier();
          borrowedDomain =
              supplier == nullptr ? nullptr : supplier->exactDocSet();
          if (borrowedDomain != nullptr) {
            skipCount(SkipStats::filterDocSetIdentityCollections);
            if (!output.empty()) {
              output[(size_t)segnum] =
                  DomainHandle::pinned(
                      borrowedDomain, op.planning.filterUses);
            }
          }
        }
        bool routedDomainsReady = routedProduction.has_value()
            && routedProduction->inheritReady;
        bool routedDefaultIdentity = routedDomainsReady
            && (op.weight->isConstantScoring() || data->topCount() == 0);
        bool identityResult =
            ((wholeMembershipAvailable && !wholeTopKCountAvailable
                                       && !wholeFieldSortAvailable)
             || exactDomain || matchEverything
             || borrowedDomain != nullptr || routedDefaultIdentity)
            && (rankFromDocOrder || data->topCount() == 0);

        std::optional<DocSetBuilder> builder;
        bool captureSharedRaw = routedProduction.has_value()
            && routedProduction->captureSharedRaw;
        if (captureSharedRaw
            || (output.size() > 0 && !identityResult
                && !wholeTopKCountAvailable)) {
          builder.emplace(seg.maxDoc());
        }

        if (wholeTopKCountAvailable) {
          assert(rankFromDocOrder);
          assert(data->scoreCollector->topCount > 0);
          if (op.wholeConstantRanking) {
            int64_t ranked = 0;
            if (wholeMembershipResult.count > 0) {
              Query::Scorer* scorer = nullptr;
              DocSet* collectorFilter = domain;
              if (wholeMembershipResult.docs.get() != nullptr) {
                scorer = QueryPrep::createDocSetScorer(
                    poolGuard.pool(), wholeMembershipResult.docs.get(), seg,
                    op.scoreProfile.value);
                collectorFilter = nullptr;
              } else {
                auto* rankingSupplier = obtainMainSupplier();
                if (rankingSupplier != nullptr) {
                  scorer = buildBoundedConstantScorer(
                      poolGuard.pool(), *rankingSupplier, collectorFilter,
                      data->scoreCollector->topCount);
                }
              }
              if (scorer != nullptr) {
                ranked = collectFirstKConstant(
                    segnum, scorer, collectorFilter,
                    *data->scoreCollector, data->scoreCollector->topCount);
              }
            }
            assert(wholeMembershipResult.count >= ranked);
            data->addHits(wholeMembershipResult.count - ranked);
          } else {
            assert(op.wholeRankingWeight != nullptr);
            Query::SegmentSource& rankingSource =
                preparedWeight != nullptr
                    && op.wholeRankingWeight == op.weight
                ? static_cast<Query::SegmentSource&>(*preparedWeight)
                : static_cast<Query::SegmentSource&>(
                      *op.wholeRankingWeight);
            auto* rankingSupplier = rankingSource.scorerSupplier(
                poolGuard.pool(), seg);
            collectKnownCountTopK(
                poolGuard.pool(), segnum, wholeMembershipResult.count,
                rankingSupplier, domain, *data->scoreCollector,
                seg.maxDoc(), op.wholeRankingWeight->allowsPruning());
          }
        } else if (identityResult) {
          DocSet* identityDomain = wholeMembershipAvailable
              ? wholeMembershipResult.docs.get()
              : exactDomain
                    ? exactDomainDocs
                    : borrowedDomain != nullptr
                          ? borrowedDomain
                          : routedDomainsReady
                                ? routedProduction->domains[0].get()
                                : domain;
          int64_t total = wholeMembershipAvailable
              ? wholeMembershipResult.count
              : identityDomain == nullptr
                    ? seg.maxDoc() : identityDomain->card();
          int64_t ranked = 0;
          if (rankFromDocOrder && data->topCount() > 0) {
            // Equal scores reduce ranking to doc order, so the domain's first
            // K docs are its top K.
            if (supplier == nullptr && op.weight != nullptr) {
              supplier = obtainMainSupplier();
            }
            Query::Scorer* scorer = nullptr;
            DocSet* collectorFilter = identityDomain;
            if (supplier != nullptr) {
              scorer = buildBoundedConstantScorer(
                  poolGuard.pool(), *supplier, identityDomain,
                  data->topCount());
            } else if (identityDomain != nullptr) {
              scorer = QueryPrep::createDocSetScorer(
                  poolGuard.pool(), identityDomain, seg,
                  op.scoreProfile.value);
              collectorFilter = nullptr;
            }
            if (scorer != nullptr) {
              ranked = collectFirstKConstant(
                  segnum, scorer, collectorFilter,
                  *data->scoreCollector, data->topCount());
            }
          }
          assert(total >= ranked);
          data->addHits(total - ranked);
        } else if (wholeFieldSortAvailable
                   || (supplier = obtainMainSupplier()) != nullptr) {
          DocSet* filter = wholeFieldSortAvailable
                  && wholeMembershipResult.docs.get() != nullptr
              ? wholeMembershipResult.docs.get() : domain;
          std::unique_ptr<DocSet> newDomain;
          // Keeps owned sets or request-pinned cache borrows alive; `filter`
          // may alias one directly, so the handles outlive collection below.
          std::vector<DomainHandle> filters;
          if (!requiresWholeIndexPrepare && op.domainVariants.empty()
              && !thisOp().filterWeights.empty()) {
            std::vector<DocSet*> filterPtrs;
            for (size_t i = 0; i < thisOp().filterWeights.size(); i++) {
              filters.push_back(QueryPrep::materializeEffectiveFilter(
                  *thisOp().filterWeights[i], thisOp().filterUses[i],
                  *op.req.reader, seg, nullptr));
              filterPtrs.push_back(filters.back().get());
            }
            if (domain) {
              filterPtrs.emplace_back(domain);
            }
            if (filterPtrs.size() == 1) {
              filter = filterPtrs[0];
            } else {
              newDomain = DocSet::intersect(filterPtrs);
              filter = newDomain.get();
            }
          }
          DocSetBuilder* builderPtr = builder.has_value() ? &*builder : nullptr;
          bool sourcePreparedAgainstFilter =
              requiresWholeIndexPrepare && preparedWeight != nullptr
              && filter == domain;
          DocSet* collectorFilter =
            sourcePreparedAgainstFilter && preparedWeight->outputIsSubsetOfDomain()
              ? nullptr
              : filter;
          // A constant-count plan satisfies this segment without constructing
          // an executor.
          bool counted = false;
          if (!requiresWholeIndexPrepare && builderPtr == nullptr && filter == nullptr
              && !data->useFieldSort && data->scoreCollector->topCount == 0) {
            Query::ScorerSupplier::BulkScorerContext countContext;
            countContext.requireConstantCount = true;
            auto countPlan = supplier->planBulk(
                Query::ScorerSupplier::BulkUse::COUNT_WINDOWS,
                countContext);
            if (countPlan.hasConstantCount()) {
              data->scoreCollector->hitCount += countPlan.constantCount;
              counted = true;
            }
          }
          if (counted) {
            // fall through to the sub-calc/merge tail below
          } else if (routedDomainsReady && !data->useFieldSort
                     && op.requirements.needExactCount
                     && data->scoreCollector->topCount > 0) {
            int64_t count = routedProduction->domains[0].get() == nullptr
                ? seg.maxDoc()
                : routedProduction->domains[0].get()->card();
            collectKnownCountTopK(
                poolGuard.pool(), segnum, count, supplier, collectorFilter,
                *data->scoreCollector, seg.maxDoc(),
                op.weight->allowsPruning());
          } else if (data->useFieldSort) {
            // Ranking permission and completeness production are separate
            // facts. A known whole-membership card satisfies exact count, but
            // its borrowed set is not a produced sub-op domain.
            bool routedExactDomain = routedDomainsReady;
            bool mustStreamForCompleteness =
                (op.requirements.needExactCount && !wholeFieldSortAvailable
                 && !routedExactDomain)
                || builderPtr != nullptr;
            bool maySkipNoncompetitiveDocs =
                !disableFieldSortPruning && !mustStreamForCompleteness;
            // Candidate pruning must beat what the source would still cost:
            // the query estimate, capped by an external filter's cardinality.
            int64_t sortSourceCost = wholeFieldSortAvailable
                ? wholeMembershipResult.count
                : routedExactDomain
                      ? routedProduction->domains[0].get() == nullptr
                            ? seg.maxDoc()
                            : routedProduction->domains[0].get()->card()
                      : supplier->cost();
            if (collectorFilter != nullptr) {
              sortSourceCost = std::min(sortSourceCost,
                                        (int64_t)collectorFilter->card());
            }
            // One segment bind for every execution arm below: repeated
            // setSegment calls are correct (slot state survives) but re-seek
            // comparator readers, a real fixed cost on string sorts.
            data->fieldCollector->setSegment(
                segnum, &seg.postingsReader(), &poolGuard.pool(),
                sortSourceCost);
            bool usedBulk = false;
            int64_t fieldHitsBefore = data->fieldCollector->hitCount;
            // A MATCH_WINDOWS plan taken by a declined arm is retained here
            // so the ordinary bulk arm never re-plans the same context.
            std::optional<Query::ScorerSupplier::BulkPlan> matchWindowsPlan;
            Query::ScorerSupplier::BulkScorerContext matchWindowsContext;
            // Best-first exact-domain route: when the whole result set is
            // already materialized (BITSET or ARRAY), no scorer needs to run
            // - the driver visits key blocks in bound order and terminates
            // on proof. Two domain sources qualify: the supplier's exact
            // cached set (a folded filter-only Boolean over match-all, no
            // other filter or sub-op domain restriction), or an explicit
            // collectorFilter under a true match-all weight. Activation also
            // requires the expected visit floor (ceil(k/d) blocks) to be
            // sub-saturating; at ceil(k/d) >= blockCount the bound floor
            // covers every block and bound order cannot beat doc order.
            if (maySkipNoncompetitiveDocs
                && (!requiresWholeIndexPrepare || wholeFieldSortAvailable)
                && !data->fieldCollector->needsScores
                && !data->fieldCollector->hasExpr
                && data->fieldCollector->topCount > 0) {
              DocSet* domainSet = nullptr;
              bool allDocs = false;
              if (wholeFieldSortAvailable
                  && wholeMembershipResult.docs.get() != nullptr) {
                domainSet = wholeMembershipResult.docs.get();
              } else if (collectorFilter == nullptr && supplier != nullptr) {
                domainSet = supplier->exactDocSet();
                // Match-all with no deletes and no filters: the domain is
                // every doc (deletes would have arrived as a liveDocs
                // domain in collectorFilter per the root domain contract).
                allDocs = domainSet == nullptr && op.weight != nullptr
                    && op.weight->matchesAllDocs();
              } else if (op.weight != nullptr
                         && op.weight->matchesAllDocs()) {
                domainSet = collectorFilter;
              }
              if (allDocs
                  || (domainSet != nullptr
                      && (domainSet->type == DocSet::BITSET
                          || domainSet->type == DocSet::ARRAY))) {
                int64_t card =
                    allDocs ? (int64_t)seg.maxDoc() : domainSet->card();
                auto bestFirst = planFieldSortBestFirst(
                    *data->fieldCollector, card, seg.maxDoc());
                if (bestFirst.available()) {
                  auto& plan = bestFirst.keys;
                  // Cap generously above the expected floor so uniform
                  // data reaches proof termination; the forward-sweep
                  // fallback keeps adversarial tie plateaus linear.
                  int64_t workCap = forceFieldSortWorkCapForTests > 0
                      ? forceFieldSortWorkCapForTests
                      : std::min(
                          plan.leafCount, 4 * bestFirst.expectedFloor + 64);
                  if (!allDocs && domainSet->type == DocSet::ARRAY) {
                    collectTopKArrayBestFirst(
                        segnum, ((ArrDocSet*)domainSet)->docs(),
                        *data->fieldCollector, poolGuard.pool(), workCap);
                  } else {
                    std::span<const uint64_t> words;  // empty = every doc
                    if (!allDocs) {
                      const FixedBitSet& bits =
                          ((BitDocSet*)domainSet)->bits();
                      words = std::span<const uint64_t>(
                          bits.words,
                          FixedBitSet::sizeInWords(bits.size()));
                    }
                    collectTopKBitSetBestFirst(
                        segnum, words,
                        *data->fieldCollector, poolGuard.pool(), workCap);
                  }
                  data->fieldCollector->recordSegmentSkipStats(segnum);
                  if (wholeFieldSortAvailable) {
                    skipCount(
                        SkipStats::wholeFieldSortBestFirstActivations);
                  }
                  usedBulk = true;
                }
              }
            }
            if (!usedBulk && wholeFieldSortAvailable) {
              skipCount(SkipStats::wholeFieldSortLadderFallbacks);
            }
            if (!usedBulk && wholeFieldSortAvailable
                && wholeMembershipResult.docs.get() != nullptr) {
              assert(wholeMembershipResult.docs.get() != nullptr);
              supplier = poolGuard.pool().make<QueryPrep::DocSetSupplier>(
                  wholeMembershipResult.docs.get(), seg);
              // The accepted whole value is already composed with the root
              // domain. It is the execution source, not an outer filter over
              // a query scorer that no longer exists.
              collectorFilter = nullptr;
            }
            if (!usedBulk && supplier == nullptr) {
              supplier = obtainMainSupplier();
            }
            // Seeded two-pass query-driven route: no materialized domain
            // exists (else best-first took it), but the sole dense numeric
            // primary still publishes block bounds, so the driver fills the
            // heap from the best-bounded blocks via one bulk scorer and
            // sweeps the complement with a second, independent one. Needs a
            // supplier whose bulk plans declare independentReplan (both
            // products are built before pass 1), a sub-saturating expected
            // floor with material headroom (4*floor+64 < blockCount), and
            // at least one expected match per key block. forceFieldSortSeeding
            // bypasses only the two economic gates, never correctness or
            // capability.
            if (!usedBulk && supplier != nullptr && !captureSharedRaw
                && maySkipNoncompetitiveDocs && !disableFieldSortSeeding
                && !disableFieldSortBulk && !requiresWholeIndexPrepare
                && !data->fieldCollector->needsScores
                && !data->fieldCollector->hasExpr
                && data->fieldCollector->topCount > 0) {
              auto plan = data->fieldCollector->maskedKeyBlockPlan();
              if (plan.batch != nullptr && plan.blockCount > 0
                  && sortSourceCost > 0) {
                int64_t expectedDepth =
                    (data->fieldCollector->topCount * (int64_t)seg.maxDoc()
                     + sortSourceCost - 1) / sortSourceCost;
                // Budget and materiality are both in leaf units (pass 1
                // enumerates leaves). Leaf-normalized materiality measured
                // BETTER than preserving the coarse-era activation envelope:
                // it admits the 1% band, where seeding takes the walk from
                // 1.66x the leaf floor to floor+O(1) and the second scorer
                // costs far less than the excess it removes.
                int64_t leafFloor =
                    std::min<int64_t>(plan.leafCount, expectedDepth);
                bool material =
                    4 * leafFloor + 64 < plan.leafCount
                    && sortSourceCost >= plan.leafCount;
                if (material || forceFieldSortSeeding) {
                  matchWindowsPlan = supplier->planBulk(
                      Query::ScorerSupplier::BulkUse::MATCH_WINDOWS,
                      matchWindowsContext);
                  auto& bulkPlan = *matchWindowsPlan;
                  if (bulkPlan.available
                          == Query::ScorerSupplier::BulkAnswer::YES
                      && bulkPlan.supportsMatchWindows
                          == Query::ScorerSupplier::BulkAnswer::YES
                      && bulkPlan.independentReplan
                          == Query::ScorerSupplier::BulkAnswer::YES
                      && !bulkPlan.hasConstantCount()) {
                    auto sweepPlan = supplier->planBulk(
                        Query::ScorerSupplier::BulkUse::MATCH_WINDOWS,
                        matchWindowsContext);
                    auto* seedBulk =
                        supplier->buildBulk(poolGuard.pool(), bulkPlan);
                    matchWindowsPlan.reset();  // consumed by the build
                    auto* sweepBulk = seedBulk == nullptr ? nullptr
                        : supplier->buildBulk(poolGuard.pool(), sweepPlan);
                    if (seedBulk != nullptr && sweepBulk != nullptr) {
                      int64_t seedBudget = leafFloor;
                      if (fieldSortSeedBudgetPerMilleForTests > 0) {
                        seedBudget = std::max<int64_t>(
                            1, leafFloor
                                * fieldSortSeedBudgetPerMilleForTests / 1000);
                      }
                      int64_t lambda = std::max<int64_t>(
                          1, sortSourceCost / plan.leafCount);
                      int64_t fillAbortBudget = std::max<int64_t>(
                          8, 4 * ((data->fieldCollector->topCount + lambda - 1)
                                  / lambda));
                      collectTopKMatchWindowedSeeded(
                          segnum, seedBulk, sweepBulk, collectorFilter,
                          *data->fieldCollector, poolGuard.pool(),
                          seg.maxDoc(), seedBudget, fillAbortBudget);
                      data->fieldCollector->recordSegmentSkipStats(segnum);
                      usedBulk = true;
                    } else if (seedBulk != nullptr) {
                      // The pass-2 product failed to build; the untouched
                      // pass-1 product runs today's single-pass walk.
                      skipCount(SkipStats::fieldSortBulkCollections);
                      collectTopKMatchWindowed(
                          segnum, seedBulk, collectorFilter, builderPtr,
                          *data->fieldCollector, seg.maxDoc(),
                          maySkipNoncompetitiveDocs);
                      data->fieldCollector->recordSegmentSkipStats(segnum);
                      usedBulk = true;
                    }
                  }
                }
              }
            }
            if (!usedBulk && supplier != nullptr && !captureSharedRaw
                && !disableFieldSortBulk
                && !data->fieldCollector->needsScores) {
              auto plan = matchWindowsPlan.has_value()
                  ? *matchWindowsPlan
                  : supplier->planBulk(
                        Query::ScorerSupplier::BulkUse::MATCH_WINDOWS,
                        matchWindowsContext);
              bool plannedNo =
                  plan.available != Query::ScorerSupplier::BulkAnswer::YES
                  || plan.supportsMatchWindows
                      != Query::ScorerSupplier::BulkAnswer::YES;
              auto* bulk = plannedNo
                  ? nullptr : supplier->buildBulk(poolGuard.pool(), plan);
              if (plannedNo) {
                supplier->recordBulkPlanCommitment(
                    Query::ScorerSupplier::BulkUse::MATCH_WINDOWS,
                    matchWindowsContext, plan);
              }
              if (bulk != nullptr) {
                assert(bulk->supportsMatchWindows());
                std::optional<FieldSortCollector::ExpressionBindings> expressionBindings;
                if (data->fieldCollector->hasExpr && data->fieldCollector->topCount > 0) {
                  expressionBindings.emplace(
                      *data->fieldCollector, poolGuard.pool(), seg);
                }
                skipCount(SkipStats::fieldSortBulkCollections);
                collectTopKMatchWindowed(
                    segnum, bulk, collectorFilter, builderPtr,
                    *data->fieldCollector, seg.maxDoc(),
                    maySkipNoncompetitiveDocs);
                data->fieldCollector->recordSegmentSkipStats(segnum);
                usedBulk = true;
              }
            }
            if (!usedBulk && supplier != nullptr) {
              Query::ScorerPlan* pullPlan = resolvePullScorer(
                  poolGuard.pool(), *supplier);
              Query::ReportedTwoPhase reportedTwoPhase =
                  pullPlan->shape().reportedTwoPhase;
              auto* scorer = pullPlan->build(poolGuard.pool());
              if (scorer != nullptr) {
                std::optional<FieldSortCollector::ExpressionBindings> expressionBindings;
                if (data->fieldCollector->hasExpr && data->fieldCollector->topCount > 0) {
                  expressionBindings.emplace(
                      *data->fieldCollector, poolGuard.pool(), seg);
                }
                collectTopK(segnum, scorer, collectorFilter, builderPtr,
                            *data->fieldCollector,
                            maySkipNoncompetitiveDocs, nullptr,
                            captureSharedRaw
                                ? DomainBuildMode::RAW_QUERY_MATCHES
                                : DomainBuildMode::FILTERED_MATCHES,
                            routedBaseDomain, reportedTwoPhase);
                data->fieldCollector->recordSegmentSkipStats(segnum);
              }
            }
            if (op.requirements.needExactCount
                && (wholeFieldSortAvailable || routedExactDomain)) {
              int64_t collected =
                  data->fieldCollector->hitCount - fieldHitsBefore;
              int64_t exactCount = wholeFieldSortAvailable
                  ? wholeMembershipResult.count
                  : routedProduction->domains[0].get() == nullptr
                        ? seg.maxDoc()
                        : routedProduction->domains[0].get()->card();
              assert(exactCount >= collected);
              data->addHits(exactCount - collected);
            }
          } else {
            assert(op.weight != nullptr);
            // Pruning is enabled only when an exact count can either be omitted
            // or supplied by an exhaustive count/domain pass or an already-known
            // domain. Without pruning, windowed scoring still beats the
            // doc-at-a-time heap disjunction by using per-clause window drives.
            bool allowPruning = op.weight->allowsPruning();
            BulkScorer* bulk = nullptr;
            bool composedExactCountTopK = false;
            ConstantScoreDisposition constantRoute =
                constantScoreDisposition(
                    *data->scoreCollector, builderPtr);
            ExactCountTopKRoute exactCountTopKRoute =
                constantRoute
                        == ConstantScoreDisposition::NOT_CONSTANT_TOP_K
                    ? ExactCountTopKRoute::EXACT_CANDIDATE_SCORING
                    : ExactCountTopKRoute::CONSTANT_FIRST_K;
            if (builderPtr == nullptr
                && data->scoreCollector->topCount > 0
                && op.countWeight != nullptr && op.rankingWeight != nullptr) {
              auto* countSupplier = op.countWeight->scorerSupplier(
                  poolGuard.pool(), seg);
              if (countSupplier != nullptr
                  && admitExactCountTopK(
                      *countSupplier, collectorFilter, seg.maxDoc())) {
                switch (exactCountTopKRoute) {
                  case ExactCountTopKRoute::EXACT_CANDIDATE_SCORING:
                    composedExactCountTopK =
                        composeVariableScoreExactCountTopK(
                            poolGuard.pool(), segnum, *supplier,
                            *countSupplier, collectorFilter,
                            *data->scoreCollector, seg.maxDoc());
                    break;
                  case ExactCountTopKRoute::CONSTANT_FIRST_K:
                    composedExactCountTopK = countThenCollectConstantTopK(
                        poolGuard.pool(), segnum, *countSupplier,
                        collectorFilter, *data->scoreCollector,
                        seg.maxDoc());
                    break;
                }
                skipCount(composedExactCountTopK
                    ? SkipStats::exactCountTopKCompositions
                    : SkipStats::exactCountTopKBulkFallbacks);
              } else if (countSupplier != nullptr) {
                skipCount(SkipStats::exactCountTopKProfitabilityRejects);
              } else {
                skipCount(SkipStats::exactCountTopKBulkFallbacks);
              }
            }
            bool useSparseConstantPull =
                builderPtr != nullptr && !captureSharedRaw
                && data->scoreCollector->topCount > 0
                && op.weight->isConstantScoring()
                && op.weight->prefersPullForSparseArrayDomain()
                && supplier->cost() <= DocSetBuilder::arrayLimitFor(seg.maxDoc());
            auto buildDeclaredBulk = [&]() -> BulkScorer* {
              if (captureSharedRaw) return nullptr;
              Query::ScorerSupplier::BulkUse use =
                  constantRoute
                          == ConstantScoreDisposition::LIMIT_ONLY
                      ? Query::ScorerSupplier::BulkUse::SCORED_WINDOWS
                      : data->scoreCollector->topCount == 0
                              || op.weight->isConstantScoring()
                              || builderPtr != nullptr
                          ? Query::ScorerSupplier::BulkUse::COUNT_WINDOWS
                          : Query::ScorerSupplier::BulkUse::SCORED_WINDOWS;
              Query::ScorerSupplier::BulkScorerContext bulkContext;
              auto plan = supplier->planBulk(use, bulkContext);
              if (plan.available != Query::ScorerSupplier::BulkAnswer::YES) {
                supplier->recordBulkPlanCommitment(
                    use, bulkContext, plan);
                return nullptr;
              }
              return supplier->buildBulk(poolGuard.pool(), plan);
            };
            if (composedExactCountTopK) {
              // countThenCollectTopK supplied both the exact hit count and
              // competitively pruned ranking.
            } else if (useSparseConstantPull) {
              auto* scorer = buildPullScorer(poolGuard.pool(), *supplier);
              if (scorer != nullptr) {
                collectConstantTopKAndDomain(
                    segnum, scorer, collectorFilter, *builder,
                    *data->scoreCollector);
              }
            } else if ((bulk = buildDeclaredBulk()) != nullptr) {
              if (data->scoreCollector->topCount == 0) {
                // limit 0: the collector keeps nothing but the total, so count
                // windows without materializing docs or scores.
                collectCountWindowed(bulk, collectorFilter, builderPtr, *data->scoreCollector,
                                     seg.maxDoc());
              } else if (op.weight->isConstantScoring()
                         && !disableConstantWindowCaptureForTests) {
                // Equal scores reduce ranking to doc order: the segment's top
                // K docs are the first K matches, captured straight off this
                // bulk's emitted windows before consumption degrades to
                // count-only. An independent capture scorer would rebuild
                // every clause (a multiterm clause re-runs its dictionary
                // scan per build).
                collectFirstKConstantWindowed(
                    segnum, bulk, collectorFilter, builderPtr,
                    *data->scoreCollector, seg.maxDoc(),
                    constantRoute
                            == ConstantScoreDisposition::LIMIT_ONLY
                        ? ConstantScoreDrain::LIMIT_ONLY
                        : ConstantScoreDrain::COMPLETE);
              } else if (op.weight->isConstantScoring()) {
                // In the test-forced arrangement, the bulk scorer drives the
                // exhaustive count/domain and an independent scorer visits only
                // this segment's first K matches.
                auto* captureSupplier = mainScorerSupplier(poolGuard.pool(), seg);
                int64_t captured = 0;
                if (captureSupplier != nullptr) {
                  auto* captureScorer = buildPullScorer(
                      poolGuard.pool(), *captureSupplier);
                  if (captureScorer != nullptr) {
                    captured = collectFirstKConstant(
                        segnum, captureScorer, collectorFilter,
                        *data->scoreCollector, data->scoreCollector->topCount);
                  }
                }
                int64_t count = countMatchesWindowed(
                    bulk, collectorFilter, builderPtr, seg.maxDoc());
                assert(count >= captured);
                data->scoreCollector->hitCount += count - captured;
              } else if (builderPtr != nullptr) {
                auto* rankingSupplier = mainScorerSupplier(poolGuard.pool(), seg);
                countThenCollectTopK(
                    poolGuard.pool(), segnum, bulk, rankingSupplier,
                    collectorFilter, builderPtr, *data->scoreCollector,
                    seg.maxDoc(), allowPruning);
              } else {
                collectTopKWindowed(
                    segnum, bulk, collectorFilter, *data->scoreCollector,
                    allowPruning ? &scoreAccumulator : nullptr,
                    seg.maxDoc(), allowPruning);
              }
            } else {
              auto* scorer = constantRoute
                      == ConstantScoreDisposition::LIMIT_ONLY
                  ? buildBoundedConstantScorer(
                        poolGuard.pool(), *supplier, collectorFilter,
                        data->scoreCollector->topCount)
                  : buildPullScorer(poolGuard.pool(), *supplier);
              if (scorer != nullptr) {
                if (constantRoute
                    == ConstantScoreDisposition::LIMIT_ONLY) {
                  collectFirstKConstant(
                      segnum, scorer, collectorFilter,
                      *data->scoreCollector,
                      data->scoreCollector->topCount);
                } else {
                  collectTopK(
                      segnum, scorer, collectorFilter, builderPtr,
                      *data->scoreCollector, allowPruning,
                      &scoreAccumulator,
                      captureSharedRaw
                          ? DomainBuildMode::RAW_QUERY_MATCHES
                          : DomainBuildMode::FILTERED_MATCHES,
                      routedBaseDomain);
                }
              }
            }
          }
        }
        if (builder.has_value()) {
          DomainHandle built(std::move(builder->build()));
          if (captureSharedRaw) {
            assert(routedProduction.has_value());
            routedProduction->inheritBases =
                op.domainVariants.produceSharedBases(
                std::move(built), routedProduction->filters);
            finishInheritDomains(*routedProduction, segnum);
            routedProduction->captureSharedRaw = false;
          } else {
            output[(size_t)segnum] = std::move(built);
          }
        }
      }
      // For maximum parallelism, we want to launch sub-tasks that depend on matching documents
      // as soon as we have that set.  Releasing the collector below could end up
      // doing a substantial amount of work.

      // since tasks are executed on a stack, push sub-calculators in reverse order.
      // directly calling a sub-calculator would be beneficial since the domain
      // we just calculated will still be in cache.
      // The downside is that it could delay loading stored fields.
      // We could launch fillQueryTopNResponse as a sub-task and then
      // launch the sub-calculators in parallel after that.
      for (int i = subCalcs.size() - 1; i >= 0; i--) {
        // TODO: launch sub-calculators in parallel (except for the first one).
        DomainHandle newDomain;
        if (routedProduction.has_value()) {
          assert(subCalcs.size() == subCalcVariants.size());
          size_t variant = subCalcVariants[(size_t) i];
          assert(variant < routedProduction->domains.size());
          if (variant == 0 && !routedProduction->inheritReady) {
            newDomain = matchEverything
                ? domainHandle : output[(size_t)segnum];
          } else {
            if (op.domainVariants.frame(variant)
                == DomainVariantPlan::Frame::INHERIT) {
              assert(routedProduction->inheritReady);
            }
            newDomain = routedProduction->domains[variant];
          }
        } else {
          newDomain = matchEverything
              ? domainHandle : output[(size_t)segnum];
        }
        assert(newDomain.isDeliverable());
        subCalcs[i]->calc(tg, segnum, std::move(newDomain));
      }

      // Releasing the collector as soon as possible can save merging work.
      // On the other hand, it could delay the sub-calculators and spoil
      // the domain (which should be in the CPU cache).
      auto count = collectorMerger.release(data.release());
      if (count == numSegs) {
        // we are done, so we can call the callback
        // we could use a nested task_group to wait until we are done here as well.

        // If there are sub-calculators that can run, lanch a separate task
        // for field loading.
        auto* tgFieldLoad = subCalcs.size() > 0 ? tg : nullptr;
        task_group_run(tgFieldLoad, [this]() {doneCollecting();});
      }




    }

    void doneCollecting() {
      auto& op = thisOp();
      if (op.rankingSink) {
        // The sink is responsible for everything downstream of "ranking is
        // ready": emit, fuse, etc.  We pass the merged collector (possibly
        // null on the empty-index path) so the sink can sort() and act on
        // the ranked output.
        op.rankingSink(*this, collectorMerger.getData());
      } else {
        op.fillQueryTopNResponse(*this);
      }
      // TODO: figure out what state we can dump before and after this call.
    };
  };


  // ProtobufSearchParser resolves everything that can fail and passes the
  // execution sources in. A fully resident validated membership plan may omit
  // the main Weight entirely.
  TopDocsReq(SearchRequest& req, std::string_view name, const ReqTopDocs& topDocsProto,
    Query::PlanningContext& planning, Query::Context* weightContext,
    Query* query, Query::Weight* weight,
    Query::Weight* variantMembershipWeight,
    Query::Weight* countWeight, Query::Weight* rankingWeight, int64_t topCount,
    SortPlan&& sortPlan, CollectionRequirements requirements,
    Query::Weight* wholeMembershipWeight,
    Query::Weight* wholeRankingWeight,
    FilterCache::Use* wholeMembershipUse,
    std::span<const uint8_t> wholeFieldSortCacheRoutes,
    bool wholeConstantRanking, Query::ScoreProfile scoreProfile,
    bool requestNeedsScores,
    std::span<ParsedFilter> filters,
    std::span<Query::Weight*> filterWeights,
    DomainVariantPlan&& domainVariants,
    Query* domainQuery, Query::Weight* domainQueryWeight,
    std::span<Query::Weight*> domainFilterWeights,
    std::span<FilterCache::Use*> residentExactDomainUses)
    : SearchOp(req, name), topDocsProto(topDocsProto), planning(planning),
      weightContext(weightContext), query(query),
      weight(weight), variantMembershipWeight(variantMembershipWeight),
      countWeight(countWeight), rankingWeight(rankingWeight),
      wholeRankingWeight(wholeRankingWeight),
      wholeConstantRanking(wholeConstantRanking), scoreProfile(scoreProfile),
      requestNeedsScores(requestNeedsScores),
      topCount(topCount), filters(filters), filterWeights(filterWeights),
      domainVariants(std::move(domainVariants)),
      requirements(requirements),
      sortPlan(std::move(sortPlan)) {
    auto wholeConsumer = this->sortPlan.useFieldSort
        ? QueryPrep::WholeMembershipConsumer::FIELD_SORT
        : requirements.needRankedDocs
              ? QueryPrep::WholeMembershipConsumer::TOP_K_COUNT
              : QueryPrep::WholeMembershipConsumer::COUNT;
    if (wholeMembershipWeight != nullptr) {
      wholeMembershipPlan = QueryPrep::WholeMembershipPlan(
          *wholeMembershipWeight, wholeMembershipUse,
          planning.filterUses,
          PreparedDomainDependence::QUERY_CANONICAL,
          wholeConsumer,
          wholeFieldSortCacheRoutes);
    } else if (wholeMembershipUse != nullptr) {
      wholeMembershipPlan = QueryPrep::WholeMembershipPlan::acceptedExisting(
          *wholeMembershipUse, planning.filterUses, wholeConsumer);
    }
    if (weight == nullptr) {
      bool residentWhole = wholeMembershipUse != nullptr
          && !requirements.needExactDomain
          && ((this->sortPlan.useFieldSort
                  && requirements.needRankedDocs && !requestNeedsScores)
              || (requirements.needExactCount
                  && (!requirements.needRankedDocs
                      || scoreProfile.kind
                          != Query::ScoreProfile::Kind::VARIABLE)));
      bool residentExact = !residentExactDomainUses.empty()
          && requirements.needExactDomain && !this->sortPlan.useFieldSort
          && (!requirements.needRankedDocs
              || scoreProfile.kind != Query::ScoreProfile::Kind::VARIABLE);
      if ((!residentWhole && !residentExact) || !filterWeights.empty()) {
        throw std::logic_error(
            "Weight-free TopDocs requires complete resident membership");
      }
    }
    if (!filterWeights.empty()) {
      filterUses = planning.pool.make_span<FilterCache::Use*>(
          filterWeights.size());
      size_t source = 0;
      for (const auto& filter : filters) {
        for (size_t i = 0; i < filter.sourceCount(); i++) {
          filterUses[source++] = planning.getFilterUse(*filter.source(i));
        }
      }
      assert(source == filterWeights.size());
    }
    if (!residentExactDomainUses.empty()) {
      for (auto* use : residentExactDomainUses) {
        exactDomainPlan.addAcceptedExisting(*use);
      }
    } else if (domainQueryWeight != nullptr) {
      if (!domainQueryWeight->matchesAllDocs()) {
        exactDomainPlan.add(
            *domainQueryWeight, planning.getFilterUse(*domainQuery));
      }
      size_t source = 0;
      for (const auto& filter : filters) {
        assert(!filter.composesUnion());
        for (size_t i = 0; i < filter.sourceCount(); i++) {
          exactDomainPlan.add(
              *domainFilterWeights[source++],
              planning.getFilterUse(*filter.source(i)));
        }
      }
      assert(source == domainFilterWeights.size());
    }
  }

  Calculator* createCalculator(Calculator* parent, int64_t slot = -1, int64_t numSlots = -1) override {
    return new Calc(*this, parent);
  }


  // Called after all segments have been collected for a TopN query to fill out
  // the DocList proto.  Hands off the merged collector's ranked output to the
  // shared streaming emitter (emitDocsResponse), which blocks until field
  // loading completes and sends multiple streaming responses (all but the
  // last).  The empty-index case (no segments -> no collector ever obtained)
  // writes an empty DocList into the request's lastResponse (with found=0 only
  // when the count was requested); submitBody then sends it.
  void fillQueryTopNResponse(TopDocsReq::Calc& calc) {
    auto& qr = *this;
    auto* mergeableCollector = calc.collectorMerger.getData();

    if (mergeableCollector == nullptr) {
      auto& searchResultProto = *calc.getTarget(nullptr);
      auto& docListProto = oneofMut<luxir::api::DocList>(searchResultProto);
      // found is opt-in (see emitDocsResponse): only populate it when the
      // count was requested, so an empty index matches the non-empty contract.
      if (qr.topDocsProto.get_number) docListProto.found = 0;
      return;
    }

    auto getDocList = [&calc](SearchResponse* resp) -> luxir::api::DocList& {
      auto& val = *calc.getTarget(resp);
      return oneofMut<luxir::api::DocList>(val);
    };

    if (mergeableCollector->useFieldSort) {
      auto& collector = *mergeableCollector->fieldCollector;
      auto sortDocs = collector.sort();
      emitDocsResponse(qr.req, getDocList,
        (int64_t)sortDocs.size(),
        [sortDocs](int64_t i) { return sortDocs[i].doc; },
        [sortDocs](int64_t i) { return sortDocs[i].score; },
        collector.totalHits(),
        qr.topDocsProto.fields,
        qr.topDocsProto.batch_size,
        qr.topDocsProto.offset,
        qr.topDocsProto.get_number,
        qr.topDocsProto.get_scores,
        qr.topDocsProto.document_format);
    } else {
      auto& collector = *mergeableCollector->scoreCollector;
      auto scoreDocs = collector.sort();
      emitDocsResponse(qr.req, getDocList,
        (int64_t)scoreDocs.size(),
        [scoreDocs](int64_t i) { return scoreDocs[i].doc; },
        [scoreDocs](int64_t i) { return scoreDocs[i].score; },
        collector.totalHits(),
        qr.topDocsProto.fields,
        qr.topDocsProto.batch_size,
        qr.topDocsProto.offset,
        qr.topDocsProto.get_number,
        qr.topDocsProto.get_scores,
        qr.topDocsProto.document_format);
    }
  }

};


}
