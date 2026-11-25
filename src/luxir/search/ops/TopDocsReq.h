// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <limits>
#include <functional>
#include "SearchOp.h"
#include "luxir/util/Signal.h"
#include "FacetExecution.h"
#include "MetricGroup.h"
#include "luxir/query/Query.h"
#include "luxir/query/QueryShape.h"
#include "luxir/search/SearchOverrides.h"
#include "luxir/query/QueryPrep.h"
#include "luxir/reader/SkipStats.h"
#include "luxir/reader/IntColReader.h"
#include "luxir/reader/StoredFieldsReader.h"
#include "luxir/reader/StrColReader.h"
#include "luxir/search/Collector.h"
#include "luxir/search/CollapseMembers.h"
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

// Build weights for a filter list after serial logical validation.
std::span<Query::Weight*> buildFilterWeights(
    std::span<ParsedFilter> filters, Query::Context& qcontext,
    int32_t requestFlags);

void declareFilterReads(ReadDeclarer& reads, std::span<ParsedFilter> filters,
                        Query::PlanningContext& planning, int32_t requestFlags);

enum class TopDocsPlacement : uint8_t {
  ROOT_OP,
  NESTED_OP,
  FUSION_SOURCE,
  ROW_OP,
};

class TopDocsReq : public SearchOp {
public:
  // Carries parsed inputs used only while materializing the TopDocs execution plan.
  struct Preparation {
    // Query before op-level filters fold in, for exact-domain construction.
    Query* domainQuery;
    // Main weight flags other than NEED_SCORES, which is stored on the op.
    int32_t weightFlags;
    TopDocsPlacement placement;
    // The main query already includes the op-level filters.
    bool foldFilters;
    // Child domain routes require a separate membership weight.
    bool routedFilters;
    // The request shape allows pruning before collapse/diversify constraints.
    bool allowPruning;
    // Score-ranked hits with a count, without grouping or child domains.
    bool exactCountTopKShape;
    // The request is eligible for bounded root top-K counting.
    bool boundedCount;
    // The request can answer a count from whole membership.
    bool pureCountShape;
    // The request permits estimating the count.
    bool estimateCount;
  };

private:
  struct SourceSelection {
    // View cache used for resident selection and weight-time registration.
    FilterCache* cache;
    // Accepted whole-membership source for count or ranking.
    FilterCache::Use* cacheFirstMembershipUse;
    // Accepted sources for an exact child domain.
    std::span<FilterCache::Use*> residentExactDomainUses;
    // Original request weight flags, including scoring.
    int32_t requestFlags;
    // Initial main weight flags after choosing a resident route.
    int32_t mainWeightFlags;
    // The request asks for score-ranked hits and an unbounded exact count.
    bool exactCountTopK;
    // Ranked exact-domain hits can use constant scores.
    bool constantExactDomainRanking;
    // The exact-count top-K route accepted a resident count source.
    bool cacheFirstTopKCount;
    // The entire exact domain can use resident sources.
    bool cacheFirstExactDomain;
    // Field sort can rank directly from resident membership.
    bool cacheFirstFieldSort;
    // Resident top-K counting still needs a variable-score ranking weight.
    bool cacheFirstVariableRanking;
    // The selected route can skip the main weight and its context.
    bool omitMainWeight;
  };

  // Additional inputs used only during materialization.
  const Preparation preparation;
  // Filled once before building the execution plan.
  std::optional<SourceSelection> sourceSelection;

  // Select and pin resident sources without building a context or weights.
  void selectSources();
  bool buildsFilterWeights() const;
  bool hasOrdinaryExactDomain() const;
  bool buildsSeparateExactDomain() const;
  bool mayRerouteMainWeight() const;
  bool mayComposeExactCountTopK() const;

public:
  // Parsed inputs shared by planning and execution.
  const ReqTopDocs& topDocsProto;  // the relevant part of the protobuf request
  Query::PlanningContext& planning;
  Query* query;
  Query* resultQuery;
  std::span<ParsedFilter> filters;
  // Logical variant routes with their weights and uses filled in at materialization.
  DomainVariantPlan domainVariants;
  CollectionRequirements requirements;
  int64_t topCount; // Collection depth (offset + page size, capped by maxDoc).
  SortPlan sortPlan;
  CollapsePlan* collapse;
  DiversifyPlan* diversify;
  std::optional<SortPlan> headSort;
  Query::ScoreProfile scoreProfile;
  bool requestNeedsScores;

  // Contexts, weights and routes bound during materialization.
  Query::Context* weightContext = nullptr;
  Query::Weight* weight = nullptr;
  Query::Weight* variantMembershipWeight = nullptr;
  Query::Weight* countWeight = nullptr;
  Query::Weight* rankingWeight = nullptr;
  Query::Weight* wholeRankingWeight = nullptr;
  bool wholeConstantRanking = false;
  bool forcedExactCountTopKComposition = false;
  std::span<const uint8_t> provedDensitySegments;
  std::span<Query::Weight*> filterWeights;
  std::span<FilterCache::Use*> filterUses;
  // The requested quantity stays in requirements; exact routes need no hold.
  bool boundedCount = false;
  // Unscored membership for a bounded count counted apart from ranking (see
  // boundedCountThenCollectTopK); null keeps the held-threshold collection.
  Query::Weight* boundedCountWeight = nullptr;
  bool countLimitProven = false;
  // The request permits an estimate of this bounded count. The estimator
  // samples through the ranking weight, scored with pruning allowed, so lazy
  // plans stay lazy: an unscored weight would materialize a multi-term union.
  bool estimateCount = false;
  QueryPrep::WholeMembershipPlan wholeMembershipPlan;
  QueryPrep::ExactDomainPlan exactDomainPlan;
  // Every field-sort collector's primaryValues (see sortValueEnvelope).
  std::optional<Query::ValueEnvelope> sortValues;

  class CalcBase;
  std::vector<TopDocsReq*> rowOps;
  std::span<ReturnField> rowFields;
  MetricGroupPlan* metricOrder = nullptr;
  boost::unordered_flat_map<std::string_view, SearchOp*> groupOps;

  // Optional sink for the merged top-K collector.  If set, the Calc invokes
  // it instead of self-emitting via fillQueryTopNResponse, letting another
  // op (e.g. FusionOp) consume this TopDocsReq's ranking.  `mc` is null on
  // the empty-index path (no segments, no collector ever obtained); sinks
  // must handle that case.
  std::function<void(CalcBase&, MergeableCollector*)> rankingSink;

  // Takes parsed, validated inputs only.
  TopDocsReq(SearchRequest& req, const std::shared_ptr<IndexReader>& view,
             std::string_view name, const ReqTopDocs& topDocsProto,
             Query::PlanningContext& planning, Query* query, Query* resultQuery,
             std::span<ParsedFilter> filters, DomainVariantPlan&& domainVariants,
             CollectionRequirements requirements, int64_t topCount,
             SortPlan&& sortPlan, CollapsePlan* collapse, DiversifyPlan* diversify,
             std::optional<SortPlan>&& headSort, Query::ScoreProfile scoreProfile,
             bool requestNeedsScores, Preparation preparation);

  // Project the possible root modes after source selection.
  void declareReads(ReadDeclarer& reads) const;
  // Runs once after construction to build the contexts, weights, filter uses and execution routes.
  void materialize() override;

  // Bind the decorators against this source's materialized query.
  void bindDecorators();
  // Open the execution context on demand if cache-first selection omitted it.
  Query::Context& decoratorQueryContext();
  // Check execution traits after materializing a row operation.
  void validateRowExecution() const;

  std::vector<ExtraColumn> rowColumns();

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
  static inline int32_t localWholeMembershipMaxCandidatesPerMatchForTests = 8;
  static inline bool forceExactCountTopKCompositionForTests = false;
  static inline bool disableProvedDensityCompositionForTests = false;
  static inline int32_t provedDensityCompositionInverseForTests = 8;
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

  // A requested estimate. Where ranking pruned a segment, the segment counts the
  // leading 1/64, then 1/8, of kEstimateSlices evenly spaced doc ranges until
  // the sample holds kEstimateMinMatches matches (about 6% relative error for
  // doc order independent of the query), and scales by the sampled fraction.
  // That visits at most 1/8 of the segment, the estimator's whole budget: a
  // full count runs only when the matches provably fit the same budget, and a
  // sample too thin to scale reports the lower bound the segment can prove.
  static constexpr int32_t kEstimateSlices = 16;
  static constexpr int64_t kEstimateMinMatches = 256;
  static constexpr int32_t kEstimateSampleShifts[] = {6, 3};
  static constexpr int32_t kEstimateBudgetShift = 3;
  // Below this, a full count costs too little to be worth sampling.
  static constexpr int64_t kEstimateMinBudgetDocs = 8192;

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
  // A bounded count pass counts ranges of this many docids (several count
  // windows), checking the shared total after each: a stop overshoots the
  // limit by at most one range.
  static inline int32_t boundedCountRangeDocsForTests = 1 << 16;

  enum class ConstantScoreDisposition : uint8_t {
    NOT_CONSTANT_TOP_K,
    LIMIT_ONLY,
    COMPLETE,
  };

  enum class ExactCountTopKRoute : uint8_t {
    EXACT_CANDIDATE_SCORING,
    CONSTANT_FIRST_K,
  };

  // Every materialized domain whose sole numeric key offers masked leaf
  // bounds takes the exact-domain driver, whatever its cardinality or
  // representation: the driver plans its phase by cost
  // (ExactDomainSortCosts::plan) from the predicted visit floor
  // (FieldSortCollector::predictedVisitFloor: the leaves bounded at or
  // before the bottom the heap is predicted to reach) and the leaves the
  // heap as it stands still admits (FieldSortCollector::leavesWithinBottom),
  // and re-plans the rest at checkpoints.
  struct ExactDomainSortPlan {
    FieldSortCollector::KeyBlockPlan keys;
    ExactDomainSortRoute route;

    bool available() const noexcept { return keys.batch != nullptr; }
  };

  // `firstDoc` and `lastDoc` span the domain in doc order.
  static ExactDomainSortPlan planExactDomainSort(
      auto& collector, int64_t card, int32_t firstDoc,
      int32_t lastDoc) {
    if (disableFieldSortPruning || disableFieldSortBestFirst || card <= 0) {
      return {};
    }
    auto keys = collector.maskedKeyBlockPlan();
    if (keys.batch == nullptr) return {};
    ExactDomainSortRoute route;
    route.card = card;
    route.checkProgress = !forceFieldSortBestFirst;
    route.gatherCapForTests = forceFieldSortWorkCapForTests;
    if (!forceFieldSortBestFirst && ExactDomainSortCosts::scansOutright(card)) {
      route.floor = keys.leafCount;
      route.phase = ExactDomainSortPhase::SCAN;
      return {keys, route};
    }
    auto rest = collector.segmentRest(
        card, collector.predictedVisitFloor(card), firstDoc, lastDoc);
    route.floor = rest.floor;
    route.empty = rest.empty;
    route.outside = rest.leaves - rest.inside;
    route.lead = rest.lead;
    route.phase = forceFieldSortBestFirst
        ? ExactDomainSortPhase::BOUND_ORDER
        : ExactDomainSortCosts::plan(
              rest, collector.leavesBeforeKeyFloor >= keys.leafCount);
    return {keys, route};
  }

  // A supplier's membership work as a walk over it meets it: under an
  // external filter the walk still produces the supplier's matches, and
  // collects only the filter's share of them.
  static Query::ScorerSupplier::MembershipWork walkMembershipWork(
      Query::ScorerSupplier& supplier, DocSet* filter, int32_t maxDoc) {
    auto work = supplier.membershipWork(maxDoc);
    if (filter != nullptr) {
      work.produce += 8 * work.matches;
      double density = (double)filter->card() / maxDoc;
      work.matches *= density;
      work.savedProduce *= density;
      work.window += 64;
    }
    return work;
  }

  // The first and last doc of a nonempty BITSET or ARRAY domain.
  static void domainSpan(DocSet& domain, int32_t& first, int32_t& last) {
    if (domain.type == DocSet::ARRAY) {
      auto docs = ((ArrDocSet&)domain).docs();
      if (docs.empty()) return;
      first = docs.front();
      last = docs.back();
      return;
    }
    const FixedBitSet& bits = ((BitDocSet&)domain).bits();
    std::span<const uint64_t> words(bits.words,
                                    FixedBitSet::sizeInWords(bits.size()));
    size_t w = 0;
    while (w < words.size() && words[w] == 0) w++;
    if (w == words.size()) return;
    first = (int32_t)(w * 64 + std::countr_zero(words[w]));
    w = words.size();
    while (words[w - 1] == 0) w--;
    last = (int32_t)((w - 1) * 64 + 63 - std::countl_zero(words[w - 1]));
  }

  // Seeded collection fixes its first pass before it sees a key, so it
  // seeds the leaves bounded before the bottom predicted somewhat past k:
  // the count of domain docs before the true k-th key varies by about
  // sqrt(k), and when the seeds hold too few of them the second pass gathers
  // what the first missed under a bottom worse than the final one.
  // Measured by instruction counts (5M docs, uniform keys, hugin
  // 2026-10-06): on 'the' TOP_1000_FF seeding 0.94, 1.00, 1.06, 1.2 and 1.5
  // times the depth-k floor cost 1.81, 1.67, 1.48, 1.58 and 1.78 G; over
  // 0, 0.5, 1, 1.5 and 2 sigmas it cost 1.13, 0.98, 0.98, 0.99 and 1.00 of
  // the earlier linear density floor, and 'medical +devices' TOP_10_FF (a
  // sparse domain: 1,756 floor leaves at k = 10) 1.14, 1.07, 1.13, 1.16 and
  // 1.20; 450 sampled field-sort requests move under 0.1% across them.
  static constexpr double kSeedDepthSigmas = 0.5;
  static int64_t seedDepth(int64_t topCount) {
    return topCount
        + (int64_t)std::ceil(kSeedDepthSigmas * std::sqrt((double)topCount));
  }

  // The values every collected doc holds in the primary sort column: what
  // the main query and each op-level filter require of that field
  // (Query::requiredValueEnvelope), intersected. Every route collects only
  // docs that match the query and pass every filter (folded into the query,
  // or composed into the default domain, which includes them all), and a
  // parent domain only narrows that, so the envelope holds for whatever
  // domain source a route reads: a resident whole membership, a cached exact
  // set, a collector filter, or the query's own matches. The comparator
  // decides whether its key follows from it (KeyBatch::segmentKeys).
  static std::optional<Query::ValueEnvelope> sortValueEnvelope(
      const SortPlan& sortPlan, const Query* query,
      std::span<const ParsedFilter> filters) {
    if (!sortPlan.useFieldSort || sortPlan.clauses.empty()
        || sortPlan.clauses[0].getKind() != SortClause::COLUMN) {
      return std::nullopt;
    }
    std::string_view field = sortPlan.clauses[0].getSortField().getFieldName();
    std::optional<Query::ValueEnvelope> result;
    auto require = [&](const Query* restriction) {
      if (restriction == nullptr) return;
      auto envelope = restriction->requiredValueEnvelope(field);
      if (!envelope.has_value()) return;
      result = result.has_value() ? result->intersect(*envelope) : *envelope;
    };
    require(query);
    for (const ParsedFilter& filter : filters) require(filter.query);
    return result;
  }

  static bool fieldSortCanUseMaskedBestFirst(const SortPlan& sortPlan) {
    if (sortPlan.clauses.size() != 1
        || sortPlan.clauses[0].getKind() != SortClause::COLUMN) {
      return false;
    }
    return FieldType::isNumericColumn(const_cast<FieldType&>(
        sortPlan.clauses[0].getSortField().getFieldType()).type());
  }

  using SegmentKeys = FieldComparator::SegmentKeys;

  // The primary sort column's keys in every segment, by segment ord, for a
  // sort the exact-domain driver can serve; empty otherwise. Read from each
  // segment's cached Field leaf metadata, as offered by the comparator.
  static std::vector<std::optional<SegmentKeys>> fieldSortSegmentKeys(
      IndexReader& reader, const SortPlan& sortPlan,
      std::optional<Query::ValueEnvelope> envelope) {
    std::vector<std::optional<SegmentKeys>> keys;
    if (!fieldSortCanUseMaskedBestFirst(sortPlan)) return keys;
    const SortField& sortField = sortPlan.clauses[0].getSortField();
    auto fields = reader.field(sortField.getFieldName());
    keys.reserve(reader.segments().size());
    for (auto& segment : reader.segments()) {
      auto* info = fields.at((size_t)segment.ord);
      const auto* meta = info ? segment.postingsReader().openNumericLeafMeta(*info) : nullptr;
      if (meta) {
        keys.emplace_back(std::in_place, *meta, sortField.isReversed(), envelope);
      } else {
        keys.emplace_back();
      }
    }
    return keys;
  }

  // Whether a segment without keys may hold docs that beat every key: under
  // MISSING_FIRST a doc without a value sorts first, and it is collected
  // unless an envelope, which no missing value satisfies, keeps it out.
  static bool fieldSortMissingFirst(
      const SortPlan& sortPlan,
      const std::optional<Query::ValueEnvelope>& envelope) {
    return !envelope.has_value() && !sortPlan.clauses.empty()
        && sortPlan.clauses[0].getKind() == SortClause::COLUMN
        && sortPlan.clauses[0].getSortField().getMissingValue()
            == FieldComparator::MISSING_FIRST;
  }

  // Field-sort segment visit order: best first by each segment's
  // topCount-th best in-envelope key (its worst when it holds fewer
  // values), ties in segment order. A segment without keys (its column
  // absent, sparse, or multi-valued there) ranks at the missing value's
  // edge: first when `missingFirst` (fieldSortMissingFirst), as its docs
  // without a value beat every key, and last otherwise. The heap then
  // fills from the segments holding the best keys, so later segments
  // settle at their first bounds: newest first for a descending sort on a
  // time-ordered key. The kept top K is order-independent (segdoc breaks
  // ties), so the order only moves work.
  static std::vector<int32_t> fieldSortVisitOrder(
      std::span<const std::optional<SegmentKeys>> keys, int64_t topCount,
      bool missingFirst) {
    std::vector<std::pair<int64_t, int32_t>> ranked;
    ranked.reserve(keys.size());
    for (size_t s = 0; s < keys.size(); s++) {
      int64_t key = !keys[s].has_value() && missingFirst
          ? std::numeric_limits<int64_t>::min()
          : std::numeric_limits<int64_t>::max();
      if (keys[s].has_value() && keys[s]->values() > 0) {
        key = keys[s]->keyAt(
            std::min(1.0, (double)topCount / keys[s]->values()));
      }
      ranked.push_back({key, (int32_t)s});
    }
    std::sort(ranked.begin(), ranked.end());
    std::vector<int32_t> order;
    order.reserve(ranked.size());
    for (auto [key, s] : ranked) order.push_back(s);
    return order;
  }

  // Complete resident masks omit the Weight and retain its request pin.
  static FilterCache::Use* planCacheFirstFieldSortWholeMembership(
      Query& query, Query::PlanningContext& planning, bool allowReaderStable) {
    auto candidate = planning.lookupExistingFilterUse(query, allowReaderStable);
    if (!candidate) return nullptr;
    bool observed = QueryPrep::WholeMembershipPlan::observesFieldSort(
        QueryPrep::WholeMembershipConsumer::FIELD_SORT, planning.filterScope(query));
    auto* use = planning.acceptExistingFilterUse(
        std::move(*candidate), FilterCache::AdmissionLane::WHOLE,
        observed ? FilterCache::Work::pending().forWholeQuery() : FilterCache::Work::assumed());
    if (observed && use != nullptr) use->observe();
    return use;
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

  void init() override {
    SearchOp::init();
    for (auto* row : rowOps) row->init();
    for (auto& [name, child] : groupOps) child->init();
    if (collapse && !topDocsProto.collapse->sort.empty()) resolveMetricGroupOrder(*this);
  }
  bool enrichGroups() const {
    return collapse != nullptr && (topDocsProto.collapse->get_count || headSort.has_value() || !groupOps.empty())
        && topCount > topDocsProto.offset;
  }
  void releaseSegmentState(IndexReader::Segment& segment) noexcept override {
    if (segmentStateRetained(segment)) return;
    for (auto* source : {weight, variantMembershipWeight, countWeight,
                         rankingWeight, wholeRankingWeight, boundedCountWeight}) {
      if (source) source->releaseSegmentState(segment);
    }
    for (auto* source : filterWeights) source->releaseSegmentState(segment);
    wholeMembershipPlan.releaseSegmentState(segment);
    exactDomainPlan.releaseSegmentState(segment);
    domainVariants.releaseSegmentState(segment);
    SearchOp::releaseSegmentState(segment);
    for (auto& [name, child] : groupOps) child->releaseSegmentState(segment);
  }

  bool facetBucketSharesSegmentState() const override {
    for (auto* source : {weight, variantMembershipWeight, countWeight,
                         rankingWeight, wholeRankingWeight, boundedCountWeight}) {
      if (source && source->hasReleasableSegmentState()) return true;
    }
    return std::ranges::any_of(filterWeights, [](const auto* source) {
             return source->hasReleasableSegmentState();
           })
        || wholeMembershipPlan.hasReleasableSegmentState()
        || exactDomainPlan.hasReleasableSegmentState()
        || domainVariants.hasReleasableSegmentState()
        || SearchOp::facetBucketSharesSegmentState();
  }

  bool bucketVariantsNeedCompleteInput(bool selectionInPath = false) const {
    return !domainVariants.segmentLocal(selectionInPath);
  }

  bool needsCompleteBucketInput() const override {
    return enrichGroups() || (weight && weight->needsPrepare())
        || std::ranges::any_of(filterWeights, [](auto* source) { return source->needsPrepare(); })
        || bucketVariantsNeedCompleteInput()
        || SearchOp::needsCompleteBucketInput()
        || std::ranges::any_of(groupOps, [](const auto& entry) {
          return entry.second->needsStructuralScope || entry.second->needsCompleteBucketInput();
        });
  }

  bool outputsEmpty(IndexReader::Segment& segment, DocSet* input,
                    std::vector<std::string>* details) const;

  class CalcBase : public SearchOp::Calculator {
    ExecutionProfileRun* pruningRun = nullptr;
#ifndef NDEBUG
    bool prunedFanout = false;
#endif
  public:
    // Page result columns live in the final response arena, pinned through
    // all streaming batches. They are sliced only after every binding closes.
    luxir::api::GroupTable* pageGroups = nullptr;
    luxir::api::Val* getTargetForSub(SearchResponse* resp, SearchOp& sub) override {
      auto& groups = thisOp().groupOps;
      auto found = groups.find(sub.name);
      if (found != groups.end() && found->second == &sub) {
        assert(pageGroups != nullptr);
        assert(resp == op.req.lastResponse); // bucket children never stream
        return build::opsSlot(pageGroups->ops, groups.size(), sub.name, resp->mr);
      }
      auto* ourVal = parent->getTargetForSub(resp, op);
      assert(ourVal != nullptr);
      // Our DocList lives in our bucket slot when this TopDocs is a facet
      // bucket child; the Val should either be unset or already hold it.
      auto& target = slotTarget<luxir::api::ArrVal>(*ourVal, resp->mr);
      assert(std::holds_alternative<luxir::api::DocList>(target.kind)
             || std::holds_alternative<std::monostate>(target.kind));
      auto& dl = oneofMut<luxir::api::DocList>(target);
      return build::opsSlot(dl.ops, op.subOps.size(), sub.name, resp->mr);
    }

    CalcBase(TopDocsReq& op, Calculator* parent, int64_t slot, int64_t numSlots,
        const StructuralConstraint* structural = nullptr)
      : SearchOp::Calculator(op, parent, slot, numSlots, structural),
        collectorMerger(nullptr, nullptr) {

      collectorMerger.creator = [&op]() -> MergeableCollector* {
        auto* collector = new MergeableCollector(
            op.topCount, op.sortPlan, op.view.get(),
            op.requestNeedsScores, op.collapse);
        if (collector->useFieldSort) {
          if (op.collapse != nullptr) collector->groupFieldCollector->primaryValues = op.sortValues;
          else collector->fieldCollector->primaryValues = op.sortValues;
        }
        return collector;
      };
      collectorMerger.destroyer = [](MergeableCollector* data) {
        delete data;
      };
      if (op.boundedCount) {
        scoreAccumulator.holdUntilHits(op.requirements.countLimit, op.countLimitProven);
        boundedTotal.setLimit(op.requirements.countLimit);
      }

      if (op.subOps.size() > 0) {
        if (op.domainVariants.empty()
            || !op.domainVariants.needsInheritProduction()) {
          output.resize(op.view->segments().size());
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
      if (op.enrichGroups()) {
        memberReplay = std::make_unique<MemberReplay>(op.req.memoryTracker,
            op.view->segments().size());
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
    }

    TopDocsReq& thisOp() {
      return static_cast<TopDocsReq&>(op);
    }



    AtomicMerger<MergeableCollector> collectorMerger;
    MaxScoreAccumulator scoreAccumulator;
    // Split bounded counts: the shared total, and a threshold open from the
    // first page for their ranking passes.
    BoundedHitTotal boundedTotal;
    MaxScoreAccumulator rankingAccumulator;


    // Calc pins finalized eligibility and its replayable prepared SegmentSource
    // from collection through all page-member/bucket execution. It releases the
    // eligibility afterwards; the prepared Weight lives until Calc destruction.
    // No query is prepared again, and no ranking floor is applied on replay.
    struct MemberReplay {
      RequestMemTracker& tracker;
      size_t ownedBytes = 0;
      RequestMemResource resource;
      std::pmr::vector<DomainHandle> domains;
      MemberReplay(RequestMemTracker& tracker, size_t segments)
          : tracker(tracker), resource(tracker, "eligibility replay", "collapse member state"), domains(&resource) {
        domains.resize(segments);
      }
      ~MemberReplay() { if (ownedBytes) tracker.release(ownedBytes); }
      DomainHandle compose(std::vector<DomainHandle> parts, size_t setBytes) {
        size_t count = std::ranges::count_if(parts, [](const auto& part) { return part.get() != nullptr; });
        if (count <= 1) return DomainVariantPlan::compose(std::move(parts));
        // Reserve intersection scratch before allocation; only its new owned
        // result remains charged. Borrowed cache/live/parent sets keep their
        // existing owners and accounting rather than being charged again.
        size_t bytes = 3 * setBytes;
        tracker.charge(bytes, "collapse member state", "retained eligibility");
        auto release = scope_guard([&]() noexcept { tracker.release(bytes); });
        auto result = DomainVariantPlan::compose(std::move(parts));
        size_t retained = result.get()->ramBytesUsed();
        assert(retained <= bytes);
        bytes -= retained;
        ownedBytes += retained;
        return result;
      }
    };
    std::unique_ptr<MemberReplay> memberReplay;
    std::unique_ptr<MetricGroupState> metricGroups;
    struct StructuralResetState {
      RequestMemResource resource;
      std::pmr::vector<std::optional<DomainHandle>> domains;
      std::pmr::vector<std::shared_ptr<DomainVariantPlan::Reservation>> preparedCharges;
      StructuralResetState(RequestMemTracker& tracker, size_t segments)
          : resource(tracker, "top_docs reset bases", "domain variants"),
            domains(&resource), preparedCharges(&resource) {
        domains.resize(segments);
      }
    };
    std::unique_ptr<StructuralResetState> structuralReset;

    DomainHandle resetBaseHandle(int32_t segment) {
      auto& seg = thisOp().view->segments()[(size_t)segment];
      DomainHandle live = DomainHandle::pinned(seg.liveDocSet(), thisOp().view).scoped(seg);
      if (!requiresWholeIndexPrepare) return structural->prefix(segment, std::move(live));
      auto& base = structuralReset->domains[(size_t)segment];
      if (!base) base = structural->prefix(segment, std::move(live));
      // Before the first selection the predicate prefix can be all live docs,
      // legitimately null on a segment without deletes. Engaged != non-null.
      assert(base->isDeliverable());
      return *base;
    }

    template <bool Structural>
    DocSet* resetBase(int32_t segment) {
      if constexpr (Structural) return resetBaseHandle(segment).get();
      else {
        auto& seg = op.view->segments()[(size_t)segment];
        return seg.liveDocSet();
      }
    }

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
        const auto& collector,
        DocSetBuilder* builder) noexcept {
      const auto& op = thisOp();
      if (collector.topCount <= 0 || op.weight == nullptr
          || !op.weight->isConstantScoring()) {
        return ConstantScoreDisposition::NOT_CONSTANT_TOP_K;
      }
      if constexpr (std::remove_cvref_t<decltype(collector)>::isCollapsed) {
        if (disableCollapsePruningForTests) return ConstantScoreDisposition::NOT_CONSTANT_TOP_K;
      }
      // First-K collection applies any collector filter before accepting hits.
      if (!TopDocsReq::disableConstantWindowCaptureForTests
          && (!op.requirements.needsCount()
              || (op.boundedCount && scoreAccumulator.canPrune()))
          && builder == nullptr) {
        skipCount(SkipStats::constantLimitOnlyCollections);
        return ConstantScoreDisposition::LIMIT_ONLY;
      }
      // Document first-K/count drains cannot supply distinct totals or child
      // domains. Only the limit-only bounded pull is group-aware.
      if constexpr (std::remove_cvref_t<decltype(collector)>::isCollapsed) {
        return ConstantScoreDisposition::NOT_CONSTANT_TOP_K;
      } else return ConstantScoreDisposition::COMPLETE;
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
      if (!QueryPrep::exactCountTopKCandidateDensity(
              candidateCost, maxDoc, densityInverse)) {
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

    // Adds this segment's share of an estimated count, given the matches
    // ranking saw before it pruned. Exact counts are never replaced by a guess,
    // and a guess stays within the bounds index statistics prove.
    void estimateSegmentCount(MemPool& pool, IndexReader::Segment& seg,
                              DocSet* collectorFilter, auto& collector,
                              int64_t seen) {
      auto& weight = *thisOp().weight;
      auto bounds = weight.countShortcut(seg, nullptr);
      if (collectorFilter == nullptr && bounds.how == Query::CountHow::METADATA) {
        collector.estimatedHits += bounds.maxMatches;
        return;
      }
      // Clamp to the bounds; minMatches ignores a collector filter.
      int64_t floor = collectorFilter == nullptr
          ? std::max(seen, bounds.minMatches) : seen;
      int64_t ceiling = bounds.maxMatches;
      if (floor >= ceiling) {
        collector.estimatedHits += floor;
        return;
      }
      int32_t maxDoc = seg.maxDoc();

      auto countRanges = [&](auto rangeOf) -> std::optional<int64_t> {
        auto* supplier = weight.scorerSupplier(pool, seg);
        if (supplier == nullptr) return std::nullopt;
        Query::ScorerSupplier::BulkScorerContext bulkContext;
        auto plan = supplier->planBulk(
            Query::ScorerSupplier::BulkUse::COUNT_WINDOWS, bulkContext);
        BulkScorer* bulk = nullptr;
        Query::Scorer* scorer = nullptr;
        if (plan.available == Query::ScorerSupplier::BulkAnswer::YES) {
          bulk = supplier->buildBulk(pool, plan);
        } else {
          supplier->recordBulkPlanCommitment(
              Query::ScorerSupplier::BulkUse::COUNT_WINDOWS, bulkContext, plan);
        }
        if (bulk == nullptr) {
          scorer = buildPullScorer(pool, *supplier);
          if (scorer == nullptr) return std::nullopt;
        }
        int64_t matches = 0;
        for (int32_t slice = 0; slice < TopDocsReq::kEstimateSlices; slice++) {
          auto [begin, end] = rangeOf(slice);
          matches += bulk != nullptr
              ? countMatchesInRange(bulk, collectorFilter, begin, end)
              : countMatchesInRange(scorer, collectorFilter, begin, end);
        }
        return matches;
      };

      int64_t budget = std::max(
          (int64_t) maxDoc >> TopDocsReq::kEstimateBudgetShift,
          TopDocsReq::kEstimateMinBudgetDocs);
      if (ceiling <= budget) {
        auto matches = countRanges([&](int32_t slice) {
          return std::pair(
              (int32_t) ((int64_t) maxDoc * slice / TopDocsReq::kEstimateSlices),
              (int32_t) ((int64_t) maxDoc * (slice + 1) / TopDocsReq::kEstimateSlices));
        });
        collector.estimatedHits += std::max(matches.value_or(0), seen);
        return;
      }

      // Each round counts only what the last one did not, so the walks add up
      // to the final round's coverage.
      auto sampleEnd = [&](int32_t slice, int32_t shift) {
        int64_t begin = (int64_t) maxDoc * slice / TopDocsReq::kEstimateSlices;
        int64_t end = (int64_t) maxDoc * (slice + 1) / TopDocsReq::kEstimateSlices;
        return (int32_t) (begin + std::max<int64_t>(1, (end - begin) >> shift));
      };
      int64_t matches = 0;
      int64_t sampled = 0;
      int32_t lastShift = -1;
      for (int32_t shift : TopDocsReq::kEstimateSampleShifts) {
        auto counted = countRanges([&](int32_t slice) {
          int32_t begin = lastShift < 0
              ? (int32_t) ((int64_t) maxDoc * slice / TopDocsReq::kEstimateSlices)
              : sampleEnd(slice, lastShift);
          int32_t end = sampleEnd(slice, shift);
          sampled += end - begin;
          return std::pair(begin, end);
        });
        if (!counted) break;
        matches += *counted;
        lastShift = shift;
        if (matches >= TopDocsReq::kEstimateMinMatches) {
          int64_t scaled = std::llround(
              (double) matches * (double) maxDoc / (double) sampled);
          collector.estimatedHits += std::clamp(scaled, floor, ceiling);
          collector.countEstimated = true;
          return;
        }
      }
      collector.estimatedHits += floor;
      collector.countFloored = true;
    }

    void collectKnownCountTopK(
        MemPool& pool, int32_t segnum, int64_t count,
        Query::ScorerSupplier* rankingSupplier, DocSet* collectorFilter,
        auto& collector,
        int32_t maxDoc, bool allowPruning,
        BulkScorer* exactScorer = nullptr) {
      int64_t before = collector.totalHits();
      rankTopK(pool, segnum, rankingSupplier, collectorFilter, collector,
               maxDoc, allowPruning,
               checkedRankingBound(allowPruning ? &scoreAccumulator : nullptr),
               exactScorer);
      int64_t ranked = collector.totalHits() - before;
      assert(count >= ranked);
      collector.hitCount += count - ranked;
    }

    // This segment's matches added to the request's bounded total with the
    // unscored weight, a docid range at a time. Returns the segment's exact
    // count, or nullopt once the total passes the limit here or in another
    // segment.
    std::optional<int64_t> countBounded(
        IndexReader::Segment& seg, DocSet* collectorFilter, BulkScorer& bulk) {
      int32_t rangeDocs = TopDocsReq::boundedCountRangeDocsForTests;
      skipCount(SkipStats::boundedCountSegments);
      int64_t count = 0;
      int32_t maxDoc = seg.maxDoc();
      for (int32_t min = 0; min < maxDoc;) {
        int32_t max = (int32_t) std::min<int64_t>(maxDoc, (int64_t) min + rangeDocs);
        int64_t matches = 0;
        int32_t next = bulk.countNextWindow(matches, nullptr, collectorFilter, min, max);
        if (next == PostingsReader::END) next = max;
        assert(next > min);
        count += matches;
        if (boundedTotal.add(matches)) {
          if (next < maxDoc) skipCount(SkipStats::boundedCountStops);
          return std::nullopt;
        }
        min = next;
      }
      return count;
    }

    // A bounded count apart from ranking: the count pass stops at the limit,
    // and ranking prunes from its first page. The total is exact unless it
    // passed the limit, which the emission reports as at least the limit.
    bool boundedCountThenCollectTopK(
        MemPool& pool, int32_t segnum, IndexReader::Segment& seg,
        Query::ScorerSupplier* rankingSupplier, DocSet* collectorFilter,
        auto& collector) {
      std::optional<int64_t> count;
      if (!boundedTotal.exceeded()) {
        auto* countSupplier = thisOp().boundedCountWeight->scorerSupplier(pool, seg);
        if (countSupplier == nullptr) return true;
        Query::ScorerSupplier::BulkScorerContext context;
        auto plan = countSupplier->planBulk(
            Query::ScorerSupplier::BulkUse::COUNT_WINDOWS, context);
        // Candidate-by-candidate verification would be repeated by ranking.
        // Keep the held pass unless counting has a bulk membership plan.
        if (plan.available != Query::ScorerSupplier::BulkAnswer::YES) return false;
        auto* bulk = countSupplier->buildBulk(pool, plan);
        if (bulk == nullptr) return false;
        count = countBounded(seg, collectorFilter, *bulk);
      }
      int64_t before = collector.totalHits();
      if (collector.topCount > 0 && (!count || *count > 0)) {
        rankTopK(pool, segnum, rankingSupplier, collectorFilter, collector,
                 seg.maxDoc(), true, checkedRankingBound(&rankingAccumulator));
      }
      if (!count) {
        collector.countIncomplete = true;
        return true;
      }
      int64_t ranked = collector.totalHits() - before;
      assert(*count >= ranked);
      collector.hitCount += *count - ranked;
      return true;
    }

    void rankTopK(
        MemPool& pool, int32_t segnum,
        Query::ScorerSupplier* rankingSupplier, DocSet* collectorFilter,
        auto& collector, int32_t maxDoc, bool allowPruning,
        MaxScoreAccumulator* accumulator, BulkScorer* exactScorer = nullptr) {
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
              accumulator, maxDoc, allowPruning, exactScorer);
        } else {
          auto* rankingScorer = buildPullScorer(pool, *rankingSupplier);
          if (rankingScorer != nullptr) {
            collectTopK(
                segnum, rankingScorer, collectorFilter, nullptr,
                collector, allowPruning, accumulator);
          }
        }
      }
    }

    void countThenCollectTopK(
        MemPool& pool, int32_t segnum, BulkScorer* countBulk,
        Query::ScorerSupplier* rankingSupplier, DocSet* collectorFilter,
        DocSetBuilder* builder, auto& collector,
        int32_t maxDoc, bool allowPruning,
        BulkScorer* exactScorer = nullptr) {
      int64_t count = countMatchesWindowed(
          countBulk, collectorFilter, builder, maxDoc);
      collectKnownCountTopK(
          pool, segnum, count, rankingSupplier, collectorFilter,
          collector, maxDoc, allowPruning, exactScorer);
    }

    // A single scorer ranks canonically by itself: count with the unscored
    // weight (windowed when it can, else pulled), then rank with pruning.
    void countThenRankWithoutRescoring(
        MemPool& pool, int32_t segnum,
        Query::ScorerSupplier& countSupplier, DocSet* collectorFilter,
        auto& collector, int32_t maxDoc) {
      int64_t count = QueryPrep::countSegment(
          pool, &countSupplier, collectorFilter, maxDoc);
      auto* rankingSupplier = thisOp().rankingWeight->scorerSupplier(
          pool, thisOp().planning.topReader.segments()[segnum]);
      collectKnownCountTopK(
          pool, segnum, count, rankingSupplier, collectorFilter,
          collector, maxDoc, true);
    }

    bool composeVariableScoreExactCountTopK(
        MemPool& pool, int32_t segnum,
        Query::ScorerSupplier& scoringSupplier,
        auto&& countSupplierFor, DocSet* collectorFilter,
        auto& collector, int32_t maxDoc, bool nativeScores) {
      Query::ScorerSupplier::BulkScorerContext bulkContext;
      bulkContext.requireExactRankingScores = nativeScores;
      auto scoringPlan = scoringSupplier.planBulk(
          Query::ScorerSupplier::BulkUse::EXACT_CANDIDATE_SCORING,
          bulkContext);
      if (bulkContext.requireExactRankingScores
          && scoringPlan.rankingScoresExact != Query::ScorerSupplier::BulkAnswer::YES) {
        skipCount(thisOp().forcedExactCountTopKComposition
            ? SkipStats::forcedExactCountTopKCertificateRejects
            : SkipStats::provedDensityCompositionCertificateRejects);
        return false;
      }
      auto* countSource = countSupplierFor();
      if (countSource == nullptr) return false;
      auto& countSupplier = *countSource;
      bool scoringDeclined =
          scoringPlan.available
              != Query::ScorerSupplier::BulkAnswer::YES
          || scoringPlan.supportsExactCandidateScoring
              != Query::ScorerSupplier::BulkAnswer::YES;
      if (scoringDeclined && scoringPlan.rankingScoresExact
              == Query::ScorerSupplier::BulkAnswer::YES) {
        scoringSupplier.recordBulkPlanCommitment(
            Query::ScorerSupplier::BulkUse::EXACT_CANDIDATE_SCORING,
            bulkContext, scoringPlan);
        countThenRankWithoutRescoring(
            pool, segnum, countSupplier, collectorFilter, collector, maxDoc);
        return true;
      }
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
        auto& collector, int32_t maxDoc) {
      auto constant = QueryPrep::supplierCount(&countSupplier);
      int64_t count = collectorFilter == nullptr ? constant.value_or(-1) : -1;

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
            collector.topCount, checkedRankingBound(nullptr));
      }
      assert(count >= captured);
      collector.hitCount += count - captured;
      return true;
    }

    template <bool Structural>
    DomainHandle materializeFilter(
        size_t filterIndex, size_t sourceOffset,
        IndexReader::Segment& seg, DocSet* baseDomain,
        const std::vector<QueryPrep::PreparedSource>* prepared,
        const std::shared_ptr<DomainVariantPlan::Reservation>& reservation,
        const DomainHandle* owner = nullptr) {
      auto& op = thisOp();
      const auto& filter = op.filters[filterIndex];
      std::vector<DomainHandle> sources;
      std::vector<DocSet*> sourcePtrs;
      sources.reserve(filter.sourceCount());
      sourcePtrs.reserve(filter.sourceCount());
      for (size_t i = 0; i < filter.sourceCount(); i++) {
        size_t source = sourceOffset + i;
        DomainHandle docs;
        if constexpr (Structural) {
          DomainVariantPlan::SegmentSources sources{*op.view, op.planning, seg, op.req.memoryTracker};
          assert(owner && owner->get() == baseDomain);
          docs = sources.resolve(*op.filterWeights[source], op.filterUses[source], *owner,
              prepared ? &(*prepared)[source] : nullptr);
        } else {
          docs = prepared != nullptr
              ? QueryPrep::materializeEffectiveFilter(
                    (*prepared)[source], *op.view, seg, baseDomain)
              : QueryPrep::materializeEffectiveFilter(
                    *op.filterWeights[source], op.filterUses[source], *op.view, seg, baseDomain);
        }
        if constexpr (!Structural) if (reservation != nullptr && docs.get() != nullptr
            && docs.isDeliverable()) {
          reservation->grow(docs.get()->ramBytesUsed());
        }
        if (docs.get() == nullptr && filter.composesUnion()) {
          return DomainHandle().scoped(seg);
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
      DomainHandle result = DomainHandle(DocSet::union_(sourcePtrs)).scoped(seg);
      if constexpr (Structural) {
        return DomainVariantPlan::account(std::move(result), op.req.memoryTracker, "selection union");
      } else {
        if (reservation != nullptr && result.get() != nullptr) {
          reservation->grow(result.get()->ramBytesUsed());
        }
        return result;
      }
    }

    template <bool Structural>
    DomainHandle buildEffectiveDomain(
        int32_t segnum, DocSet* baseDomain) {
      auto& op = thisOp();
      if (op.filterWeights.empty()) return DomainHandle().scoped(op.view->segments()[segnum]);
      auto& seg = op.view->segments()[segnum];

      std::vector<DomainHandle> filters;
      std::vector<DocSet*> filterPtrs;
      filters.reserve(op.filters.size());
      filterPtrs.reserve(op.filters.size());
      size_t source = 0;
      for (size_t i = 0; i < op.filters.size(); i++) {
        filters.push_back(materializeFilter<Structural>(
            i, source, seg, baseDomain, &preparedFilterSources, nullptr,
            Structural ? &inputDomains[(size_t)segnum] : nullptr));
        source += op.filters[i].sourceCount();
        if (filters.back().get() != nullptr) {
          filterPtrs.push_back(filters.back().get());
        }
      }
      assert(source == op.filterWeights.size());
      if (filterPtrs.empty()) return DomainHandle().scoped(seg);
      if (filterPtrs.size() == 1) {
        auto result = std::move(filters[0]);
        return std::move(result).pinnedWith(op.planning.filterUses);
      }
      return DomainHandle(DocSet::intersect(filterPtrs)).scoped(seg);
    }

    void configureDiversify();

    template <bool MetricGroups = false>
    void completeEmpty(FanoutScope* tg) {
      std::span<const DomainHandle> noDomains;
      for (auto& subCalc : subCalcs) {
        subCalc->acceptAll(tg, noDomains);
      }
      doneCollecting<MetricGroups>();
    }

    template <bool Structural>
    void prepareMainVariants(FanoutScope* tg);

    template <bool Structural>
    void prepareDomainQueryVariants(
        std::span<DocSet* const> liveDomains,
        FanoutScope* tg);

    template <bool Structural, bool MetricGroups = false, typename Admission = CollapseAdmission>
    void prepareAndDispatch(FanoutScope* tg);

    template <bool Structural>
    void prepareDomains(FanoutScope* tg);

    uint64_t fanoutStealUnits(int32_t segnum, uint64_t docs) override {
      uint64_t steal = 0;
      for (auto& calc : subCalcs) steal = taskWorkAdd(steal, calc->fanoutStealUnits(segnum, docs));
      return steal;
    }

    template <bool Structural, bool MetricGroups = false, typename Admission = CollapseAdmission>
    void dispatch(
        FanoutScope* tg,
        std::span<const DomainHandle> domains) {
      assert(domains.size() == thisOp().view->segments().size());
      // Ranked lists keep their visit order on the caller.
      auto& op = thisOp();
      std::vector<int32_t> order;
      if (domains.size() > 1 && subCalcs.empty()
          && op.sortPlan.useFieldSort && op.topCount > 0
          && !disableFieldSortPruning) {
        order = fieldSortVisitOrder(
            fieldSortSegmentKeys(*op.view, op.sortPlan, op.sortValues),
            op.topCount, fieldSortMissingFirst(op.sortPlan, op.sortValues));
      }
      size_t n = domains.size();
      bool ranked = subCalcs.empty() && op.requirements.needRankedDocs && op.topCount > 0;
      FanoutKind kind = !ranked ? FanoutKind::EXHAUSTIVE
          : !MetricGroups && rankingSharesBound() ? FanoutKind::PRUNED : FanoutKind::SERIAL;
#ifndef NDEBUG
      prunedFanout = kind == FanoutKind::PRUNED;
#endif
      bool back = fanoutCallerStartsAtBack(tg);
      auto segmentAt = [&](size_t i) {
        size_t visit = ranked && back ? n - 1 - i : i;
        return order.empty() ? (int32_t)visit : order[visit];
      };
      task_group_fanout(tg, n, kind, [&](size_t i) {
        int32_t segnum = segmentAt(i);
        uint64_t units = fanoutDomainUnits(op.view->segments()[(size_t)segnum], domains[(size_t)segnum]);
        return FanoutSize{units, fanoutStealUnits(segnum, units)};
      }, [&](size_t i) {
        int32_t segnum = segmentAt(i);
        return [this, tg, segnum, domain = domains[(size_t)segnum]]() {
          assert(domain.isDeliverable());
          domain.assertScope(thisOp().view->segments()[(size_t)segnum]);
          doCollect<Structural, MetricGroups, Admission>(tg, segnum, domain);
        };
      });
    }

    template <bool Structural>
    std::vector<DomainHandle> materializeRoutedFilters(
        int32_t segnum, DocSet* baseDomain,
        const std::shared_ptr<DomainVariantPlan::Reservation>& reservation,
        const DomainHandle* owner = nullptr) {
      auto& op = thisOp();
      auto& seg = op.view->segments()[(size_t) segnum];
      std::vector<DomainHandle> filters;
      filters.reserve(op.filters.size());
      size_t source = 0;
      for (size_t i = 0; i < op.filters.size(); i++) {
        DomainHandle filter = materializeFilter<Structural>(
            i, source, seg, baseDomain,
            requiresWholeIndexPrepare ? &preparedFilterSources : nullptr,
            reservation, owner);
        source += op.filters[i].sourceCount();
        filters.push_back(
            std::move(filter).pinnedWith(op.planning.filterUses));
        assert(filters.back().isDeliverable());
      }
      assert(source == op.filterWeights.size());
      return filters;
    }

    template <bool Structural>
    std::vector<DomainHandle> materializeResetParentFilters(
        int32_t segnum,
        const std::shared_ptr<DomainVariantPlan::Reservation>& reservation) {
      auto& op = thisOp();
      auto& seg = op.view->segments()[(size_t) segnum];
      DomainHandle base;
      DocSet* live;
      if constexpr (Structural) {
        base = resetBaseHandle(segnum);
        live = base.get();
      } else live = resetBase<false>(segnum);
      std::vector<DomainHandle> filters;
      filters.reserve(op.filters.size());
      size_t source = 0;
      for (size_t i = 0; i < op.filters.size(); i++) {
        filters.push_back(std::move(materializeFilter<Structural>(
            i, source, seg, live,
            requiresWholeIndexPrepare ? &preparedResetFilterSources : nullptr,
            reservation, &base)).pinnedWith(op.planning.filterUses));
        source += op.filters[i].sourceCount();
      }
      assert(source == op.filterWeights.size());
      return filters;
    }

    template <bool Structural>
    DomainHandle materializeDomainSource(
        size_t source, int32_t segnum, DocSet* base,
        const std::shared_ptr<DomainVariantPlan::Reservation>& reservation,
        const DomainHandle* owner = nullptr) {
      auto& op = thisOp();
      auto& seg = op.view->segments()[(size_t) segnum];
      DocSet* live = resetBase<Structural>(segnum);
      if (!preparedDomainExtraSets.empty()
          && op.domainVariants.sourceIsResetExtra(source)
          && DomainVariantPlan::sameEffectiveDomain(
              base, live, seg.maxDoc())) {
        const auto& set = preparedDomainExtraSets[(size_t) segnum][source];
        if constexpr (Structural) if (!set.get() && base) {
          // An unrestricted extra is the scoped base, even when it happens
          // to contain every live document. Preserve that base's owner.
          assert(owner && owner->get() == base);
          return *owner;
        }
        return set;
      }
      if constexpr (Structural) {
        DomainVariantPlan::SegmentSources sources{*op.view, op.planning, seg, op.req.memoryTracker};
        assert(owner && owner->get() == base);
        return sources.resolve(*op.domainVariants.sourceWeight(source), op.domainVariants.segmentSourceUse(source),
            *owner,
            preparedDomainSources.empty() ? nullptr : &preparedDomainSources[source]);
      }
      DomainHandle result = preparedDomainSources.empty()
          ? QueryPrep::materializeEffectiveFilter(
                *op.domainVariants.sourceWeight(source),
                op.domainVariants.sourceUse(source),
                *op.view, seg, base)
          : QueryPrep::materializeEffectiveFilter(
                preparedDomainSources[source], *op.view, seg, base);
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

    template <bool Structural>
    void finishResetDomains(RoutedProduction& result, int32_t segnum) {
      auto& op = thisOp();
      if (!op.domainVariants.hasResetVariants()) return;
      if constexpr (Structural) {
        DomainHandle base = resetBaseHandle(segnum);
        std::vector<DomainHandle> bases;
        if (!result.resetFilters.empty()) {
          bases.resize(op.domainVariants.variantCount());
          for (size_t variant = 1; variant < bases.size(); variant++) {
            if (op.domainVariants.frame(variant) != DomainVariantPlan::Frame::RESET) continue;
            std::vector<DomainHandle> parts{base};
            for (size_t filter = 0; filter < result.resetFilters.size(); filter++) {
              if (op.domainVariants.includesFilter(variant, filter)) {
                parts.push_back(result.resetFilters[filter]);
              }
            }
            bases[variant] = DomainVariantPlan::composeTracked(std::move(parts), op.req.memoryTracker);
          }
        }
        std::vector<DomainHandle> domains;
        if (op.domainVariants.segmentLocal(structural->hasSelection())) {
          DomainVariantPlan::SegmentSources sources{
              *op.view, op.planning, op.view->segments()[(size_t)segnum], op.req.memoryTracker};
          domains = op.domainVariants.produceSegment(sources, {}, base, bases, DomainVariantPlan::Frame::RESET);
        } else {
          auto resolve = [&](size_t source, DomainHandle eligibility, DomainVariantPlan::Frame) {
            return materializeDomainSource<Structural>(
                source, segnum, eligibility.get(), result.reservation, &eligibility);
          };
          domains = op.domainVariants.produceSegment({}, base, resolve, op.req.memoryTracker,
              preparedDomainVariants.domains[(size_t)segnum], preparedDomainVariants.ready,
              bases, DomainVariantPlan::Frame::RESET);
        }
        for (size_t variant = 1; variant < domains.size(); variant++) {
          if (op.domainVariants.frame(variant) == DomainVariantPlan::Frame::RESET) {
            result.domains[variant] = std::move(domains[variant]);
          }
        }
        return;
      }
      DocSet* live = resetBase<Structural>(segnum);
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
        parts.push_back(materializeDomainSource<Structural>(
            op.domainVariants.resetSource(variant), segnum, live,
            result.reservation));
        for (size_t filter = 0; filter < result.resetFilters.size(); filter++) {
          if (op.domainVariants.includesFilter(variant, filter)) {
            parts.push_back(result.resetFilters[filter]);
          }
        }
        for (size_t source : op.domainVariants.extraSources(variant)) {
          parts.push_back(materializeDomainSource<Structural>(
              source, segnum, live, result.reservation));
        }
        result.domains[variant] = DomainVariantPlan::compose(
            std::move(parts));
        result.domains[variant] =
            std::move(result.domains[variant]).retainedWith(
                result.reservation);
      }
    }

    template <bool Structural>
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
          parts.push_back(materializeDomainSource<Structural>(
              source, segnum, base.get(), result.reservation, &base));
        }
        if constexpr (Structural) result.domains[variant] = DomainVariantPlan::composeTracked(
            std::move(parts), op.req.memoryTracker);
        else result.domains[variant] = DomainVariantPlan::compose(std::move(parts));
        result.domains[variant] =
            std::move(result.domains[variant]).retainedWith(
                result.reservation);
      }
      result.inheritReady = true;
    }

    template <bool Structural>
    std::shared_ptr<DomainVariantPlan::Reservation> reserveProduction(
        int32_t maxDoc, std::string detail) {
      if constexpr (Structural) return std::make_shared<DomainVariantPlan::Reservation>(
          op.req.memoryTracker, 0, std::move(detail));
      return thisOp().domainVariants.reserveProduction(op.req.memoryTracker, maxDoc, std::move(detail));
    }

    template <bool Structural>
    std::shared_ptr<DomainVariantPlan::Reservation> reserveVariantPreparation(
        int32_t maxDoc, std::string detail) {
      if constexpr (Structural) return std::make_shared<DomainVariantPlan::Reservation>(
          op.req.memoryTracker, 0, std::move(detail));
      return thisOp().domainVariants.reserveVariantPreparation(
          op.req.memoryTracker, maxDoc, std::move(detail));
    }

    template <bool Structural>
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
        reservation = reserveProduction<Structural>(seg.maxDoc(), detail);
        op.planning.filterUses->enableRoutedAccounting(
            (size_t) segnum, op.req.memoryTracker, detail);
        result.filters = materializeRoutedFilters<Structural>(
            segnum, baseDomain.get(), reservation, &baseDomain);
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
          result.filters = materializeRoutedFilters<Structural>(
              segnum, baseDomain.get(), reservation, &baseDomain);
        }
      }
      result.defaultEligibility = op.domainVariants.defaultDomain<Structural>(
          baseDomain, result.filters,
              Structural ? &op.req.memoryTracker : nullptr);
      assert(result.defaultEligibility.isDeliverable());
      result.reservation = reservation;
      result.domains.resize(op.domainVariants.variantCount());
      if (op.domainVariants.needsResetParentFilters()) {
        if (preparedResetRoutedFilters.empty()) {
          result.resetFilters = materializeResetParentFilters<Structural>(
              segnum, reservation);
        } else {
          result.resetFilters = std::move(
              preparedResetRoutedFilters[(size_t) segnum]);
        }
      }
      finishResetDomains<Structural>(result, segnum);

      if (!op.domainVariants.needsInheritProduction()) {
        return result;
      }

      if (!preparedVariantInheritBases.empty()) {
        result.inheritBases = std::move(
            preparedVariantInheritBases[(size_t) segnum]);
        finishInheritDomains<Structural>(result, segnum);
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
      [[maybe_unused]] bool admitted = false;
      if constexpr (Structural) {
        // Costs still choose the route; only produced storage is charged.
        selected = selection.offers[0];
        admitted = true;
      }
      if constexpr (!Structural) for (size_t i = 0; i < selection.count; i++) {
        if (reservation->tryGrow(selection.offers[i].peakBytes)) {
          selected = selection.offers[i];
          admitted = true;
          break;
        }
      }
      if constexpr (!Structural) if (!admitted) {
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
          result.inheritBases = op.domainVariants.produceSharedBases<Structural>(
              baseDomain, result.filters,
              Structural ? &op.req.memoryTracker : nullptr);
          break;
        case DomainVariantPlan::Offer::SHARED_M: {
          if (implicitMatches) {
            result.inheritBases = op.domainVariants.produceSharedBases<Structural>(
                baseDomain, result.filters,
              Structural ? &op.req.memoryTracker : nullptr);
          } else if (reusableRaw != nullptr) {
            DomainHandle rawMatches = op.domainVariants.constrainRaw<Structural>(
                DomainHandle::pinned(
                    reusableRaw, op.planning.filterUses).scoped(seg),
                baseDomain,
              Structural ? &op.req.memoryTracker : nullptr);
            result.inheritBases = op.domainVariants.produceSharedBases<Structural>(
                std::move(rawMatches), result.filters,
              Structural ? &op.req.memoryTracker : nullptr);
          } else {
            // Capture M in the existing ranking walk. Bulk paths fall back to
            // scorer-driven collection because their in-window filtering does
            // not expose pre-filter match words yet.
            result.captureSharedRaw = true;
          }
          break;
        }
        case DomainVariantPlan::Offer::INDEPENDENT:
          result.inheritBases = op.domainVariants.produceIndependentBases<Structural>(
              baseDomain, result.filters, [&](DocSet* eligibility) {
                return DomainHandle(QueryPrep::materialize(
                    source, seg, eligibility,
                    Query::SupplierExecutionMode::ORDINARY,
                    QueryPrep::MaterializeMode::DOMAIN_DRIVEN));
              }, Structural ? &op.req.memoryTracker : nullptr);
          break;
      }
      if (!result.captureSharedRaw) {
        finishInheritDomains<Structural>(result, segnum);
      }
      return result;
    }

    void releaseSegmentState(IndexReader::Segment& segment) noexcept override {
      auto& op = thisOp();
      if (op.segmentStateRetained(segment)) return;
      if (preparedWeight) preparedWeight->releaseSegmentState(segment);
      for (const auto* sources : {&preparedFilterSources, &preparedResetFilterSources,
                                  &preparedDomainSources}) {
        for (const auto& source : *sources) source.releaseSegmentState(segment);
      }
      op.releaseSegmentState(segment);
    }

    template <bool Structural, bool MetricGroups = false, typename Admission = CollapseAdmission>
    void doCollect(FanoutScope* tg, int32_t segnum,
                   DomainHandle domainHandle) {
      if constexpr (Admission::isDiversified) {
        doCollectPolicy<Structural, true, false, Admission>(tg, segnum, std::move(domainHandle));
      } else if constexpr (MetricGroups) {
        doCollectPolicy<Structural, true, true>(tg, segnum, std::move(domainHandle));
      } else if (thisOp().collapse != nullptr) {
        doCollectPolicy<Structural, true>(tg, segnum, std::move(domainHandle));
      }
      else doCollectPolicy<Structural, false>(tg, segnum, std::move(domainHandle));
    }

    // Every ranking route, including count compositions and their fallbacks,
    // must share the bound before ranked segments may fan out with pruning.
    bool rankingSharesBound() const {
      const auto& op = static_cast<const TopDocsReq&>(this->op);
      return !op.sortPlan.useFieldSort
          && !op.requirements.needExactDomain && op.domainVariants.empty()
          && op.weight && !op.weight->isConstantScoring() && op.weight->allowsPruning()
          && (!op.rankingWeight || op.rankingWeight->allowsPruning())
          && (!op.wholeRankingWeight || op.wholeRankingWeight->allowsPruning())
          && (!op.collapse || (!op.requirements.needsCount() && !disableCollapsePruningForTests));
    }

    MaxScoreAccumulator* checkedRankingBound(MaxScoreAccumulator* accumulator) {
      // Check the actual argument at every ranking route. Segments with no
      // scorer do no ranking and have no bound to share.
      assert(!prunedFanout || accumulator == &scoreAccumulator || accumulator == &rankingAccumulator);
      return accumulator;
    }

    bool canRecordSegmentCount(IndexReader::Segment& seg, DocSet* domain) {
      auto& op = thisOp();
      return !op.sortPlan.useFieldSort && op.requirements.countLimit < 0 && op.filterWeights.empty()
          && !requiresWholeIndexPrepare && op.wholeMembershipPlan.canRecordCount(seg, domain);
    }

    template <bool Collapsed, bool MetricGroups>
    auto* skipSegment(IndexReader::Segment& seg, DomainHandle& domain, std::vector<std::string> details) {
      auto& op = thisOp();
      auto release = scope_guard([&]() noexcept { releaseSegmentState(seg); });
      size_t ord = (size_t)seg.ord;
      // Drop the inputs that prepareRoutedProduction would consume for this segment.
      if (!preparedRoutedReservations.empty()) {
        for (auto* inputs : {&preparedRoutedFilters, &preparedResetRoutedFilters, &preparedVariantInheritBases}) {
          if (!inputs->empty()) (*inputs)[ord] = std::vector<DomainHandle>{};
        }
        preparedRoutedReservations[ord].reset();
      }

      // Only a proof for the key can publish raw empty membership.
      if (op.weight && op.weight->provenEmpty(seg) && !op.wholeMembershipPlan.empty()) {
        FilterCache::UseRegistry::SegmentScope cacheTiming(*op.planning.filterUses, ord);
        auto poolGuard = MemPool::threadLocalPoolGuard();
        op.wholeMembershipPlan.resolve(*op.view, seg, domain.get(), preparedWeight.get(),
            [&](Query::Weight& resolver) { return resolver.scorerSupplier(poolGuard.pool(), seg); });
        if (!Collapsed && canRecordSegmentCount(seg, domain.get())) op.wholeMembershipPlan.recordCount(seg, 0);
      }
      domain = DomainHandle::empty().scoped(seg);
      if (!output.empty()) output[ord] = domain;
      if (memberReplay) memberReplay->domains[ord] = domain;
      Signal::emit("searchTopDocsSegmentPruned", this);
      if (op.req.proto.profile) op.req.recordPrunedSegment(pruningRun, seg, std::move(details));
      if constexpr (MetricGroups) return metricGroups->merger.obtain();
      else return collectorMerger.obtain();
    }

    // One collection ladder, specialized once per segment. Document requests
    // instantiate their original admission loop without per-hit group checks.
    template <bool Structural, bool Collapsed, bool MetricGroups = false,
              typename Admission = CollapseAdmission>
    void doCollectPolicy(
        FanoutScope* tg, int32_t segnum,
        DomainHandle domainHandle) {
      auto& op = thisOp();
      assert(domainHandle.isDeliverable());
      DocSet* domain = domainHandle.get(op.view->segments()[segnum]);

      // A constant-scoring match-all is the identity on its domain: every doc
      // in the domain matches, all with the same score.  So the domain IS the
      // result set - its cardinality is the exact hit count, doc order is the
      // ranking, and sub-ops inherit it unchanged.  None of that needs a
      // scorer, so the whole collection ladder below is skipped.  (Scores must
      // be constant for doc order to be the ranking; a rescore over a
      // match-all matches everything but reorders it.)
      // Constant-score first-K and cardinality shortcuts rank documents.
      // Groups require key reads; the bounded pull below stops at L groups,
      // while this identity arm also supplies document-cardinality totals.
      bool matchEverything = !Collapsed && op.weight != nullptr && op.weight->matchesAllDocs()
        && op.weight->isConstantScoring() && op.filterWeights.empty();
      std::unique_ptr<std::conditional_t<MetricGroups, MetricGroupData, MergeableCollector>> data;
      std::optional<RoutedProduction> routedProduction;
      DocSet* routedBaseDomain = nullptr;
      int64_t numSegs = (int64_t)op.view->segments().size();

      auto& seg = op.planning.topReader.segments()[segnum];
      std::vector<std::string> details;
      bool skipped = op.outputsEmpty(seg, domain, op.req.proto.profile ? &details : nullptr);
      if (skipped) {
        data.reset(skipSegment<Collapsed, MetricGroups>(seg, domainHandle, std::move(details)));
      } else {
        auto release = scope_guard([&]() noexcept { releaseSegmentState(seg); });
        auto poolGuard = MemPool::threadLocalPoolGuard();
        if (!op.domainVariants.empty()) {
          routedBaseDomain = domain;
          routedProduction.emplace(prepareRoutedProduction<Structural>(
              poolGuard.pool(), segnum, domainHandle));
          domainHandle = routedProduction->defaultEligibility;
          domain = domainHandle.get();
        }

        if constexpr (Collapsed) {
          if (memberReplay) {
            // Retain precisely the eligibility used by collection. Prepared
            // sources (notably kNN) outlive this pass and are replayed as-is.
            if (!requiresWholeIndexPrepare && op.domainVariants.empty()
                && !op.filterWeights.empty()) {
              std::vector<DomainHandle> parts{domainHandle};
              for (size_t i = 0; i < op.filterWeights.size(); i++) {
                parts.push_back(QueryPrep::materializeEffectiveFilter(
                    *op.filterWeights[i], op.filterUses[i], *op.view, seg, nullptr));
              }
              domainHandle = memberReplay->compose(std::move(parts),
                  op.domainVariants.estimatedSetBytes(seg.maxDoc()));
              domain = domainHandle.get();
            }
            memberReplay->domains[(size_t)segnum] = domainHandle;
          }
        }

        std::optional<DocSetBuilder> builder;
        bool captureSharedRaw = routedProduction.has_value()
            && routedProduction->captureSharedRaw;
        if constexpr (MetricGroups) {
          data.reset(metricGroups->merger.obtain());
          if (captureSharedRaw || !output.empty()) builder.emplace(seg.maxDoc());
          std::vector<DomainHandle> parts{domainHandle};
          if (!requiresWholeIndexPrepare && op.domainVariants.empty() && !memberReplay) {
            for (size_t i = 0; i < op.filterWeights.size(); i++) {
              parts.push_back(QueryPrep::materializeEffectiveFilter(
                  *op.filterWeights[i], op.filterUses[i], *op.view, seg, nullptr));
            }
          }
          auto eligibility = DomainVariantPlan::compose(std::move(parts));
          auto* supplier = mainScorerSupplier(poolGuard.pool(), seg);
          auto* scorer = supplier ? buildPullScorer(poolGuard.pool(), *supplier) : nullptr;
          if (scorer) data->collect(segnum, scorer, eligibility.get(),
              builder ? &*builder : nullptr,
              captureSharedRaw ? DomainBuildMode::RAW_QUERY_MATCHES : DomainBuildMode::FILTERED_MATCHES,
              routedBaseDomain);
        } else {
          // Wait until last moment to obtain collector in hopes of reusing an existing one.
          // Keep ownership until release so a scoring error cannot orphan it.
          data.reset(collectorMerger.obtain());
          auto* scoreCollector = data->template scores<Collapsed, Admission>();
          auto* fieldCollector = data->template fields<Collapsed, Admission>();
          using ActiveFieldCollector = FieldCollector<Collapsed, Admission>;

          bool recordCount = !Collapsed && canRecordSegmentCount(seg, domain);

          // A segment ranking never pruned has its exact count already. Track
          // pruning per segment, restoring the collector's own flag afterward.
          int64_t hitsBeforeSegment = recordCount ? scoreCollector->totalHits() : 0;
          bool prunedBeforeSegment = false;
          DocSet* estimateFilter = nullptr;
          bool estimatorReady = false;
          if constexpr (!Collapsed) {
            if (op.estimateCount) {
              hitsBeforeSegment = scoreCollector->totalHits();
              prunedBeforeSegment = scoreCollector->countIncomplete;
              scoreCollector->countIncomplete = false;
            }
          }

          // Field-sorted ranking is the one thing a match-all still has to
          // iterate for: it orders by column values the domain says nothing
          // about.  A count-only field sort (limit 0) reads nothing back, so it
          // takes the domain answer like the score-ranked case.
          bool rankFromDocOrder = !data->useFieldSort;
          Query::ScorerSupplier* supplier = nullptr;
          DocSet* borrowedDomain = nullptr;
          bool exactDomain = false;
          DocSet* exactDomainDocs = nullptr;
          FilterCache::UseRegistry::SegmentScope cacheTiming(*op.planning.filterUses, (size_t)segnum);
          if (!requiresWholeIndexPrepare && op.requirements.needExactDomain
              && !op.exactDomainPlan.empty()) {
            auto result = op.exactDomainPlan.produce(
                poolGuard.pool(), *op.view, seg, domain);
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
          bool wholeFallbackSupplierPending = false;
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
          QueryPrep::WholeMembershipResult wholeMembershipResult;
          // Match-all identity needs no membership fact, but field ranking
          // still needs the plan's constant count to prune with exact totals.
          if (!exactDomain
              && !(matchEverything && (rankFromDocOrder || data->topCount() == 0))
              && !op.wholeMembershipPlan.empty()) {
            wholeMembershipResult = op.wholeMembershipPlan.resolve(
                *op.view, seg, domain, preparedWeight.get(),
                [&](Query::Weight& resolver) {
                  return &resolver == op.weight ? obtainMainSupplier()
                      : resolver.scorerSupplier(poolGuard.pool(), seg);
                }, op.requirements.countLimit < 0
                        && op.topCount <= kExactCountTopKMaxDepth
                        && op.weight != nullptr && !op.weight->needsPrepare()
                        && !op.weight->consumesDomain() && !op.weight->isConstantScoring()
                        && !op.weight->canComposeExactCountTopK()
                        && op.wholeRankingWeight != nullptr
                        && op.wholeRankingWeight->allowsPruning()
                    ? exactCountTopKMinCandidateDensityInverseForTests : 0,
                localWholeMembershipMaxCandidatesPerMatchForTests);
          }
          bool wholeMembershipAvailable = wholeMembershipResult.available;
          bool wholeTopKCountAvailable = wholeMembershipAvailable
              && op.wholeMembershipPlan.isTopKCount();
          bool wholeFieldSortAvailable = wholeMembershipAvailable
              && op.wholeMembershipPlan.isFieldSort();
          wholeFallbackSupplierPending =
              !wholeMembershipAvailable && op.wholeMembershipPlan.hasCacheUse();
          // A root, non-prepared filter-only Boolean can expose its one cached
          // required clause as the exact result. Outer domains remain explicit
          // intersections and must use the collection ladder below.
          if (!Collapsed && !exactDomain && !wholeMembershipAvailable
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
                        borrowedDomain, op.planning.filterUses).scoped(seg);
              }
            }
          }
          bool routedDomainsReady = routedProduction.has_value()
              && routedProduction->inheritReady;
          bool routedDefaultIdentity = routedDomainsReady
              && (op.weight->isConstantScoring() || data->topCount() == 0);
          bool identityResult = !Collapsed &&
              ((wholeMembershipAvailable && !wholeTopKCountAvailable
                                         && !wholeFieldSortAvailable)
               || exactDomain || matchEverything
               || borrowedDomain != nullptr || routedDefaultIdentity)
              && (rankFromDocOrder || data->topCount() == 0);

          if (captureSharedRaw
              || (output.size() > 0 && !identityResult
                  && !wholeTopKCountAvailable)) {
            builder.emplace(seg.maxDoc());
          }

          // Known document counts cannot supply distinct-group totals or
          // missing=drop eligibility. Those plans are disabled at parse time.
          if (!Collapsed && wholeTopKCountAvailable) {
            assert(rankFromDocOrder);
            assert(scoreCollector->topCount > 0);
            if (op.wholeConstantRanking) {
              (void)checkedRankingBound(nullptr);
              int64_t ranked = 0;
              if (wholeMembershipResult.count > 0) {
                Query::Scorer* scorer = nullptr;
                DocSet* collectorFilter = domain;
                auto constant = op.scoreProfile.kind == Query::ScoreProfile::Kind::VARIABLE
                    ? std::optional<float>{} : std::optional<float>{op.scoreProfile.value};
                if (!constant || wholeMembershipResult.docs.get() == nullptr) {
                  auto* rankingSupplier = obtainMainSupplier();
                  if (rankingSupplier != nullptr) {
                    scorer = buildBoundedConstantScorer(
                        poolGuard.pool(), *rankingSupplier, collectorFilter,
                        scoreCollector->topCount);
                    if (scorer != nullptr && !constant) constant = scorer->constantContribution();
                  }
                }
                // Execution can prove uniformity without a query-tree score
                // quote. A resident membership set still needs that score.
                if (wholeMembershipResult.docs.get() != nullptr && constant) {
                  scorer = QueryPrep::createDocSetScorer(
                      poolGuard.pool(), wholeMembershipResult.docs.get(), seg, *constant);
                  collectorFilter = nullptr;
                }
                if (scorer != nullptr) {
                  ranked = collectFirstKConstant(
                      segnum, scorer, collectorFilter,
                      *scoreCollector, scoreCollector->topCount, checkedRankingBound(nullptr));
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
                  rankingSupplier, domain, *scoreCollector,
                  seg.maxDoc(), op.wholeRankingWeight->allowsPruning());
            }
          } else if (identityResult) {
            (void)checkedRankingBound(nullptr);
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
                    *scoreCollector, data->topCount(), checkedRankingBound(nullptr));
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
                && !memberReplay && !thisOp().filterWeights.empty()) {
              std::vector<DocSet*> filterPtrs;
              for (size_t i = 0; i < thisOp().filterWeights.size(); i++) {
                filters.push_back(QueryPrep::materializeEffectiveFilter(
                    *thisOp().filterWeights[i], thisOp().filterUses[i],
                    *op.view, seg, nullptr));
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
            estimateFilter = collectorFilter;
            estimatorReady = op.estimateCount && op.weight != nullptr
                && !op.weight->needsPrepare() && !requiresWholeIndexPrepare;
            if constexpr (Collapsed) {
              if (!data->useFieldSort) {
                int64_t sourceCost = supplier->cost();
                if (collectorFilter != nullptr) sourceCost = std::min(
                    sourceCost, (int64_t)collectorFilter->card());
                scoreCollector->setSegment(segnum, sourceCost);
              }
            }
            // A constant-count plan satisfies document totals without an
            // executor. Collapse must enumerate identities instead.
            bool counted = false;
            if (!Collapsed && !requiresWholeIndexPrepare && builderPtr == nullptr && filter == nullptr
                && !data->useFieldSort && scoreCollector->topCount == 0) {
              if (auto count = QueryPrep::supplierCount(supplier)) {
                scoreCollector->hitCount += *count;
                counted = true;
              }
            }
            if (counted) {
              (void)checkedRankingBound(nullptr);
              // fall through to the sub-calc/merge tail below
            } else if (!Collapsed && routedDomainsReady && !data->useFieldSort
                       && op.requirements.needsCount()
                       && scoreCollector->topCount > 0) {
              int64_t count = routedProduction->domains[0].get() == nullptr
                  ? seg.maxDoc()
                  : routedProduction->domains[0].get()->card();
              collectKnownCountTopK(
                  poolGuard.pool(), segnum, count, supplier, collectorFilter,
                  *scoreCollector, seg.maxDoc(),
                  op.weight->allowsPruning());
            } else if (data->useFieldSort) {
              (void)checkedRankingBound(nullptr);
              // Ranking permission and completeness production are separate
              // facts. A known whole-membership card satisfies exact count, but
              // its borrowed set is not a produced sub-op domain.
              bool routedExactDomain = routedDomainsReady;
              // Known document cardinality cannot replace distinct key reads.
              // Collapse totals disable every skipping arm, even over an
              // exact cached domain.
              bool mustStreamForCompleteness =
                  (Collapsed && op.requirements.needsCount())
                  || (op.requirements.needsCount() && !wholeFieldSortAvailable
                   && !routedExactDomain)
                  || builderPtr != nullptr;
              bool maySkipNoncompetitiveDocs =
                  !disableFieldSortPruning && !mustStreamForCompleteness
                  && !(Collapsed && disableCollapsePruningForTests);
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
              fieldCollector->setSegment(
                  seg, &poolGuard.pool(),
                  sortSourceCost);
              bool usedBulk = false;
              int64_t fieldHitsBefore = fieldCollector->hitCount;
              Query::ScorerSupplier::BulkScorerContext matchWindowsContext;
              // Group proof: gathered leaves admit by exact identity before
              // truncation. A leaf loses on a worse bound, or on an equal
              // sole-clause bound when its first doc loses the floor's doc tie.
              // Bound-order visits reset sparse key readers on backward reads.
              // Exact-domain route: when the whole result set is already
              // materialized (BITSET or ARRAY), no scorer needs to run - the
              // driver visits leaves in the column's persisted bound order,
              // terminating on proof, sweeps the domain's leaves in doc order,
              // or scans every domain doc, whichever costs least
              // (ExactDomainSortCosts). The domain sources: a resident whole
              // membership, the supplier's exact cached set (a folded
              // filter-only Boolean over match-all, no other filter or sub-op
              // domain restriction), an explicit collectorFilter under a true
              // match-all weight, or every doc.
              if (maySkipNoncompetitiveDocs
                  && (!requiresWholeIndexPrepare || wholeFieldSortAvailable)
                  && !fieldCollector->needsScores
                  && !fieldCollector->hasExpr
                  && fieldCollector->topCount > 0) {
                DocSet* domainSet = nullptr;
                bool allDocs = false;
                if (wholeFieldSortAvailable
                    && wholeMembershipResult.docs.get() != nullptr) {
                  domainSet = wholeMembershipResult.docs.get();
                } else if (op.weight != nullptr
                           && op.weight->matchesAllDocs()) {
                  // Match-all's domain is collectorFilter, or every doc.
                  // Deletes arrive in collectorFilter under the root contract.
                  domainSet = collectorFilter;
                  allDocs = collectorFilter == nullptr;
                } else if (collectorFilter == nullptr && supplier != nullptr) {
                  domainSet = supplier->exactDocSet();
                }
                if (allDocs
                    || (domainSet != nullptr
                        && (domainSet->type == DocSet::BITSET
                            || domainSet->type == DocSet::ARRAY))) {
                  int64_t card =
                      allDocs ? (int64_t)seg.maxDoc() : domainSet->card();
                  int32_t firstDoc = 0;
                  int32_t lastDoc = seg.maxDoc() - 1;
                  if (!allDocs) {
                    domainSpan(*domainSet, firstDoc, lastDoc);
                  }
                  auto exactDomain = planExactDomainSort(
                      *fieldCollector, card, firstDoc, lastDoc);
                  if (exactDomain.available()) {
                    if (!allDocs && domainSet->type == DocSet::ARRAY) {
                      collectTopKArrayDomain(
                          segnum, ((ArrDocSet*)domainSet)->docs(),
                          *fieldCollector, poolGuard.pool(),
                          exactDomain.route);
                    } else {
                      std::span<const uint64_t> words;  // empty = every doc
                      if (!allDocs) {
                        const FixedBitSet& bits =
                            ((BitDocSet*)domainSet)->bits();
                        words = std::span<const uint64_t>(
                            bits.words,
                            FixedBitSet::sizeInWords(bits.size()));
                      }
                      collectTopKBitSetDomain(
                          segnum, words,
                          *fieldCollector, poolGuard.pool(),
                          exactDomain.route);
                    }
                    fieldCollector->recordSegmentSkipStats(segnum);
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
              // Every walk arm below prices against the docs it expects to
              // collect: the supplier's membership yield (for a conjunction
              // the independence product, not its lead's cost), under an
              // external filter only the filter's share of it. Seeding prices
              // the same work; leaf descent the same domain.
              std::optional<Query::ScorerSupplier::MembershipWork> walkWork;
              std::optional<ExactDomainSortCosts::Rest> walkRest;
              if (!usedBulk && supplier != nullptr && maySkipNoncompetitiveDocs
                  && fieldCollector->topCount > 0
                  && fieldCollector->primaryKeys.has_value()) {
                walkWork = walkMembershipWork(*supplier, collectorFilter,
                                              seg.maxDoc());
                int64_t card = (int64_t)std::max(1.0, walkWork->matches);
                fieldCollector->setWalkCard(card);
                walkRest = fieldCollector->segmentRest(card,
                    fieldCollector->predictedVisitFloor(card), 0, seg.maxDoc() - 1);
                matchWindowsContext.expectedConsumption = fieldCollector->expectedWindowDemand(
                    *walkRest, supplier->cost(), seg.maxDoc());
              }
              // Group seeding changes only visit order. The visited-leaf mask
              // prevents duplicates, and the sweep covers every unvisited
              // competitive leaf. Document quantiles price the route but never
              // terminate it; duplicate groups can only make that price wrong.
              // Pre-fill from the best-bounded leaves, then reposition the same
              // unscored product for the competitive sweep. The resolved plan
              // includes every executing child and any integrated filters.
              // Admission prices joint yield, production, seeks and sweep
              // windows (QueryDrivenSeedCosts); forceFieldSortSeeding bypasses
              // only that economics, never capability.
              if (!usedBulk && supplier != nullptr && !captureSharedRaw
                  && maySkipNoncompetitiveDocs && !disableFieldSortSeeding
                  && !disableFieldSortBulk && !requiresWholeIndexPrepare
                  && !fieldCollector->needsScores
                  && !fieldCollector->hasExpr
                  && fieldCollector->topCount > 0) {
                auto plan = fieldCollector->maskedKeyBlockPlan();
                if (plan.batch != nullptr && plan.blockCount > 0
                    && sortSourceCost > 0) {
                  assert(walkWork.has_value());
                  const auto& work = *walkWork;
                  auto& seedCollector = *fieldCollector;
                  int64_t jointCard = (int64_t)std::max(1.0, work.matches);
                  int64_t leafFloor = walkRest->floor;
                  int64_t seedBudget = seedCollector.predictedVisitFloor(
                      jointCard, seedDepth(seedCollector.topCount));
                  bool startable = leafFloor > 0 && seedBudget > 0;
                  // The rest of the price reads the bound order's head and the
                  // doc-order trend: once per segment, and only for a schedule
                  // that could start. Many-segment indexes pay it per segment.
                  std::optional<QueryDrivenSeedCosts> seedCosts;
                  if (startable || forceFieldSortSeeding) {
                    double trend = seedCollector.primaryKeys->docOrderTrend();
                    int64_t head = seedCollector.headLeaf(leafFloor);
                    seedCosts.emplace(QueryDrivenSeedCosts{
                        work,
                        *walkRest,
                        head >= 0 && 2 * head >= seedCollector.primaryKeys->leafCount(),
                        plan.leafSize, trend});
                  }
                  // Before selecting leaves use a complete-prefix seek price.
                  // Actual round positions refine it after selection.
                  bool profitable = startable
                      && seedCosts->worth(0, 0, seedBudget, 1, leafFloor, work.matches);
                  if (profitable || forceFieldSortSeeding) {
                    auto seedContext = matchWindowsContext;
                    seedContext.expectedConsumption = fieldCollector->expectedWindowDemand(
                        *walkRest, supplier->cost(), seg.maxDoc(), seedBudget);
                    auto bulkPlan = supplier->planBulk(
                        Query::ScorerSupplier::BulkUse::MATCH_WINDOWS,
                        seedContext);
                    if (bulkPlan.available
                            == Query::ScorerSupplier::BulkAnswer::YES
                        && bulkPlan.supportsMatchWindows
                            == Query::ScorerSupplier::BulkAnswer::YES
                        && bulkPlan.supportsReposition()
                            == Query::ScorerSupplier::BulkAnswer::YES
                        && !bulkPlan.hasConstantCount()) {
                      auto* bulk = supplier->buildBulk(poolGuard.pool(), bulkPlan);
                      if (bulk != nullptr) {
                        if (fieldSortSeedBudgetPerMilleForTests > 0) {
                          seedBudget = std::max<int64_t>(
                              1, seedBudget
                                  * fieldSortSeedBudgetPerMilleForTests / 1000);
                        }
                        collectTopKMatchWindowedSeeded(
                            segnum, bulk,
                            collectorFilter, *fieldCollector,
                            poolGuard.pool(), seg.maxDoc(), seedBudget, *seedCosts);
                        fieldCollector->recordSegmentSkipStats(segnum);
                        usedBulk = true;
                      }
                    }
                  }
                }
              }
              if (!usedBulk && supplier != nullptr && !captureSharedRaw
                  && !disableFieldSortBulk
                  && !fieldCollector->needsScores) {
                auto plan = supplier->planBulk(
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
                  std::optional<typename ActiveFieldCollector::ExpressionBindings> expressionBindings;
                  if (fieldCollector->hasExpr && fieldCollector->topCount > 0) {
                    expressionBindings.emplace(
                        *fieldCollector, poolGuard.pool(), seg);
                  }
                  skipCount(SkipStats::fieldSortBulkCollections);
                  collectTopKMatchWindowed(
                      segnum, bulk, collectorFilter, builderPtr,
                      *fieldCollector, seg.maxDoc(),
                      maySkipNoncompetitiveDocs);
                  fieldCollector->recordSegmentSkipStats(segnum);
                  usedBulk = true;
                }
              }
              if (!usedBulk && supplier != nullptr) {
                auto pullContext = supplier->makePlanContext(Query::Demand{});
                pullContext.expectedSpan = matchWindowsContext.expectedConsumption.span;
                Query::ScorerPlan* pullPlan = supplier->resolve(poolGuard.pool(), pullContext);
                Query::ReportedTwoPhase reportedTwoPhase =
                    pullPlan->shape().reportedTwoPhase;
                auto* scorer = pullPlan->build(poolGuard.pool());
                if (scorer != nullptr) {
                  std::optional<typename ActiveFieldCollector::ExpressionBindings> expressionBindings;
                  if (fieldCollector->hasExpr && fieldCollector->topCount > 0) {
                    expressionBindings.emplace(
                        *fieldCollector, poolGuard.pool(), seg);
                  }
                  collectTopK(segnum, scorer, collectorFilter, builderPtr,
                              *fieldCollector,
                              maySkipNoncompetitiveDocs, checkedRankingBound(nullptr),
                              captureSharedRaw
                                  ? DomainBuildMode::RAW_QUERY_MATCHES
                                  : DomainBuildMode::FILTERED_MATCHES,
                              routedBaseDomain, reportedTwoPhase);
                  fieldCollector->recordSegmentSkipStats(segnum);
                }
              }
              if (!Collapsed && op.requirements.needsCount()
                  && (wholeFieldSortAvailable || routedExactDomain)) {
                int64_t collected =
                    fieldCollector->hitCount - fieldHitsBefore;
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
              bool allowPruning = op.weight->allowsPruning()
                  && (!Collapsed || (!op.requirements.needsCount()
                      && !disableCollapsePruningForTests));
              BulkScorer* bulk = nullptr;
              bool composedExactCountTopK = false;
              ConstantScoreDisposition constantRoute =
                  constantScoreDisposition(*scoreCollector, builderPtr);
              ExactCountTopKRoute exactCountTopKRoute =
                  constantRoute
                          == ConstantScoreDisposition::NOT_CONSTANT_TOP_K
                      ? ExactCountTopKRoute::EXACT_CANDIDATE_SCORING
                      : ExactCountTopKRoute::CONSTANT_FIRST_K;
              if (!Collapsed && builderPtr == nullptr
                  && scoreCollector->topCount > 0
                  && op.countWeight != nullptr && op.rankingWeight != nullptr) {
                bool forced = op.forcedExactCountTopKComposition;
                bool proved = !op.provedDensitySegments.empty()
                    && op.provedDensitySegments[(size_t)segnum];
                bool native = forced || proved;
                bool canonical = domain == nullptr || domain == seg.liveDocSet();
                if (forced && !canonical) skipCount(SkipStats::forcedExactCountTopKCapabilityRejects);
                Query::ScorerSupplier* countSupplier = nullptr;
                auto obtainCountSupplier = [&]() {
                  if (countSupplier == nullptr) {
                    skipCount(SkipStats::exactCountTopKCountSuppliers);
                    countSupplier = op.countWeight->scorerSupplier(poolGuard.pool(), seg);
                  }
                  return countSupplier;
                };
                if ((native && canonical)
                    || (!native && op.provedDensitySegments.empty()
                        && obtainCountSupplier() != nullptr
                        && admitExactCountTopK(*countSupplier, collectorFilter, seg.maxDoc()))) {
                  switch (exactCountTopKRoute) {
                    case ExactCountTopKRoute::EXACT_CANDIDATE_SCORING:
                      composedExactCountTopK =
                          composeVariableScoreExactCountTopK(
                              poolGuard.pool(), segnum, *supplier,
                              obtainCountSupplier, collectorFilter,
                              *scoreCollector, seg.maxDoc(), native);
                      break;
                    case ExactCountTopKRoute::CONSTANT_FIRST_K:
                      // Native-score composition is never constant scoring.
                      assert(countSupplier != nullptr);
                      composedExactCountTopK = countThenCollectConstantTopK(
                          poolGuard.pool(), segnum, *countSupplier,
                          collectorFilter, *scoreCollector,
                          seg.maxDoc());
                      break;
                  }
                  if (forced && composedExactCountTopK) skipCount(SkipStats::forcedExactCountTopKCompositions);
                  if (proved && composedExactCountTopK) skipCount(SkipStats::provedDensityCompositions);
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
                  !Collapsed && builderPtr != nullptr && !captureSharedRaw
                  && scoreCollector->topCount > 0
                  && op.weight->isConstantScoring()
                  && op.weight->prefersPullForSparseArrayDomain()
                  && supplier->cost() <= DocSetBuilder::arrayLimitFor(seg.maxDoc());
              auto buildDeclaredBulk = [&]() -> BulkScorer* {
                // Count-only bulk windows omit document identities. Group
                // totals must visit every key, even at limit zero.
                if (captureSharedRaw || (Collapsed && (scoreCollector->topCount == 0
                    || constantRoute == ConstantScoreDisposition::LIMIT_ONLY))) return nullptr;
                Query::ScorerSupplier::BulkUse use = Collapsed
                    ? Query::ScorerSupplier::BulkUse::SCORED_WINDOWS :
                    constantRoute
                            == ConstantScoreDisposition::LIMIT_ONLY
                        ? Query::ScorerSupplier::BulkUse::SCORED_WINDOWS
                        : scoreCollector->topCount == 0
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
              bool composedBoundedCountTopK = !Collapsed
                  && op.boundedCountWeight != nullptr && builderPtr == nullptr
                  && !captureSharedRaw && !composedExactCountTopK
                  && boundedCountThenCollectTopK(
                      poolGuard.pool(), segnum, seg, supplier, collectorFilter,
                      *scoreCollector);
              if (composedExactCountTopK || composedBoundedCountTopK) {
                // The separate count supplied the hit count and competitively
                // pruned ranking.
              } else if (useSparseConstantPull) {
                (void)checkedRankingBound(nullptr);
                auto* scorer = buildPullScorer(poolGuard.pool(), *supplier);
                if (scorer != nullptr) {
                  collectConstantTopKAndDomain(
                      segnum, scorer, collectorFilter, *builder,
                      *scoreCollector);
                }
              } else if ((bulk = buildDeclaredBulk()) != nullptr) {
                if (scoreCollector->topCount == 0) {
                  (void)checkedRankingBound(nullptr);
                  // limit 0: the collector keeps nothing but the total, so count
                  // windows without materializing docs or scores.
                  collectCountWindowed(bulk, collectorFilter, builderPtr, *scoreCollector,
                                       seg.maxDoc());
                } else if (!Collapsed && op.weight->isConstantScoring()
                           && !disableConstantWindowCaptureForTests) {
                  // Equal scores reduce ranking to doc order: the segment's top
                  // K docs are the first K matches, captured straight off this
                  // bulk's emitted windows before consumption degrades to
                  // count-only. An independent capture scorer would rebuild
                  // every clause (a multiterm clause re-runs its dictionary
                  // scan per build).
                  collectFirstKConstantWindowed(
                      segnum, bulk, collectorFilter, builderPtr,
                      *scoreCollector, seg.maxDoc(),
                      constantRoute
                              == ConstantScoreDisposition::LIMIT_ONLY
                          ? ConstantScoreDrain::LIMIT_ONLY
                          : op.boundedCount ? ConstantScoreDrain::BOUNDED
                                            : ConstantScoreDrain::COMPLETE,
                      checkedRankingBound(op.boundedCount ? &scoreAccumulator : nullptr));
                } else if (!Collapsed && op.weight->isConstantScoring()) {
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
                          *scoreCollector, scoreCollector->topCount, checkedRankingBound(nullptr));
                    }
                  }
                  int64_t count = countMatchesWindowed(
                      bulk, collectorFilter, builderPtr, seg.maxDoc());
                  assert(count >= captured);
                  scoreCollector->hitCount += count - captured;
                } else if (builderPtr != nullptr) {
                  auto* rankingSupplier = mainScorerSupplier(poolGuard.pool(), seg);
                  countThenCollectTopK(
                      poolGuard.pool(), segnum, bulk, rankingSupplier,
                      collectorFilter, builderPtr, *scoreCollector,
                      seg.maxDoc(), allowPruning);
                } else {
                  collectTopKWindowed(
                      segnum, bulk, collectorFilter, *scoreCollector,
                      checkedRankingBound(allowPruning ? &scoreAccumulator : nullptr),
                      seg.maxDoc(), allowPruning);
                }
              } else {
                auto* scorer = constantRoute
                        == ConstantScoreDisposition::LIMIT_ONLY
                    ? buildBoundedConstantScorer(
                          poolGuard.pool(), *supplier, collectorFilter,
                          Collapsed ? supplier->cost() : scoreCollector->topCount)
                    : buildPullScorer(poolGuard.pool(), *supplier);
                if (scorer != nullptr) {
                  if (constantRoute
                          == ConstantScoreDisposition::LIMIT_ONLY
                      || (!Collapsed && op.boundedCount
                          && op.weight->isConstantScoring())) {
                    collectFirstKConstant(
                        segnum, scorer, collectorFilter,
                        *scoreCollector,
                        scoreCollector->topCount,
                        checkedRankingBound(op.boundedCount ? &scoreAccumulator : nullptr));
                  } else {
                    collectTopK(
                        segnum, scorer, collectorFilter, builderPtr,
                        *scoreCollector, allowPruning,
                        checkedRankingBound(&scoreAccumulator),
                        captureSharedRaw
                            ? DomainBuildMode::RAW_QUERY_MATCHES
                            : DomainBuildMode::FILTERED_MATCHES,
                        routedBaseDomain);
                  }
                }
              }
            }
          }
          if (recordCount) {
            op.wholeMembershipPlan.recordCount(seg, scoreCollector->totalHits() - hitsBeforeSegment);
          }
          if constexpr (!Collapsed) {
            if (op.estimateCount) {
              int64_t seen = scoreCollector->totalHits() - hitsBeforeSegment;
              bool pruned = scoreCollector->countIncomplete;
              scoreCollector->countIncomplete = pruned || prunedBeforeSegment;
              if (!pruned) {
                scoreCollector->estimatedHits += seen;
              } else if (estimatorReady) {
                estimateSegmentCount(
                    poolGuard.pool(), seg, estimateFilter, *scoreCollector, seen);
              } else {
                scoreCollector->estimatedHits += seen;
                scoreCollector->countFloored = true;
              }
            }
          }
        }
        if (builder.has_value()) {
          DomainHandle built = DomainHandle(builder->build()).scoped(op.view->segments()[segnum]);
          if constexpr (Structural) built = DomainVariantPlan::account(
              std::move(built), op.req.memoryTracker, "captured bucket matches");
          if (captureSharedRaw) {
            assert(routedProduction.has_value());
            routedProduction->inheritBases =
                op.domainVariants.produceSharedBases<Structural>(
                std::move(built), routedProduction->filters,
              Structural ? &op.req.memoryTracker : nullptr);
            finishInheritDomains<Structural>(*routedProduction, segnum);
            routedProduction->captureSharedRaw = false;
          } else {
            output[(size_t)segnum] = std::move(built);
          }
        }
      }
      auto childDomain = [&](size_t child) {
        DomainHandle newDomain;
        if (routedProduction.has_value()) {
          assert(subCalcs.size() == subCalcVariants.size());
          size_t variant = subCalcVariants[child];
          assert(variant < routedProduction->domains.size());
          if (variant == 0 && !routedProduction->inheritReady) {
            newDomain = matchEverything ? domainHandle : output[(size_t)segnum];
          } else {
            if (op.domainVariants.frame(variant) == DomainVariantPlan::Frame::INHERIT) {
              assert(routedProduction->inheritReady);
            }
            newDomain = routedProduction->domains[variant];
          }
        } else {
          newDomain = skipped || matchEverything ? domainHandle : output[(size_t)segnum];
        }
        assert(newDomain.isDeliverable());
        return newDomain;
      };
      // Consume the domain while it is in cache and make siblings available
      // before merging the collector. The first child stays on this thread.
      task_group_fanout(tg, subCalcs.size(), [&](size_t i) {
        size_t child = subCalcs.size() - 1 - i;
        auto* subCalc = subCalcs[child].get();
        auto domain = childDomain(child);
        return [subCalc, tg, segnum, domain = std::move(domain)]() {
          Signal::emit("searchTopDocsChildStart", subCalc);
          subCalc->accept(tg, segnum, domain);
        };
      });
      auto count = [&] {
        if constexpr (MetricGroups) return metricGroups->merger.release(data.release());
        else return collectorMerger.release(data.release());
      }();
      if (count == numSegs) doneCollecting<MetricGroups>();
    }

    template <bool MetricGroups = false>
    void doneCollecting() {
      auto& op = thisOp();
      if constexpr (MetricGroups) {
        op.fillMetricGroupResponse(*this);
      } else if (op.rankingSink) {
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


  template <bool Structural, bool MetricGroups = false, typename Admission = CollapseAdmission>
  class Calc final : public CalcBase {
  public:
    Calc(TopDocsReq& op, Calculator* parent, int64_t slot, int64_t numSlots,
        const StructuralConstraint* structural = nullptr)
      : CalcBase(op, parent, slot, numSlots, structural) {
      if constexpr (Admission::isDiversified) configureDiversify();
      if constexpr (MetricGroups) metricGroups = std::make_unique<MetricGroupState>(op, this);
      if constexpr (Structural) {
        assert(this->structural && op.domainVariants.hasResetVariants());
        structuralReset = std::make_unique<StructuralResetState>(
            op.req.memoryTracker, op.view->segments().size());
        requiresWholeIndexPrepare |= op.bucketVariantsNeedCompleteInput(this->structural->hasSelection());
      }
      if (requiresWholeIndexPrepare) {
        auto numSegs = op.view->segments().size();
        inputDomains.resize(numSegs);
        inputDomainViews.resize(numSegs);
        effectiveDomains.resize(numSegs);
        effectiveDomainViews.resize(numSegs);
      }
    }

    void calc(FanoutScope* tg, int32_t segnum, DomainHandle domain) override {
      if constexpr (Structural) assert(tg == nullptr);
      assert(domain.isDeliverable());
      if (segnum < 0) {
        assert(segnum == -1);
        completeEmpty<MetricGroups>(tg);
        return;
      }
      if (!requiresWholeIndexPrepare) {
        doCollect<Structural, MetricGroups, Admission>(tg, segnum, std::move(domain));
        return;
      }

      domain.assertScope(op.view->segments()[segnum]);
      assert(segnum < (int32_t)inputDomains.size());
      // Gather every input before testing segment outputs. A kNN weight
      // chooses its global top k from all of these domains.
      inputDomains[(size_t)segnum] = std::move(domain);
      inputDomainViews[(size_t)segnum] = inputDomains[(size_t)segnum].get();
      auto count =
          gatheredDomainsSeen.fetch_add(1, std::memory_order_acq_rel) + 1;
      if (count != (int32_t)thisOp().view->segments().size()) return;
      prepareAndDispatch<Structural, MetricGroups, Admission>(tg);
    }

    void calcAll(FanoutScope* tg, std::span<const DomainHandle> domains) override {
      if constexpr (Structural) assert(tg == nullptr);
      assert(domains.size() == thisOp().view->segments().size());
      if (domains.empty()) {
        completeEmpty<MetricGroups>(tg);
        return;
      }
      if (!requiresWholeIndexPrepare) {
        dispatch<Structural, MetricGroups, Admission>(tg, domains);
        return;
      }
      std::copy(domains.begin(), domains.end(), inputDomains.begin());
      for (size_t i = 0; i < inputDomains.size(); i++) {
        assert(inputDomains[i].isDeliverable());
        inputDomains[i].assertScope(op.view->segments()[i]);
        inputDomainViews[i] = inputDomains[i].get();
      }
      prepareAndDispatch<Structural, MetricGroups, Admission>(tg);
    }
  };

  Calculator* createCalculator(
      Calculator* parent, int64_t slot = -1, int64_t numSlots = -1,
      const StructuralConstraint* structural = nullptr) override;

  // A facet bucket child runs one independent TopDocs per bucket over that
  // bucket's domain; its DocList lands in the bucket's slot of the parent
  // FacetResult's ops entry.
  bool canEmitAsBucketChild() const override { return true; }

  // Retained by one bucket binding while its segments are fed: the
  // calculator, the merged collector's heap (a score-doc entry, plus one key
  // per sort clause for a field sort), per-segment domain handles, and the
  // child calculators the Calc constructs and keeps. A coarse block-sizing
  // estimate, not memory accounting.
  size_t facetBucketResidentBytes() const override {
    size_t perDoc = sortPlan.useFieldSort
        ? 16 + 16 * sortPlan.clauses.size() : 16;
    size_t heap = (size_t)topCount > std::numeric_limits<size_t>::max() / perDoc
        ? std::numeric_limits<size_t>::max() : (size_t)topCount * perDoc;
    size_t bytes = saturatingAdd(sizeof(CalcBase), heap);
    bytes = saturatingAdd(
        bytes, view->segments().size() * 4 * sizeof(DomainHandle));
    for (const auto& [name, child] : subOps) {
      unused(name);
      bytes = saturatingAdd(bytes, child->facetBucketResidentBytes());
    }
    return bytes;
  }


  // Called after all segments have been collected for a TopN query to fill out
  // the DocList proto.  Hands off the merged collector's ranked output to the
  // shared emitter (emitDocsResponse), which blocks until field loading
  // completes.  At the root or under another root-dispatched op it streams
  // (all batches but the last go out as they complete); beneath a facet bucket
  // slot the whole list assembles into the final response instead (see
  // DocEmission).  The empty-index case (no segments -> no collector ever
  // obtained) writes an empty DocList (with found=0 only when the count was
  // requested); submitBody then sends it.
  Calculator* createDiversifyCalculator(Calculator* parent, int64_t slot, int64_t numSlots,
                                       const StructuralConstraint* structural);
  void fillDiversifyResponse(CalcBase& calc, MergeableCollector& collector);
  void fillQueryTopNResponse(CalcBase& calc);
  void fillMetricGroupResponse(CalcBase& calc);


};


}
