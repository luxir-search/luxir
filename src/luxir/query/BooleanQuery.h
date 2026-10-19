// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <typeindex>

#include <boost/unordered/unordered_flat_map.hpp>

#include "Query.h"
#include "AllQuery.h"
#include "BoostQuery.h"
#include "ConstantScoreQuery.h"
#include "TermQuery.h"
#include "QueryPrep.h"

namespace luxir {

class BooleanQuery final : public luxir::Query {
  using DirectTermScorer = TermQuery::Scorer;
  using PreparedDocSetScorer = QueryPrep::DocSetScorer;
  using PreparedDocSetSupplier = QueryPrep::DocSetSupplier;

  std::span<Query*> mandatory;
  std::span<Query*> optional;
  std::span<Query*> prohibited;
  std::span<Query*> filter;
  // Minimum number of `optional` clauses a doc must match.  Unset (0): with
  // any mandatory or filter clause present the optional side only ranks
  // coincident matches; with only optional clauses at least one must match.
  // >= 1 makes the optional group a real constraint alongside
  // mandatory/filter clauses (> 1 selects the min-should-match scorer).
  int minShouldMatch;

public:
  // Test hook: keep score-only optional clauses in unscored plans.
  static inline bool disableUnscoredOptionalDropForTests = false;

  enum NormalizeRule : uint32_t {
    R1_REQUIRED_INLINE = 1u << 0,
    R2_DISJUNCTION_FLATTEN = 1u << 1,
    R3_REQUIRED_DISJUNCTION_HOIST = 1u << 2,
    R4_SINGLE_CLAUSE_UNWRAP = 1u << 3,
    R5_MATCH_ALL_ELIMINATE = 1u << 4,
  };

  // Snapshot of the query-pointer plan immediately before duplicate removal
  // and weight creation. It deliberately exposes no child Query pointers.
  struct NormalizeTestView {
    size_t mandatoryCount = 0;
    size_t optionalCount = 0;
    size_t prohibitedCount = 0;
    size_t filterCount = 0;
    int minShouldMatch = 0;
    uint32_t ruleMask = 0;
    std::type_index singleChildType = typeid(void);
    boost::container::small_vector<std::type_index, 16> mandatoryTypes;
    boost::container::small_vector<std::type_index, 16> optionalTypes;
    boost::container::small_vector<std::type_index, 16> prohibitedTypes;
    boost::container::small_vector<std::type_index, 16> filterTypes;
  };

  struct CompiledPlanTestView {
    const void* identity = nullptr;
    std::span<Query* const> mandatory;
    std::span<Query* const> optional;
    std::span<Query* const> prohibited;
    std::span<Query* const> filter;
  };

private:
  using ClauseList = boost::container::small_vector<Query*, 16>;

  struct NormalizedBoolean {
    ClauseList mandatory;
    ClauseList optional;
    ClauseList prohibited;
    ClauseList filter;
    int minShouldMatch;
    Query* singleChild = nullptr;
    uint32_t ruleMask = 0;

    explicit NormalizedBoolean(const BooleanQuery& query)
      : mandatory(query.mandatory.begin(), query.mandatory.end()),
        optional(query.optional.begin(), query.optional.end()),
        prohibited(query.prohibited.begin(), query.prohibited.end()),
        filter(query.filter.begin(), query.filter.end()),
        minShouldMatch(query.minShouldMatch) {}
  };

  struct CompiledBoolean {
    std::span<Query*> mandatory;
    std::span<Query*> optional;
    std::span<Query*> prohibited;
    std::span<Query*> filter;
    int minShouldMatch = 0;
    Query* singleChild = nullptr;
    uint32_t ruleMask = 0;

    bool dropsOptional(bool needsScores) const {
      return !disableUnscoredOptionalDropForTests && !needsScores
          && (!mandatory.empty() || !filter.empty())
          && minShouldMatch < 1;
    }

    // The one clause that alone determines unscored membership, or null.
    // Optional clauses that drop under !needsScores cannot change the match
    // set, so a lone required clause is the entire membership shape; a
    // mandatory clause and a filter clause are interchangeable here.
    Query* unscoredSingleChild() const {
      if (singleChild != nullptr) return singleChild;
      if (!prohibited.empty()) return nullptr;
      if (!optional.empty() && !dropsOptional(false)) return nullptr;
      if (mandatory.size() + filter.size() != 1) return nullptr;
      return mandatory.empty() ? filter.front() : mandatory.front();
    }
  };

  struct TransparentBoolean {
    BooleanQuery* query = nullptr;
    float boost = 1.0f;
  };

  static TransparentBoolean transparentBoolean(Query* query,
                                                bool allowConstantScore) {
    float boost = 1.0f;
    while (true) {
      switch (query->getKind()) {
        case QueryKind::BOOST: {
          auto* wrapped = static_cast<BoostQuery*>(query);
          boost = checkedBoostProduct(boost, wrapped->getBoost());
          query = wrapped->getChild();
          continue;
        }
        case QueryKind::CONSTANT_SCORE:
          if (allowConstantScore) {
            query = static_cast<ConstantScoreQuery*>(query)->getChild();
            continue;
          }
          break;
        default:
          break;
      }
      break;
    }
    return {query->getKind() == QueryKind::BOOLEAN
                ? static_cast<BooleanQuery*>(query) : nullptr,
            boost};
  }

  static Query* scoringClause(MemPool& pool, Query* query, float boost) {
    if (boost == 1.0f) return query;
    auto* wrapper = pool.make<BoostQuery>(query, boost);
    wrapper->markPlannerGenerated();
    return wrapper;
  }

  static bool isPureNegative(const BooleanQuery& query) {
    return query.mandatory.empty() && query.optional.empty()
        && !query.prohibited.empty() && query.filter.empty()
        && query.minShouldMatch <= 1;
  }

  static bool isComplementForm(const BooleanQuery& query) {
    bool optionalCarrier = query.mandatory.empty()
        && query.optional.size() == 1
        && query.optional[0]->getKind() == QueryKind::ALL;
    bool requiredCarrier = query.optional.empty()
        && query.mandatory.size() == 1
        && query.mandatory[0]->getKind() == QueryKind::ALL;
    return (optionalCarrier || requiredCarrier) && !query.prohibited.empty()
        && query.filter.empty() && query.minShouldMatch <= 1;
  }

  static void normalizeRequiredList(NormalizedBoolean& plan, ClauseList& parents,
                                    bool parentIsFilter, MemPool& pool) {
    size_t i = 0;
    while (i < parents.size()) {
      TransparentBoolean child = transparentBoolean(
        parents[i], /*allowConstantScore=*/parentIsFilter);
      BooleanQuery* inner = child.query;
      if (inner != nullptr && (isPureNegative(*inner) || isComplementForm(*inner))) {
        // Required complements contribute no score: drop their match-all
        // carrier (if present), strip wrappers, and merge the exclusions.
        // If an exclusion is itself a pure-negative Boolean it stays opaque;
        // only the current required complement is inlined.
        plan.prohibited.insert(plan.prohibited.end(), inner->prohibited.begin(),
                               inner->prohibited.end());
        parents.erase(parents.begin() + (ptrdiff_t) i);
        plan.ruleMask |= R1_REQUIRED_INLINE;
        // Re-examine the clause shifted into this slot.
        continue;
      }
      bool hasRequired = inner != nullptr
        && (!inner->mandatory.empty() || !inner->filter.empty());
      // A sole required child may lift its rank-only optionals into an outer
      // Boolean that has no optional constraint of its own. This is the same
      // required body and the same optional score decoration, while exposing
      // outer filters to cost-ordered required planning. Semantically any
      // required child under a zero minShouldMatch outer could lift (every
      // optional is rank-only once a required clause exists, and the score
      // is the same sum re-associated), but lifting from one of several
      // required children turns a conjunction that probes the child's
      // optionals per candidate into a flat mandatory/optional plan, which
      // consumes a dense optional wholesale. Until the mandatory/optional
      // planner probes such optionals per candidate, multi-child lifting
      // is a measured regression, so it stays opaque here.
      bool liftRankOnlyOptionals =
          !parentIsFilter && inner != nullptr
          && inner->minShouldMatch == 0 && parents.size() == 1
          && !plan.filter.empty() && plan.optional.empty()
          && plan.minShouldMatch == 0;
      bool optionalsAllowed = parentIsFilter
          || (inner != nullptr
              && (inner->optional.empty() || liftRankOnlyOptionals));
      if (inner == nullptr || inner->minShouldMatch != 0 || !hasRequired
          || !optionalsAllowed) {
        i++;
        continue;
      }

      ClauseList replacement;
      if (parentIsFilter) {
        replacement.insert(replacement.end(), inner->mandatory.begin(), inner->mandatory.end());
        replacement.insert(replacement.end(), inner->filter.begin(), inner->filter.end());
      } else {
        for (Query* query : inner->mandatory) {
          replacement.push_back(scoringClause(pool, query, child.boost));
        }
        if (liftRankOnlyOptionals) {
          for (Query* query : inner->optional) {
            plan.optional.push_back(
                scoringClause(pool, query, child.boost));
          }
        }
        plan.filter.insert(plan.filter.end(), inner->filter.begin(), inner->filter.end());
      }
      plan.prohibited.insert(plan.prohibited.end(), inner->prohibited.begin(),
                             inner->prohibited.end());

      parents.erase(parents.begin() + (ptrdiff_t) i);
      parents.insert(parents.begin() + (ptrdiff_t) i,
                     replacement.begin(), replacement.end());
      plan.ruleMask |= R1_REQUIRED_INLINE;
      // Re-examine the replacement (or the clause shifted into this slot).
    }
  }

  // A bare match-all in a required list is the conjunction's identity.
  // scoreProfile() treats AUTO_UNIFORM mandatory clauses as membership-only,
  // so removal never changes scores. The last required clause always stays:
  // a sole match-all IS the domain (browse-all, complement carrier), and
  // optionals never gate matching once any required clause exists, so
  // removing their match-all domain would change the match set. Wrapped
  // match-alls (Boost/ConstantScore) are explicit scores and stay.
  static void eliminateMatchAll(NormalizedBoolean& plan) {
    auto sweep = [&plan](ClauseList& list) {
      size_t i = 0;
      while (i < list.size()) {
        if (plan.mandatory.size() + plan.filter.size() <= 1) return;
        if (list[i]->getKind() == QueryKind::ALL) {
          list.erase(list.begin() + (ptrdiff_t) i);
          plan.ruleMask |= R5_MATCH_ALL_ELIMINATE;
          continue;
        }
        i++;
      }
    };
    sweep(plan.filter);
    sweep(plan.mandatory);
  }

  static void flattenDisjunctions(NormalizedBoolean& plan, MemPool& pool) {
    if (plan.minShouldMatch > 1) return;
    size_t i = 0;
    while (i < plan.optional.size()) {
      TransparentBoolean child = transparentBoolean(
        plan.optional[i], /*allowConstantScore=*/false);
      BooleanQuery* inner = child.query;
      bool pureDisjunction = inner != nullptr && !inner->optional.empty()
        && inner->mandatory.empty() && inner->prohibited.empty() && inner->filter.empty()
        && inner->minShouldMatch <= 1;
      if (!pureDisjunction) {
        i++;
        continue;
      }

      ClauseList replacement;
      for (Query* query : inner->optional) {
        replacement.push_back(scoringClause(pool, query, child.boost));
      }
      plan.optional.erase(plan.optional.begin() + (ptrdiff_t) i);
      plan.optional.insert(plan.optional.begin() + (ptrdiff_t) i,
                           replacement.begin(), replacement.end());
      plan.ruleMask |= R2_DISJUNCTION_FLATTEN;
      // Re-examine the first inserted clause for nested disjunctions.
    }
  }

  NormalizedBoolean normalize(MemPool& pool) const {
    NormalizedBoolean plan(*this);

    normalizeRequiredList(plan, plan.mandatory, /*parentIsFilter=*/false, pool);
    normalizeRequiredList(plan, plan.filter, /*parentIsFilter=*/true, pool);
    eliminateMatchAll(plan);
    flattenDisjunctions(plan, pool);

    // A sole required disjunction can become the positive side beside filters
    // or exclusions. Multiple required clauses retain their boundaries so the
    // conjunction planner can consume each clause's decomposable structure.
    if (plan.optional.empty() && plan.mandatory.size() == 1) {
      for (size_t i = 0; i < plan.mandatory.size(); i++) {
        TransparentBoolean child = transparentBoolean(
          plan.mandatory[i], /*allowConstantScore=*/false);
        BooleanQuery* inner = child.query;
        bool pureDisjunction = inner != nullptr && !inner->optional.empty()
          && inner->mandatory.empty() && inner->prohibited.empty() && inner->filter.empty();
        if (!pureDisjunction) continue;

        plan.mandatory.erase(plan.mandatory.begin() + (ptrdiff_t) i);
        for (Query* query : inner->optional) {
          plan.optional.push_back(scoringClause(pool, query, child.boost));
        }
        plan.minShouldMatch = std::max(1, inner->minShouldMatch);
        plan.ruleMask |= R3_REQUIRED_DISJUNCTION_HOIST;
        flattenDisjunctions(plan, pool);
        break;
      }
    }

    // Pure negation is an engine-level complement. Seed a required match-all;
    // its AUTO_UNIFORM profile makes it required-but-non-scoring. This covers
    // raw API trees; expr/simple_query already emit the equivalent carrier.
    // Keep minShouldMatch unchanged so deliberately impossible direct-
    // construction values remain impossible.
    if (plan.mandatory.empty() && plan.optional.empty() && plan.filter.empty()
        && !plan.prohibited.empty()) {
      auto* seed = pool.make<AllQuery>();
      seed->markPlannerGenerated();
      plan.mandatory.push_back(seed);
    }

    if (plan.prohibited.empty() && plan.filter.empty()) {
      if (plan.minShouldMatch == 0 && plan.mandatory.size() == 1
          && plan.optional.empty()
          && luxir::scoreProfile(*plan.mandatory[0]).kind
              != ScoreProfile::Kind::AUTO_UNIFORM) {
        plan.singleChild = plan.mandatory[0];
      } else if (plan.minShouldMatch <= 1 && plan.mandatory.empty()
                 && plan.optional.size() == 1) {
        plan.singleChild = plan.optional[0];
      }
      if (plan.singleChild != nullptr) {
        plan.ruleMask |= R4_SINGLE_CLAUSE_UNWRAP;
      }
    }
    return plan;
  }

  struct PeeledClause {
    Query* core;
    float boost;
  };

  static PeeledClause peelScoringClause(Query* query) {
    float boost = 1.0f;
    Query* core = luxir::peelBoost(query, boost);
    return {core, boost};
  }

  struct IdentityHash {
    using is_avalanching = void;

    size_t operator()(uint64_t value) const { return (size_t) value; }
  };

  struct DuplicateSlot {
    size_t representative;
    float boost = 0.0f;
    int32_t count = 1;
  };

  struct DuplicateGroups {
    boost::container::small_vector<int32_t, 16> byClause;
    boost::container::small_vector<DuplicateSlot, 16> slots;
    int32_t duplicateSlots = 0;
  };

  static DuplicateGroups findDuplicateGroups(
      MemPool& pool, std::span<Query*> clauses, bool sumBoosts) {
    DuplicateGroups result;
    result.byClause.resize(clauses.size(), -1);
    if (clauses.size() < 2) return result;

    using Entry = std::pair<const uint64_t, int32_t>;
    using SlotByHash = boost::unordered_flat_map<
        uint64_t, int32_t, IdentityHash, std::equal_to<uint64_t>,
        MemPool::allocator<Entry>>;
    SlotByHash slotByHash(
        0, IdentityHash{}, std::equal_to<uint64_t>{},
        MemPool::allocator<Entry>(pool));
    slotByHash.reserve(clauses.size());
    result.slots.reserve(clauses.size());
    for (size_t i = 0; i < clauses.size(); i++) {
      PeeledClause peeled = peelScoringClause(clauses[i]);
      uint64_t hash = peeled.core->hash();
      auto found = slotByHash.find(hash);
      if (found == slotByHash.end()) {
        int32_t slot = (int32_t) result.slots.size();
        slotByHash.emplace(hash, slot);
        result.slots.push_back({i, peeled.boost, 1});
        continue;
      }
      int32_t slot = found->second;
      DuplicateSlot& duplicate = result.slots[(size_t) slot];
      Query* representative =
          peelScoringClause(clauses[duplicate.representative]).core;
      if (!representative->equals(*peeled.core)) continue;
      if (duplicate.count == 1) {
        result.byClause[duplicate.representative] = slot;
        result.duplicateSlots++;
      }
      result.byClause[i] = slot;
      duplicate.count++;
      if (sumBoosts) {
        duplicate.boost += peeled.boost;
        if (!std::isfinite(duplicate.boost)) {
          throw std::runtime_error(
              "summed duplicate-clause boost must be finite");
        }
      }
    }
    return result;
  }

  // Request-plan duplicate-clause normalization. Lucene dedups at rewrite,
  // not query creation; a Luxir query tree is built per request and has no
  // rewrite phase, so the serial logical plan is the equivalent seam. It
  // keeps query objects untouched, persists merged clones in the request pool,
  // and is shared by every Weight created for this Query.
  // Luxir intentionally sums boosts: Lucene's computeQueryTermWeight only
  // saturates duplicate qtf when BM25 k3 is configured, the default is linear
  // (equal to boost summing), and Luxir has no qtf hook.
  //
  // TODO: full duplicate removal ACROSS the mandatory/optional/prohibited
  // lists (an optional clause duplicating a mandatory one folds its boost
  // into the mandatory clause; a prohibited duplicate of a required clause
  // matches nothing).
  static std::span<Query*> mergeDuplicateScoringClauses(
      luxir::MemPool& pool, std::span<Query*> clauses,
      int32_t* removedOut = nullptr) {
    DuplicateGroups duplicates = findDuplicateGroups(pool, clauses, true);
    if (duplicates.duplicateSlots == 0) {
      if (removedOut != nullptr) *removedOut = 0;
      return clauses;
    }

    boost::container::small_vector<Query*, 16> out;
    for (size_t i = 0; i < clauses.size(); i++) {
      int32_t group = duplicates.byClause[i];
      if (group < 0) {
        out.push_back(clauses[i]);
        continue;
      }
      DuplicateSlot& duplicate = duplicates.slots[(size_t) group];
      if (i != duplicate.representative) continue;
      Query* core = peelScoringClause(clauses[i]).core;
      if (core->getKind() == QueryKind::TERM) {
        auto* term = static_cast<TermQuery*>(core);
        core = pool.make<TermQuery>(
            term->getField(), term->getTerm(), duplicate.boost,
            term->shouldUseFrontierBound());
        core->markPlannerGenerated();
      } else {
        core = scoringClause(pool, core, duplicate.boost);
      }
      out.push_back(core);
    }
    if (removedOut != nullptr) {
      *removedOut = (int32_t) (clauses.size() - out.size());
    }
    auto* kept = pool.make_arr<Query*>(out.size());
    std::copy(out.begin(), out.end(), kept);
    return {kept, out.size()};
  }

  static std::span<Query*> dropDuplicateMembershipClauses(
      luxir::MemPool& pool, std::span<Query*> clauses) {
    DuplicateGroups duplicates = findDuplicateGroups(pool, clauses, false);
    if (duplicates.duplicateSlots == 0) return clauses;
    boost::container::small_vector<Query*, 16> out;
    for (size_t i = 0; i < clauses.size(); i++) {
      int32_t group = duplicates.byClause[i];
      if (group >= 0
          && i != duplicates.slots[(size_t) group].representative) continue;
      out.push_back(clauses[i]);
    }
    auto* kept = pool.make_arr<Query*>(out.size());
    std::copy(out.begin(), out.end(), kept);
    return {kept, out.size()};
  }

  static std::span<Query*> persistClauses(
      MemPool& pool, std::span<Query*> clauses) {
    if (clauses.empty()) return {};
    auto* persisted = pool.make_arr<Query*>(clauses.size());
    std::copy(clauses.begin(), clauses.end(), persisted);
    return {persisted, clauses.size()};
  }

  CompiledBoolean& logicalPlan(PlanningContext& context) const {
    return context.getOrCreateLogicalPlan<CompiledBoolean>(*this, [&]() {
      NormalizedBoolean normalized = normalize(context.pool);
      auto* compiled = context.pool.make<CompiledBoolean>();
      compiled->singleChild = normalized.singleChild;
      compiled->ruleMask = normalized.ruleMask;
      compiled->minShouldMatch = normalized.minShouldMatch;
      if (compiled->singleChild != nullptr) return compiled;

      auto mandatoryClauses = mergeDuplicateScoringClauses(
          context.pool, normalized.mandatory);
      int32_t removedOptional = 0;
      auto optionalClauses = mergeDuplicateScoringClauses(
          context.pool, normalized.optional, &removedOptional);
      auto prohibitedClauses = dropDuplicateMembershipClauses(
          context.pool, normalized.prohibited);
      auto filterClauses = dropDuplicateMembershipClauses(
          context.pool, normalized.filter);

      // Luxir-defined min_match semantics under duplicate removal, split by
      // the intent the value expresses (Lucene instead refuses to dedup when
      // min_match > 1 and lets duplicates satisfy multiple match slots):
      // - Above half the raw clauses, preserve the miss budget.
      // - At or below half, preserve the absolute distinct-term count.
      if (compiled->minShouldMatch > 1 && removedOptional > 0) {
        if ((int64_t) compiled->minShouldMatch * 2
            > (int64_t) normalized.optional.size()) {
          compiled->minShouldMatch = std::max(
              1, compiled->minShouldMatch - removedOptional);
        } else {
          compiled->minShouldMatch = std::min(
              compiled->minShouldMatch, (int32_t) optionalClauses.size());
        }
      }

      // NormalizedBoolean owns small_vector storage. Persist every final span
      // in the request pool so all Weights for this Query share one plan.
      compiled->mandatory = persistClauses(context.pool, mandatoryClauses);
      compiled->optional = persistClauses(context.pool, optionalClauses);
      compiled->prohibited = persistClauses(context.pool, prohibitedClauses);
      compiled->filter = persistClauses(context.pool, filterClauses);
      return compiled;
    });
  }

public:
  static inline bool disableBulkDomainDriveForTests = false;
  static inline bool disableWindowDispatchForTests = false;
  static inline bool disablePartitionLatchForTests = false;
  static inline bool disableMandOptBulkForTests = false;
  // Test hook: route scored top-k queries with direct window filters through
  // the pull ConjunctionScorer instead of the window-filter bulk scorer.
  static inline bool disableFilteredScoredBulkForTests = false;
  // Test hook: admit a filtered MandOpt bulk whose mandatory scorer only has
  // the scalar fillScoreBlock fallback below its measured density crossover.
  static inline bool disableFilteredMandOptFillGateForTests = false;
  // Test hook: retain the body-led exact MandOpt pass instead of composing
  // the cost-ordered required/filter candidate feed with optional scoring.
  static inline bool disableExactFilteredMandOptCompositionForTests =
      std::getenv("LUXIR_DISABLE_EXACT_FILTERED_MANDOPT_COMPOSITION") !=
      nullptr;
  // Test hook: fill filter masks instead of probing candidate docs.
  static inline bool disableFilterMaskProbeForTests = false;
  // Test hook: enumerate all terms instead of using the count identity for
  // skewed term disjunctions.
  static inline bool disableDisjunctionCountIdentityForTests = false;
  // Test hook: use the pull DisjunctionScorer for sparse filtered term unions
  // instead of head/tail WAND candidate formation.
  static inline bool disableFilteredUnionWandForTests = false;
  // Test hook: use pull MandNot for prohibited scored disjunctions.
  static inline bool disableBulkExclusionForTests = false;
  // Test hook: bypass prohibited membership whose benefit comes from removing
  // verification rather than crossing the density router.
  static inline bool disableProhibitedVerificationCacheForTests = false;
  // Test hook: route unscored filter suppliers by density instead of admitting
  // exact filters as exhaustive conjunction clauses.
  static inline bool disableFilterClauseCountForTests = false;
  // Test hook: route exact scored filter suppliers by density instead of
  // admitting exact filters as exhaustive conjunction clauses.
  static inline bool disableExactFilterCachePolicyForTests = false;
  // Test hook: use conjunction/pull execution for exact filtered term
  // disjunctions instead of batching candidates from the filter feed.
  static inline bool disableFilteredDisjunctionBatchForTests = false;
  // Test hook: iterate an array-backed filter through its virtual DocSet
  // scorer instead of consuming its sorted span directly.
  static inline bool disableFilteredDisjunctionArrayFeedForTests = false;
  // Test hook: iterate a term through its virtual scorer instead of copying
  // decoded postings blocks into the filtered-disjunction candidate batch.
  static inline bool disableFilteredDisjunctionPostingsBlockGatherForTests =
      false;
  // Test hook: use body-led mask or pull execution for scored filtered term
  // conjunctions instead of candidate batching.
  static inline bool disableFilteredConjunctionBatchForTests = false;
  // Test hook: prevent a term filter from owning the candidate postings
  // feed. A scored term may still own the feed.
  static inline bool disableFilteredConjunctionPostingsFeedForTests = false;
  // Test/bench hook: retain an enclosing filter as a post-body WindowFilter
  // instead of admitting its supplier into exact COUNT conjunction planning.
  static inline bool disableIntegratedFilteredCountForTests =
      std::getenv("LUXIR_DISABLE_INTEGRATED_FILTERED_COUNT") != nullptr;
  // Test/bench hook: force dense/candidate/pull exact conjunction execution
  // instead of the direct-term docs-only bulk scorer.
  static inline bool disableExactTermCountForTests =
      std::getenv("LUXIR_DISABLE_EXACT_TERM_COUNT") != nullptr;
  // Test hook: use body-led execution for multi-term exact filtered
  // conjunctions.
  static inline bool disableFilteredConjMultiTermForTests = false;
  // Test hook: keep the filter-owned route when a scored term could own
  // the candidate feed.
  static inline bool disableCandidateTermFeedForTests =
      std::getenv("LUXIR_DISABLE_CANDIDATE_TERM_FEED") != nullptr;
  // Candidate batching requires the cheapest body clause to be sufficiently
  // more expensive than the filter feed. A cached DocSet competes with a
  // cheap window mask and therefore requires the stricter ratio.
  static inline int64_t multiTermBatchMinRatioDocSet = 4;
  static inline int64_t multiTermBatchMinRatioPostings = 2;
  // A term-owned candidate feed is limited to terms matching at most
  // maxDoc/32, which bounds candidate materialization to sparse scoring terms.
  static constexpr int64_t kTermFeedMaxLeadFraction = 32;
  static inline int64_t kTermFeedMaxLeadFractionForTests =
      kTermFeedMaxLeadFraction;
  // Window-fill consumption converts per-candidate verification into
  // per-span decoding. Demand.span = min(maxDoc,
  // Demand.candidates * WINDOW_SIZE) bounds the worst-case fill span: every
  // surviving other-side doc may populate one full window, saturated at the
  // segment size. Materialization is therefore admitted whenever that span
  // could exceed the fence, leaving sparse verification only for provably
  // tiny other sides below fence/4096. Known accepted cost: conjunctions whose
  // true intermediate is far below the cheapest single clause (for example,
  // +sarah +fisher has an intermediate of 195 but minOther of 10458) re-pay
  // the bounded one-time materialization, about 3.4ms at r50, because this
  // bound cannot observe the intermediate. A selectivity-product exposure
  // estimate is the enumerated future refinement. Measured with gcc-release
  // on a 16-core Zen 3 desktop, 2026-08-11, with the quick-board A/B as the
  // confirming oracle.
  static constexpr int64_t WINDOW_FILL_SPAN_SCALE =
      DocsEnumMeta::L1_DOCS;
  static inline int64_t windowFillSpanScaleForTests =
      WINDOW_FILL_SPAN_SCALE;
  // A cached DocSet has a cheap mask route, so a term may own the candidate
  // feed only when it is materially sparser than the filter. A postings filter
  // would otherwise need a per-query mask fill and does not use this gate.
  static inline int64_t kTermFeedMinDocSetFilterRatio = 3;
  // Test hook: retain unmatched candidates between term clauses in the
  // count-only filtered-disjunction batch instead of compacting them.
  static inline bool disableFilteredDisjunctionCountCompactionForTests = false;
  // Scored exact filtered disjunction batching is limited to filters matching
  // at most maxDoc/44. Unscored count uses this batch only when its existing
  // dense route declines.
  static constexpr int32_t kFilteredDisjunctionBatchDensityInverse = 44;
  static inline int32_t filteredDisjunctionBatchDensityInverseForTests =
      kFilteredDisjunctionBatchDensityInverse;
  // Test hook: use eager single-phase clauses instead of deferring exact
  // matches until all approximations agree.
  static inline bool disableTwoPhaseForTests = false;
  // Pull-conjunction refinement band: refine bounds to block granularity when
  // theta reaches this fraction of the group-granular range bound.
  static constexpr double kRefineBeta = 0.75;
  static constexpr size_t kCostAwareOrderMinClauses = 4;
  static float optionalUpperBound(float upper) {
    if (std::isnan(upper)) {
      return std::numeric_limits<float>::infinity();
    }
    return std::max(0.0f, upper);
  }
  static ScoreBounds optionalScoreBounds(ScoreBounds bounds) {
    bounds.lo = std::min(0.0f, bounds.lo);
    bounds.hi = optionalUpperBound(bounds.hi);
    return bounds;
  }
  static float addScoreBound(float left, float right, bool upper) {
    float sum = left + right;
    if (std::isnan(sum)) {
      return upper ? std::numeric_limits<float>::infinity()
                   : -std::numeric_limits<float>::infinity();
    }
    if (!std::isfinite(sum)) {
      return sum;
    }
    return std::nextafter(
        sum, upper ? std::numeric_limits<float>::infinity()
                   : -std::numeric_limits<float>::infinity());
  }
  static ScoreBounds addScoreBounds(ScoreBounds left, ScoreBounds right) {
    return {addScoreBound(left.lo, right.lo, false),
            addScoreBound(left.hi, right.hi, true)};
  }
  static ScoreBounds sumRequiredScoreBounds(std::span<Query::Scorer*> scorers,
                                            int32_t upTo, bool refined) {
    if (scorers.empty()) {
      return ScoreBounds::exact(0.0f);
    }
    ScoreBounds sum = refined ? scorers[0]->refineScoreBounds(upTo)
                              : scorers[0]->getScoreBounds(upTo);
    for (size_t i = 1; i < scorers.size(); i++) {
      sum = addScoreBounds(
          sum, refined ? scorers[i]->refineScoreBounds(upTo)
                       : scorers[i]->getScoreBounds(upTo));
    }
    return sum;
  }
  static ScoreBounds sumOptionalScoreBounds(std::span<Query::Scorer*> scorers,
                                            int32_t upTo, bool refined) {
    if (scorers.empty()) {
      return ScoreBounds::exact(0.0f);
    }
    ScoreBounds sum = optionalScoreBounds(
        refined ? scorers[0]->refineScoreBounds(upTo)
                : scorers[0]->getScoreBounds(upTo));
    for (size_t i = 1; i < scorers.size(); i++) {
      sum = addScoreBounds(
          sum, optionalScoreBounds(
              refined ? scorers[i]->refineScoreBounds(upTo)
                      : scorers[i]->getScoreBounds(upTo)));
    }
    return sum;
  }
  // Scored top-k pull-versus-bulk routing depends on filter density. Below
  // maxDoc/kMaskFilterDensityInverse, the filter is selective enough to lead a
  // pull conjunction. Denser filters admit filtered bulk execution, which
  // chooses mask fill or per-candidate probing separately.
  static constexpr int64_t kMaskFilterDensityInverse = luxir::kMaskFilterDensityInverse;
  // Relative cost of one monotonic filter advance vs streaming one filter
  // posting. Probe when leadCost * this weight is below filterCost.
  static constexpr int64_t kMaskProbeAdvanceWeight = 6;
  // Probing pays one scalar advance per candidate instead of filling the
  // filter's bits for the window. Require the filter to match at least
  // maxDoc/8; sparser filters are cheap enough to stream into a mask.
  static constexpr int64_t kMaskProbeMinFilterDensityInverse = 8;
  // MandOpt bulk drains its mandatory scorer through fillScoreBlock. A direct
  // term has a block-native implementation and keeps the general /256 filter
  // crossover. A compound mandatory scorer using the scalar default needs a
  // substantially denser filter before streaming the body wins:
  //   filter density    pull / bulk, +(climate policy)^3 report
  //   1.02%             0.49-0.57
  //   2.16%             0.79-0.85
  //   3.95%             0.94-1.03
  //   4.98%             0.84-0.92
  //   7.96%             1.16-1.32
  // Results hold across TOP_10/100/1000/TOP_100_COUNT and cached/uncached
  // filters. /16 places every non-marginal point on its winning side.
  static constexpr int64_t kMandOptScalarFillDensityInverse = 16;

  static bool filterDensityBelow(int64_t cost, int32_t maxDoc,
                                 int64_t densityInverse) noexcept {
    assert(cost >= 0);
    assert(maxDoc >= 0);
    assert(densityInverse > 0);
    return maxDoc > 0
        && cost <= ((int64_t) maxDoc - 1) / densityInverse;
  }

  BooleanQuery(std::span<Query*> mandatory, std::span<Query*> optional, std::span<Query*> prohibited,
               std::span<Query*> filter, int minShouldMatch = 0)
          : Query(QueryKind::BOOLEAN), mandatory(mandatory), optional(optional),
            prohibited(prohibited), filter(filter),
            minShouldMatch(minShouldMatch) {
  }

  bool recoverableAsPredicate() const noexcept override {
    for (auto clauses : {mandatory, optional, prohibited, filter}) {
      for (auto* clause : clauses) if (!clause->recoverableAsPredicate()) return false;
    }
    return true;
  }

  bool equalsSameKind(const Query& other) const override {
    const auto& rhs = static_cast<const BooleanQuery&>(other);
    if (minShouldMatch != rhs.minShouldMatch
        || mandatory.size() != rhs.mandatory.size()
        || optional.size() != rhs.optional.size()
        || prohibited.size() != rhs.prohibited.size()
        || filter.size() != rhs.filter.size()) {
      return false;
    }
    auto sameList = [](std::span<Query*> a, std::span<Query*> b) {
      for (size_t i = 0; i < a.size(); i++) {
        if (!sameScoringClause(a[i], b[i])) return false;
      }
      return true;
    };
    return sameList(mandatory, rhs.mandatory)
        && sameList(optional, rhs.optional)
        && sameList(prohibited, rhs.prohibited)
        && sameList(filter, rhs.filter);
  }

  uint64_t hashImpl() const override {
    uint64_t value = mixHash(Query::hashImpl(), minShouldMatch);
    auto mixList = [&](std::span<Query*> clauses) {
      value = mixSampledSequence(
          value, clauses,
          [](uint64_t seed, Query* clause) {
            return mixHash(seed, scoringClauseHash(clause));
          });
    };
    mixList(mandatory);
    mixList(optional);
    mixList(prohibited);
    mixList(filter);
    return value;
  }

  VerificationWork shapeMembershipVerificationWork() const {
    bool unknown = false;
    bool partial = false;
    auto summarize = [&](std::span<Query*> clauses) {
      for (Query* clause : clauses) {
        VerificationWork work = luxir::membershipVerificationWork(*clause);
        if (work == VerificationWork::PRESENT) return true;
        partial |= work == VerificationWork::PARTIAL;
        unknown |= work == VerificationWork::UNKNOWN;
      }
      return false;
    };
    if (summarize(mandatory) || summarize(filter)) {
      return VerificationWork::PRESENT;
    }
    bool hasRequired = !mandatory.empty() || !filter.empty();
    if (minShouldMatch >= 1 || !hasRequired) {
      bool optionalPresent = summarize(optional);
      int requiredOptionals = std::max(1, minShouldMatch);
      if (optionalPresent) {
        if (requiredOptionals >= (int)optional.size()) {
          return VerificationWork::PRESENT;
        }
        partial = true;
      }
    }
    if (summarize(prohibited)) return VerificationWork::PRESENT;
    if (partial) return VerificationWork::PARTIAL;
    return unknown ? VerificationWork::UNKNOWN
                   : VerificationWork::ABSENT;
  }

  FieldSortConjunction fieldSortConjunction(
      PlanningContext& context) const override {
    CompiledBoolean& plan = logicalPlan(context);
    // A dropped-optional singleton classifies as its residue: '+a b' resolves
    // to the same execution as bare 'a', which is not a conjunction, so the
    // flat-conjunction veto must not fire on it.
    if (Query* child = plan.unscoredSingleChild()) {
      return child->fieldSortConjunction(context);
    }

    bool hasRequired = !plan.mandatory.empty() || !plan.filter.empty();
    if (!hasRequired || !plan.prohibited.empty()
        || (!plan.optional.empty() && !plan.dropsOptional(false))) {
      return FieldSortConjunction::NOT_FLAT;
    }

    bool hasOther = false;
    bool hasDynamicTerms = false;
    auto classify = [&](std::span<Query*> clauses) {
      for (Query* clause : clauses) {
        while (clause->getKind() == QueryKind::BOOST) {
          clause = static_cast<BoostQuery*>(clause)->getChild();
        }
        while (clause->getKind() == QueryKind::CONSTANT_SCORE) {
          clause = static_cast<ConstantScoreQuery*>(clause)->getChild();
          while (clause->getKind() == QueryKind::BOOST) {
            clause = static_cast<BoostQuery*>(clause)->getChild();
          }
        }
        if (clause->getKind() == QueryKind::TERM) continue;
        QueryKind clauseKind = clause->getKind();
        if (clauseKind == QueryKind::AUTOMATON
            || clauseKind == QueryKind::PREFIX
            || clauseKind == QueryKind::TERM_RANGE
            || clauseKind == QueryKind::TERM_IN_SET) {
          hasDynamicTerms = true;
        } else {
          hasOther = true;
        }
      }
    };
    classify(plan.mandatory);
    classify(plan.filter);
    if (hasDynamicTerms) return FieldSortConjunction::DYNAMIC_TERMS;
    return hasOther ? FieldSortConjunction::NOT_FLAT
                    : FieldSortConjunction::FLAT_LITERAL_CONJUNCTION;
  }

  // Every match satisfies every MUST and FILTER clause, so their envelopes
  // intersect. Optional clauses (even under minShouldMatch) and prohibited
  // clauses require nothing of a single field here. Normalization only
  // rearranges the same constraints, so the raw tree answers for it.
  std::optional<ValueEnvelope> requiredValueEnvelope(
      std::string_view field) const override {
    std::optional<ValueEnvelope> result;
    for (auto clauses : {mandatory, filter}) {
      for (Query* clause : clauses) {
        auto envelope = clause->requiredValueEnvelope(field);
        if (!envelope.has_value()) continue;
        result = result.has_value() ? result->intersect(*envelope)
                                    : *envelope;
      }
    }
    return result;
  }

  bool shapeCanOmitWeightForCacheFirstMembership() const {
    auto allSupported = [](std::span<Query*> clauses) {
      return std::all_of(
          clauses.begin(), clauses.end(), [](Query* clause) {
            return luxir::canOmitWeightForCacheFirstMembership(*clause);
          });
    };
    bool dropOptional = (!mandatory.empty() || !filter.empty())
        && minShouldMatch < 1;
    return allSupported(mandatory) && allSupported(prohibited)
        && allSupported(filter)
        && (dropOptional || allSupported(optional));
  }

  bool shapeHasRequiredConjunction() const {
    return mandatory.size() + filter.size() > 1;
  }

  bool shapeDirectCountAvailable(IndexReader& reader) const {
    if (prohibited.empty() && filter.empty()) {
      if (mandatory.size() == 1 && optional.empty()) {
        return luxir::directCountAvailable(*mandatory[0], reader);
      }
      if (mandatory.empty() && optional.size() == 1) {
        return luxir::directCountAvailable(*optional[0], reader);
      }
      if (mandatory.empty() && optional.empty()) return true;
    }
    return false;
  }

  void validateLogicalImpl(
      PlanningContext& context, float multiplier = 1.0f) const override {
    checkedBoostProduct(multiplier, 1.0f);
    // Build the logical plan here so its errors surface as validation
    // errors; nodes it generates are marked and skip the validation check.
    logicalPlan(context);

    auto validate = [&](std::span<Query*> clauses, float childMultiplier) {
      for (Query* clause : clauses) {
        clause->validateLogical(context, childMultiplier);
      }
    };
    validate(mandatory, multiplier);
    validate(optional, multiplier);
    validate(prohibited, 1.0f);
    validate(filter, 1.0f);
  }

  FilterKeyScope appendFilterKey(FilterKeyBuilder& out,
                                 const FilterKeyContext& ctx) const override {
    out.appendKind(kind);
    out.appendInt32(minShouldMatch);
    FilterKeyScope scope = FilterKeyScope::SEGMENT_STABLE;
    auto appendRole = [&](FilterKeyTag role, std::span<Query*> clauses) {
      out.appendTag(role);
      out.appendSize(clauses.size());
      for (Query* clause : clauses) {
        scope = strongestFilterKeyScope(
            scope, clause->appendFilterKey(out, ctx));
      }
    };
    appendRole(FilterKeyTag::BOOLEAN_MANDATORY, mandatory);
    bool dropOptional = (!mandatory.empty() || !filter.empty())
        && minShouldMatch < 1;
    appendRole(FilterKeyTag::BOOLEAN_OPTIONAL,
               dropOptional ? std::span<Query*>{} : optional);
    appendRole(FilterKeyTag::BOOLEAN_PROHIBITED, prohibited);
    appendRole(FilterKeyTag::BOOLEAN_FILTER, filter);
    return scope;
  }

  ScoreProfile shapeScoreProfile() const {
    float sum = 0.0f;
    bool explicitScore = false;

    // AUTO_UNIFORM mandatory clauses are membership-only. Variable and
    // explicit mandatory clauses contribute normally.
    for (Query* clause : mandatory) {
      ScoreProfile profile = luxir::scoreProfile(*clause);
      if (profile.kind == ScoreProfile::Kind::VARIABLE) {
        return ScoreProfile::variable();
      }
      if (profile.kind == ScoreProfile::Kind::EXPLICIT_UNIFORM) {
        sum += profile.value;
        explicitScore = true;
      }
    }

    if (optional.empty()) {
      return explicitScore ? ScoreProfile::explicitUniform(sum)
                           : ScoreProfile::automatic(sum);
    }

    int requiredOptionals = minShouldMatch;
    if (mandatory.empty() && filter.empty()) {
      requiredOptionals = std::max(1, requiredOptionals);
    }
    bool everyOptionalRequired = requiredOptionals >= (int)optional.size();
    if (!everyOptionalRequired) {
      // A non-zero optional contribution is data-dependent unless every
      // optional must match. Zero-uniform optionals do not affect the sum.
      for (Query* clause : optional) {
        ScoreProfile profile = luxir::scoreProfile(*clause);
        if (profile.kind == ScoreProfile::Kind::VARIABLE
            || profile.value != 0.0f) {
          return ScoreProfile::variable();
        }
        explicitScore |= profile.kind == ScoreProfile::Kind::EXPLICIT_UNIFORM;
      }
      return explicitScore ? ScoreProfile::explicitUniform(sum)
                           : ScoreProfile::automatic(sum);
    }

    for (Query* clause : optional) {
      ScoreProfile profile = luxir::scoreProfile(*clause);
      if (profile.kind == ScoreProfile::Kind::VARIABLE) {
        return ScoreProfile::variable();
      }
      sum += profile.value;
      explicitScore |= profile.kind == ScoreProfile::Kind::EXPLICIT_UNIFORM;
    }
    return explicitScore ? ScoreProfile::explicitUniform(sum)
                         : ScoreProfile::automatic(sum);
  }

  NormalizeTestView normalizationForTest(MemPool& pool) const {
    NormalizedBoolean plan = normalize(pool);
    NormalizeTestView view;
    view.mandatoryCount = plan.mandatory.size();
    view.optionalCount = plan.optional.size();
    view.prohibitedCount = plan.prohibited.size();
    view.filterCount = plan.filter.size();
    view.minShouldMatch = plan.minShouldMatch;
    view.ruleMask = plan.ruleMask;
    if (plan.singleChild != nullptr) {
      view.singleChildType = typeid(*plan.singleChild);
    }
    for (Query* query : plan.mandatory) view.mandatoryTypes.emplace_back(typeid(*query));
    for (Query* query : plan.optional) view.optionalTypes.emplace_back(typeid(*query));
    for (Query* query : plan.prohibited) view.prohibitedTypes.emplace_back(typeid(*query));
    for (Query* query : plan.filter) view.filterTypes.emplace_back(typeid(*query));
    return view;
  }

  CompiledPlanTestView compiledPlanForTest(Context& context) const {
    CompiledBoolean& plan = logicalPlan(context.planningContext());
    return {
      .identity = &plan,
      .mandatory = {plan.mandatory.data(), plan.mandatory.size()},
      .optional = {plan.optional.data(), plan.optional.size()},
      .prohibited = {plan.prohibited.data(), plan.prohibited.size()},
      .filter = {plan.filter.data(), plan.filter.size()},
    };
  }

  Query::Weight* createWeight(Context& context, int32_t flags,
                              float multiplier = 1.0f) override;

  struct ConjunctionClauseLayout {
    Query::ClauseShape windowFillKind = Query::ClauseShape::NONE;
    std::span<Query::Scorer*> denseMembers;
    TermQuery::Scorer* directTerm = nullptr;
    QueryPrep::DocSetScorer* directDocSet = nullptr;
    std::span<Query::Scorer*> termMembers;
  };

  // Execution: BooleanQuery::Weight and the scorers it builds are defined in
  // BooleanWeight.h, which only BooleanQuery.cpp and white-box tests include.
  class Weight;
  class MandOptScorer;
  class FilterOnlyBulkScorer;
  class MandOptBulkScorer;
  class OptionalScoreBulkScorer;
  class MandNotScorer;
  class ConjunctionScorer;
  struct PostingsCandidateBatch;
  class ExactDocsOnlyTermConjunctionBulkScorer;
  class ConjunctionBulkScorer;
  class DisjunctionScorer;
  class MaxScoreDisjunctionScorer;
  class FilteredDisjunctionBulkScorer;
  class MaxScoreBulkScorer;
  class MinShouldMatchWandScorer;
  class MinShouldMatchScorer;

  // Candidate-batch helpers for the bulk scorers.
  static PostingsCandidateBatch copyPostingsCandidates(
      DocsFreqEnum& docsEnum, std::span<int32_t> outDocs,
      int32_t nextDoc, int32_t min, int32_t upTo);
  static PostingsCandidateBatch copyPostingsCandidates(
      DocsOnlyEnum& docsEnum, std::span<int32_t> outDocs,
      int32_t nextDoc, int32_t min, int32_t upTo);
  static void applyDomainBitsToWindow(std::span<uint64_t> windowBits,
                                      int32_t windowStart, int32_t windowEnd,
                                      const FixedBitSet* domainBits);
  static void applyDocSetToWindow(std::span<uint64_t> windowBits,
                                  int32_t windowStart, int32_t windowEnd,
                                  DocSet* filter);
};

} // namespace luxir

// Boolean shape helpers above call the free dispatch functions.
// Include guards make this safe when QueryShape.h includes BooleanQuery.h.
#include "QueryShape.h"
