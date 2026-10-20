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


class TopDocsReq : public SearchOp {
protected:

public:
  class CalcBase;

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

  // Pre-Weight FIELD_SORT outcome: an accepted resident value (the main
  // Weight is omitted), a build veto (no segment may touch the cache), or
  // neither, leaving the Weight-bearing router to plan builds per segment.
  struct CacheFirstFieldSortPlan {
    FilterCache::Use* acceptedUse = nullptr;
    bool buildVetoed = false;

    bool omitsWeight() const noexcept { return acceptedUse != nullptr; }
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
    return FieldType::sortsByIntColumn(const_cast<FieldType&>(
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
      const auto* meta = info ? info->numericLeafMeta() : nullptr;
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

  // The visit floors a request is predicted to need, segment by segment in
  // visit order from an empty heap: each segment's cards[s] domain docs
  // collected into the heap the earlier ones are predicted to have left
  // (FieldSortCollector::predictBottom). This is the index-wide view a
  // decision taken before any heap exists needs: a small segment saturates
  // on its own, yet past the first few segments a good bottom bounds every
  // segment, and only a whole-index prediction sees it.
  // Cache pricing only: these document quantiles never bound collection.
  // Group multiplicity can bias the estimate; the executing group collector
  // still requires L distinct groups and keeps every tied primary bound.
  static void plannedVisitFloors(
      std::span<const std::optional<SegmentKeys>> keys,
      std::span<const int32_t> order, std::span<const int64_t> cards,
      int64_t topCount, std::span<int64_t> floors) {
    int64_t heapSize = 0;
    int64_t bottom = 0;
    for (int32_t s : order) {
      const auto& segmentKeys = keys[(size_t)s];
      int64_t card = cards[(size_t)s];
      floors[(size_t)s] = 0;
      if (!segmentKeys.has_value() || card <= 0
          || segmentKeys->values() <= 0) {
        continue;
      }
      auto predicted = FieldSortCollector::predictBottom(
          *segmentKeys, card, topCount, heapSize, bottom);
      floors[(size_t)s] = predicted.has_value()
          ? FieldSortCollector::visitFloorAt(*segmentKeys, *predicted, card,
                                             topCount)
          : segmentKeys->leafCount();
      if (heapSize + card >= topCount) {
        bottom = predicted.value_or(segmentKeys->keyAt(1));
      }
      heapSize = std::min(topCount, heapSize + card);
    }
  }

  // Whether the exact-domain driver could bound its visits over a whole
  // membership of the estimated per-segment cards: the planned visit
  // floors (plannedVisitFloors) cover under half the leaves of the segments
  // holding domain docs. The build pays per later request through the
  // leaves the bounds skip; at saturation the driver would only trade
  // query evaluation for gathers, which does not repay the cache space.
  static bool fieldSortVisitsBounded(
      IndexReader& reader, const SortPlan& sortPlan,
      std::optional<Query::ValueEnvelope> envelope,
      std::span<const int64_t> cards, int64_t topCount) {
    if (disableFieldSortPruning || disableFieldSortBestFirst) return false;
    auto keys = fieldSortSegmentKeys(reader, sortPlan, envelope);
    if (std::none_of(keys.begin(), keys.end(),
                     [](const auto& k) { return k.has_value(); })) {
      return false;
    }
    if (forceFieldSortBestFirst) return true;
    std::vector<int64_t> floors(keys.size());
    plannedVisitFloors(
        keys,
        fieldSortVisitOrder(keys, topCount,
                            fieldSortMissingFirst(sortPlan, envelope)),
        cards, topCount, floors);
    int64_t visited = 0;
    int64_t leaves = 0;
    for (size_t s = 0; s < keys.size(); s++) {
      if (!keys[s].has_value() || cards[s] <= 0) continue;
      visited += floors[s];
      leaves += keys[s]->leafCount();
    }
    return 2 * visited < leaves;
  }

  // Pre-Weight FIELD_SORT planning. A complete resident whole membership
  // serves every shape: the Calc serves it per segment through the
  // exact-domain driver, or through the doc-order ladder over it when the
  // sort offers no masked leaf bounds, and either replaces query
  // evaluation. That includes shapes this planner never builds for, whose
  // membership another consumer (a count over the same query) made
  // resident; accepting applies the ordinary shared-hit effects. The inert
  // lookup records nothing when the value is absent. Building stays a
  // separate decision: verification-bearing and non-flat numeric shapes
  // return undecided so the Weight-bearing router admits per segment on its
  // estimates; flat literal conjunctions and docs-only non-numeric sorts
  // veto every segment here, so a field sort never builds them, records
  // their sighting, or creates their cache entry.
  static CacheFirstFieldSortPlan planCacheFirstFieldSortWholeMembership(
      Query& query, Query::PlanningContext& planning,
      const SortPlan& sortPlan, bool allowReaderStable) {
    auto candidate = planning.lookupExistingFilterUse(
        query, allowReaderStable);
    if (candidate.has_value()) {
      FilterCache::Use* use = planning.acceptExistingFilterUse(
          std::move(*candidate), FilterCache::AdmissionLane::WHOLE);
      // A concurrent detach after the lookup leaves the Weight-bearing
      // router authoritative.
      return {use, false};
    }
    if (membershipVerificationWork(query)
        == Query::VerificationWork::PRESENT) {
      return {};
    }
    if (fieldSortCanUseMaskedBestFirst(sortPlan)
        && query.fieldSortConjunction(planning)
            != Query::FieldSortConjunction::FLAT_LITERAL_CONJUNCTION) {
      return {};
    }
    return {nullptr, true};
  }

  // Building membership pays for FIELD_SORT when it removes verification
  // work from the query-driven ladder, or when the estimated cardinalities
  // let the exact-domain driver bound its visits across the whole index
  // (fieldSortVisitsBounded) - one decision for every segment, so a
  // membership a later request finds is complete. Flat term conjunction
  // cost is only the cheapest posting list, not a useful estimate of
  // intersection membership, so it cannot establish that by itself.
  // VerificationWork includes nested two-phase children hidden behind a
  // single-phase compound protocol. Serving an already resident value is not
  // decided here: the pre-Weight planner accepts it for every shape. Plan
  // before getFilterUse(): a request rejected in every segment must not
  // create a Use, record a WHOLE-lane sighting, or admit metadata.
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
    size_t segments = reader.segments().size();
    std::vector<int64_t> cards(segments, 0);
    std::vector<uint8_t> boundable(segments, 0);
    MemPool pool;
    for (auto& segment : reader.segments()) {
      auto savepoint = pool.getSavePoint();
      auto* supplier = weight.scorerSupplier(pool, segment);
      size_t s = (size_t)segment.ord;
      routes[s] = 0;
      if (supplier != nullptr) {
        int64_t estimatedCard = supplier->cost();
        if (segment.liveDocs() != nullptr) {
          estimatedCard = std::min(
              estimatedCard,
              (int64_t)segment.liveDocs()->docset().card());
        }
        cards[s] = estimatedCard;
        auto context = supplier->makePlanContext(
            Query::Demand::fromLeadCost(
                estimatedCard, Query::ExecutionUse::MATCH_WINDOWS));
        routes[s] = supplier->verificationWork(context)
            == Query::VerificationWork::PRESENT;
        boundable[s] = !routes[s]
            && supplier->describeScorer(context).termConjunctionClause
                != Query::ClauseShape::FLAT_CONJUNCTION;
      }
      pool.rewind(savepoint);
    }
    // Whole membership is planned only with every filter folded into the
    // query, so the query alone carries the envelope.
    if (std::find(boundable.begin(), boundable.end(), 1) != boundable.end()
        && fieldSortVisitsBounded(reader, sortPlan,
                                  sortValueEnvelope(sortPlan, &query, {}),
                                  cards, topCount)) {
      for (size_t s = 0; s < segments; s++) routes[s] |= boundable[s];
    }
    return std::find(routes.begin(), routes.end(), 1) != routes.end();
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

  CollapsePlan* collapse = nullptr;
  DiversifyPlan* diversify = nullptr;
  MetricGroupPlan* metricOrder = nullptr;
  std::optional<SortPlan> headSort;
  boost::unordered_flat_map<std::string_view, SearchOp*> groupOps;
  NamedQueries* namedQueries = nullptr;  // get_matched_queries; null without named clauses

  void init() override {
    SearchOp::init();
    for (auto& [name, child] : groupOps) child->init();
    if (collapse && !topDocsProto.collapse->sort.empty()) resolveMetricGroupOrder(*this);
  }
  bool enrichGroups() const {
    return collapse != nullptr && (topDocsProto.collapse->get_number || headSort.has_value() || !groupOps.empty())
        && topCount > topDocsProto.offset;
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
  int64_t topCount; // Collection depth (offset + page size, capped by maxDoc).
  std::span<ParsedFilter> filters;
  std::span<Query::Weight*> filterWeights;
  std::span<FilterCache::Use*> filterUses;
  DomainVariantPlan domainVariants;
  CollectionRequirements requirements;
  QueryPrep::WholeMembershipPlan wholeMembershipPlan;
  QueryPrep::ExactDomainPlan exactDomainPlan;
  SortPlan sortPlan;
  // Every field-sort collector's primaryValues (see sortValueEnvelope).
  std::optional<Query::ValueEnvelope> sortValues;

  // Optional sink for the merged top-K collector.  If set, the Calc invokes
  // it instead of self-emitting via fillQueryTopNResponse, letting another
  // op (e.g. FusionOp) consume this TopDocsReq's ranking.  `mc` is null on
  // the empty-index path (no segments, no collector ever obtained); sinks
  // must handle that case.
  std::function<void(CalcBase&, MergeableCollector*)> rankingSink;


  void releaseSegmentState(IndexReader::Segment& segment) noexcept override {
    if (segmentStateRetained(segment)) return;
    for (auto* source : {weight, variantMembershipWeight, countWeight,
                         rankingWeight, wholeRankingWeight}) {
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
                         rankingWeight, wholeRankingWeight}) {
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

  class CalcBase : public SearchOp::Calculator {
  public:
    // Page result columns live in the final response arena, pinned through
    // all streaming batches. They are sliced only after every binding closes.
    luxir::api::GroupTable* pageGroups = nullptr;
    luxir::api::Val* getTargetForSub(SearchResponse* resp, Calculator* sub) override {
      auto& groups = thisOp().groupOps;
      auto found = groups.find(sub->getOp().name);
      if (found != groups.end() && found->second == &sub->getOp()) {
        assert(pageGroups != nullptr);
        assert(resp == op.req.lastResponse); // bucket children never stream
        return build::opsSlot(pageGroups->ops, groups.size(), sub->getOp().name, resp->mr);
      }
      auto* ourVal = parent->getTargetForSub(resp, this);
      assert(ourVal != nullptr);
      // Our DocList lives in our bucket slot when this TopDocs is a facet
      // bucket child; the Val should either be unset or already hold it.
      auto& target = slotTarget<luxir::api::ArrVal>(*ourVal, resp->mr);
      assert(std::holds_alternative<luxir::api::DocList>(target.kind)
             || std::holds_alternative<std::monostate>(target.kind));
      auto& dl = oneofMut<luxir::api::DocList>(target);
      return build::opsSlot(dl.ops, op.subOps.size(), sub->getOp().name, resp->mr);
    }

    CalcBase(TopDocsReq& op, Calculator* parent, int64_t slot, int64_t numSlots,
        const StructuralConstraint* structural = nullptr)
      : SearchOp::Calculator(op, parent, slot, numSlots, structural),
        collectorMerger(nullptr, nullptr) {

      collectorMerger.creator = [&op]() -> MergeableCollector* {
        auto* collector = new MergeableCollector(
            op.topCount, op.sortPlan, op.req.reader.get(),
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
      if (op.enrichGroups()) {
        memberReplay = std::make_unique<MemberReplay>(op.req.memoryTracker,
            op.req.reader->segments().size());
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
      auto& seg = thisOp().req.reader->segments()[(size_t)segment];
      DomainHandle live = DomainHandle::pinned(
          seg.liveDocs() ? &seg.liveDocs()->docset() : nullptr, thisOp().req.reader);
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
        auto& seg = op.req.reader->segments()[(size_t)segment];
        return seg.liveDocs() ? &seg.liveDocs()->docset() : nullptr;
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
      if (!TopDocsReq::disableConstantWindowCaptureForTests
          && !op.requirements.needExactCount && builder == nullptr
          && op.filters.empty()) {
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
        auto& collector,
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
        DocSetBuilder* builder, auto& collector,
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
        auto& collector, int32_t maxDoc) {
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
            collector.topCount);
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
          auto& sources = structural->producer->filterSources(op.filterWeights, op.filterUses,
              *op.req.reader, op.planning, seg.ord, op.req.memoryTracker);
          assert(owner && owner->get() == baseDomain);
          docs = sources.resolve(source, *owner,
              prepared ? &(*prepared)[source] : nullptr);
        } else {
          docs = prepared != nullptr
              ? QueryPrep::materializeEffectiveFilter(
                    (*prepared)[source], *op.req.reader, seg, baseDomain)
              : QueryPrep::materializeEffectiveFilter(
                    *op.filterWeights[source], op.filterUses[source], *op.req.reader, seg, baseDomain);
        }
        if constexpr (!Structural) if (reservation != nullptr && docs.get() != nullptr
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
      if (op.filterWeights.empty()) return {};
      auto& seg = op.req.reader->segments()[segnum];

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
      if (filterPtrs.empty()) return {};
      if (filterPtrs.size() == 1) {
        auto result = std::move(filters[0]);
        return std::move(result).pinnedWith(op.planning.filterUses);
      }
      return DomainHandle(DocSet::intersect(filterPtrs));
    }

    void configureDiversify();

    template <bool MetricGroups = false>
    void completeEmpty(oneapi::tbb::task_group* tg) {
      std::span<const DomainHandle> noDomains;
      for (auto& subCalc : subCalcs) {
        subCalc->calcAll(tg, noDomains);
      }
      doneCollecting<MetricGroups>();
    }

    template <bool Structural>
    void prepareMainVariants(oneapi::tbb::task_group* tg);

    template <bool Structural>
    void prepareDomainQueryVariants(
        std::span<DocSet* const> liveDomains,
        oneapi::tbb::task_group* tg);

    template <bool Structural, bool MetricGroups = false, typename Admission = CollapseAdmission>
    void prepareAndDispatch(oneapi::tbb::task_group* tg);

    template <bool Structural>
    void prepareDomains(oneapi::tbb::task_group* tg);

    template <bool Structural, bool MetricGroups = false, typename Admission = CollapseAdmission>
    void dispatch(
        oneapi::tbb::task_group* tg,
        std::span<const DomainHandle> domains) {
      assert(domains.size() == thisOp().req.reader->segments().size());
      // Keep the same task-stack ordering as Calculator::calcAll, except
      // that a field sort visits segments in its visit order
      // (fieldSortVisitOrder). Sequentially, one collector then carries its
      // heap from segment to segment, so the segments holding the best keys
      // go first. In parallel the segments are spawned in reverse:
      // the best segment is the final index and runs inline, so this
      // thread's heap fills with good keys early. Thieves steal the worst
      // segments, with heaps of their own.
      auto& op = thisOp();
      std::vector<int32_t> order;
      if (domains.size() > 1 && subCalcs.empty()
          && op.sortPlan.useFieldSort && op.topCount > 0
          && !disableFieldSortPruning) {
        order = fieldSortVisitOrder(
            fieldSortSegmentKeys(*op.req.reader, op.sortPlan, op.sortValues),
            op.topCount, fieldSortMissingFirst(op.sortPlan, op.sortValues));
      }
      task_group_fanout(tg, domains.size(), [&](size_t i) {
        int32_t segnum = order.empty() ? (int32_t)i : order[tg ? order.size() - 1 - i : i];
        return [this, tg, segnum, domain = domains[(size_t)segnum]]() {
          assert(domain.isDeliverable());
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
      auto& seg = op.req.reader->segments()[(size_t) segnum];
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
      auto& seg = op.req.reader->segments()[(size_t) segnum];
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
      auto& seg = op.req.reader->segments()[(size_t) segnum];
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
        auto& sources = structural->producer->segmentSources(
            op.domainVariants, *op.req.reader, op.planning, segnum, op.req.memoryTracker);
        assert(owner && owner->get() == base);
        return sources.resolve(source, *owner,
            preparedDomainSources.empty() ? nullptr : &preparedDomainSources[source]);
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
          auto& sources = structural->producer->segmentSources(
              op.domainVariants, *op.req.reader, op.planning, segnum, op.req.memoryTracker);
          domains = op.domainVariants.produceSegment(sources, {}, base,
              op.req.memoryTracker, bases, DomainVariantPlan::Frame::RESET);
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
                    reusableRaw, op.planning.filterUses),
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
    void doCollect(oneapi::tbb::task_group* tg, int32_t segnum,
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

    // One collection ladder, specialized once per segment. Document requests
    // instantiate their original admission loop without per-hit group checks.
    template <bool Structural, bool Collapsed, bool MetricGroups = false,
              typename Admission = CollapseAdmission>
    void doCollectPolicy(
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
      // Constant-score first-K and cardinality shortcuts rank documents.
      // Groups require key reads; the bounded pull below stops at L groups,
      // while this identity arm also supplies document-cardinality totals.
      bool matchEverything = !Collapsed && op.weight != nullptr && op.weight->matchesAllDocs()
        && op.weight->isConstantScoring() && op.filterWeights.empty();
      std::unique_ptr<std::conditional_t<MetricGroups, MetricGroupData, MergeableCollector>> data;
      std::optional<RoutedProduction> routedProduction;
      DocSet* routedBaseDomain = nullptr;
      int64_t numSegs = (int64_t)op.req.reader->segments().size();

      {
        auto& seg = op.planning.topReader.segments()[segnum];
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
                    *op.filterWeights[i], op.filterUses[i], *op.req.reader, seg, nullptr));
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
                  *op.filterWeights[i], op.filterUses[i], *op.req.reader, seg, nullptr));
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
                *op.req.reader, seg, domain, preparedWeight.get(),
                [&](Query::Weight& resolver) {
                  return &resolver == op.weight ? obtainMainSupplier()
                      : resolver.scorerSupplier(poolGuard.pool(), seg);
                });
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
                        borrowedDomain, op.planning.filterUses);
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
                        scoreCollector->topCount);
                  }
                }
                if (scorer != nullptr) {
                  ranked = collectFirstKConstant(
                      segnum, scorer, collectorFilter,
                      *scoreCollector, scoreCollector->topCount);
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
                    *scoreCollector, data->topCount());
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
              // fall through to the sub-calc/merge tail below
            } else if (!Collapsed && routedDomainsReady && !data->useFieldSort
                       && op.requirements.needExactCount
                       && scoreCollector->topCount > 0) {
              int64_t count = routedProduction->domains[0].get() == nullptr
                  ? seg.maxDoc()
                  : routedProduction->domains[0].get()->card();
              collectKnownCountTopK(
                  poolGuard.pool(), segnum, count, supplier, collectorFilter,
                  *scoreCollector, seg.maxDoc(),
                  op.weight->allowsPruning());
            } else if (data->useFieldSort) {
              // Ranking permission and completeness production are separate
              // facts. A known whole-membership card satisfies exact count, but
              // its borrowed set is not a produced sub-op domain.
              bool routedExactDomain = routedDomainsReady;
              // Known document cardinality cannot replace distinct key reads.
              // Collapse totals disable every skipping arm, even over an
              // exact cached domain.
              bool mustStreamForCompleteness =
                  (Collapsed && op.requirements.needExactCount)
                  || (op.requirements.needExactCount && !wholeFieldSortAvailable
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
              // A MATCH_WINDOWS plan taken by a declined arm is retained here
              // so the ordinary bulk arm never re-plans the same context.
              std::optional<Query::ScorerSupplier::BulkPlan> matchWindowsPlan;
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
              if (!usedBulk && supplier != nullptr && maySkipNoncompetitiveDocs
                  && fieldCollector->topCount > 0
                  && fieldCollector->primaryKeys.has_value()) {
                walkWork = walkMembershipWork(*supplier, collectorFilter,
                                              seg.maxDoc());
                fieldCollector->setWalkCard(
                    (int64_t)std::max(1.0, walkWork->matches));
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
                  int64_t leafFloor = seedCollector.predictedVisitFloor(jointCard);
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
                        seedCollector.segmentRest(jointCard, leafFloor, 0,
                                                   seg.maxDoc() - 1),
                        head >= 0 && 2 * head >= seedCollector.primaryKeys->leafCount(),
                        plan.leafSize, trend});
                  }
                  // Before selecting leaves use a complete-prefix seek price.
                  // Actual round positions refine it after selection.
                  bool profitable = startable
                      && seedCosts->worth(0, 0, seedBudget, 1, leafFloor, work.matches);
                  if (profitable || forceFieldSortSeeding) {
                    matchWindowsPlan = supplier->planBulk(
                        Query::ScorerSupplier::BulkUse::MATCH_WINDOWS,
                        matchWindowsContext);
                    auto& bulkPlan = *matchWindowsPlan;
                    if (bulkPlan.available
                            == Query::ScorerSupplier::BulkAnswer::YES
                        && bulkPlan.supportsMatchWindows
                            == Query::ScorerSupplier::BulkAnswer::YES
                        && bulkPlan.supportsReposition()
                            == Query::ScorerSupplier::BulkAnswer::YES
                        && !bulkPlan.hasConstantCount()) {
                      auto* bulk = supplier->buildBulk(poolGuard.pool(), bulkPlan);
                      matchWindowsPlan.reset();  // consumed by the build
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
                Query::ScorerPlan* pullPlan = resolvePullScorer(
                    poolGuard.pool(), *supplier);
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
                              maySkipNoncompetitiveDocs, nullptr,
                              captureSharedRaw
                                  ? DomainBuildMode::RAW_QUERY_MATCHES
                                  : DomainBuildMode::FILTERED_MATCHES,
                              routedBaseDomain, reportedTwoPhase);
                  fieldCollector->recordSegmentSkipStats(segnum);
                }
              }
              if (!Collapsed && op.requirements.needExactCount
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
                  && (!Collapsed || (!op.requirements.needExactCount
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
                              *scoreCollector, seg.maxDoc());
                      break;
                    case ExactCountTopKRoute::CONSTANT_FIRST_K:
                      composedExactCountTopK = countThenCollectConstantTopK(
                          poolGuard.pool(), segnum, *countSupplier,
                          collectorFilter, *scoreCollector,
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
              if (composedExactCountTopK) {
                // countThenCollectTopK supplied both the exact hit count and
                // competitively pruned ranking.
              } else if (useSparseConstantPull) {
                auto* scorer = buildPullScorer(poolGuard.pool(), *supplier);
                if (scorer != nullptr) {
                  collectConstantTopKAndDomain(
                      segnum, scorer, collectorFilter, *builder,
                      *scoreCollector);
                }
              } else if ((bulk = buildDeclaredBulk()) != nullptr) {
                if (scoreCollector->topCount == 0) {
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
                          : ConstantScoreDrain::COMPLETE);
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
                          *scoreCollector, scoreCollector->topCount);
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
                      allowPruning ? &scoreAccumulator : nullptr,
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
                      == ConstantScoreDisposition::LIMIT_ONLY) {
                    collectFirstKConstant(
                        segnum, scorer, collectorFilter,
                        *scoreCollector,
                        scoreCollector->topCount);
                  } else {
                    collectTopK(
                        segnum, scorer, collectorFilter, builderPtr,
                        *scoreCollector, allowPruning,
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
        }
        if (builder.has_value()) {
          DomainHandle built(builder->build());
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
          newDomain = matchEverything ? domainHandle : output[(size_t)segnum];
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
          subCalc->calc(tg, segnum, domain);
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
      sortPlan(std::move(sortPlan)),
      sortValues(sortValueEnvelope(this->sortPlan, query, filters)) {
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
            op.req.memoryTracker, op.req.reader->segments().size());
        requiresWholeIndexPrepare |= op.bucketVariantsNeedCompleteInput(this->structural->hasSelection());
      }
      if (requiresWholeIndexPrepare) {
        auto numSegs = op.req.reader->segments().size();
        inputDomains.resize(numSegs);
        inputDomainViews.resize(numSegs);
        effectiveDomains.resize(numSegs);
        effectiveDomainViews.resize(numSegs);
      }
    }

    void calc(oneapi::tbb::task_group* tg, int32_t segnum, DomainHandle domain) override {
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

      assert(segnum < (int32_t)inputDomains.size());
      inputDomains[(size_t)segnum] = std::move(domain);
      inputDomainViews[(size_t)segnum] = inputDomains[(size_t)segnum].get();
      auto count =
          gatheredDomainsSeen.fetch_add(1, std::memory_order_acq_rel) + 1;
      if (count != (int32_t)thisOp().req.reader->segments().size()) return;
      prepareAndDispatch<Structural, MetricGroups, Admission>(tg);
    }

    void calcAll(oneapi::tbb::task_group* tg, std::span<const DomainHandle> domains) override {
      if constexpr (Structural) assert(tg == nullptr);
      assert(domains.size() == thisOp().req.reader->segments().size());
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
        bytes, req.reader->segments().size() * 4 * sizeof(DomainHandle));
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
