#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <typeindex>

#include "Query.h"
#include "AllQuery.h"
#include "BoostQuery.h"
#include "ConstantScoreQuery.h"
#include "MultiTermQuery.h"
#include "NumericRangeQuery.h"
#include "PhraseQuery.h"
#include "TermQuery.h"
#include "ScoreCompact.h"
#include "solux/reader/SkipStats.h"
#include "QueryPrep.h"
#include "solux/util/screaming.h"

namespace solux {

class BooleanQuery final : public solux::Query {
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

  struct TransparentBoolean {
    BooleanQuery* query = nullptr;
    float boost = 1.0f;
  };

  static TransparentBoolean transparentBoolean(Query* query,
                                                bool allowConstantScore) {
    float boost = 1.0f;
    while (true) {
      if (auto* wrapped = dynamic_cast<BoostQuery*>(query)) {
        boost = checkedBoostProduct(boost, wrapped->getBoost());
        query = wrapped->getChild();
        continue;
      }
      if (allowConstantScore) {
        if (auto* wrapped = dynamic_cast<ConstantScoreQuery*>(query)) {
          query = wrapped->getChild();
          continue;
        }
      }
      break;
    }
    return {dynamic_cast<BooleanQuery*>(query), boost};
  }

  static Query* scoringClause(MemPool& pool, Query* query, float boost) {
    return boost == 1.0f ? query : pool.make<BoostQuery>(query, boost);
  }

  static bool isPureNegative(const BooleanQuery& query) {
    return query.mandatory.empty() && query.optional.empty()
        && !query.prohibited.empty() && query.filter.empty()
        && query.minShouldMatch <= 1;
  }

  static bool isComplementForm(const BooleanQuery& query) {
    bool optionalCarrier = query.mandatory.empty()
        && query.optional.size() == 1
        && dynamic_cast<AllQuery*>(query.optional[0]) != nullptr;
    bool requiredCarrier = query.optional.empty()
        && query.mandatory.size() == 1
        && dynamic_cast<AllQuery*>(query.mandatory[0]) != nullptr;
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
      // outer filters to cost-ordered required planning. Multiple required
      // children stay opaque because lifting would reorder their score sums.
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
        if (dynamic_cast<AllQuery*>(list[i]) != nullptr) {
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
      plan.mandatory.push_back(pool.make<AllQuery>());
    }

    if (plan.prohibited.empty() && plan.filter.empty()) {
      if (plan.minShouldMatch == 0 && plan.mandatory.size() == 1
          && plan.optional.empty()
          && plan.mandatory[0]->scoreProfile().kind
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

  struct DedupableTerm {
    TermQuery* term = nullptr;
    float wrapperBoost = 1.0f;
  };

  static DedupableTerm dedupableTerm(Query* query) {
    float wrapperBoost = 1.0f;
    while (auto* wrapped = dynamic_cast<BoostQuery*>(query)) {
      wrapperBoost = checkedBoostProduct(wrapperBoost, wrapped->getBoost());
      query = wrapped->getChild();
    }
    auto* term = dynamic_cast<TermQuery*>(query);
    if (term == nullptr || term->hasInjectedStats()) return {};
    return {term, wrapperBoost};
  }

  static bool sameTermIdentity(const TermQuery& lhs, const TermQuery& rhs) {
    return lhs.shouldUseFrontierBound() == rhs.shouldUseFrontierBound()
        && lhs.getField() == rhs.getField() && lhs.getTerm() == rhs.getTerm();
  }

  // Weight-time duplicate-clause normalization. Lucene dedups at rewrite, not
  // query creation; a Solux query tree is built per request and has no rewrite
  // phase, so weight creation is the equivalent seam - it keeps query objects
  // untouched (merges clone into the request pool) and sits where the
  // similarity would be consulted if query-term weighting ever became a knob.
  // Solux intentionally sums boosts: Lucene's computeQueryTermWeight only
  // saturates duplicate qtf when BM25 k3 is configured, the default is linear
  // (equal to boost summing), and Solux has no qtf hook.
  //
  // TODO: replace the O(n^2) scans with hashing, extend identity beyond
  // TermQuery (needs Query equality), and do full duplicate removal ACROSS
  // the mandatory/optional/prohibited lists (an optional clause duplicating
  // a mandatory one folds its boost into the mandatory clause; a prohibited
  // duplicate of a required clause matches nothing).
  static std::span<Query*> mergeDuplicateScoringTerms(solux::MemPool& pool,
                                                      std::span<Query*> clauses,
                                                      int32_t* removedOut = nullptr) {
    boost::container::small_vector<Query*, 16> out;
    boost::container::small_vector<bool, 16> consumed(clauses.size(), false);
    bool changed = false;
    for (size_t i = 0; i < clauses.size(); i++) {
      if (consumed[i]) continue;
      Query* query = clauses[i];
      auto dedup = dedupableTerm(query);
      if (dedup.term != nullptr) {
        auto* term = dedup.term;
        float boost = checkedBoostProduct(dedup.wrapperBoost, term->getBoost());
        bool merged = false;
        for (size_t j = i + 1; j < clauses.size(); j++) {
          if (consumed[j]) continue;
          auto other = dedupableTerm(clauses[j]);
          if (other.term == nullptr || !sameTermIdentity(*term, *other.term)) continue;
          boost += checkedBoostProduct(other.wrapperBoost, other.term->getBoost());
          if (!std::isfinite(boost)) {
            throw std::runtime_error("summed duplicate-term boost must be finite");
          }
          consumed[j] = true;
          merged = true;
        }
        if (merged) {
          query = pool.make<TermQuery>(term->getField(), term->getTerm(), boost,
                                       term->shouldUseFrontierBound());
          changed = true;
        }
      }
      out.push_back(query);
    }
    if (removedOut != nullptr) {
      *removedOut = (int32_t) (clauses.size() - out.size());
    }
    if (!changed) {
      return clauses;
    }
    auto* kept = pool.make_arr<Query*>(out.size());
    std::copy(out.begin(), out.end(), kept);
    return {kept, out.size()};
  }

  static std::span<Query*> dropDuplicateFilterTerms(solux::MemPool& pool,
                                                    std::span<Query*> clauses) {
    boost::container::small_vector<Query*, 16> out;
    bool changed = false;
    for (size_t i = 0; i < clauses.size(); i++) {
      auto term = dedupableTerm(clauses[i]);
      bool duplicate = false;
      if (term.term != nullptr) {
        for (Query* prior : out) {
          auto priorTerm = dedupableTerm(prior);
          if (priorTerm.term != nullptr && sameTermIdentity(*priorTerm.term, *term.term)) {
            duplicate = true;
            break;
          }
        }
      }
      if (duplicate) {
        changed = true;
        continue;
      }
      out.push_back(clauses[i]);
    }
    if (!changed) {
      return clauses;
    }
    auto* kept = pool.make_arr<Query*>(out.size());
    std::copy(out.begin(), out.end(), kept);
    return {kept, out.size()};
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
      std::getenv("SOLUX_DISABLE_EXACT_FILTERED_MANDOPT_COMPOSITION") !=
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
      std::getenv("SOLUX_DISABLE_INTEGRATED_FILTERED_COUNT") != nullptr;
  // Test/bench hook: force dense/candidate/pull exact conjunction execution
  // instead of the direct-term docs-only bulk scorer.
  static inline bool disableExactTermCountForTests =
      std::getenv("SOLUX_DISABLE_EXACT_TERM_COUNT") != nullptr;
  // Test hook: use body-led execution for multi-term exact filtered
  // conjunctions.
  static inline bool disableFilteredConjMultiTermForTests = false;
  // Test hook: keep the filter-owned route when a scored term could own
  // the candidate feed.
  static inline bool disableCandidateTermFeedForTests =
      std::getenv("SOLUX_DISABLE_CANDIDATE_TERM_FEED") != nullptr;
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
  // on Fenrir, 2026-08-11, with the quick-board A/B as the confirming oracle.
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
  static constexpr int64_t kMaskFilterDensityInverse = solux::kMaskFilterDensityInverse;
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
          : mandatory(mandatory), optional(optional), prohibited(prohibited), filter(filter),
            minShouldMatch(minShouldMatch) {
  }

  FilterKeyScope appendFilterKey(FilterKeyBuilder& out,
                                 const FilterKeyContext& ctx) const override {
    out.appendTag(FilterKeyTag::BOOLEAN);
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

  ScoreProfile scoreProfile() const override {
    float sum = 0.0f;
    bool explicitScore = false;

    // AUTO_UNIFORM mandatory clauses are membership-only. Variable and
    // explicit mandatory clauses contribute normally.
    for (Query* clause : mandatory) {
      ScoreProfile profile = clause->scoreProfile();
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
        ScoreProfile profile = clause->scoreProfile();
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
      ScoreProfile profile = clause->scoreProfile();
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

  Query::Weight* createWeight(Context& context, int32_t flags,
                              float multiplier = 1.0f) override {
    NormalizedBoolean plan = normalize(context.pool);
    if (plan.singleChild != nullptr) {
      return plan.singleChild->createWeight(context, flags, multiplier);
    }
    return context.pool.make<BooleanQuery::Weight>(context, plan, flags, multiplier);
  }

  struct ConjunctionClauseLayout {
    Query::ClauseShape windowFillKind = Query::ClauseShape::NONE;
    std::span<Query::Scorer*> denseMembers;
    TermQuery::Scorer* directTerm = nullptr;
    QueryPrep::DocSetScorer* directDocSet = nullptr;
    std::span<Query::Scorer*> termMembers;
  };

  class ConjunctionBulkScorer;
  class ExactDocsOnlyTermConjunctionBulkScorer;

  class Weight final : public Query::Weight {
    std::span<Query::Weight*> mandatoryWeights;
    std::span<uint8_t> mandatoryScores;
    std::span<Query::Weight*> optionalWeights;
    std::span<Query::Weight*> prohibitedWeights;
    std::span<Query::Weight*> filterWeights;
    std::span<FilterCache::Use*> filterUses;
    int minShouldMatch = 0;
    bool needsScores = false;
    bool allowsPruning = false;
    bool sparseFilteredTopKEligible = false;
    SparseFilteredTopKFamily sparseFilteredTopKFamilyKind =
        SparseFilteredTopKFamily::CONJUNCTION;

    static bool sparseFilteredTopKMandatory(Query* query) {
      while (auto* boost = dynamic_cast<BoostQuery*>(query)) {
        query = boost->getChild();
      }
      if (dynamic_cast<TermQuery*>(query) != nullptr) {
        return true;
      }
      auto* phrase = dynamic_cast<PhraseQuery*>(query);
      return phrase != nullptr && phrase->getSlop() == 0;
    }

    static bool lessMaxScore(float a, float b) {
      bool finiteA = std::isfinite(a);
      bool finiteB = std::isfinite(b);
      if (finiteA != finiteB) return finiteA;
      return a < b;
    }

    static bool filterDensityRoutesToPull(int64_t filterCost,
                                          int32_t maxDoc) {
      return filterDensityBelow(
          filterCost, maxDoc, kMaskFilterDensityInverse);
    }

    // Match MaxScoreDisjunctionScorer's stable score order when an externally
    // driven disjunction uses the plain heap scorer instead.
    static void sortByMaxScore(MemPool& targetPool,
                               std::span<Query::Scorer*> scorers) {
      auto bounds = targetPool.make_span<float>(scorers.size());
      for (size_t i = 0; i < scorers.size(); i++) {
        bounds[i] = optionalUpperBound(
            scorers[i]->getMaxScoreForSetup(PostingsReader::END));
      }
      for (size_t i = 1; i < scorers.size(); i++) {
        Query::Scorer* scorer = scorers[i];
        float bound = bounds[i];
        size_t j = i;
        while (j > 0 && lessMaxScore(bound, bounds[j - 1])) {
          scorers[j] = scorers[j - 1];
          bounds[j] = bounds[j - 1];
          j--;
        }
        scorers[j] = scorer;
        bounds[j] = bound;
      }
    }

    // Returns a span of Weights, corresponding to the given span of Queries. Some weights can be null.
    std::span<Query::Weight*> createWeights(solux::MemPool& targetPool, Context& context,
                                            std::span<Query*> queries, int32_t flags,
                                            float multiplier) {
      if (queries.size() == 0) {
        return {};
      }
      auto weights = targetPool.make_arr<Query::Weight*>(queries.size());
      for (int i = 0; i < queries.size(); ++i) {
        weights[i] = queries[i]->createWeight(context, flags, multiplier);
      }
      return {weights, queries.size()};
    }

    void createMandatoryWeights(Context& context, std::span<Query*> queries,
                                int32_t flags, float multiplier) {
      if (queries.empty()) return;
      mandatoryWeights = context.pool.make_span<Query::Weight*>(queries.size());
      mandatoryScores = context.pool.make_span<uint8_t>(queries.size());
      int32_t noScore = flags & ~NEED_SCORES;
      bool parentNeedsScores = (flags & NEED_SCORES) != 0;
      for (size_t i = 0; i < queries.size(); i++) {
        bool contributes = parentNeedsScores
            && queries[i]->scoreProfile().kind
                != ScoreProfile::Kind::AUTO_UNIFORM;
        mandatoryScores[i] = contributes ? 1 : 0;
        mandatoryWeights[i] = queries[i]->createWeight(
            context, contributes ? flags : noScore, multiplier);
      }
    }

    // Outcome of building the required (mandatory + filter) conjunction for a
    // segment. `scorer` is null when there are no required clauses at all;
    // `unsatisfiable` is true when a required clause cannot match this segment,
    // so the whole boolean cannot match it.
    struct Required {
      Query::Scorer* scorer;
      bool unsatisfiable;
      int64_t cost;
      size_t scoringCount;
      bool twoPhase;
    };

    struct PullChildPlan {
      int64_t cost = 0;
      Query::ScorerSupplier* supplier = nullptr;
      Query::ScorerPlan* plan = nullptr;
      bool scoring = false;
    };

    static Query::PlanContext scorerBuildContext(
        const Query::Demand& demand) {
      Query::PlanContext buildContext =
          MultiTermQuery::Weight::scorerBuildContext(
              demand, PhraseQuery::ScorerControls::disableSortForTests,
              PhraseQuery::ScorerControls::disableRepeatDedupForTests,
              PhraseQuery::disableShapesForTests);
      buildContext.numericRangeDisableShapesForTests =
          NumericRangeQuery::disableShapesForTests;
      buildContext.disableBooleanTwoPhaseForTests =
          disableTwoPhaseForTests;
      buildContext.disableDisjunctionTwoPhaseForTests =
          DisjunctionScorer::disableDisjTwoPhaseForTests;
      buildContext.disableMandNotTwoPhaseForTests =
          MandNotScorer::disableNotTwoPhaseForTests;
      buildContext.disableFilteredUnionWandForTests =
          disableFilteredUnionWandForTests;
      return buildContext;
    }

    static Query::PlanContext scorerBuildContext(
        int64_t leadCost,
        Query::ExecutionUse horizon = Query::ExecutionUse::PULL) {
      return scorerBuildContext(
          Query::Demand::fromLeadCost(leadCost, horizon));
    }

    // Build the required-clause conjunction from the child plans retained by
    // the Boolean plan. Mandatory clauses score; filters only constrain
    // iteration. Resolve has already frozen the pre-expansion supplier order
    // and passed the sparsest required cost to every child plan.
    static Required assembleRequired(
        MemPool& targetPool,
        std::span<PullChildPlan> entries,
        int64_t leadCost,
        bool allowsPruning,
        bool enableTwoPhase) {
      size_t scoringCapacity = 0;
      for (const PullChildPlan& entry : entries) {
        scoringCapacity += entry.scoring ? 1 : 0;
      }
      if (entries.empty()) return {nullptr, false, 0, 0, false};

      auto* all = targetPool.make_arr<Query::Scorer*>(entries.size());
      auto* costs = targetPool.make_arr<int64_t>(entries.size());
      auto twoPhase = targetPool.make_span<uint8_t>(entries.size());
      Query::Scorer** scoring = scoringCapacity == 0
        ? nullptr
        : targetPool.make_arr<Query::Scorer*>(scoringCapacity);
      size_t allCount = 0;
      size_t scoringCount = 0;
      for (auto& e : entries) {
        auto* scorer = e.plan->build(targetPool);
        if (scorer == nullptr) return {nullptr, true, 0, 0, false};
        all[allCount] = scorer;
        assert(e.plan->shape().reportedTwoPhase
               != Query::ReportedTwoPhase::UNKNOWN);
        twoPhase[allCount] = e.plan->shape().reportedTwoPhase
            == Query::ReportedTwoPhase::YES;
        costs[allCount++] = e.cost;
        if (e.scoring) scoring[scoringCount++] = scorer;
      }

      // A lone scoring clause (one mandatory, no filters) needs no wrapper.
      if (allCount == 1 && scoringCount == 1) {
        return {all[0], false, leadCost, 1,
                twoPhase[0] != 0};
      }
      return {targetPool.make<BooleanQuery::ConjunctionScorer>(
                targetPool, std::span<Query::Scorer*>(all, allCount),
                std::span<int64_t>(costs, allCount),
                twoPhase.first(allCount),
                std::span<Query::Scorer*>(scoring, scoringCount),
                allowsPruning, enableTwoPhase),
              false, leadCost, scoringCount, false};
    }

    // Keep clause wiring in one place so prepared and non-prepared execution
    // cannot diverge on filter/prohibited semantics.
    static Query::Scorer* assembleScorer(
        MemPool& targetPool,
        IndexReader::Segment& segment,
        std::span<PullChildPlan> requiredPlans,
        std::span<PullChildPlan> optionalPlans,
        std::span<PullChildPlan> prohibitedPlans,
        int64_t requiredLeadCost,
        size_t mandatoryCount,
        size_t filterCount,
        size_t optionalSourceCount,
        size_t prohibitedSourceCount,
        int minShouldMatch,
        bool needsScores,
        bool externallyDriven,
        bool allowsPruning,
        const Query::PlanContext& planContext) {
      Required req = assembleRequired(
          targetPool, requiredPlans, requiredLeadCost, allowsPruning,
          !planContext.disableBooleanTwoPhaseForTests);
      if (req.unsatisfiable) return nullptr;
      Query::Scorer* reqScorer = req.scorer;
      bool hasScoringMandatory = req.scoringCount > 0;
      // Whether the optional group CONSTRAINS matching (Lucene bool
      // semantics): min_match >= 1 makes it a real constraint; otherwise
      // optionals only rank, provided a required or filter clause already
      // carries the match (reqScorer non-null iff such clauses exist).  With
      // nothing else, at least one optional must match - the classic
      // should-only boolean.
      bool optionalsConstrain = minShouldMatch >= 1 || reqScorer == nullptr;

      // For min-should-match (> 1) order the optional scorers by cost so the
      // pigeonhole lead/tail split leads with the cheapest (sparsest) iterators.
      auto optionalScorersStorage =
          targetPool.make_span<Query::Scorer*>(optionalPlans.size());
      auto optionalTwoPhaseStorage =
          targetPool.make_span<uint8_t>(optionalPlans.size());
      auto optionalDirectTermStorage =
          targetPool.make_span<uint8_t>(optionalPlans.size());
      auto optionalCostsStorage = minShouldMatch >= 1
          ? targetPool.make_span<int64_t>(optionalPlans.size())
          : std::span<int64_t>{};
      size_t optionalCount = 0;
      for (PullChildPlan& child : optionalPlans) {
        Query::Scorer* scorer = child.plan->build(targetPool);
        if (scorer == nullptr) continue;
        optionalScorersStorage[optionalCount] = scorer;
        assert(child.plan->shape().reportedTwoPhase
               != Query::ReportedTwoPhase::UNKNOWN);
        optionalTwoPhaseStorage[optionalCount] =
            child.plan->shape().reportedTwoPhase
                == Query::ReportedTwoPhase::YES;
        optionalDirectTermStorage[optionalCount] =
            child.plan->shape().directKind
                == Query::DirectScorerKind::TERM;
        if (minShouldMatch >= 1) {
          optionalCostsStorage[optionalCount] = child.cost;
        }
        optionalCount++;
      }
      auto optionalScorers = optionalScorersStorage.first(optionalCount);
      auto optionalTwoPhase =
          optionalTwoPhaseStorage.first(optionalCount);
      auto optionalDirectTerm =
          optionalDirectTermStorage.first(optionalCount);
      auto optionalCosts = minShouldMatch >= 1
          ? optionalCostsStorage.first(optionalCount)
          : std::span<int64_t>{};
      Query::Scorer* optScorer = nullptr;
      bool optTwoPhase = false;
      if (!optionalScorers.empty()) {
        int optCount = (int)optionalScorers.size();
        bool directWindowFilters = false;
        if (mandatoryCount == 0 && filterCount != 0
            && reqScorer != nullptr) {
          directWindowFilters = requiredPlans.size() == filterCount
              && std::all_of(
                  requiredPlans.begin(), requiredPlans.end(),
                  [](const PullChildPlan& child) {
                    return child.plan->shape().windowFillClause
                        == Query::ClauseShape::DIRECT;
                  });
        }
        bool flatTermDisjunction = optCount >= 2
            && std::all_of(
                optionalDirectTerm.begin(), optionalDirectTerm.end(),
                [](uint8_t value) { return value != 0; });
        // The filtered scored-bulk density gate has selected pull for this
        // exact shape. The filter remains the conjunction lead, and WAND is the
        // sole scoring disjunction member.
        bool useFilteredUnionWand = !disableFilteredUnionWandForTests
            && needsScores && minShouldMatch == 1
            && mandatoryCount == 0 && prohibitedSourceCount == 0
            && directWindowFilters && flatTermDisjunction
            && req.scoringCount == 0
            && filterDensityRoutesToPull(req.cost, segment.maxDoc());
        bool plainExternalDisjunction = externallyDriven && needsScores
          && reqScorer == nullptr && prohibitedSourceCount == 0
          && minShouldMatch <= 1 && optCount >= 2;
        if (plainExternalDisjunction) {
          sortByMaxScore(targetPool, optionalScorers);
        }
        bool useMaxScoreDisjunction = needsScores && reqScorer == nullptr
          && prohibitedSourceCount == 0 && minShouldMatch <= 1 && optCount >= 2
          && !plainExternalDisjunction;
        // minShouldMatch applies to optional scorers that exist in this segment.
        if (minShouldMatch <= 1) {
          if (optCount == 1) {
            optScorer = optionalScorers[0];
            optTwoPhase = optionalTwoPhase[0] != 0;
          } else if (useFilteredUnionWand) {
            optScorer = targetPool.make<BooleanQuery::MinShouldMatchWandScorer>(
              targetPool, optionalScorers, 1);
          } else if (useMaxScoreDisjunction) {
            optScorer = targetPool.make<BooleanQuery::MaxScoreDisjunctionScorer>(
              targetPool, optionalScorers, segment.maxDoc());
          } else {
            optScorer = targetPool.make<BooleanQuery::DisjunctionScorer>(
                targetPool, optionalScorers, optionalTwoPhase,
                !planContext.disableBooleanTwoPhaseForTests
                    && !planContext.disableDisjunctionTwoPhaseForTests);
            optTwoPhase = !planContext.disableBooleanTwoPhaseForTests
                && !planContext.disableDisjunctionTwoPhaseForTests
                && std::any_of(
                    optionalTwoPhase.begin(), optionalTwoPhase.end(),
                    [](uint8_t value) { return value != 0; });
          }
        } else if (optCount == minShouldMatch) {
          // Every surviving optional clause is required and scores.
          optScorer = targetPool.make<BooleanQuery::ConjunctionScorer>(
            targetPool, optionalScorers, optionalCosts, optionalTwoPhase,
            optionalScorers,
            allowsPruning,
            !planContext.disableBooleanTwoPhaseForTests);
        } else if (optCount > minShouldMatch) {
          bool useWand = needsScores && reqScorer == nullptr
              && prohibitedSourceCount == 0;
          if (useWand) {
            optScorer = targetPool.make<BooleanQuery::MinShouldMatchWandScorer>(
              targetPool, optionalScorers, minShouldMatch);
          } else {
            optScorer = targetPool.make<BooleanQuery::MinShouldMatchScorer>(
              targetPool, optionalScorers, minShouldMatch);
          }
        }
      }

      Query::Scorer* boolScorer = nullptr;
      if (reqScorer == nullptr) {
        // No required clauses: the optional side stands alone.
        if (optScorer == nullptr) return nullptr;
        boolScorer = optScorer;
      } else if (optScorer == nullptr) {
        // reqScorer present, but no optional scorer survived this segment (the
        // optional terms are absent, or fewer survive than minShouldMatch).  A
        // constraining optional group means no match here - returning
        // reqScorer would wrongly emit filter/required-only docs.  A rank-only
        // group is simply absent.
        if (optionalsConstrain && optionalSourceCount != 0) return nullptr;
        boolScorer = reqScorer;
      } else if (!optionalsConstrain) {
        // min_match unset with required/filter clauses: optionals rank
        // coincident matches but never decide them.
        boolScorer = targetPool.make<BooleanQuery::MandOptScorer>(
            targetPool, reqScorer, req.twoPhase, optScorer, optTwoPhase,
            !planContext.disableBooleanTwoPhaseForTests);
      } else {
        // min_match >= 1 alongside required/filter clauses: the optional
        // group is a constraint, conjoined with the required side.  Scoring
        // clauses on both sides contribute (filters score nothing).
        std::span<Query::Scorer*> allSpan(targetPool.make_arr<Query::Scorer*>(2), 2);
        allSpan[0] = reqScorer;
        allSpan[1] = optScorer;
        std::span<int64_t> allCosts(targetPool.make_arr<int64_t>(2), 2);
        allCosts[0] = req.cost;
        allCosts[1] = optionalCost(
            optionalCosts, minShouldMatch, segment.maxDoc());
        size_t nscoring = hasScoringMandatory ? 2 : 1;
        std::span<Query::Scorer*> scoringSpan(targetPool.make_arr<Query::Scorer*>(nscoring), nscoring);
        scoringSpan[nscoring - 1] = optScorer;
        if (hasScoringMandatory) scoringSpan[0] = reqScorer;
        std::array<uint8_t, 2> allTwoPhase{
            (uint8_t)req.twoPhase, (uint8_t)optTwoPhase};
        boolScorer = targetPool.make<BooleanQuery::ConjunctionScorer>(
            targetPool, allSpan, allCosts,
            allTwoPhase, scoringSpan, allowsPruning,
            !planContext.disableBooleanTwoPhaseForTests);
      }

      auto prohibitedStorage =
          targetPool.make_span<Query::Scorer*>(prohibitedPlans.size());
      auto prohibitedTwoPhaseStorage =
          targetPool.make_span<uint8_t>(prohibitedPlans.size());
      size_t prohibitedCount = 0;
      for (PullChildPlan& child : prohibitedPlans) {
        Query::Scorer* scorer = child.plan->build(targetPool);
        if (scorer != nullptr) {
          prohibitedStorage[prohibitedCount] = scorer;
          assert(child.plan->shape().reportedTwoPhase
                 != Query::ReportedTwoPhase::UNKNOWN);
          prohibitedTwoPhaseStorage[prohibitedCount] =
              child.plan->shape().reportedTwoPhase
                  == Query::ReportedTwoPhase::YES;
          prohibitedCount++;
        }
      }
      auto prohibitedScorers = prohibitedStorage.first(prohibitedCount);
      auto prohibitedTwoPhase =
          prohibitedTwoPhaseStorage.first(prohibitedCount);
      if (!prohibitedScorers.empty()) {
        Query::Scorer* prohibitedScorer = prohibitedScorers.size() == 1
          ? prohibitedScorers[0]
          : targetPool.make<BooleanQuery::DisjunctionScorer>(
              targetPool, prohibitedScorers, prohibitedTwoPhase,
              !planContext.disableBooleanTwoPhaseForTests
                  && !planContext.disableDisjunctionTwoPhaseForTests);
        bool prohibitedHasTwoPhase = prohibitedScorers.size() == 1
            ? prohibitedTwoPhase[0] != 0
            : !planContext.disableBooleanTwoPhaseForTests
                && !planContext.disableDisjunctionTwoPhaseForTests
                && std::any_of(
                    prohibitedTwoPhase.begin(), prohibitedTwoPhase.end(),
                    [](uint8_t value) { return value != 0; });
        boolScorer = targetPool.make<BooleanQuery::MandNotScorer>(
            targetPool, boolScorer, prohibitedScorer,
            prohibitedHasTwoPhase,
            !planContext.disableBooleanTwoPhaseForTests
                && !planContext.disableMandNotTwoPhaseForTests);
      }
      return boolScorer;
    }

    // Estimates the optional group's match cost: disjunction is the sum (capped),
    // min-should-match the sum of the cheapest n - mm + 1, all-required the rarest.
    static int64_t optionalCost(std::span<const int64_t> costs,
                                int minShouldMatch, int64_t maxDoc) {
      int n = (int) costs.size();
      if (n == 0) return 0;
      if (minShouldMatch >= n) {
        int64_t m = maxDoc;
        for (auto c : costs) m = std::min(m, c);
        return m;
      }
      int take = minShouldMatch <= 1 ? n : n - minShouldMatch + 1;
      boost::container::small_vector<int64_t, 16> sorted(costs.begin(), costs.end());
      if (take < n) std::sort(sorted.begin(), sorted.end());
      int64_t sum = 0;
      for (int i = 0; i < take; i++) {
        int64_t cost = sorted[(size_t) i];
        if (cost >= maxDoc - sum) return maxDoc;
        sum += cost;
      }
      return sum;
    }

    static int64_t optionalCost(
        MemPool& targetPool,
        IndexReader::Segment& segment,
        std::span<Query::SegmentSource* const> optionalSources,
        int minShouldMatch,
        int64_t maxDoc) {
      boost::container::small_vector<int64_t, 16> costs;
      for (auto* source : optionalSources) {
        auto* supplier = source->scorerSupplier(targetPool, segment);
        costs.push_back(supplier == nullptr ? 0 : supplier->cost());
      }
      return optionalCost(costs, minShouldMatch, maxDoc);
    }

    // Estimates the boolean match cost from child supplier costs.
    static int64_t compositeCost(
        MemPool& targetPool,
        IndexReader::Segment& segment,
        std::span<Query::SegmentSource* const> mandatorySources,
        std::span<Query::SegmentSource* const> optionalSources,
        std::span<Query::ScorerSupplier* const> filterSuppliers,
        int minShouldMatch) {
      int64_t maxDoc = segment.maxDoc();
      bool hasMandatory = !mandatorySources.empty();
      if (hasMandatory || !filterSuppliers.empty()) {
        // Required = mandatory + filter, conjoined: at most the rarest clause.
        int64_t minReq = maxDoc;
        for (auto* source : mandatorySources) {
          auto* supplier = source->scorerSupplier(targetPool, segment);
          if (supplier == nullptr) return 0;
          minReq = std::min(minReq, supplier->cost());
        }
        for (auto* supplier : filterSuppliers) {
          if (supplier == nullptr) return 0;
          minReq = std::min(minReq, supplier->cost());
        }
        // A rank-only optional group (min_match unset) never decides a match,
        // so it cannot tighten the estimate; a constraining group
        // (min_match >= 1) is conjoined and can.
        if (minShouldMatch >= 1 && !optionalSources.empty()) {
          return std::min(minReq, optionalCost(targetPool, segment, optionalSources, minShouldMatch, maxDoc));
        }
        return minReq;
      }
      return optionalCost(targetPool, segment, optionalSources, minShouldMatch, maxDoc);
    }

    class Supplier final : public Query::ScorerSupplier {
      MemPool& pool;
      IndexReader::Segment& segment;
      std::span<Query::SegmentSource* const> mandatorySources;
      std::span<const uint8_t> mandatoryScores;
      std::span<Query::SegmentSource* const> optionalSources;
      std::span<Query::SegmentSource* const> prohibitedSources;
      std::span<Query::ScorerSupplier* const> filterSuppliers;
      std::span<Query::ScorerSupplier* const> mandatoryShapeSuppliers;
      std::span<Query::ScorerSupplier* const> optionalShapeSuppliers;
      std::span<Query::ScorerSupplier* const> prohibitedShapeSuppliers;
      int64_t shapeRequiredCost;
      int minShouldMatch;
      bool needsScores;
      bool allowsPruning;
      bool twoPhaseDisjunctionPull = false;

      static Query::ScorerShape emptyShape() {
        Query::ScorerShape shape;
        shape.matchState = Query::MatchState::EMPTY;
        return shape;
      }

      static Query::ScorerShape opaqueShape(
          Query::MatchState matchState,
          Query::ReportedTwoPhase reportedTwoPhase =
              Query::ReportedTwoPhase::NO) {
        return {
          .matchState = matchState,
          .directKind = Query::DirectScorerKind::OTHER,
          .reportedTwoPhase = reportedTwoPhase,
          .windowFillClause = Query::ClauseShape::NONE,
          .termDisjunctionClause = Query::ClauseShape::NONE,
          .termConjunctionClause = Query::ClauseShape::NONE,
          .independentTerm = Query::IndependentTermAccess::UNSUPPORTED,
          .docsOnly = Query::DocsOnlyAccess::UNSUPPORTED,
          .directDocSet = Query::DirectDocSetAccess::UNSUPPORTED,
        };
      }

      static Query::ScorerShape maskSupplierAccess(
          Query::ScorerShape shape) {
        shape.independentTerm =
            Query::IndependentTermAccess::UNSUPPORTED;
        shape.docsOnly = Query::DocsOnlyAccess::UNSUPPORTED;
        shape.directDocSet = Query::DirectDocSetAccess::UNSUPPORTED;
        return shape;
      }

      static Query::MatchState requiredMatchState(
          std::span<Query::ScorerSupplier* const> suppliers,
          const Query::PlanContext& buildContext) {
        Query::MatchState state = Query::MatchState::NONEMPTY;
        for (auto* supplier : suppliers) {
          if (supplier == nullptr) return Query::MatchState::EMPTY;
          Query::MatchState member =
              supplier->describeScorer(buildContext).matchState;
          if (member == Query::MatchState::EMPTY) {
            return Query::MatchState::EMPTY;
          }
          if (member == Query::MatchState::UNKNOWN) {
            state = Query::MatchState::UNKNOWN;
          }
        }
        return state;
      }

      static bool hasUnknownElision(
          std::span<Query::ScorerSupplier* const> suppliers,
          const Query::PlanContext& buildContext) {
        for (auto* supplier : suppliers) {
          if (supplier != nullptr
              && supplier->describeScorer(buildContext).matchState
                  == Query::MatchState::UNKNOWN) {
            return true;
          }
        }
        return false;
      }

      static bool fillExpansionMemos(
          std::span<Query::ScorerSupplier* const> suppliers,
          const Query::PlanContext& buildContext) {
        bool filled = false;
        for (auto* supplier : suppliers) {
          if (supplier != nullptr
              && supplier->fillExpansionMemo(buildContext)) {
            filled = true;
          }
        }
        return filled;
      }

      static Query::ScorerShape describeResolved(
          Query::ScorerSupplier* supplier,
          const Query::PlanContext& buildContext) {
        Query::ScorerShape shape = supplier->describeScorer(buildContext);
        bool unknown = shape.matchState == Query::MatchState::UNKNOWN
            || shape.directKind == Query::DirectScorerKind::UNKNOWN
            || shape.reportedTwoPhase == Query::ReportedTwoPhase::UNKNOWN
            || shape.windowFillClause == Query::ClauseShape::UNKNOWN
            || shape.termDisjunctionClause == Query::ClauseShape::UNKNOWN;
        if (unknown && supplier->fillExpansionMemo(buildContext)) {
          shape = supplier->describeScorer(buildContext);
        }
        return shape;
      }

      static size_t nonemptyCount(
          std::span<Query::ScorerSupplier* const> suppliers,
          const Query::PlanContext& buildContext) {
        size_t count = 0;
        for (auto* supplier : suppliers) {
          if (supplier != nullptr
              && supplier->describeScorer(buildContext).matchState
                  == Query::MatchState::NONEMPTY) {
            count++;
          }
        }
        return count;
      }

      static Query::ReportedTwoPhase disjunctionTwoPhase(
          std::span<Query::ScorerSupplier* const> suppliers,
          const Query::PlanContext& buildContext) {
        if (buildContext.disableBooleanTwoPhaseForTests
            || buildContext.disableDisjunctionTwoPhaseForTests) {
          return Query::ReportedTwoPhase::NO;
        }
        bool unknown = false;
        for (auto* supplier : suppliers) {
          if (supplier == nullptr) continue;
          Query::ScorerShape member =
              supplier->describeScorer(buildContext);
          if (member.matchState != Query::MatchState::NONEMPTY) continue;
          if (member.reportedTwoPhase == Query::ReportedTwoPhase::YES) {
            return Query::ReportedTwoPhase::YES;
          }
          unknown |= member.reportedTwoPhase
              == Query::ReportedTwoPhase::UNKNOWN;
        }
        return unknown ? Query::ReportedTwoPhase::UNKNOWN
                       : Query::ReportedTwoPhase::NO;
      }

      static Query::ClauseShape flatClauseShape(
          std::span<Query::ScorerSupplier* const> suppliers,
          const Query::PlanContext& buildContext,
          bool windowFill) {
        bool unknown = false;
        for (auto* supplier : suppliers) {
          if (supplier == nullptr) continue;
          Query::ScorerShape member =
              supplier->describeScorer(buildContext);
          if (member.matchState != Query::MatchState::NONEMPTY) continue;
          bool direct = windowFill
              ? member.windowFillClause == Query::ClauseShape::DIRECT
              : member.directKind == Query::DirectScorerKind::TERM;
          if (direct) continue;
          bool memberUnknown = windowFill
              ? member.windowFillClause == Query::ClauseShape::UNKNOWN
              : member.directKind == Query::DirectScorerKind::UNKNOWN;
          if (!memberUnknown) return Query::ClauseShape::NONE;
          unknown = true;
        }
        return unknown ? Query::ClauseShape::UNKNOWN
                       : Query::ClauseShape::FLAT_DISJUNCTION;
      }

      Query::ScorerShape requiredShape(
          const Query::PlanContext& buildContext) const {
        size_t requiredCount =
            mandatoryShapeSuppliers.size() + filterSuppliers.size();
        if (requiredCount == 0) return emptyShape();

        Query::PlanContext memberContext = childPlanContext(
            buildContext, shapeRequiredCost);

        Query::MatchState state =
            requiredMatchState(mandatoryShapeSuppliers, memberContext);
        if (state != Query::MatchState::EMPTY) {
          Query::MatchState filters =
              requiredMatchState(filterSuppliers, memberContext);
          if (filters == Query::MatchState::EMPTY) {
            state = Query::MatchState::EMPTY;
          } else if (filters == Query::MatchState::UNKNOWN) {
            state = Query::MatchState::UNKNOWN;
          }
        }
        if (state == Query::MatchState::EMPTY) return emptyShape();

        if (requiredCount == 1 && mandatoryShapeSuppliers.size() == 1
            && mandatoryScores[0] != 0) {
          return mandatoryShapeSuppliers[0]->describeScorer(memberContext);
        }
        Query::ScorerShape shape = opaqueShape(state);
        if (requiredCount >= 1) {
          bool allTerms = true;
          for (auto suppliers : {
                   mandatoryShapeSuppliers, filterSuppliers}) {
            for (auto* supplier : suppliers) {
              if (supplier == nullptr) {
                allTerms = false;
                continue;
              }
              allTerms &= supplier->describeScorer(memberContext).directKind
                  == Query::DirectScorerKind::TERM;
            }
          }
          if (allTerms) {
            shape.termConjunctionClause =
                Query::ClauseShape::FLAT_CONJUNCTION;
          }
        }
        return shape;
      }

      std::optional<bool> directWindowFilters(
          const Query::PlanContext& buildContext) const {
        if (!mandatorySources.empty() || filterSuppliers.empty()) return false;
        Query::PlanContext memberContext = childPlanContext(
            buildContext, shapeRequiredCost);
        for (auto* supplier : filterSuppliers) {
          if (supplier == nullptr) return false;
          Query::ScorerShape shape = supplier->describeScorer(memberContext);
          if (shape.windowFillClause == Query::ClauseShape::UNKNOWN) {
            return std::nullopt;
          }
          if (shape.windowFillClause != Query::ClauseShape::DIRECT) {
            return false;
          }
        }
        return true;
      }

      Query::ScorerShape optionalShape(
          const Query::PlanContext& buildContext,
          bool hasRequired) const {
        Query::PlanContext memberContext = childPlanContext(
            buildContext, std::numeric_limits<int64_t>::max());
        size_t count = nonemptyCount(
            optionalShapeSuppliers, memberContext);
        if (count == 0 || (minShouldMatch > 1
                           && count < (size_t) minShouldMatch)) {
          return emptyShape();
        }
        if (count == 1 && minShouldMatch <= 1) {
          for (auto* supplier : optionalShapeSuppliers) {
            if (supplier != nullptr) {
              Query::ScorerShape shape =
                  supplier->describeScorer(memberContext);
              if (shape.matchState == Query::MatchState::NONEMPTY) {
                return shape;
              }
            }
          }
          std::unreachable();
        }

        if (minShouldMatch > 1) {
          return opaqueShape(Query::MatchState::NONEMPTY);
        }

        Query::ClauseShape termShape = flatClauseShape(
            optionalShapeSuppliers, memberContext, false);
        std::optional<bool> directFilters =
            directWindowFilters(buildContext);
        bool filteredUnionWandCandidate =
            !buildContext.disableFilteredUnionWandForTests
            && needsScores && minShouldMatch == 1
            && mandatorySources.empty() && prohibitedSources.empty()
            && filterDensityRoutesToPull(
                shapeRequiredCost, segment.maxDoc());
        if (filteredUnionWandCandidate
            && (!directFilters.has_value()
                || termShape == Query::ClauseShape::UNKNOWN)) {
          Query::ScorerShape shape;
          shape.matchState = Query::MatchState::NONEMPTY;
          shape.directKind = Query::DirectScorerKind::OTHER;
          return shape;
        }
        bool useFilteredUnionWand = filteredUnionWandCandidate
            && *directFilters
            && termShape == Query::ClauseShape::FLAT_DISJUNCTION;
        bool plainExternalDisjunction =
            buildContext.demand.candidates
                != std::numeric_limits<int64_t>::max()
            && needsScores && !hasRequired && prohibitedSources.empty()
            && minShouldMatch <= 1;
        bool useMaxScoreDisjunction = needsScores && !hasRequired
            && prohibitedSources.empty() && minShouldMatch <= 1
            && !plainExternalDisjunction;
        if (useFilteredUnionWand || useMaxScoreDisjunction) {
          return opaqueShape(Query::MatchState::NONEMPTY);
        }

        Query::ScorerShape shape = opaqueShape(
            Query::MatchState::NONEMPTY,
            disjunctionTwoPhase(optionalShapeSuppliers, memberContext));
        shape.windowFillClause = flatClauseShape(
            optionalShapeSuppliers, memberContext, true);
        shape.termDisjunctionClause = flatClauseShape(
            optionalShapeSuppliers, memberContext, false);
        return shape;
      }

      static Query::ReportedTwoPhase mandOptTwoPhase(
          Query::ScorerShape required, Query::ScorerShape optional,
          const Query::PlanContext& buildContext) {
        if (buildContext.disableBooleanTwoPhaseForTests) {
          return Query::ReportedTwoPhase::NO;
        }
        if (required.reportedTwoPhase == Query::ReportedTwoPhase::YES
            || optional.reportedTwoPhase == Query::ReportedTwoPhase::YES) {
          return Query::ReportedTwoPhase::YES;
        }
        if (required.reportedTwoPhase == Query::ReportedTwoPhase::NO
            && optional.reportedTwoPhase == Query::ReportedTwoPhase::NO) {
          return Query::ReportedTwoPhase::NO;
        }
        return Query::ReportedTwoPhase::UNKNOWN;
      }

      static Query::PlanContext childPlanContext(
          const Query::PlanContext& parentContext,
          const Query::Demand& demand) {
        Query::PlanContext childContext = parentContext;
        childContext.demand = demand;
        return childContext;
      }

      static Query::PlanContext childPlanContext(
          const Query::PlanContext& parentContext,
          int64_t leadCost) {
        return childPlanContext(
            parentContext, Query::Demand::fromLeadCost(
                leadCost, parentContext.demand.horizon));
      }

      class PullScorerPlan final : public Query::ScorerPlan {
        Supplier& supplier;
        std::span<PullChildPlan> required;
        std::span<PullChildPlan> optional;
        std::span<PullChildPlan> prohibited;
        int64_t requiredLeadCost;
        bool requiredUnsatisfiable;

      protected:
        Query::Scorer* buildScorer(MemPool& targetPool) override {
          if (requiredUnsatisfiable) return nullptr;
          return assembleScorer(
              targetPool, supplier.segment, required, optional, prohibited,
              requiredLeadCost, supplier.mandatorySources.size(),
              supplier.filterSuppliers.size(),
              supplier.optionalSources.size(),
              supplier.prohibitedSources.size(), supplier.minShouldMatch,
              supplier.needsScores,
              demand().candidates
                  != std::numeric_limits<int64_t>::max(),
              supplier.allowsPruning, context());
        }

      public:
        PullScorerPlan(
            Supplier& supplier,
            const Query::PlanContext& planContext,
            const Query::ScorerShape& shape,
            int64_t cost,
            std::span<PullChildPlan> required,
            std::span<PullChildPlan> optional,
            std::span<PullChildPlan> prohibited,
            int64_t requiredLeadCost,
            bool requiredUnsatisfiable)
          : Query::ScorerPlan(planContext, shape, cost),
            supplier(supplier), required(required), optional(optional),
            prohibited(prohibited), requiredLeadCost(requiredLeadCost),
            requiredUnsatisfiable(requiredUnsatisfiable) {}
      };

    public:
      Supplier(MemPool& pool, IndexReader::Segment& segment,
               std::span<Query::SegmentSource* const> mandatorySources,
               std::span<const uint8_t> mandatoryScores,
               std::span<Query::SegmentSource* const> optionalSources,
               std::span<Query::SegmentSource* const> prohibitedSources,
               std::span<Query::ScorerSupplier* const> filterSuppliers,
               std::span<Query::ScorerSupplier* const>
                   mandatoryShapeSuppliers,
               std::span<Query::ScorerSupplier* const>
                   optionalShapeSuppliers,
               std::span<Query::ScorerSupplier* const>
                   prohibitedShapeSuppliers,
               int64_t shapeRequiredCost,
               int minShouldMatch,
               bool needsScores,
               bool allowsPruning)
        : pool(pool), segment(segment), mandatorySources(mandatorySources),
          mandatoryScores(mandatoryScores),
          optionalSources(optionalSources), prohibitedSources(prohibitedSources),
          filterSuppliers(filterSuppliers),
          mandatoryShapeSuppliers(mandatoryShapeSuppliers),
          optionalShapeSuppliers(optionalShapeSuppliers),
          prohibitedShapeSuppliers(prohibitedShapeSuppliers),
          shapeRequiredCost(shapeRequiredCost),
          minShouldMatch(minShouldMatch),
          needsScores(needsScores), allowsPruning(allowsPruning) {}

      int64_t cost() override {
        int64_t maxDoc = segment.maxDoc();
        if (!mandatoryShapeSuppliers.empty() || !filterSuppliers.empty()) {
          int64_t requiredCost = maxDoc;
          for (auto* supplier : mandatoryShapeSuppliers) {
            if (supplier == nullptr) return 0;
            requiredCost = std::min(requiredCost, supplier->cost());
          }
          for (auto* supplier : filterSuppliers) {
            if (supplier == nullptr) return 0;
            requiredCost = std::min(requiredCost, supplier->cost());
          }
          if (minShouldMatch >= 1 && !optionalShapeSuppliers.empty()) {
            boost::container::small_vector<int64_t, 16> costs;
            for (auto* supplier : optionalShapeSuppliers) {
              costs.push_back(supplier == nullptr ? 0 : supplier->cost());
            }
            requiredCost = std::min(
                requiredCost,
                optionalCost(costs, minShouldMatch, maxDoc));
          }
          return requiredCost;
        }
        boost::container::small_vector<int64_t, 16> costs;
        for (auto* supplier : optionalShapeSuppliers) {
          costs.push_back(supplier == nullptr ? 0 : supplier->cost());
        }
        return optionalCost(costs, minShouldMatch, maxDoc);
      }

      Query::ScorerShape describeScorer(
          const Query::PlanContext& buildContext) const override {
        bool hasRequired = !mandatorySources.empty()
            || !filterSuppliers.empty();
        Query::PlanContext unboundedContext = childPlanContext(
            buildContext, std::numeric_limits<int64_t>::max());
        Query::ScorerShape required = requiredShape(buildContext);
        if (hasRequired
            && required.matchState == Query::MatchState::EMPTY) {
          return maskSupplierAccess(required);
        }
        if (hasUnknownElision(optionalShapeSuppliers, unboundedContext)) {
          return maskSupplierAccess({});
        }

        Query::ScorerShape optional =
            optionalShape(buildContext, hasRequired);
        bool optionalsConstrain = minShouldMatch >= 1 || !hasRequired;
        Query::ScorerShape positive;
        if (!hasRequired) {
          positive = optional;
        } else if (optional.matchState == Query::MatchState::EMPTY) {
          if (optionalsConstrain && !optionalSources.empty()) {
            positive = emptyShape();
          } else {
            positive = required;
          }
        } else if (!optionalsConstrain) {
          Query::MatchState state = required.matchState;
          positive = opaqueShape(
              state, mandOptTwoPhase(required, optional, buildContext));
        } else {
          positive = opaqueShape(required.matchState);
        }
        if (positive.matchState == Query::MatchState::EMPTY) {
          return maskSupplierAccess(positive);
        }

        if (hasUnknownElision(prohibitedShapeSuppliers, unboundedContext)) {
          return maskSupplierAccess({});
        }
        if (nonemptyCount(prohibitedShapeSuppliers, unboundedContext) != 0) {
          positive = opaqueShape(positive.matchState);
        }
        return maskSupplierAccess(positive);
      }

      Query::UnresolvedSupplierCause unresolvedScorerCause(
          const Query::PlanContext& buildContext) const override {
        Query::PlanContext requiredContext = childPlanContext(
            buildContext, shapeRequiredCost);
        for (auto suppliers : {
                 mandatoryShapeSuppliers, filterSuppliers}) {
          Query::UnresolvedSupplierCause cause = [&]() {
            for (auto* supplier : suppliers) {
              if (supplier != nullptr
                  && supplier->describeScorer(requiredContext).hasUnknown()) {
                return supplier->unresolvedScorerCause(requiredContext);
              }
            }
            return Query::UnresolvedSupplierCause::NONE;
          }();
          if (cause != Query::UnresolvedSupplierCause::NONE) return cause;
        }
        Query::PlanContext unboundedContext = childPlanContext(
            buildContext, std::numeric_limits<int64_t>::max());
        for (auto suppliers : {
                 optionalShapeSuppliers, prohibitedShapeSuppliers}) {
          for (auto* supplier : suppliers) {
            if (supplier != nullptr
                && supplier->describeScorer(unboundedContext).hasUnknown()) {
              return supplier->unresolvedScorerCause(unboundedContext);
            }
          }
        }
        return Query::UnresolvedSupplierCause::OTHER;
      }

      bool fillExpansionMemo(
          const Query::PlanContext& buildContext) override {
        bool filled = fillExpansionMemos(
            mandatoryShapeSuppliers, buildContext);
        filled = fillExpansionMemos(filterSuppliers, buildContext)
            || filled;
        filled = fillExpansionMemos(optionalShapeSuppliers, buildContext)
            || filled;
        filled = fillExpansionMemos(prohibitedShapeSuppliers, buildContext)
            || filled;
        return filled;
      }

      Query::ScorerPlan* resolve(
          MemPool& planPool,
          const Query::PlanContext& planContext) override {
        int64_t resolvedCost = cost();
        boost::container::small_vector<PullChildPlan, 16> required;
        boost::container::small_vector<PullChildPlan, 16> optional;
        boost::container::small_vector<PullChildPlan, 16> prohibited;
        bool requiredUnsatisfiable = false;

        for (size_t i = 0; i < mandatoryShapeSuppliers.size(); i++) {
          Query::ScorerSupplier* child = mandatoryShapeSuppliers[i];
          if (child == nullptr) {
            requiredUnsatisfiable = true;
            continue;
          }
          required.push_back({
              child->cost(), child, nullptr, mandatoryScores[i] != 0});
        }
        for (size_t i = 0; i < filterSuppliers.size(); i++) {
          Query::ScorerSupplier* child = filterSuppliers[i];
          if (child == nullptr) {
            requiredUnsatisfiable = true;
            continue;
          }
          required.push_back({child->cost(), child, nullptr, false});
        }

        int64_t requiredLeadCost =
            std::numeric_limits<int64_t>::max();
        for (const PullChildPlan& child : required) {
          requiredLeadCost = std::min(requiredLeadCost, child.cost);
        }
        std::sort(
            required.begin(), required.end(),
            [](const PullChildPlan& left, const PullChildPlan& right) {
              return left.cost < right.cost;
            });

        for (size_t i = 0; i < optionalShapeSuppliers.size(); i++) {
          Query::ScorerSupplier* child = optionalShapeSuppliers[i];
          if (child == nullptr) continue;
          int64_t childCost = minShouldMatch >= 1 ? child->cost() : 0;
          optional.push_back({childCost, child, nullptr, false});
        }
        if (minShouldMatch > 1) {
          std::sort(
              optional.begin(), optional.end(),
              [](const PullChildPlan& left, const PullChildPlan& right) {
                return left.cost < right.cost;
              });
        }
        for (Query::ScorerSupplier* child : prohibitedShapeSuppliers) {
          if (child != nullptr) {
            prohibited.push_back({0, child, nullptr, false});
          }
        }

        auto requiredPlans =
            planPool.make_span<PullChildPlan>(required.size());
        auto optionalPlans =
            planPool.make_span<PullChildPlan>(optional.size());
        auto prohibitedPlans =
            planPool.make_span<PullChildPlan>(prohibited.size());
        std::copy(required.begin(), required.end(), requiredPlans.begin());
        std::copy(optional.begin(), optional.end(), optionalPlans.begin());
        std::copy(prohibited.begin(), prohibited.end(), prohibitedPlans.begin());

        if (!requiredUnsatisfiable) {
          Query::PlanContext requiredContext = childPlanContext(
              planContext, requiredLeadCost);
          for (PullChildPlan& child : requiredPlans) {
            child.plan = child.supplier->resolve(
                planPool, requiredContext);
            assert(child.plan != nullptr);
          }
          Query::PlanContext unboundedContext = childPlanContext(
              planContext, std::numeric_limits<int64_t>::max());
          for (PullChildPlan& child : optionalPlans) {
            child.plan = child.supplier->resolve(
                planPool, unboundedContext);
            assert(child.plan != nullptr);
          }
          for (PullChildPlan& child : prohibitedPlans) {
            child.plan = child.supplier->resolve(
                planPool, unboundedContext);
            assert(child.plan != nullptr);
          }
        }

        Query::ScorerShape shape = describeScorer(planContext);
        return planPool.make<PullScorerPlan>(
            *this, planContext, shape, resolvedCost,
            requiredPlans, optionalPlans, prohibitedPlans,
            requiredLeadCost, requiredUnsatisfiable);
      }

      Query::PlanContext makePlanContext(
          const Query::Demand& demand) const override {
        return scorerBuildContext(demand);
      }

      DocSet* exactDocSet() override {
        // Apply the filter-clause test hook to the degenerate single-filter
        // DocSet path as well.
        if (!needsScores && disableFilterClauseCountForTests) {
          return nullptr;
        }
        if (!mandatorySources.empty() || !optionalSources.empty()
            || !prohibitedSources.empty() || filterSuppliers.size() != 1
            || filterSuppliers[0] == nullptr || minShouldMatch != 0) {
          return nullptr;
        }
        return filterSuppliers[0]->exactDocSet();
      }

      Query::ScorerSupplier::ExactCountTopKCosts
      exactCountTopKCosts() override {
        if (!mandatorySources.empty() || optionalSources.size() < 2
            || !prohibitedSources.empty() || filterSuppliers.size() != 1
            || filterSuppliers[0] == nullptr || minShouldMatch != 1) {
          return {};
        }
        return {
          filterSuppliers[0]->cost(),
          optionalCost(
              pool, segment, optionalSources, minShouldMatch,
              segment.maxDoc())
        };
      }

      enum class ConjunctionMode : uint8_t {
        SCORED_BODY,
        EXHAUSTIVE,
        CANDIDATE,
      };

      // The route selected at construction/admission time. Runtime sampling
      // may later latch a candidate route back to dense execution.
      enum class ConjunctionRoute : uint8_t {
        GENERIC,
        CANDIDATE_DOC_SET_FILTER,
        CANDIDATE_POSTINGS_FILTER,
        CANDIDATE_SCORING_TERM,
        COUNT_EXACT_DOCS_ONLY,
        COUNT_POSTINGS_SAMPLE,
        COUNT_DENSE,
        COUNT_DOC_SET_SPARSE,
      };

      enum class EnclosingFilters : uint8_t {
        NOT_SUPPLIED,
        CONSUMED,
      };

      struct ConjunctionBulkResult {
        BulkScorer* bulk;
        ConjunctionRoute route;
        EnclosingFilters enclosingFilters;

        ConjunctionBulkResult(
            BulkScorer* bulk, ConjunctionRoute route,
            EnclosingFilters enclosingFilters)
            : bulk(bulk), route(route),
              enclosingFilters(enclosingFilters) {
          assert(bulk != nullptr);
        }

        bool usesCandidateRoute() const {
          return route == ConjunctionRoute::CANDIDATE_DOC_SET_FILTER
              || route == ConjunctionRoute::CANDIDATE_POSTINGS_FILTER
              || route == ConjunctionRoute::CANDIDATE_SCORING_TERM;
        }
      };

      using MaybeConjunctionBulk = std::optional<ConjunctionBulkResult>;

      using AcceptedConjunctionRoutes = uint32_t;

      enum class ConjunctionPlanStatus : uint8_t {
        READY,
        DECLINED,
        NO_MATCH,
      };

      struct BooleanBulkBuildState : BulkBuildState {
        enum class Kind : uint8_t {
          CONJUNCTION,
          MAND_OPT,
          MAX_SCORE,
          FILTERED_DISJUNCTION,
          EXACT_FILTERED_MAND_OPT,
          FILTERED_BODY,
          FILTER_ONLY,
          DELEGATE_FILTERED_CHILD,
        };

        Kind kind;

        explicit BooleanBulkBuildState(Kind kind) : kind(kind) {}
      };

      struct PlannedEntry {
        int64_t cost;
        Query::ScorerSupplier* supplier;
        Query::ScorerShape shape;
        bool optionalGroup;
        bool filter;
        bool docSetFilter;
        bool scoring;
        size_t order;
        Query::Demand buildDemand;
        Query::ScorerPlan* plan = nullptr;
      };

      struct PlannedSupplier {
        Query::ScorerSupplier* supplier;
        Query::ScorerShape shape;
        Query::Demand buildDemand;
        Query::ScorerPlan* plan = nullptr;
      };

      struct ConjunctionPlan : BooleanBulkBuildState {
        ConjunctionMode mode = ConjunctionMode::SCORED_BODY;
        ConjunctionRoute route = ConjunctionRoute::GENERIC;
        BulkUse use = BulkUse::MATCH_WINDOWS;
        EnclosingFilters enclosingFilters = EnclosingFilters::NOT_SUPPLIED;
        std::span<PlannedEntry> entries;
        std::span<PlannedSupplier> optionalGroup;
        std::span<PlannedSupplier> prohibited;
        int64_t leadCost = 0;
        int64_t nonLeadCost = 0;
        bool hasOptionalGroup = false;
        bool termFeed = false;
        bool docSetSparseEligible = false;
        bool recordConjunctionConstruction = false;
        bool hasDirectDenseClause = false;
        bool negatedCount = false;
        Query::ScorerPlan* independentLeadPlan = nullptr;
        Query::UnresolvedSupplierCause unresolvedCause =
            Query::UnresolvedSupplierCause::NONE;

        ConjunctionPlan()
          : BooleanBulkBuildState(
                BooleanBulkBuildState::Kind::CONJUNCTION) {}
      };

      struct MandOptPlan : BooleanBulkBuildState {
        PlannedSupplier mandatory;
        std::span<PlannedSupplier> optional;
        std::span<int64_t> optionalCosts;
        int64_t mandatoryCost = 0;

        MandOptPlan()
          : BooleanBulkBuildState(Kind::MAND_OPT) {}
      };

      enum class MaxScoreCountRoute : uint8_t {
        ORDINARY,
        DISJUNCTION_IDENTITY,
        DISJUNCTION_OF_CONJUNCTIONS,
      };

      enum class DisjunctionIdentityFallback : uint8_t {
        NONE,
        DELETES,
        NON_TERM,
        PROFITABILITY,
      };

      struct MaxScorePlan : BooleanBulkBuildState {
        std::span<PlannedSupplier> optional;
        std::span<int64_t> optionalCosts;
        std::span<PlannedSupplier> prohibited;
        int64_t aggregateClauseCost = 0;
        MaxScoreCountRoute countRoute = MaxScoreCountRoute::ORDINARY;
        DisjunctionIdentityFallback identityFallback =
            DisjunctionIdentityFallback::NONE;
        size_t identityLargestIndex = 0;

        MaxScorePlan()
          : BooleanBulkBuildState(Kind::MAX_SCORE) {}
      };

      struct FilteredDisjunctionPlan : BooleanBulkBuildState {
        PlannedSupplier filter{};
        DocSet* filterDocs = nullptr;
        bool arrayFilterFeed = false;
        bool postingsFilterFeed = false;
        std::span<PlannedSupplier> optional;

        FilteredDisjunctionPlan()
          : BooleanBulkBuildState(Kind::FILTERED_DISJUNCTION) {}
      };

      struct ExactFilteredMandOptPlan : BooleanBulkBuildState {
        ConjunctionPlan* required = nullptr;
        std::span<PlannedSupplier> optional;

        ExactFilteredMandOptPlan()
          : BooleanBulkBuildState(Kind::EXACT_FILTERED_MAND_OPT) {}
      };

      struct FilteredBodyPlan : BooleanBulkBuildState {
        Query::ScorerSupplier* bodySupplier = nullptr;
        BulkPlan bodyPlan;
        std::span<PlannedSupplier> filters;
        int64_t bodyCost = 0;
        int64_t filterCost = 0;

        FilteredBodyPlan()
          : BooleanBulkBuildState(Kind::FILTERED_BODY) {}
      };

      struct PlannedBulkSupplier {
        Query::ScorerSupplier* supplier = nullptr;
        int64_t cost = 0;
        BulkPlan plan;
      };

      struct FilterOnlyPlan : BooleanBulkBuildState {
        std::span<PlannedBulkSupplier> filters;

        FilterOnlyPlan()
          : BooleanBulkBuildState(Kind::FILTER_ONLY) {}
      };

      struct DelegateFilteredChildPlan : BooleanBulkBuildState {
        Query::ScorerSupplier* child = nullptr;
        BulkPlan childPlan;

        DelegateFilteredChildPlan()
          : BooleanBulkBuildState(Kind::DELEGATE_FILTERED_CHILD) {}
      };

      struct ConjunctionPlanningResult {
        ConjunctionPlanStatus status;
        ConjunctionPlan plan;
      };

      static constexpr AcceptedConjunctionRoutes routeBit(
          ConjunctionRoute route) {
        return 1u << (uint32_t) route;
      }

      static constexpr AcceptedConjunctionRoutes candidateRouteMask() {
        return routeBit(ConjunctionRoute::CANDIDATE_DOC_SET_FILTER)
            | routeBit(ConjunctionRoute::CANDIDATE_POSTINGS_FILTER)
            | routeBit(ConjunctionRoute::CANDIDATE_SCORING_TERM);
      }

      static constexpr AcceptedConjunctionRoutes countRouteMask(
          bool includeDocSetSparse) {
        AcceptedConjunctionRoutes routes =
            routeBit(ConjunctionRoute::COUNT_EXACT_DOCS_ONLY)
            | routeBit(ConjunctionRoute::COUNT_POSTINGS_SAMPLE)
            | routeBit(ConjunctionRoute::COUNT_DENSE);
        if (includeDocSetSparse) {
          routes |= routeBit(ConjunctionRoute::COUNT_DOC_SET_SPARSE);
        }
        return routes;
      }

      static constexpr AcceptedConjunctionRoutes allRouteMask() {
        return (1u << ((uint32_t) ConjunctionRoute::COUNT_DOC_SET_SPARSE + 1))
            - 1;
      }

      static std::optional<bool> directTerm(
          const Query::ScorerShape& shape) {
        if (shape.directKind == Query::DirectScorerKind::UNKNOWN) {
          return std::nullopt;
        }
        return shape.directKind == Query::DirectScorerKind::TERM;
      }

      static std::optional<bool> directDocSet(
          const Query::ScorerShape& shape) {
        if (shape.directDocSet == Query::DirectDocSetAccess::UNKNOWN) {
          return std::nullopt;
        }
        return shape.directDocSet == Query::DirectDocSetAccess::SUPPORTED;
      }

      static std::optional<bool> windowFillClause(
          const Query::ScorerShape& shape) {
        switch (shape.windowFillClause) {
          case Query::ClauseShape::DIRECT:
            return true;
          case Query::ClauseShape::FLAT_DISJUNCTION:
            return !ConjunctionBulkScorer::disableDisjGroupBulkForTests;
          case Query::ClauseShape::FLAT_CONJUNCTION:
            return false;
          case Query::ClauseShape::NONE:
            return false;
          case Query::ClauseShape::UNKNOWN:
            return std::nullopt;
        }
        std::unreachable();
      }

      static Query::UnresolvedSupplierCause firstUnresolvedCause(
          std::span<Query::ScorerSupplier* const> suppliers,
          const Query::PlanContext& buildContext) {
        for (auto* supplier : suppliers) {
          if (supplier != nullptr
              && supplier->describeScorer(buildContext).hasUnknown()) {
            return supplier->unresolvedScorerCause(buildContext);
          }
        }
        return Query::UnresolvedSupplierCause::OTHER;
      }

      static Query::ScorerShape plannedOptionalGroupShape(
          std::span<Query::ScorerSupplier* const> suppliers,
          const Query::PlanContext& buildContext) {
        size_t nonempty = nonemptyCount(suppliers, buildContext);
        if (nonempty == 0) {
          return emptyShape();
        }
        if (nonempty == 1) {
          for (auto* supplier : suppliers) {
            if (supplier == nullptr) continue;
            Query::ScorerShape shape =
                supplier->describeScorer(buildContext);
            if (shape.matchState == Query::MatchState::NONEMPTY) {
              return shape;
            }
          }
          std::unreachable();
        }
        Query::ScorerShape shape = opaqueShape(
            Query::MatchState::NONEMPTY,
            disjunctionTwoPhase(suppliers, buildContext));
        shape.windowFillClause = flatClauseShape(
            suppliers, buildContext, true);
        shape.termDisjunctionClause = flatClauseShape(
            suppliers, buildContext, false);
        return shape;
      }

      Query::Demand windowFillDemand(
          int64_t candidates, BulkUse use) const {
        assert(candidates >= 0);
        assert(windowFillSpanScaleForTests > 0);
        int64_t span = candidates
                > segment.maxDoc() / windowFillSpanScaleForTests
            ? segment.maxDoc()
            : candidates * windowFillSpanScaleForTests;
        return Query::Demand::fromCandidatesAndSpan(
            candidates, span, use);
      }

      ConjunctionPlanningResult planConjunctionOnce(
          MemPool& targetPool, ConjunctionMode mode,
          AcceptedConjunctionRoutes acceptedRoutes,
          BulkUse use,
          std::span<Query::ScorerSupplier* const>
              enclosingFilterSuppliers,
          bool perEntryDemand = false) {
        ConjunctionPlan plan;
        plan.mode = mode;
        plan.use = use;
        assert(!perEntryDemand || use == BulkUse::COUNT_WINDOWS);
        plan.enclosingFilters = enclosingFilterSuppliers.empty()
            ? EnclosingFilters::NOT_SUPPLIED
            : EnclosingFilters::CONSUMED;
        bool exhaustive = mode == ConjunctionMode::EXHAUSTIVE;
        bool includeFilters = mode != ConjunctionMode::SCORED_BODY;
        if (!includeFilters && !enclosingFilterSuppliers.empty()) {
          assert(false);
          return {ConjunctionPlanStatus::DECLINED, plan};
        }

        boost::container::small_vector<PlannedEntry, 16> entries;
        size_t entryOrder = 0;
        for (size_t i = 0; i < mandatoryShapeSuppliers.size(); i++) {
          auto* supplier = mandatoryShapeSuppliers[i];
          if (supplier == nullptr) {
            return {ConjunctionPlanStatus::NO_MATCH, plan};
          }
          entries.push_back({
              supplier->cost(), supplier, {}, false, false, false,
              mandatoryScores[i] != 0, entryOrder++});
        }
        if (includeFilters) {
          for (auto* supplier : filterSuppliers) {
            if (supplier == nullptr) {
              return {ConjunctionPlanStatus::NO_MATCH, plan};
            }
            entries.push_back({
                supplier->cost(), supplier, {}, false, true, false,
                false, entryOrder++});
          }
          for (auto* supplier : enclosingFilterSuppliers) {
            if (supplier == nullptr) {
              return {ConjunctionPlanStatus::NO_MATCH, plan};
            }
            entries.push_back({
                supplier->cost(), supplier, {}, false, true, false,
                false, entryOrder++});
          }
        }

        boost::container::small_vector<Query::ScorerSupplier*, 16>
            optionalSuppliers;
        boost::container::small_vector<int64_t, 16> optionalCosts;
        plan.hasOptionalGroup = exhaustive && minShouldMatch >= 1
            && !optionalSources.empty();
        if (plan.hasOptionalGroup) {
          for (auto* supplier : optionalShapeSuppliers) {
            if (supplier == nullptr) continue;
            optionalSuppliers.push_back(supplier);
            optionalCosts.push_back(supplier->cost());
          }
          if (optionalSuppliers.empty()) {
            return {ConjunctionPlanStatus::NO_MATCH, plan};
          }
          entries.push_back({
              optionalCost(optionalCosts, 1, segment.maxDoc()), nullptr, {},
              true, false, false, false, entryOrder++});
        }
        if (entries.empty()
            || (entries.size() < 2
                && (!exhaustive || prohibitedSources.empty()))) {
          return {ConjunctionPlanStatus::DECLINED, plan};
        }
        std::sort(entries.begin(), entries.end(),
                  [](const PlannedEntry& a, const PlannedEntry& b) {
                    return a.cost != b.cost ? a.cost < b.cost
                                            : a.order < b.order;
                  });
        plan.leadCost = entries[0].cost;
        for (size_t i = 1; i < entries.size(); i++) {
          plan.nonLeadCost += entries[i].cost;
        }

        Query::Demand optionalGroupBuildDemand =
            Query::Demand::fromLeadCost(plan.leadCost, use);
        for (size_t i = 0; i < entries.size(); i++) {
          PlannedEntry& entry = entries[i];
          entry.buildDemand = Query::Demand::fromLeadCost(
              plan.leadCost, use);
          if (perEntryDemand) {
            entry.buildDemand = entries.size() == 1
                ? Query::Demand::fromCandidatesAndSpan(
                    segment.maxDoc(), segment.maxDoc(), use)
                : windowFillDemand(
                    entries[i == 0 ? 1 : 0].cost, use);
          }
          Query::PlanContext buildContext =
              scorerBuildContext(entry.buildDemand);
          if (entry.optionalGroup) {
            optionalGroupBuildDemand = entry.buildDemand;
            if (hasUnknownElision(optionalSuppliers, buildContext)) {
              plan.unresolvedCause = firstUnresolvedCause(
                  optionalSuppliers, buildContext);
              return {ConjunctionPlanStatus::DECLINED, plan};
            }
            entry.shape = plannedOptionalGroupShape(
                optionalSuppliers, buildContext);
            if (entry.shape.matchState == Query::MatchState::EMPTY) {
              return {ConjunctionPlanStatus::NO_MATCH, plan};
            }
          } else {
            entry.shape = entry.supplier->describeScorer(buildContext);
            if (plan.unresolvedCause
                    == Query::UnresolvedSupplierCause::NONE
                && entry.shape.hasUnknown()) {
              plan.unresolvedCause =
                  entry.supplier->unresolvedScorerCause(buildContext);
            }
            if (entry.shape.matchState == Query::MatchState::EMPTY) {
              return {ConjunctionPlanStatus::NO_MATCH, plan};
            }
          }
        }

        boost::container::small_vector<Query::ScorerSupplier*, 16>
            prohibitedSuppliers;
        for (auto* supplier : prohibitedShapeSuppliers) {
          if (supplier != nullptr) {
            prohibitedSuppliers.push_back(supplier);
          }
        }
        Query::Demand prohibitedBuildDemand = perEntryDemand
            ? windowFillDemand(entries[0].cost, use)
            : Query::Demand::fromLeadCost(plan.leadCost, use);
        Query::PlanContext prohibitedBuildContext =
            scorerBuildContext(prohibitedBuildDemand);
        if (hasUnknownElision(
                prohibitedSuppliers, prohibitedBuildContext)) {
          plan.unresolvedCause = firstUnresolvedCause(
              prohibitedSuppliers, prohibitedBuildContext);
              return {ConjunctionPlanStatus::DECLINED, plan};
        }

        plan.entries = targetPool.make_span<PlannedEntry>(entries.size());
        std::copy(entries.begin(), entries.end(), plan.entries.begin());
        plan.optionalGroup =
            targetPool.make_span<PlannedSupplier>(optionalSuppliers.size());
        for (size_t i = 0; i < optionalSuppliers.size(); i++) {
          Query::PlanContext buildContext =
              scorerBuildContext(optionalGroupBuildDemand);
          plan.optionalGroup[i] = {
              optionalSuppliers[i],
              optionalSuppliers[i]->describeScorer(buildContext),
              optionalGroupBuildDemand};
          if (plan.unresolvedCause
                  == Query::UnresolvedSupplierCause::NONE
              && plan.optionalGroup[i].shape.hasUnknown()) {
            plan.unresolvedCause =
                optionalSuppliers[i]->unresolvedScorerCause(buildContext);
          }
        }
        plan.prohibited = targetPool.make_span<PlannedSupplier>(
            prohibitedSuppliers.size());
        for (size_t i = 0; i < prohibitedSuppliers.size(); i++) {
          plan.prohibited[i] = {
              prohibitedSuppliers[i],
              prohibitedSuppliers[i]->describeScorer(
                  prohibitedBuildContext),
              prohibitedBuildDemand};
          if (plan.unresolvedCause
                  == Query::UnresolvedSupplierCause::NONE
              && plan.prohibited[i].shape.hasUnknown()) {
            plan.unresolvedCause =
                prohibitedSuppliers[i]->unresolvedScorerCause(
                    prohibitedBuildContext);
          }
        }
        plan.negatedCount = std::any_of(
            plan.prohibited.begin(), plan.prohibited.end(),
            [](const PlannedSupplier& prohibited) {
              return prohibited.shape.matchState
                  == Query::MatchState::NONEMPTY;
            });

        auto finishRoute = [&](ConjunctionRoute route,
                               bool constructsConjunction) {
          plan.route = route;
          plan.recordConjunctionConstruction = constructsConjunction;
          ConjunctionPlanStatus status =
              (acceptedRoutes & routeBit(route)) != 0
              ? ConjunctionPlanStatus::READY
              : ConjunctionPlanStatus::DECLINED;
          return ConjunctionPlanningResult{status, plan};
        };

        bool exactFilteredTermConjunction =
            mode == ConjunctionMode::EXHAUSTIVE
            && !needsScores && !disableExactTermCountForTests
            && (!filterSuppliers.empty()
                || !enclosingFilterSuppliers.empty());
        if (exactFilteredTermConjunction) {
          bool structurallySupported = plan.entries.size()
                  >= ConjunctionBulkScorer::kIntegratedCountMinClauses
              && prohibitedSources.empty() && !plan.hasOptionalGroup;
          if (!structurallySupported) {
            if (!enclosingFilterSuppliers.empty()) {
              return {ConjunctionPlanStatus::DECLINED, plan};
            }
            exactFilteredTermConjunction = false;
          }
        }
        if (exactFilteredTermConjunction) {
          bool supported = true;
          for (const PlannedEntry& entry : plan.entries) {
            if (entry.shape.docsOnly == Query::DocsOnlyAccess::UNKNOWN) {
              return {ConjunctionPlanStatus::DECLINED, plan};
            }
            supported &= entry.shape.docsOnly
                == Query::DocsOnlyAccess::SUPPORTED;
          }
          if (supported) {
            return finishRoute(
                ConjunctionRoute::COUNT_EXACT_DOCS_ONLY, false);
          }
          if (!enclosingFilterSuppliers.empty()) {
            return {ConjunctionPlanStatus::DECLINED, plan};
          }
        }

        for (const PlannedEntry& entry : plan.entries) {
          if (entry.shape.directKind == Query::DirectScorerKind::UNKNOWN
              || entry.shape.windowFillClause
                  == Query::ClauseShape::UNKNOWN
              || entry.shape.termDisjunctionClause
                  == Query::ClauseShape::UNKNOWN) {
              return {ConjunctionPlanStatus::DECLINED, plan};
          }
        }

        if (mode != ConjunctionMode::EXHAUSTIVE) {
          for (const PlannedEntry& entry : plan.entries) {
            if (entry.shape.reportedTwoPhase
                == Query::ReportedTwoPhase::UNKNOWN) {
              return {ConjunctionPlanStatus::DECLINED, plan};
            }
            if (entry.shape.reportedTwoPhase
                == Query::ReportedTwoPhase::YES) {
              return {ConjunctionPlanStatus::DECLINED, plan};
            }
          }
        }

        if (mode == ConjunctionMode::CANDIDATE) {
          if (plan.entries[0].filter) {
            for (size_t i = 1; i < plan.entries.size(); i++) {
              std::optional<bool> term = directTerm(plan.entries[i].shape);
              if (!term.has_value()) {
              return {ConjunctionPlanStatus::DECLINED, plan};
              }
              if (!*term) {
                return {ConjunctionPlanStatus::DECLINED, plan};
              }
            }
            std::optional<bool> leadDocSet =
                directDocSet(plan.entries[0].shape);
            if (!leadDocSet.has_value()) {
              return {ConjunctionPlanStatus::DECLINED, plan};
            }
            plan.entries[0].docSetFilter = *leadDocSet;
            int64_t minRatio = *leadDocSet
                ? multiTermBatchMinRatioDocSet
                : multiTermBatchMinRatioPostings;
            if (plan.entries[1].cost
                < minRatio * plan.entries[0].cost) {
              return {ConjunctionPlanStatus::DECLINED, plan};
            }
          } else {
            std::optional<bool> leadTerm =
                directTerm(plan.entries[0].shape);
            if (!leadTerm.has_value()
                || plan.entries[0].shape.independentTerm
                    == Query::IndependentTermAccess::UNKNOWN) {
              return {ConjunctionPlanStatus::DECLINED, plan};
            }
            if (disableCandidateTermFeedForTests
                || !plan.entries[0].scoring || !*leadTerm
                || plan.entries[0].shape.independentTerm
                    != Query::IndependentTermAccess::SUPPORTED
                || plan.entries[0].cost
                       * kTermFeedMaxLeadFractionForTests
                    > segment.maxDoc()) {
              return {ConjunctionPlanStatus::DECLINED, plan};
            }
            for (PlannedEntry& entry : plan.entries) {
              std::optional<bool> term = directTerm(entry.shape);
              if (!term.has_value()) {
              return {ConjunctionPlanStatus::DECLINED, plan};
              }
              if (entry.filter) {
                std::optional<bool> docSet = directDocSet(entry.shape);
                if (!docSet.has_value()) {
              return {ConjunctionPlanStatus::DECLINED, plan};
                }
                entry.docSetFilter = *docSet;
                if (!*docSet && !*term) {
                  return {ConjunctionPlanStatus::DECLINED, plan};
                }
              } else if (!entry.scoring || !*term) {
                return {ConjunctionPlanStatus::DECLINED, plan};
              }
              if (entry.docSetFilter
                  && plan.entries[0].cost
                         * kTermFeedMinDocSetFilterRatio
                      > entry.cost) {
                return {ConjunctionPlanStatus::DECLINED, plan};
              }
            }
            plan.termFeed = true;
          }

          for (const PlannedSupplier& prohibited : plan.prohibited) {
            if (prohibited.shape.matchState == Query::MatchState::EMPTY) {
              continue;
            }
            if (prohibited.shape.termDisjunctionClause
                == Query::ClauseShape::UNKNOWN) {
              return {ConjunctionPlanStatus::DECLINED, plan};
            }
            if (prohibited.shape.termDisjunctionClause
                    != Query::ClauseShape::DIRECT
                && prohibited.shape.termDisjunctionClause
                    != Query::ClauseShape::FLAT_DISJUNCTION) {
              return {ConjunctionPlanStatus::DECLINED, plan};
            }
          }

          if (plan.termFeed) {
            return finishRoute(
                ConjunctionRoute::CANDIDATE_SCORING_TERM, true);
          }
          if (plan.entries[0].docSetFilter) {
            plan.docSetSparseEligible = true;
            return finishRoute(
                ConjunctionRoute::CANDIDATE_DOC_SET_FILTER, true);
          }
          if (!disableFilteredConjunctionPostingsFeedForTests) {
            std::optional<bool> leadTerm =
                directTerm(plan.entries[0].shape);
            if (!leadTerm.has_value()) {
              return {ConjunctionPlanStatus::DECLINED, plan};
            }
            if (*leadTerm) {
              return finishRoute(
                  ConjunctionRoute::CANDIDATE_POSTINGS_FILTER, true);
            }
          }
          return finishRoute(ConjunctionRoute::GENERIC, true);
        }

        if (mode == ConjunctionMode::SCORED_BODY) {
          return finishRoute(ConjunctionRoute::GENERIC, true);
        }

        bool postingsSampleCandidate = !needsScores
            && !disableIntegratedFilteredCountForTests
            && (!filterSuppliers.empty()
                || !enclosingFilterSuppliers.empty())
            && plan.entries.size()
                >= ConjunctionBulkScorer::kIntegratedCountMinClauses
            && prohibitedSources.empty() && !plan.hasOptionalGroup
            && segment.maxDoc() >= DocsEnumMeta::L1_DOCS
            && plan.leadCost >= std::max<int64_t>(
                1, (int64_t) segment.maxDoc()
                    / ConjunctionBulkScorer::
                        termTailDenseThresholdInverseForTests);
        if (postingsSampleCandidate) {
          bool allTerms = true;
          for (const PlannedEntry& entry : plan.entries) {
            std::optional<bool> term = directTerm(entry.shape);
            if (!term.has_value()) {
              return {ConjunctionPlanStatus::DECLINED, plan};
            }
            allTerms &= *term;
          }
          if (allTerms) {
            plan.hasDirectDenseClause =
                !ConjunctionBulkScorer::disableDirectDenseClausesForTests;
            return finishRoute(
                ConjunctionRoute::COUNT_POSTINGS_SAMPLE, true);
          }
        }

        bool allDense = true;
        for (const PlannedEntry& entry : plan.entries) {
          std::optional<bool> dense = windowFillClause(entry.shape);
          if (!dense.has_value()) {
              return {ConjunctionPlanStatus::DECLINED, plan};
          }
          allDense &= *dense;
        }
        bool allDenseProhibited = true;
        for (const PlannedSupplier& prohibited : plan.prohibited) {
          if (prohibited.shape.matchState == Query::MatchState::EMPTY) {
            continue;
          }
          std::optional<bool> dense = windowFillClause(prohibited.shape);
          if (!dense.has_value()) {
              return {ConjunctionPlanStatus::DECLINED, plan};
          }
          allDenseProhibited &= *dense;
        }

        if (!ConjunctionBulkScorer::disableDirectDenseClausesForTests) {
          for (const PlannedEntry& entry : plan.entries) {
            if (entry.shape.windowFillClause
                != Query::ClauseShape::DIRECT) {
              continue;
            }
            if (entry.shape.directKind
                == Query::DirectScorerKind::UNKNOWN) {
              return {ConjunctionPlanStatus::DECLINED, plan};
            }
            plan.hasDirectDenseClause |=
                entry.shape.directKind == Query::DirectScorerKind::TERM
                || entry.shape.directKind
                    == Query::DirectScorerKind::DOC_SET;
          }
        }

        bool sparseEligibleTails = plan.entries.size() > 1;
        if (allDense) {
          for (size_t i = 1; i < plan.entries.size(); i++) {
            std::optional<bool> term = directTerm(plan.entries[i].shape);
            if (!term.has_value()) {
              return {ConjunctionPlanStatus::DECLINED, plan};
            }
            bool directDenseDocSet =
                !ConjunctionBulkScorer::disableDirectDenseClausesForTests
                && plan.entries[i].shape.windowFillClause
                    == Query::ClauseShape::DIRECT
                && plan.entries[i].shape.directKind
                    == Query::DirectScorerKind::DOC_SET;
            sparseEligibleTails &= *term || directDenseDocSet;
          }
        }
        int32_t denseThresholdInverse = sparseEligibleTails
            ? ConjunctionBulkScorer::termTailDenseThresholdInverseForTests
            : ConjunctionBulkScorer::denseThresholdInverseForTests;
        bool denseCount = allDense && allDenseProhibited
            && segment.maxDoc() >= DocsEnumMeta::L1_DOCS
            && plan.leadCost >= std::max<int64_t>(
                1, (int64_t) segment.maxDoc() / denseThresholdInverse);
        if (denseCount) {
          return finishRoute(ConjunctionRoute::COUNT_DENSE, true);
        }

        std::optional<bool> leadDocSet =
            directDocSet(plan.entries[0].shape);
        if (!leadDocSet.has_value()) {
              return {ConjunctionPlanStatus::DECLINED, plan};
        }
        plan.entries[0].docSetFilter = *leadDocSet;
        plan.docSetSparseEligible = *leadDocSet;
        if (plan.docSetSparseEligible) {
          for (size_t i = 1; i < plan.entries.size(); i++) {
            std::optional<bool> term = directTerm(plan.entries[i].shape);
            if (!term.has_value()) {
              return {ConjunctionPlanStatus::DECLINED, plan};
            }
            plan.docSetSparseEligible &= *term;
          }
        }
        if (plan.docSetSparseEligible && prohibitedSources.empty()) {
          return finishRoute(
              ConjunctionRoute::COUNT_DOC_SET_SPARSE, true);
        }
        return finishRoute(ConjunctionRoute::GENERIC, true);
      }

      ConjunctionPlanningResult refreshConjunctionShapes(
          MemPool& targetPool, ConjunctionMode mode,
          AcceptedConjunctionRoutes acceptedRoutes,
          std::span<Query::ScorerSupplier* const> enclosingFilterSuppliers,
          const ConjunctionPlanningResult& original) {
        assert(original.plan.use == BulkUse::COUNT_WINDOWS);
        ConjunctionPlanningResult refreshed = planConjunctionOnce(
            targetPool, mode, acceptedRoutes, original.plan.use,
            enclosingFilterSuppliers, true);
        if (refreshed.status == ConjunctionPlanStatus::READY
            && refreshed.plan.route == original.plan.route) {
          assert(refreshed.plan.leadCost == original.plan.leadCost);
          assert(refreshed.plan.nonLeadCost == original.plan.nonLeadCost);
          return refreshed;
        }
        return original;
      }

      ConjunctionPlanningResult planConjunction(
          MemPool& targetPool, ConjunctionMode mode,
          AcceptedConjunctionRoutes acceptedRoutes,
          BulkUse use,
          std::span<Query::ScorerSupplier* const>
              enclosingFilterSuppliers = {}) {
        // Expansion capture does not depend on leadCost. Resolve it before
        // planConjunctionOnce reads supplier costs and sorts the entries, so
        // memo-backed costs determine both entry order and the build context.
        Query::PlanContext buildContext =
            scorerBuildContext(segment.maxDoc(), use);
        fillExpansionMemos(mandatoryShapeSuppliers, buildContext);
        fillExpansionMemos(filterSuppliers, buildContext);
        fillExpansionMemos(optionalShapeSuppliers, buildContext);
        fillExpansionMemos(prohibitedShapeSuppliers, buildContext);
        fillExpansionMemos(enclosingFilterSuppliers, buildContext);
        ConjunctionPlanningResult planning = planConjunctionOnce(
            targetPool, mode, acceptedRoutes, use,
            enclosingFilterSuppliers);
        if (use == BulkUse::COUNT_WINDOWS
            && planning.status == ConjunctionPlanStatus::READY
            && planning.plan.leadCost != segment.maxDoc()) {
          return refreshConjunctionShapes(
              targetPool, mode, acceptedRoutes, enclosingFilterSuppliers,
              planning);
        }
        return planning;
      }

      void resolveConjunctionChildren(
          MemPool& planPool, ConjunctionPlan& plan) {
        auto resolveChild = [&](PlannedSupplier& child) {
          Query::PlanContext planContext =
              scorerBuildContext(child.buildDemand);
          child.plan = child.supplier->resolve(planPool, planContext);
          assert(child.plan != nullptr);
        };

        for (PlannedEntry& entry : plan.entries) {
          if (entry.optionalGroup) {
            for (PlannedSupplier& child : plan.optionalGroup) {
              resolveChild(child);
            }
            continue;
          }
          Query::PlanContext planContext =
              scorerBuildContext(entry.buildDemand);
          entry.plan = entry.supplier->resolve(planPool, planContext);
          assert(entry.plan != nullptr);
        }
        if (plan.termFeed) {
          Query::PlanContext planContext = scorerBuildContext(
              plan.leadCost, plan.use);
          plan.independentLeadPlan =
              plan.entries[0].supplier->resolve(planPool, planContext);
          assert(plan.independentLeadPlan != nullptr);
        }
        for (PlannedSupplier& child : plan.prohibited) {
          resolveChild(child);
        }
      }

      static BulkPlan knownBulkPlan(
          BulkAnswer available, BulkAnswer matchWindows,
          BulkAnswer exactCandidateScoring,
          BulkAnswer consumesFilters = BulkAnswer::NO,
          BulkAnswer acceptsWindowFilter = BulkAnswer::NO) {
        BulkPlan plan{
          available,
          matchWindows,
          exactCandidateScoring,
          consumesFilters,
        };
        plan.acceptsWindowFilter = acceptsWindowFilter;
        return plan;
      }

      static BulkPlan noBulkPlan() {
        return knownBulkPlan(
            BulkAnswer::NO, BulkAnswer::NO, BulkAnswer::NO);
      }

      static BulkPlan matchWindowBulkPlan() {
        return knownBulkPlan(
            BulkAnswer::YES, BulkAnswer::YES, BulkAnswer::NO,
            BulkAnswer::NO, BulkAnswer::YES);
      }

      BulkPlan planConjunctionBulk(
          BulkUse use, ConjunctionMode mode,
          AcceptedConjunctionRoutes acceptedRoutes,
          std::span<Query::ScorerSupplier* const>
              enclosingFilterSuppliers = {}) {
        ConjunctionPlanningResult planning = planConjunction(
            pool, mode, acceptedRoutes, use,
            enclosingFilterSuppliers);
        switch (planning.status) {
          case ConjunctionPlanStatus::READY: {
            bool acceptsWindowFilter = true;
            for (const PlannedEntry& entry : planning.plan.entries) {
              acceptsWindowFilter &=
                  entry.shape.termDisjunctionClause
                      == Query::ClauseShape::DIRECT
                  || (!ConjunctionBulkScorer::
                           disableDisjGroupBulkForTests
                      && entry.shape.termDisjunctionClause
                          == Query::ClauseShape::FLAT_DISJUNCTION);
            }
            BulkPlan bulkPlan = knownBulkPlan(
                BulkAnswer::YES, BulkAnswer::YES,
                mode == ConjunctionMode::CANDIDATE
                    ? BulkAnswer::YES : BulkAnswer::NO,
                enclosingFilterSuppliers.empty()
                    ? BulkAnswer::NO : BulkAnswer::YES,
                acceptsWindowFilter
                    ? BulkAnswer::YES : BulkAnswer::NO);
            resolveConjunctionChildren(pool, planning.plan);
            auto* buildPlan = pool.make<ConjunctionPlan>(planning.plan);
            buildPlan->owner = this;
            bulkPlan.buildState = buildPlan;
            return bulkPlan;
          }
          case ConjunctionPlanStatus::DECLINED:
          case ConjunctionPlanStatus::NO_MATCH:
            return noBulkPlan();
        }
        std::unreachable();
      }

      bool filteredDisjunctionDensityDecline() const {
        if (disableFilteredDisjunctionBatchForTests || allowsPruning
            || !mandatorySources.empty() || optionalSources.size() < 2
            || !prohibitedSources.empty() || filterSuppliers.size() != 1
            || minShouldMatch != 1 || filterSuppliers[0] == nullptr) {
          return false;
        }
        int32_t densityInverse = needsScores
            ? filteredDisjunctionBatchDensityInverseForTests
            : QueryPrep::kSparseBatchCountOwnDensityInverse;
        return densityInverse <= 0
            || filterSuppliers[0]->cost()
                > segment.maxDoc() / densityInverse;
      }

      // Build a bulk conjunction from the required clause boundaries.
      // EXHAUSTIVE includes filters and count-only optional groups.
      // CANDIDATE includes direct filters but retains scored, single-phase
      // construction. The cheapest required clause can then own a docs-only
      // feed for exact or pruned collection; scoring clauses alone supply
      // score bounds. SCORED_BODY leaves filters to a window mask selected by
      // the filtered scored-body route.
      MaybeConjunctionBulk buildConjunction(
          MemPool& targetPool, const ConjunctionPlan& plan) {
        auto assertBuildDemand = [&](const Query::Demand& demand) {
          assert(demand.horizon == plan.use);
          if (plan.use == BulkUse::COUNT_WINDOWS) {
            assert(demand.candidates >= plan.leadCost);
            assert(demand.candidates <= segment.maxDoc());
            assert(demand.span >= demand.candidates);
            assert(demand.span <= segment.maxDoc());
          } else {
            assert(demand.candidates == plan.leadCost);
            assert(demand.span == plan.leadCost);
          }
        };
        for (const PlannedEntry& entry : plan.entries) {
          assertBuildDemand(entry.buildDemand);
        }
        for (const PlannedSupplier& member : plan.optionalGroup) {
          assertBuildDemand(member.buildDemand);
        }
        for (const PlannedSupplier& prohibited : plan.prohibited) {
          assertBuildDemand(prohibited.buildDemand);
        }
        if (plan.route == ConjunctionRoute::COUNT_EXACT_DOCS_ONLY) {
          auto countTermEnums =
              targetPool.make_span<DocsOnlyEnum*>(plan.entries.size());
          for (size_t i = 0; i < plan.entries.size(); i++) {
            assert(plan.entries[i].shape.docsOnly
                   == Query::DocsOnlyAccess::SUPPORTED);
            assert(plan.entries[i].plan != nullptr);
            countTermEnums[i] =
                plan.entries[i].plan->buildDocsOnly(targetPool);
            if (countTermEnums[i] == nullptr) {
              return {};
            }
          }
          return ConjunctionBulkResult{
            targetPool.make<ExactDocsOnlyTermConjunctionBulkScorer>(
                targetPool, countTermEnums, segment.maxDoc()),
            plan.route, plan.enclosingFilters
          };
        }

        auto* arr =
            targetPool.make_arr<Query::Scorer*>(plan.entries.size());
        auto clauseScores =
            targetPool.make_span<uint8_t>(plan.entries.size());
        auto materializeClause = [&](Query::Scorer** scorerSlot,
                                     const Query::ScorerShape& shape) {
          Query::Scorer* scorer = *scorerSlot;
          ConjunctionClauseLayout layout;
          layout.windowFillKind = shape.windowFillClause;
          switch (shape.directKind) {
            case Query::DirectScorerKind::TERM:
              layout.directTerm =
                  dynamic_cast<TermQuery::Scorer*>(scorer);
              assert(layout.directTerm != nullptr);
              break;
            case Query::DirectScorerKind::DOC_SET:
              layout.directDocSet =
                  dynamic_cast<QueryPrep::DocSetScorer*>(scorer);
              assert(layout.directDocSet != nullptr);
              break;
            case Query::DirectScorerKind::OTHER:
            case Query::DirectScorerKind::UNKNOWN:
              break;
          }
          switch (shape.windowFillClause) {
            case Query::ClauseShape::DIRECT:
              layout.denseMembers = {scorerSlot, 1};
              break;
            case Query::ClauseShape::FLAT_DISJUNCTION:
              if (!ConjunctionBulkScorer::disableDisjGroupBulkForTests) {
                layout.denseMembers = scorer->flatDisjunctionScorers();
                assert(!layout.denseMembers.empty());
              }
              break;
            case Query::ClauseShape::FLAT_CONJUNCTION:
              assert(false);
              break;
            case Query::ClauseShape::NONE:
            case Query::ClauseShape::UNKNOWN:
              break;
          }
          switch (shape.termDisjunctionClause) {
            case Query::ClauseShape::DIRECT:
              assert(layout.directTerm != nullptr);
              layout.termMembers = {scorerSlot, 1};
              break;
            case Query::ClauseShape::FLAT_DISJUNCTION:
              if (!ConjunctionBulkScorer::disableDisjGroupBulkForTests) {
                layout.termMembers = scorer->flatDisjunctionScorers();
                assert(!layout.termMembers.empty());
#ifndef NDEBUG
                for (Query::Scorer* member : layout.termMembers) {
                  assert(dynamic_cast<TermQuery::Scorer*>(member) != nullptr);
                }
#endif
              }
              break;
            case Query::ClauseShape::FLAT_CONJUNCTION:
              assert(false);
              break;
            case Query::ClauseShape::NONE:
            case Query::ClauseShape::UNKNOWN:
              break;
          }
          return layout;
        };
        auto clauseLayouts =
            targetPool.make_span<ConjunctionClauseLayout>(
                plan.entries.size());
        for (size_t i = 0; i < plan.entries.size(); i++) {
          const PlannedEntry& entry = plan.entries[i];
          Query::Scorer* scorer;
          if (entry.optionalGroup) {
            auto* members = targetPool.make_arr<Query::Scorer*>(
                plan.optionalGroup.size());
            auto memberTwoPhase = targetPool.make_span<uint8_t>(
                plan.optionalGroup.size());
            size_t memberCount = 0;
            for (const PlannedSupplier& memberPlan : plan.optionalGroup) {
              assert(memberPlan.plan != nullptr);
              auto* member = memberPlan.plan->build(targetPool);
              if (member != nullptr) {
                members[memberCount] = member;
                assert(memberPlan.plan->shape().reportedTwoPhase
                       != Query::ReportedTwoPhase::UNKNOWN);
                memberTwoPhase[memberCount] =
                    memberPlan.plan->shape().reportedTwoPhase
                        == Query::ReportedTwoPhase::YES;
                memberCount++;
              }
            }
            if (memberCount == 0) {
              return {};
            }
            scorer = memberCount == 1
                ? members[0]
                : targetPool.make<BooleanQuery::DisjunctionScorer>(
                    targetPool,
                    std::span<Query::Scorer*>(members, memberCount),
                    memberTwoPhase.first(memberCount),
                    false);
          } else {
            assert(entry.plan != nullptr);
            scorer = entry.plan->build(targetPool);
          }
          if (scorer == nullptr) {
            return {};
          }
          if (plan.mode != ConjunctionMode::EXHAUSTIVE) {
            assert(entry.plan->shape().reportedTwoPhase
                   == Query::ReportedTwoPhase::NO);
            if (entry.plan->shape().reportedTwoPhase
                != Query::ReportedTwoPhase::NO) {
              return {};
            }
          }
          arr[i] = scorer;
          clauseScores[i] = entry.scoring;
          clauseLayouts[i] = materializeClause(arr + i, entry.shape);
        }

        TermQuery::Scorer* candidateLeadScoreScorer = nullptr;
        std::span<uint8_t> candidateFilters;
        std::span<DocSet*> candidateFilterDocSets;
        if (plan.termFeed) {
          assert(plan.independentLeadPlan != nullptr);
          candidateLeadScoreScorer = dynamic_cast<TermQuery::Scorer*>(
              plan.independentLeadPlan->buildIndependent(targetPool));
          assert(candidateLeadScoreScorer != nullptr);
          if (candidateLeadScoreScorer == nullptr) {
            return {};
          }
          candidateFilters =
              targetPool.make_span<uint8_t>(plan.entries.size());
          candidateFilterDocSets =
              targetPool.make_span<DocSet*>(plan.entries.size());
          std::fill(candidateFilters.begin(), candidateFilters.end(), 0);
          std::fill(
              candidateFilterDocSets.begin(),
              candidateFilterDocSets.end(), nullptr);
          for (size_t i = 0; i < plan.entries.size(); i++) {
            if (!plan.entries[i].filter) continue;
            candidateFilters[i] = 1;
            if (auto* supplier =
                    dynamic_cast<QueryPrep::DocSetSupplier*>(
                        plan.entries[i].supplier)) {
              candidateFilterDocSets[i] = supplier->docSet();
            }
          }
        }

        bool runtimeDocSetSparseEligible =
            plan.entries[0].docSetFilter;
        if (runtimeDocSetSparseEligible) {
          for (size_t i = 1; i < plan.entries.size(); i++) {
            if (clauseLayouts[i].directTerm == nullptr) {
              runtimeDocSetSparseEligible = false;
              break;
            }
          }
        }
#ifndef NDEBUG
        assert(runtimeDocSetSparseEligible == plan.docSetSparseEligible);
#endif

        TermQuery::Scorer* candidatePostingsLead = nullptr;
        if (plan.route == ConjunctionRoute::CANDIDATE_SCORING_TERM
            || plan.route
                == ConjunctionRoute::CANDIDATE_POSTINGS_FILTER
            || plan.route == ConjunctionRoute::COUNT_POSTINGS_SAMPLE) {
          candidatePostingsLead = clauseLayouts[0].directTerm;
          assert(candidatePostingsLead != nullptr);
          if (candidatePostingsLead == nullptr) {
            return {};
          }
        }

        Query::Scorer** prohibitedArr = nullptr;
        size_t prohibitedCount = 0;
        std::span<ConjunctionClauseLayout> prohibitedLayouts;
        std::span<TermQuery::Scorer*> candidateProhibitedTerms;
        if (plan.mode == ConjunctionMode::EXHAUSTIVE) {
          prohibitedArr = targetPool.make_arr<Query::Scorer*>(
              plan.prohibited.size());
          prohibitedLayouts =
              targetPool.make_span<ConjunctionClauseLayout>(
              plan.prohibited.size());
          for (const PlannedSupplier& prohibited : plan.prohibited) {
            assert(prohibited.plan != nullptr);
            auto* scorer = prohibited.plan->build(targetPool);
            if (scorer != nullptr) {
              prohibitedArr[prohibitedCount] = scorer;
              prohibitedLayouts[prohibitedCount] =
                  materializeClause(
                      prohibitedArr + prohibitedCount,
                      prohibited.shape);
              prohibitedCount++;
            }
          }
        } else if (plan.mode == ConjunctionMode::CANDIDATE
                   && !plan.prohibited.empty()) {
          auto& terms = *targetPool.make_vec<TermQuery::Scorer*>();
          for (const PlannedSupplier& prohibited : plan.prohibited) {
            assert(prohibited.plan != nullptr);
            auto* scorer = prohibited.plan->build(targetPool);
            if (scorer == nullptr) continue;
            if (auto* term = dynamic_cast<TermQuery::Scorer*>(scorer)) {
              terms.push_back(term);
              continue;
            }
            auto members = scorer->flatDisjunctionScorers();
            assert(!members.empty());
            if (members.empty()) {
              return {};
            }
            for (auto* member : members) {
              auto* term = dynamic_cast<TermQuery::Scorer*>(member);
              assert(term != nullptr);
              if (term == nullptr) {
                return {};
              }
              terms.push_back(term);
            }
          }
          candidateProhibitedTerms = {terms.data(), terms.size()};
        }

        auto* bulk = targetPool.make<BooleanQuery::ConjunctionBulkScorer>(
            targetPool,
            std::span<Query::Scorer*>(arr, plan.entries.size()),
            std::span<Query::Scorer*>(prohibitedArr, prohibitedCount),
            candidateProhibitedTerms, clauseScores, segment.maxDoc(),
            plan.leadCost, plan.nonLeadCost,
            plan.mode != ConjunctionMode::EXHAUSTIVE,
            plan.entries[0].docSetFilter, candidatePostingsLead,
            candidateLeadScoreScorer, candidateFilters,
            candidateFilterDocSets, plan.negatedCount, false,
            clauseLayouts,
            prohibitedLayouts.first(prohibitedCount));

#ifndef NDEBUG
        ConjunctionRoute builtRoute = ConjunctionRoute::GENERIC;
        if (plan.mode == ConjunctionMode::CANDIDATE) {
          if (plan.termFeed && candidatePostingsLead != nullptr) {
            builtRoute = ConjunctionRoute::CANDIDATE_SCORING_TERM;
          } else if (candidatePostingsLead != nullptr) {
            builtRoute = ConjunctionRoute::CANDIDATE_POSTINGS_FILTER;
          } else if (runtimeDocSetSparseEligible) {
            builtRoute = ConjunctionRoute::CANDIDATE_DOC_SET_FILTER;
          }
        } else if (plan.mode == ConjunctionMode::EXHAUSTIVE) {
          if (bulk->willSampleCandidateCount()) {
            builtRoute = ConjunctionRoute::COUNT_POSTINGS_SAMPLE;
          } else if (bulk->willCountDense()) {
            builtRoute = ConjunctionRoute::COUNT_DENSE;
          } else if (runtimeDocSetSparseEligible
                     && prohibitedSources.empty()) {
            builtRoute = ConjunctionRoute::COUNT_DOC_SET_SPARSE;
          }
        }
        assert(builtRoute == plan.route);
#endif
        return ConjunctionBulkResult{
          bulk, plan.route, plan.enclosingFilters
        };
      }

      void recordConjunctionPlanCommitment(
          const ConjunctionPlan& plan) const {
        if (plan.recordConjunctionConstruction
            && plan.hasDirectDenseClause) {
          skipCount(SkipStats::conjDirectDenseEngagements);
        }
      }

      void recordCandidateConjunctionEngagement(
          const ConjunctionBulkResult& result) const {
        assert(result.usesCandidateRoute());
        skipCount(SkipStats::filteredConjBatchEngagements);
        if (mandatorySources.size() > 1) {
          skipCount(SkipStats::filteredConjBatchMultiTermEngagements);
        }
        if (result.route
            == ConjunctionRoute::CANDIDATE_POSTINGS_FILTER) {
          skipCount(SkipStats::filteredConjBatchPostingsFeedEngagements);
        }
        if (result.route == ConjunctionRoute::CANDIDATE_SCORING_TERM) {
          skipCount(SkipStats::candidateTermFeedEngagements);
        }
      }

      BulkPlan planMandOptBulk(
          BulkUse use,
          const Query::ScorerSupplier::BulkScorerContext& bulkContext) {
        auto* mandSupplier = mandatoryShapeSuppliers[0];
        if (mandSupplier == nullptr) {
          return noBulkPlan();
        }
        if (bulkContext.hasFilter()
            && !disableFilteredMandOptFillGateForTests
            && filterDensityBelow(
                bulkContext.filterCost, segment.maxDoc(),
                kMandOptScalarFillDensityInverse)
            && mandSupplier->scoreBlockFillKind()
                == Query::ScorerSupplier::ScoreBlockFillKind::DEFAULT_SCALAR) {
          return noBulkPlan();
        }
        int64_t mandCost = mandSupplier->cost();
        Query::Demand demand = Query::Demand::fromLeadCost(mandCost, use);
        Query::PlanContext buildContext = scorerBuildContext(demand);
        Query::ScorerShape mandShape =
            describeResolved(mandSupplier, buildContext);
        if (mandShape.matchState != Query::MatchState::NONEMPTY
            || mandShape.reportedTwoPhase != Query::ReportedTwoPhase::NO) {
          return noBulkPlan();
        }

        boost::container::small_vector<PlannedSupplier, 16> optional;
        boost::container::small_vector<int64_t, 16> optionalCosts;
        for (auto* supplier : optionalShapeSuppliers) {
          if (supplier == nullptr) {
            continue;
          }
          int64_t cost = supplier->cost();
          Query::ScorerShape shape =
              describeResolved(supplier, buildContext);
          if (shape.matchState == Query::MatchState::EMPTY) {
            continue;
          }
          if (shape.matchState != Query::MatchState::NONEMPTY
              || shape.reportedTwoPhase != Query::ReportedTwoPhase::NO) {
            return noBulkPlan();
          }
          optional.push_back({supplier, shape, demand});
          optionalCosts.push_back(cost);
        }
        if (optional.empty()) {
          return noBulkPlan();
        }

        auto* state = pool.make<MandOptPlan>();
        state->owner = this;
        state->mandatory = {mandSupplier, mandShape, demand};
        state->mandatory.plan = mandSupplier->resolve(pool, buildContext);
        state->mandatoryCost = mandCost;
        state->optional = pool.make_span<PlannedSupplier>(optional.size());
        state->optionalCosts = pool.make_span<int64_t>(optionalCosts.size());
        for (size_t i = 0; i < optional.size(); i++) {
          optional[i].plan = optional[i].supplier->resolve(
              pool, buildContext);
          state->optional[i] = optional[i];
          state->optionalCosts[i] = optionalCosts[i];
        }
        BulkPlan plan = matchWindowBulkPlan();
        plan.buildState = state;
        plan.acceptsWindowFilter = BulkAnswer::YES;
        return plan;
      }

      BulkScorer* buildMandOptBulk(
          MemPool& targetPool, const MandOptPlan& plan) {
        Query::Scorer* mandatory = plan.mandatory.plan->build(targetPool);
        assert(mandatory != nullptr);
        auto optional = targetPool.make_span<Query::Scorer*>(
            plan.optional.size());
        for (size_t i = 0; i < plan.optional.size(); i++) {
          optional[i] = plan.optional[i].plan->build(targetPool);
          assert(optional[i] != nullptr);
        }
        return targetPool.make<BooleanQuery::MandOptBulkScorer>(
            targetPool, mandatory, optional, plan.optionalCosts,
            segment.maxDoc(), plan.mandatoryCost);
      }

      BulkPlan planMaxScoreBulk(BulkUse use,
                                bool withBulkExclusion = false) {
        Query::Demand optionalDemand = Query::Demand::fromLeadCost(
            std::numeric_limits<int64_t>::max(), use);
        Query::PlanContext optionalContext =
            scorerBuildContext(optionalDemand);
        boost::container::small_vector<PlannedSupplier, 16> optional;
        boost::container::small_vector<int64_t, 16> optionalCosts;
        // Count-only identity routing needs every per-segment term df even for
        // the common two-clause case. Scored execution keeps its existing
        // four-clause threshold for retaining costs.
        bool retainCosts = !needsScores
            || optionalSources.size() >= kCostAwareOrderMinClauses;
        int64_t aggregateClauseCost = 0;
        for (auto* supplier : optionalShapeSuppliers) {
          if (supplier == nullptr) {
            continue;
          }
          int64_t cost = supplier->cost();
          Query::ScorerShape shape =
              describeResolved(supplier, optionalContext);
          if (shape.matchState == Query::MatchState::EMPTY) {
            continue;
          }
          if (shape.matchState != Query::MatchState::NONEMPTY) {
            return noBulkPlan();
          }
          optional.push_back({supplier, shape, optionalDemand});
          optionalCosts.push_back(cost);
          if (cost > 0
              && aggregateClauseCost < std::numeric_limits<int64_t>::max()) {
            int64_t room = std::numeric_limits<int64_t>::max()
                - aggregateClauseCost;
            if (cost >= room) {
              aggregateClauseCost = std::numeric_limits<int64_t>::max();
            } else {
              aggregateClauseCost += cost;
            }
          }
        }
        if (optional.size() < 2) {
          return noBulkPlan();
        }

        Query::Demand exclusionDemand = Query::Demand::fromLeadCost(
            aggregateClauseCost, use);
        Query::PlanContext exclusionContext =
            scorerBuildContext(exclusionDemand);
        boost::container::small_vector<PlannedSupplier, 16> prohibited;
        if (withBulkExclusion) {
          for (auto* supplier : prohibitedShapeSuppliers) {
            if (supplier == nullptr) {
              continue;
            }
            Query::ScorerShape shape =
                describeResolved(supplier, exclusionContext);
            if (shape.matchState == Query::MatchState::EMPTY) {
              continue;
            }
            if (shape.matchState != Query::MatchState::NONEMPTY
                || (shape.windowFillClause != Query::ClauseShape::DIRECT
                    && shape.windowFillClause
                        != Query::ClauseShape::FLAT_DISJUNCTION)) {
              return noBulkPlan();
            }
            prohibited.push_back({supplier, shape, exclusionDemand});
          }
        }

        auto* state = pool.make<MaxScorePlan>();
        state->owner = this;
        state->aggregateClauseCost = aggregateClauseCost;
        state->optional = pool.make_span<PlannedSupplier>(optional.size());
        state->optionalCosts = retainCosts
            ? pool.make_span<int64_t>(optional.size())
            : std::span<int64_t>{};
        for (size_t i = 0; i < optional.size(); i++) {
          optional[i].plan = optional[i].supplier->resolve(
              pool, optionalContext);
          state->optional[i] = optional[i];
          if (retainCosts) state->optionalCosts[i] = optionalCosts[i];
        }
        state->prohibited =
            pool.make_span<PlannedSupplier>(prohibited.size());
        for (size_t i = 0; i < prohibited.size(); i++) {
          prohibited[i].plan = prohibited[i].supplier->resolve(
              pool, exclusionContext);
          state->prohibited[i] = prohibited[i];
        }

        if (!needsScores && !disableDisjunctionCountIdentityForTests) {
          if (segment.liveDocs() != nullptr) {
            state->identityFallback =
                DisjunctionIdentityFallback::DELETES;
          } else {
          bool allTerms = true;
          size_t largestIndex = 0;
          int64_t largestDf = -1;
          int64_t totalDf = 0;
          for (size_t i = 0; i < optional.size(); i++) {
            allTerms &= optional[i].shape.directKind
                == Query::DirectScorerKind::TERM;
            int64_t df = optionalCosts[i];
            if (df > largestDf) {
              largestDf = df;
              largestIndex = i;
            }
            totalDf = df >= std::numeric_limits<int64_t>::max() - totalDf
                ? std::numeric_limits<int64_t>::max()
                : totalDf + df;
          }
          int64_t otherDf = totalDf == std::numeric_limits<int64_t>::max()
              ? totalDf : totalDf - largestDf;
          if (!allTerms) {
            state->identityFallback =
                DisjunctionIdentityFallback::NON_TERM;
          } else if (otherDf > 0
              && otherDf
                  <= MaxScoreBulkScorer::kDisjunctionCountIdentityMaxProbes
              && otherDf
                  <= largestDf
                      / MaxScoreBulkScorer::
                          kDisjunctionCountIdentityMinDfRatio) {
            state->countRoute =
                MaxScoreCountRoute::DISJUNCTION_IDENTITY;
            state->identityLargestIndex = largestIndex;
          } else {
            state->identityFallback =
                DisjunctionIdentityFallback::PROFITABILITY;
          }
          }
        }

        if (!needsScores
            && state->countRoute == MaxScoreCountRoute::ORDINARY
            && !MaxScoreBulkScorer::disableDisjConjBulkForTests) {
          bool hasConjunction = false;
          bool supported = true;
          for (const PlannedSupplier& child : optional) {
            supported &= child.shape.termConjunctionClause
                    == Query::ClauseShape::DIRECT
                || child.shape.termConjunctionClause
                    == Query::ClauseShape::FLAT_CONJUNCTION;
            hasConjunction |= child.shape.termConjunctionClause
                == Query::ClauseShape::FLAT_CONJUNCTION;
          }
          if (supported && hasConjunction) {
            state->countRoute =
                MaxScoreCountRoute::DISJUNCTION_OF_CONJUNCTIONS;
          }
        }

        BulkPlan plan = matchWindowBulkPlan();
        plan.buildState = state;
        plan.acceptsWindowFilter = BulkAnswer::YES;
        return plan;
      }

      BulkScorer* buildMaxScoreBulk(
          MemPool& targetPool, const MaxScorePlan& plan) {
        switch (plan.identityFallback) {
          case DisjunctionIdentityFallback::NONE:
            break;
          case DisjunctionIdentityFallback::DELETES:
            skipCount(SkipStats::disjCountIdentityDeleteFallbacks);
            break;
          case DisjunctionIdentityFallback::NON_TERM:
            skipCount(SkipStats::disjCountIdentityNonTermFallbacks);
            break;
          case DisjunctionIdentityFallback::PROFITABILITY:
            skipCount(
                SkipStats::disjCountIdentityProfitabilityFallbacks);
            break;
        }
        auto optional = targetPool.make_span<Query::Scorer*>(
            plan.optional.size());
        for (size_t i = 0; i < plan.optional.size(); i++) {
          optional[i] = plan.optional[i].plan->build(targetPool);
          assert(optional[i] != nullptr);
        }
        auto& exclusion = *targetPool.make_vec<Query::Scorer*>();
        for (const PlannedSupplier& child : plan.prohibited) {
          Query::Scorer* scorer = child.plan->build(targetPool);
          assert(scorer != nullptr);
          scorer->recordWindowFilterCommit(true);
          if (child.shape.windowFillClause == Query::ClauseShape::DIRECT) {
            exclusion.push_back(scorer);
          } else {
            auto members = scorer->flatDisjunctionScorers();
            assert(!members.empty());
            for (Query::Scorer* member : members) {
              member->recordWindowFilterCommit(true);
              exclusion.push_back(member);
            }
          }
        }
        if (!plan.prohibited.empty()) {
          skipCount(SkipStats::bulkExclusionEngagements);
        }
        return targetPool.make<BooleanQuery::MaxScoreBulkScorer>(
            targetPool, optional, plan.optionalCosts,
            std::span<Query::Scorer*>(exclusion.data(), exclusion.size()),
            segment.maxDoc(), plan.aggregateClauseCost,
            plan.countRoute
                == MaxScoreCountRoute::DISJUNCTION_OF_CONJUNCTIONS,
            plan.countRoute
                == MaxScoreCountRoute::DISJUNCTION_IDENTITY,
            plan.identityLargestIndex);
      }

      int64_t minFilterCost() const {
        int64_t cost = std::numeric_limits<int64_t>::max();
        for (auto* supplier : filterSuppliers) {
          if (supplier == nullptr) {
            return -1;
          }
          cost = std::min(cost, supplier->cost());
        }
        return cost;
      }

      BulkPlan planFilteredDisjunctionBulk(BulkUse use) {
        if (disableFilteredDisjunctionBatchForTests || allowsPruning
            || mandatorySources.size() != 0 || optionalSources.size() < 2
            || prohibitedSources.size() != 0 || filterSuppliers.size() != 1
            || minShouldMatch != 1) {
          return noBulkPlan();
        }
        auto* filterSupplier = filterSuppliers[0];
        if (filterSupplier == nullptr) return noBulkPlan();
        int64_t filterCost = filterSupplier->cost();
        int32_t densityInverse = needsScores
            ? filteredDisjunctionBatchDensityInverseForTests
            : QueryPrep::kSparseBatchCountOwnDensityInverse;
        if (densityInverse <= 0
            || filterCost > segment.maxDoc() / densityInverse) {
          return noBulkPlan();
        }

        Query::Demand filterDemand = Query::Demand::fromLeadCost(
            filterCost, use);
        Query::PlanContext filterContext =
            scorerBuildContext(filterDemand);
        PlannedSupplier filter{};
        DocSet* filterDocs = nullptr;
        bool arrayFilterFeed = false;
        bool postingsFilterFeed = false;
        if (auto* docSetSupplier =
                dynamic_cast<QueryPrep::DocSetSupplier*>(filterSupplier)) {
          DocSet* docs = docSetSupplier->docSet();
          if (docs == nullptr || docs->card() == 0) {
            return noBulkPlan();
          }
          if (!disableFilteredDisjunctionArrayFeedForTests
              && docs->type == DocSet::ARRAY) {
            filterDocs = docs;
            arrayFilterFeed = true;
          } else {
            Query::ScorerShape shape =
                filterSupplier->describeScorer(filterContext);
            filter = {filterSupplier, shape, filterDemand};
          }
        } else if (!QueryPrep::
                       disableSparseBatchPostingsFeedForTests) {
          Query::ScorerShape shape =
              describeResolved(filterSupplier, filterContext);
          if (shape.matchState == Query::MatchState::NONEMPTY
              && shape.directKind == Query::DirectScorerKind::TERM) {
            filter = {filterSupplier, shape, filterDemand};
            postingsFilterFeed = true;
          }
        }
        if (!arrayFilterFeed && filter.supplier == nullptr) {
          return noBulkPlan();
        }

        Query::Demand optionalDemand = Query::Demand::fromLeadCost(
            std::numeric_limits<int64_t>::max(), use);
        Query::PlanContext optionalContext =
            scorerBuildContext(optionalDemand);
        boost::container::small_vector<PlannedSupplier, 16> entries;
        for (auto* supplier : optionalShapeSuppliers) {
          if (supplier == nullptr) {
            continue;
          }
          Query::ScorerShape shape =
              describeResolved(supplier, optionalContext);
          if (shape.matchState == Query::MatchState::EMPTY) {
            continue;
          }
          if (shape.matchState != Query::MatchState::NONEMPTY
              || shape.directKind != Query::DirectScorerKind::TERM) {
            return noBulkPlan();
          }
          entries.push_back({supplier, shape, optionalDemand});
        }
        if (entries.empty()) {
          return noBulkPlan();
        }
        if (!needsScores) {
          std::sort(entries.begin(), entries.end(),
                    [](const PlannedSupplier& a,
                       const PlannedSupplier& b) {
                      return a.supplier->cost() > b.supplier->cost();
                    });
        }

        auto* state = pool.make<FilteredDisjunctionPlan>();
        state->owner = this;
        state->filter = filter;
        state->filterDocs = filterDocs;
        state->arrayFilterFeed = arrayFilterFeed;
        state->postingsFilterFeed = postingsFilterFeed;
        if (filter.supplier != nullptr) {
          state->filter.plan = filter.supplier->resolve(
              pool, filterContext);
        }
        state->optional = pool.make_span<PlannedSupplier>(entries.size());
        for (size_t i = 0; i < entries.size(); i++) {
          entries[i].plan = entries[i].supplier->resolve(
              pool, optionalContext);
          state->optional[i] = entries[i];
        }
        BulkPlan plan = knownBulkPlan(
            BulkAnswer::YES, BulkAnswer::YES, BulkAnswer::YES);
        plan.buildState = state;
        return plan;
      }

      BulkScorer* buildFilteredDisjunctionBulk(
          MemPool& targetPool, const FilteredDisjunctionPlan& plan) {
        Query::Scorer* filterScorer = nullptr;
        std::span<const int32_t> filterDocs;
        TermQuery::Scorer* postingsScorer = nullptr;
        if (plan.arrayFilterFeed) {
          assert(plan.filterDocs != nullptr
                 && plan.filterDocs->type == DocSet::ARRAY);
          filterDocs = ((ArrDocSet*) plan.filterDocs)->docs();
        } else {
          filterScorer = plan.filter.plan->build(targetPool);
          assert(filterScorer != nullptr);
          if (plan.postingsFilterFeed) {
            postingsScorer = dynamic_cast<TermQuery::Scorer*>(filterScorer);
            assert(postingsScorer != nullptr);
          }
        }
        auto terms = targetPool.make_span<TermQuery::Scorer*>(
            plan.optional.size());
        for (size_t i = 0; i < plan.optional.size(); i++) {
          Query::Scorer* scorer = plan.optional[i].plan->build(targetPool);
          terms[i] = dynamic_cast<TermQuery::Scorer*>(scorer);
          assert(terms[i] != nullptr);
        }
        return targetPool.make<BooleanQuery::FilteredDisjunctionBulkScorer>(
            targetPool, filterScorer, filterDocs, terms,
            segment.maxDoc(), postingsScorer);
      }

      BulkPlan planExactFilteredMandOptBulk(BulkUse use) {
        if (disableExactFilteredMandOptCompositionForTests || allowsPruning
            || mandatorySources.size() != 1 || optionalSources.empty()
            || !prohibitedSources.empty() || filterSuppliers.size() != 1
            || minShouldMatch >= 1) {
          return noBulkPlan();
        }

        ConjunctionPlanningResult required = planConjunction(
            pool, ConjunctionMode::CANDIDATE,
            candidateRouteMask(), BulkUse::EXACT_CANDIDATE_SCORING);
        if (required.status != ConjunctionPlanStatus::READY
            || (required.plan.route
                    != ConjunctionRoute::CANDIDATE_DOC_SET_FILTER
                && required.plan.route
                    != ConjunctionRoute::CANDIDATE_POSTINGS_FILTER
                && required.plan.route
                    != ConjunctionRoute::CANDIDATE_SCORING_TERM)) {
          return noBulkPlan();
        }

        auto* mandatorySupplier = mandatoryShapeSuppliers[0];
        if (mandatorySupplier == nullptr) {
          return noBulkPlan();
        }
        int64_t leadCost = std::min(
            mandatorySupplier->cost(), filterSuppliers[0]->cost());
        Query::Demand demand = Query::Demand::fromLeadCost(leadCost, use);
        Query::PlanContext context = scorerBuildContext(demand);
        boost::container::small_vector<PlannedSupplier, 16> optional;
        for (auto* supplier : optionalShapeSuppliers) {
          if (supplier == nullptr) {
            continue;
          }
          Query::ScorerShape shape = describeResolved(supplier, context);
          if (shape.matchState == Query::MatchState::EMPTY) {
            continue;
          }
          if (shape.matchState != Query::MatchState::NONEMPTY
              || shape.reportedTwoPhase
                  != Query::ReportedTwoPhase::NO) {
            return noBulkPlan();
          }
          optional.push_back({supplier, shape, demand});
        }

        auto* state = pool.make<ExactFilteredMandOptPlan>();
        state->owner = this;
        state->required = pool.make<ConjunctionPlan>(required.plan);
        resolveConjunctionChildren(pool, *state->required);
        state->optional = pool.make_span<PlannedSupplier>(optional.size());
        for (size_t i = 0; i < optional.size(); i++) {
          optional[i].plan = optional[i].supplier->resolve(pool, context);
          state->optional[i] = optional[i];
        }
        BulkPlan plan = knownBulkPlan(
            BulkAnswer::YES, BulkAnswer::YES, BulkAnswer::YES);
        plan.buildState = state;
        return plan;
      }

      BulkScorer* buildExactFilteredMandOptBulk(
          MemPool& targetPool, const ExactFilteredMandOptPlan& plan) {
        MaybeConjunctionBulk required =
            buildConjunction(targetPool, *plan.required);
        assert(required && required->usesCandidateRoute());
        if (!required) return nullptr;
        recordCandidateConjunctionEngagement(*required);
        skipCount(SkipStats::exactFilteredMandOptCompositions);
        if (plan.optional.empty()) return required->bulk;
        auto optional = targetPool.make_span<Query::Scorer*>(
            plan.optional.size());
        for (size_t i = 0; i < plan.optional.size(); i++) {
          optional[i] = plan.optional[i].plan->build(targetPool);
          assert(optional[i] != nullptr);
        }
        return targetPool.make<BooleanQuery::OptionalScoreBulkScorer>(
            required->bulk, optional);
      }

      BulkPlan planFilteredScoredBulk(BulkUse use) {
        // This route makes its scored body the membership source. A filter-led
        // query with no mandatory clause instead has rank-only optionals, so it
        // must stay on the pull MandOptScorer path.
        if (!needsScores || filterSuppliers.empty()
            || minShouldMatch > 1
            || (mandatorySources.empty() && minShouldMatch < 1)) {
          return noBulkPlan();
        }
        int64_t localFilterCost = minFilterCost();
        if (localFilterCost < 0) {
          return noBulkPlan();
        }
        if (filterDensityRoutesToPull(
                localFilterCost, segment.maxDoc())) {
          return noBulkPlan();
        }

        BulkPlan exactMandOpt = planExactFilteredMandOptBulk(use);
        if (exactMandOpt.available == BulkAnswer::YES) {
          return exactMandOpt;
        }

        // Let a direct filter participate in the same cost ordering as the
        // scored required terms. The cheapest required clause owns a docs-only
        // candidate feed, while the scored clauses define the competitive
        // window bound and score only survivors. If admission declines, retain
        // the existing scored-body plus WindowFilter plan below.
        if (allowsPruning && !disableFilteredConjunctionBatchForTests
            && filterSuppliers.size() == 1
            && !mandatorySources.empty() && optionalSources.empty()
            && minShouldMatch == 0) {
          ConjunctionPlanningResult candidate = planConjunction(
              pool, ConjunctionMode::CANDIDATE,
              candidateRouteMask(), BulkUse::EXACT_CANDIDATE_SCORING);
          if (candidate.status == ConjunctionPlanStatus::READY) {
            resolveConjunctionChildren(pool, candidate.plan);
            auto* state = pool.make<ConjunctionPlan>(candidate.plan);
            state->owner = this;
            BulkPlan plan = knownBulkPlan(
                BulkAnswer::YES, BulkAnswer::YES, BulkAnswer::YES);
            plan.buildState = state;
            return plan;
          }
        }

        // Exclusions are admitted only by the candidate conjunction above.
        // The scored-body plus WindowFilter fallback has no AND-NOT stage.
        if (!prohibitedSources.empty()) {
          return noBulkPlan();
        }

        // Ask the positive body to plan its own bulk route. This keeps route
        // identity and filter-sensitive admission in the supplier that will
        // construct the scorer, independent of whether normalization exposed
        // the body directly or wrapped it as a single child.
        Query::ScorerSupplier* bodySupplier = nullptr;
        if (mandatorySources.size() == 1 && optionalSources.empty()
            && minShouldMatch < 1) {
          bodySupplier = mandatorySources[0]->scorerSupplier(
              pool, segment);
        } else if (mandatorySources.empty() && optionalSources.size() == 1) {
          bodySupplier = optionalSources[0]->scorerSupplier(
              pool, segment);
        } else {
          bodySupplier = makeSupplier(
              pool, segment, mandatorySources, mandatoryScores,
              optionalSources,
              std::span<Query::SegmentSource* const>{},
              std::span<Query::ScorerSupplier* const>{}, minShouldMatch,
              needsScores, allowsPruning);
        }
        if (bodySupplier == nullptr) {
          return noBulkPlan();
        }
        int64_t bodyCost = bodySupplier->cost();
        BulkScorerContext childContext{
            localFilterCost, filterSuppliers};
        BulkPlan bodyPlan = bodySupplier->planBulk(use, childContext);
        if (bodyPlan.available != BulkAnswer::YES) {
          return noBulkPlan();
        }
        if (bodyPlan.consumesFilters == BulkAnswer::YES) {
          auto* state = pool.make<DelegateFilteredChildPlan>();
          state->owner = this;
          state->child = bodySupplier;
          state->childPlan = bodyPlan;
          BulkPlan plan = bodyPlan;
          plan.buildState = state;
          return plan;
        }
        if (bodyPlan.consumesFilters != BulkAnswer::NO
            || bodyPlan.acceptsWindowFilter != BulkAnswer::YES) {
          return noBulkPlan();
        }

        Query::Demand filterDemand = Query::Demand::fromLeadCost(
            bodyCost, use);
        Query::PlanContext filterContext =
            scorerBuildContext(filterDemand);
        boost::container::small_vector<PlannedSupplier, 16> filters;
        for (Query::ScorerSupplier* supplier : filterSuppliers) {
          if (supplier == nullptr) return noBulkPlan();
          Query::ScorerShape shape =
              describeResolved(supplier, filterContext);
          if (shape.matchState != Query::MatchState::NONEMPTY
              || shape.windowFillClause != Query::ClauseShape::DIRECT) {
            return noBulkPlan();
          }
          filters.push_back({supplier, shape, filterDemand});
        }
        auto* state = pool.make<FilteredBodyPlan>();
        state->owner = this;
        state->bodySupplier = bodySupplier;
        state->bodyPlan = bodyPlan;
        state->bodyCost = bodyCost;
        state->filterCost = localFilterCost;
        state->filters = pool.make_span<PlannedSupplier>(filters.size());
        for (size_t i = 0; i < filters.size(); i++) {
          filters[i].plan = filters[i].supplier->resolve(
              pool, filterContext);
          state->filters[i] = filters[i];
        }
        BulkPlan plan = bodyPlan;
        plan.buildState = state;
        return plan;
      }

      BulkScorer* buildFilteredBodyBulk(
          MemPool& targetPool, const FilteredBodyPlan& plan) {
        BulkScorer* bulk = plan.bodySupplier->buildBulk(
            targetPool, plan.bodyPlan);
        assert(bulk != nullptr);
        auto scorers = targetPool.make_span<Query::Scorer*>(
            plan.filters.size());
        for (size_t i = 0; i < plan.filters.size(); i++) {
          scorers[i] = plan.filters[i].plan->build(targetPool);
          assert(scorers[i] != nullptr);
        }
        bool probe = !disableFilterMaskProbeForTests
            && plan.filterCost > 0
            && plan.filterCost >= (int64_t) segment.maxDoc()
                                     / kMaskProbeMinFilterDensityInverse
            && plan.bodyCost
                <= (plan.filterCost - 1) / kMaskProbeAdvanceWeight;
        auto* filter = targetPool.make<WindowFilter>(
            targetPool, scorers, probe, plan.filterCost);
        bool attached = bulk->attachWindowFilter(filter);
        assert(attached);
        return attached ? bulk : nullptr;
      }

      BulkPlan planFilterOnlyBulk(BulkUse use) {
        boost::container::small_vector<PlannedBulkSupplier, 16> entries;
        entries.reserve(filterSuppliers.size());
        for (auto* supplier : filterSuppliers) {
          if (supplier == nullptr) return noBulkPlan();
          BulkPlan child = supplier->planBulk(use, {});
          if (child.available != BulkAnswer::YES
              || child.hasConstantCount()) return noBulkPlan();
          entries.push_back({supplier, supplier->cost(), child});
        }
        std::sort(entries.begin(), entries.end(),
                  [](const PlannedBulkSupplier& a,
                     const PlannedBulkSupplier& b) {
                    return a.cost < b.cost;
                  });
        auto* state = pool.make<FilterOnlyPlan>();
        state->owner = this;
        state->filters = pool.make_span<PlannedBulkSupplier>(entries.size());
        std::copy(entries.begin(), entries.end(), state->filters.begin());
        BulkPlan plan = knownBulkPlan(
            BulkAnswer::YES, BulkAnswer::YES, BulkAnswer::NO);
        plan.buildState = state;
        return plan;
      }

      BulkScorer* buildFilterOnlyBulk(
          MemPool& targetPool, const FilterOnlyPlan& plan) {
        auto bulks = targetPool.make_span<BulkScorer*>(
            plan.filters.size());
        for (size_t i = 0; i < plan.filters.size(); i++) {
          bulks[i] = plan.filters[i].supplier->buildBulk(
              targetPool, plan.filters[i].plan);
          assert(bulks[i] != nullptr);
        }
        if (bulks.size() == 1) {
          // Wrap even the single-clause case: the wrapper owns the
          // non-scoring contract (constant-0 score windows, no impact
          // pruning by the lead).
          return targetPool.make<BooleanQuery::FilterOnlyBulkScorer>(
              bulks[0], std::span<uint64_t>{}, std::span<uint64_t>{},
              0, segment.maxDoc());
        }

        int32_t maxDoc = segment.maxDoc();
        size_t wordCount = FixedBitSet::sizeInWords(maxDoc);
        auto filterWords = targetPool.make_span<uint64_t>(wordCount);
        auto clauseWords = targetPool.make_span<uint64_t>(wordCount);
        bool first = true;
        for (size_t i = 1; i < bulks.size(); i++) {
          DocSetBuilder builder(maxDoc);
          int64_t count = 0;
          for (int32_t cursor = 0;
               cursor != PostingsReader::END && cursor < maxDoc; ) {
            int32_t next = bulks[i]->countNextWindow(
                count, &builder, nullptr, cursor, maxDoc);
            if (next == PostingsReader::END) {
              break;
            }
            assert(next > cursor);
            cursor = next;
          }
          assert(count == builder.card());
          auto docs = builder.build();
          std::fill(clauseWords.begin(), clauseWords.end(), 0);
          if (docs->type == DocSet::BITSET) {
            const auto& bits = ((BitDocSet*) docs.get())->bits();
            std::copy(bits.words, bits.words + wordCount, clauseWords.begin());
          } else {
            for (int32_t doc : ((ArrDocSet*) docs.get())->docs()) {
              clauseWords[(size_t) doc >> 6] |= 1ULL << (doc & 63);
            }
          }
          if (first) {
            std::copy(clauseWords.begin(), clauseWords.end(), filterWords.begin());
            first = false;
          } else {
            for (size_t word = 0; word < wordCount; word++) {
              filterWords[word] &= clauseWords[word];
            }
          }
        }

        int32_t filterCard = 0;
        for (uint64_t word : filterWords) {
          filterCard += (int32_t) std::popcount(word);
        }
        return targetPool.make<BooleanQuery::FilterOnlyBulkScorer>(
            bulks[0], filterWords, clauseWords, filterCard, maxDoc);
      }

      BulkPlan planBulk(
          BulkUse use, const BulkScorerContext& bulkContext) override {
        if (bulkContext.requireConstantCount
            && mandatorySources.empty() && prohibitedSources.empty()
            && filterSuppliers.empty() && minShouldMatch <= 1
            && optionalShapeSuppliers.size() == 1
            && optionalShapeSuppliers[0] != nullptr) {
          return optionalShapeSuppliers[0]->planBulk(use, bulkContext);
        }
        bool hasEnclosingFilters = bulkContext.hasFilter()
            || !bulkContext.filterSuppliers.empty();
        if (hasEnclosingFilters && !filterSuppliers.empty()) {
          return noBulkPlan();
        }

        if (hasEnclosingFilters && !disableIntegratedFilteredCountForTests
            && !needsScores && optionalSources.empty()
            && prohibitedSources.empty() && minShouldMatch < 1) {
          if (mandatorySources.size() == 1) {
            Query::ScorerSupplier* child = mandatoryShapeSuppliers[0];
            if (child != nullptr) {
              BulkPlan childPlan = child->planBulk(use, bulkContext);
              if (childPlan.available == BulkAnswer::YES
                  && childPlan.consumesFilters == BulkAnswer::YES) {
                auto* state = pool.make<DelegateFilteredChildPlan>();
                state->owner = this;
                state->child = child;
                state->childPlan = childPlan;
                BulkPlan plan = childPlan;
                plan.buildState = state;
                return plan;
              }
            }
          } else if (mandatorySources.size() >= 2
                     && !bulkContext.filterSuppliers.empty()) {
            BulkPlan plan = planConjunctionBulk(
                use, ConjunctionMode::EXHAUSTIVE, allRouteMask(),
                bulkContext.filterSuppliers);
            if (plan.available == BulkAnswer::YES) return plan;
          }
        }
        if (bulkContext.requireFilterConsumption) {
          return noBulkPlan();
        }

        if (mandatorySources.empty() && optionalSources.empty()
            && prohibitedSources.empty() && !filterSuppliers.empty()) {
          return planFilterOnlyBulk(use);
        }

        if (needsScores && !prohibitedSources.empty()) {
          if (!filterSuppliers.empty()
              && !disableFilteredScoredBulkForTests) {
            BulkPlan filtered = planFilteredScoredBulk(use);
            if (filtered.available == BulkAnswer::YES) return filtered;
          }
          bool maxScoreShape = mandatorySources.empty()
              && filterSuppliers.empty() && minShouldMatch <= 1
              && optionalSources.size() >= 2;
          if (!maxScoreShape || disableBulkExclusionForTests) {
            return noBulkPlan();
          }
          return planMaxScoreBulk(use, true);
        }

        if (!filterSuppliers.empty() && mandatorySources.empty()
            && prohibitedSources.empty() && optionalSources.size() >= 2
            && minShouldMatch == 1 && !allowsPruning) {
          BulkPlan disjunction = planFilteredDisjunctionBulk(use);
          if (disjunction.available == BulkAnswer::YES) {
            return disjunction;
          }
          if (twoPhaseDisjunctionPull) return noBulkPlan();
        }

        if (needsScores && !allowsPruning
            && !disableFilteredConjunctionBatchForTests
            && !filterSuppliers.empty()
            && mandatorySources.size() >= 1
            && (!disableFilteredConjMultiTermForTests
                || mandatorySources.size() == 1)
            && optionalSources.empty() && prohibitedSources.empty()) {
          BulkPlan candidate = planConjunctionBulk(
              BulkUse::EXACT_CANDIDATE_SCORING,
              ConjunctionMode::CANDIDATE, candidateRouteMask());
          if (candidate.available == BulkAnswer::YES) return candidate;
        }

        bool hasFilteredCountBody = !mandatorySources.empty()
            || (minShouldMatch >= 1 && !optionalSources.empty());
        if (!needsScores && hasFilteredCountBody
            && optionalSources.empty() == (minShouldMatch < 1)
            && (!prohibitedSources.empty() || !filterSuppliers.empty())
            && minShouldMatch <= 1) {
          if (!prohibitedSources.empty()
              && ConjunctionBulkScorer::disableNegatedCountForTests) {
            return noBulkPlan();
          }
          if (!disableIntegratedFilteredCountForTests
              && mandatorySources.size() == 1
              && optionalSources.empty() && prohibitedSources.empty()
              && !filterSuppliers.empty() && minShouldMatch < 1) {
            Query::ScorerSupplier* child = mandatoryShapeSuppliers[0];
            int64_t filterCost = minFilterCost();
            if (child != nullptr && filterCost >= 0) {
              BulkScorerContext context{
                  filterCost, filterSuppliers, true};
              BulkPlan childPlan = child->planBulk(use, context);
              if (childPlan.available == BulkAnswer::YES
                  && childPlan.consumesFilters == BulkAnswer::YES) {
                auto* state = pool.make<DelegateFilteredChildPlan>();
                state->owner = this;
                state->child = child;
                state->childPlan = childPlan;
                BulkPlan plan = childPlan;
                plan.buildState = state;
                return plan;
              }
            }
          }
          BulkPlan conjunction = planConjunctionBulk(
              use, ConjunctionMode::EXHAUSTIVE,
              countRouteMask(!disableFilterClauseCountForTests));
          if (conjunction.available == BulkAnswer::YES) {
            return conjunction;
          }
          bool pureFilteredDisjunction = mandatorySources.empty()
              && prohibitedSources.empty() && optionalSources.size() >= 2
              && minShouldMatch == 1;
          return pureFilteredDisjunction
              ? planFilteredDisjunctionBulk(use) : noBulkPlan();
        }

        if (needsScores && !filterSuppliers.empty()) {
          return disableFilteredScoredBulkForTests
              ? noBulkPlan() : planFilteredScoredBulk(use);
        }
        if (optionalSources.empty() && prohibitedSources.empty()
            && filterSuppliers.empty() && mandatorySources.size() >= 2) {
          return planConjunctionBulk(
              use, ConjunctionMode::SCORED_BODY,
              routeBit(ConjunctionRoute::GENERIC));
        }
        if (!disableMandOptBulkForTests && mandatorySources.size() == 1
            && !optionalSources.empty() && prohibitedSources.empty()
            && filterSuppliers.empty() && minShouldMatch < 1) {
          return planMandOptBulk(use, bulkContext);
        }
        if (!mandatorySources.empty() || !prohibitedSources.empty()
            || !filterSuppliers.empty() || minShouldMatch > 1
            || optionalSources.size() < 2) {
          return noBulkPlan();
        }
        return planMaxScoreBulk(use, false);
      }

      void recordBulkPlanCommitment(
          BulkUse use, const BulkScorerContext& bulkContext,
          const BulkPlan& plan) override {
        if (plan.available == BulkAnswer::NO && needsScores
            && !prohibitedSources.empty()) {
          bool maxScoreShape = mandatorySources.empty()
              && filterSuppliers.empty() && minShouldMatch <= 1
              && optionalSources.size() >= 2;
          if (!maxScoreShape) {
            skipCount(SkipStats::bulkExclusionShapeFallbacks);
          } else if (disableBulkExclusionForTests) {
            skipCount(SkipStats::bulkExclusionDisabledFallbacks);
          } else {
            size_t positiveCount = 0;
            Query::PlanContext context = scorerBuildContext(
                Query::Demand::fromLeadCost(
                    std::numeric_limits<int64_t>::max(), use));
            for (Query::ScorerSupplier* supplier
                 : optionalShapeSuppliers) {
              if (supplier != nullptr
                  && describeResolved(supplier, context).matchState
                      == Query::MatchState::NONEMPTY) {
                positiveCount++;
              }
            }
            if (positiveCount < 2) {
              skipCount(
                  SkipStats::bulkExclusionPositiveSegmentFallbacks);
            } else {
              skipCount(SkipStats::bulkExclusionUnsupportedFallbacks);
            }
          }
        }
        if (plan.available == BulkAnswer::NO
            && bulkContext.hasFilter()
            && !disableMandOptBulkForTests
            && mandatorySources.size() == 1
            && !optionalSources.empty() && prohibitedSources.empty()
            && filterSuppliers.empty() && minShouldMatch < 1
            && !disableFilteredMandOptFillGateForTests
            && filterDensityBelow(
                bulkContext.filterCost, segment.maxDoc(),
                kMandOptScalarFillDensityInverse)
            && mandatoryShapeSuppliers[0] != nullptr
            && mandatoryShapeSuppliers[0]->scoreBlockFillKind()
                == Query::ScorerSupplier::ScoreBlockFillKind::
                    DEFAULT_SCALAR) {
          skipCount(SkipStats::mandOptBulkScalarFillFallbacks);
        }
        int64_t localFilterCost = minFilterCost();
        if (plan.available == BulkAnswer::NO && needsScores
            && !filterSuppliers.empty() && localFilterCost >= 0
            && !filterDensityRoutesToPull(
                localFilterCost, segment.maxDoc())) {
          Query::ScorerSupplier* body = nullptr;
          if (mandatorySources.size() == 1 && optionalSources.empty()
              && minShouldMatch < 1) {
            body = mandatoryShapeSuppliers[0];
          } else if (mandatorySources.empty()
                     && optionalSources.size() == 1) {
            body = optionalShapeSuppliers[0];
          } else {
            body = makeSupplier(
                pool, segment, mandatorySources, mandatoryScores,
                optionalSources,
                std::span<Query::SegmentSource* const>{},
                std::span<Query::ScorerSupplier* const>{},
                minShouldMatch, needsScores, allowsPruning);
          }
          if (body != nullptr) {
            BulkScorerContext childContext{
                localFilterCost, filterSuppliers};
            BulkPlan childPlan = body->planBulk(use, childContext);
            if (childPlan.available == BulkAnswer::NO) {
              body->recordBulkPlanCommitment(
                  use, childContext, childPlan);
            }
          }
        }
        if (use == BulkUse::COUNT_WINDOWS
            && plan.available == BulkAnswer::NO) {
          recordDisjunctionCountIdentityShapeFallbacks();
        }
        if (bulkContext.hasFilter()
            || !bulkContext.filterSuppliers.empty()
            || bulkContext.requireFilterConsumption) {
          return;
        }
        if (use == BulkUse::EXACT_CANDIDATE_SCORING) {
          if (filteredDisjunctionDensityDecline()) {
            skipCount(SkipStats::filteredDisjBatchDensityFallbacks);
          }
          return;
        }

        bool hasFilteredCountBody = !mandatorySources.empty()
            || (minShouldMatch >= 1 && !optionalSources.empty());
        if (needsScores || !hasFilteredCountBody
            || optionalSources.empty() != (minShouldMatch < 1)
            || (prohibitedSources.empty() && filterSuppliers.empty())
            || minShouldMatch > 1) {
          return;
        }
        bool pureFilteredDisjunction = mandatorySources.empty()
            && prohibitedSources.empty() && optionalSources.size() >= 2
            && minShouldMatch == 1;
        if (!pureFilteredDisjunction) {
          return;
        }
        if (filteredDisjunctionDensityDecline()) {
          skipCount(SkipStats::filteredDisjBatchDensityFallbacks);
        }
        BulkPlan disjunction = planFilteredDisjunctionBulk(use);
        if (disjunction.available == BulkAnswer::NO) {
          skipCount(SkipStats::disjCountIdentityFilterFallbacks);
        }
      }

      void recordDisjunctionCountIdentityShapeFallbacks() const {
        if (needsScores || optionalSources.size() < 2) return;
        if (!mandatorySources.empty()) {
          skipCount(SkipStats::disjCountIdentityRequiredFallbacks);
        }
        if (!prohibitedSources.empty()) {
          skipCount(SkipStats::disjCountIdentityProhibitedFallbacks);
        }
        if (minShouldMatch > 1) {
          skipCount(SkipStats::disjCountIdentityMinMatchFallbacks);
        }
      }

      BulkScorer* buildBulk(
          MemPool& targetPool, const BulkPlan& plan) override {
        assert(plan.buildState != nullptr
               && plan.buildState->owner == this);
        auto* state = static_cast<const BooleanBulkBuildState*>(
            plan.buildState);
        switch (state->kind) {
          case BooleanBulkBuildState::Kind::CONJUNCTION: {
            const auto& conjunction =
                *static_cast<const ConjunctionPlan*>(state);
            if (conjunction.use == BulkUse::COUNT_WINDOWS) {
              recordDisjunctionCountIdentityShapeFallbacks();
            }
            recordConjunctionPlanCommitment(conjunction);
            MaybeConjunctionBulk result =
                buildConjunction(targetPool, conjunction);
            if (!result) return nullptr;
            if (result->usesCandidateRoute()) {
              recordCandidateConjunctionEngagement(*result);
            } else if (conjunction.mode == ConjunctionMode::EXHAUSTIVE
                       && result->route
                           == ConjunctionRoute::COUNT_POSTINGS_SAMPLE) {
              skipCount(SkipStats::filteredConjBatchEngagements);
              skipCount(
                  SkipStats::filteredConjBatchPostingsFeedEngagements);
            }
            return result->bulk;
          }
          case BooleanBulkBuildState::Kind::MAND_OPT:
            return buildMandOptBulk(
                targetPool, *static_cast<const MandOptPlan*>(state));
          case BooleanBulkBuildState::Kind::MAX_SCORE:
            return buildMaxScoreBulk(
                targetPool, *static_cast<const MaxScorePlan*>(state));
          case BooleanBulkBuildState::Kind::FILTERED_DISJUNCTION:
            return buildFilteredDisjunctionBulk(
                targetPool,
                *static_cast<const FilteredDisjunctionPlan*>(state));
          case BooleanBulkBuildState::Kind::EXACT_FILTERED_MAND_OPT:
            return buildExactFilteredMandOptBulk(
                targetPool,
                *static_cast<const ExactFilteredMandOptPlan*>(state));
          case BooleanBulkBuildState::Kind::FILTERED_BODY:
            return buildFilteredBodyBulk(
                targetPool,
                *static_cast<const FilteredBodyPlan*>(state));
          case BooleanBulkBuildState::Kind::FILTER_ONLY:
            return buildFilterOnlyBulk(
                targetPool, *static_cast<const FilterOnlyPlan*>(state));
          case BooleanBulkBuildState::Kind::DELEGATE_FILTERED_CHILD: {
            const auto& delegated =
                *static_cast<const DelegateFilteredChildPlan*>(state);
            return delegated.child->buildBulk(
                targetPool, delegated.childPlan);
          }
        }
        std::unreachable();
      }
    };

    static Query::ScorerSupplier* makeSupplier(
        MemPool& targetPool,
        IndexReader::Segment& segment,
        std::span<Query::SegmentSource* const> mandatorySources,
        std::span<const uint8_t> mandatoryScores,
        std::span<Query::SegmentSource* const> optionalSources,
        std::span<Query::SegmentSource* const> prohibitedSources,
        std::span<Query::ScorerSupplier* const> filterSuppliers,
        int minShouldMatch,
        bool needsScores,
        bool allowsPruning) {
      // Weight-time optional removal can expose a Boolean child that
      // normalization could not unwrap while the optionals were still present.
      // Preserve the child's complete supplier contract, including bulkScorer.
      if (minShouldMatch < 1 && mandatorySources.size() == 1
          && optionalSources.empty()
          && prohibitedSources.empty() && filterSuppliers.empty()) {
        auto* child = mandatorySources[0]->scorerSupplier(targetPool, segment);
        if (child != nullptr) return child;
      }
      auto mandatoryShapeSuppliers = QueryPrep::collectSuppliers(
          targetPool, segment, mandatorySources);
      auto optionalShapeSuppliers = QueryPrep::collectSuppliers(
          targetPool, segment, optionalSources);
      auto prohibitedShapeSuppliers = QueryPrep::collectSuppliers(
          targetPool, segment, prohibitedSources);
      int64_t shapeRequiredCost = segment.maxDoc();
      for (auto* supplier : mandatoryShapeSuppliers) {
        if (supplier != nullptr) {
          shapeRequiredCost = std::min(shapeRequiredCost, supplier->cost());
        }
      }
      for (auto* supplier : filterSuppliers) {
        if (supplier != nullptr) {
          shapeRequiredCost = std::min(shapeRequiredCost, supplier->cost());
        }
      }
      return targetPool.make<Supplier>(
        targetPool, segment, mandatorySources, mandatoryScores, optionalSources,
        prohibitedSources, filterSuppliers, mandatoryShapeSuppliers,
        optionalShapeSuppliers, prohibitedShapeSuppliers, shapeRequiredCost,
        minShouldMatch, needsScores, allowsPruning);
    }

    class BooleanPreparedWeight final : public Query::Weight::PreparedWeight {
      std::vector<QueryPrep::PreparedSource> mandatorySources;
      std::vector<uint8_t> mandatoryScores;
      std::vector<QueryPrep::PreparedSource> optionalSources;
      std::vector<QueryPrep::PreparedSource> prohibitedSources;
      std::vector<DomainHandle> filterDomains;
      bool hasFilters = false;
      int minShouldMatch = 0;
      bool needsScores = false;
      bool allowsPruning = false;

    public:
      BooleanPreparedWeight(std::vector<QueryPrep::PreparedSource>&& mandatorySources,
                            std::span<const uint8_t> mandatoryScores,
                            std::vector<QueryPrep::PreparedSource>&& optionalSources,
                            std::vector<QueryPrep::PreparedSource>&& prohibitedSources,
                            std::vector<DomainHandle>&& filterDomains,
                            bool hasFilters, int minShouldMatch, bool needsScores,
                            bool allowsPruning)
        : mandatorySources(std::move(mandatorySources)),
          mandatoryScores(mandatoryScores.begin(), mandatoryScores.end()),
          optionalSources(std::move(optionalSources)),
          prohibitedSources(std::move(prohibitedSources)),
          filterDomains(std::move(filterDomains)),
          hasFilters(hasFilters), minShouldMatch(minShouldMatch),
          needsScores(needsScores), allowsPruning(allowsPruning) {}

      Query::ScorerSupplier* scorerSupplier(MemPool& targetPool, IndexReader::Segment& segment) override {
        std::span<Query::ScorerSupplier*> filterSuppliers;
        if (hasFilters) {
          auto* filterDomain = filterDomains[(size_t)segment.ord].get();
          filterSuppliers = {targetPool.make_arr<Query::ScorerSupplier*>(1), 1};
          filterSuppliers[0] = targetPool.make<QueryPrep::DocSetSupplier>(filterDomain, segment);
        }
        return makeSupplier(
          targetPool, segment,
          QueryPrep::segmentSources(targetPool, QueryPrep::preparedSpan(mandatorySources)),
          mandatoryScores,
          QueryPrep::segmentSources(targetPool, QueryPrep::preparedSpan(optionalSources)),
          QueryPrep::segmentSources(targetPool, QueryPrep::preparedSpan(prohibitedSources)),
          filterSuppliers, minShouldMatch, needsScores, allowsPruning);
      }

      Query::Scorer* createScorer(MemPool& targetPool, IndexReader::Segment& segment) override {
        Query::ScorerSupplier* supplier = scorerSupplier(targetPool, segment);
        Query::Demand demand = Query::Demand::fromLeadCost(
            std::numeric_limits<int64_t>::max());
        return supplier->resolve(
            targetPool, supplier->makePlanContext(demand))->build(targetPool);
      }

      bool outputIsSubsetOfDomain() const noexcept override {
        // Filter domains are materialized inside the outer prepare domains and
        // installed as a required supplier, so every emitted doc is a member
        // of the domain this weight was prepared against.
        return hasFilters;
      }

      PreparedDomainDependence domainDependence() const noexcept override {
        if (hasFilters) return PreparedDomainDependence::PREPARE_DOMAIN;
        auto strongest = PreparedDomainDependence::QUERY_CANONICAL;
        auto include = [&](const std::vector<QueryPrep::PreparedSource>& sources) {
          for (const auto& source : sources) {
            if ((uint8_t) source.domainDependence > (uint8_t) strongest) {
              strongest = source.domainDependence;
            }
          }
        };
        include(mandatorySources);
        include(optionalSources);
        include(prohibitedSources);
        return strongest;
      }
    };


  public:
    static inline bool disableUnscoredOptionalDropForTests = false;

    Weight(Context& context, NormalizedBoolean& query, int32_t flags, float multiplier)
      : Query::Weight(context, flags) {
      needsScores = (flags & Query::NEED_SCORES) != 0;
      allowsPruning = (flags & Query::ALLOW_PRUNING) != 0;
      // Only mandatory and optional clauses can contribute to score.
      int32_t noScore = flags & ~NEED_SCORES;
      // Duplicate clauses normalize here (see mergeDuplicateScoringTerms).
      auto mandatoryClauses = mergeDuplicateScoringTerms(context.pool, query.mandatory);
      int32_t removedOptional = 0;
      auto optionalClauses =
        mergeDuplicateScoringTerms(context.pool, query.optional, &removedOptional);
      auto prohibitedClauses = dropDuplicateFilterTerms(context.pool, query.prohibited);
      auto filterClauses = dropDuplicateFilterTerms(context.pool, query.filter);
      if (!filterClauses.empty()) {
        filterUses = context.pool.make_span<FilterCache::Use*>(filterClauses.size());
        for (size_t i = 0; i < filterClauses.size(); i++) {
          filterUses[i] = context.getFilterUse(*filterClauses[i]);
        }
      }
      createMandatoryWeights(context, mandatoryClauses, flags, multiplier);
      // With a mandatory clause and minShouldMatch unset, optional clauses are
      // a pure score add (MandOpt) - they never affect membership.  Without
      // scores they contribute nothing, so skip building their weights entirely
      // (Lucene's BooleanWeight scorer simplification).  This also exposes
      // "+a b" count-only requests to the single-clause count() shortcut.
      // minShouldMatch >= 1 makes the optional group a membership constraint
      // even under a mandatory clause, so it must be kept.
      bool dropOptional = !disableUnscoredOptionalDropForTests && !needsScores
        && (!mandatoryClauses.empty() || !filterClauses.empty())
        && query.minShouldMatch < 1;
      optionalWeights = dropOptional
        ? std::span<Query::Weight*>{}
        : createWeights(context.pool, context, optionalClauses, flags, multiplier);
      prohibitedWeights = createWeights(
          context.pool, context, prohibitedClauses,
          noScore | EXCLUSION_WINDOW_FILL, 1.0f);
      int32_t exhaustiveFilterFlags = flags & ~(NEED_SCORES | ALLOW_PRUNING);
      filterWeights = createWeights(context.pool, context, filterClauses,
                                    exhaustiveFilterFlags, 1.0f);
      // Solux-defined min_match semantics under duplicate removal, split by
      // the intent the value expresses (Lucene instead refuses to dedup when
      // min_match > 1 and lets duplicates satisfy multiple match slots):
      // - min_match above half the clauses ("10 words, mm=9") is a MISS
      //   BUDGET: the user allows N - mm absences. Each removed duplicate
      //   decrements min_match (floored at 1), keeping the budget constant: a
      //   doc containing the duplicated term satisfies that slot, and a doc
      //   missing it is charged for the absence only once.
      // - min_match at or below half ("10 words, mm=2") is an ABSOLUTE
      //   COUNT: match at least mm distinct words. It stays as-is, capped
      //   at the deduped clause count so an all-duplicates query remains
      //   satisfiable.
      // The common producer is min_match computed from raw token counts of
      // pasted text, where repeats would otherwise skew either reading.
      minShouldMatch = query.minShouldMatch;
      if (minShouldMatch > 1 && removedOptional > 0) {
        if ((int64_t) minShouldMatch * 2 > (int64_t) query.optional.size()) {
          minShouldMatch = std::max(1, minShouldMatch - removedOptional);
        } else {
          minShouldMatch = std::min(minShouldMatch, (int32_t) optionalClauses.size());
        }
      }

      if (QueryPrep::anyNeedsPrepare(mandatoryWeights) ||
          QueryPrep::anyNeedsPrepare(optionalWeights) ||
          QueryPrep::anyNeedsPrepare(prohibitedWeights) ||
          QueryPrep::anyNeedsPrepare(filterWeights)) {
        traits |= NEEDS_PREPARE;
      }
      // Boolean scoring is constant only when every match gets the same sum.
      // Optional clauses make the sum data-dependent; mandatory clauses are
      // safe only if each mandatory child is constant.
      bool constant = optionalWeights.empty()
        || (mandatoryWeights.empty() && filterWeights.empty()
            && optionalWeights.size() == 1
            && optionalWeights[0]->isConstantScoring());
      for (auto* w : mandatoryWeights) {
        if (!w->isConstantScoring()) constant = false;
      }
      if (constant) traits |= IS_CONSTANT_SCORING;
      if (query.mandatory.empty() && query.optional.empty()
          && query.prohibited.empty() && query.filter.size() == 1
          && dynamic_cast<TermQuery*>(query.filter[0]) != nullptr) {
        traits |= PREFER_PULL_FOR_SPARSE_ARRAY_DOMAIN;
      }
      bool directTermUnion = mandatoryClauses.empty()
          && prohibitedClauses.empty() && optionalClauses.size() >= 2
          && filterClauses.size() <= 1 && minShouldMatch == 1
          && std::all_of(
              optionalClauses.begin(), optionalClauses.end(),
              [](Query* clause) {
                return dynamic_cast<TermQuery*>(clause) != nullptr;
              });
      if (directTermUnion) {
        traits |= CAN_COMPOSE_EXACT_COUNT_TOPK;
      }
      bool sparseFilteredTopKConjunction = !filterWeights.empty()
          && !mandatoryClauses.empty() && optionalClauses.empty()
          && prohibitedClauses.empty() && minShouldMatch == 0
          && std::all_of(
              mandatoryClauses.begin(), mandatoryClauses.end(),
              sparseFilteredTopKMandatory);
      sparseFilteredTopKEligible = sparseFilteredTopKConjunction
          || (directTermUnion && !filterWeights.empty());
      if (directTermUnion && !filterWeights.empty()) {
        sparseFilteredTopKFamilyKind = SparseFilteredTopKFamily::UNION;
      }
    }

    std::unique_ptr<Query::Weight::PreparedWeight> prepare(Query::Weight::PrepareContext& ctx) override {
      auto filterSources = QueryPrep::prepareFilterSources(
          filterWeights, filterUses, ctx);
      std::vector<DomainHandle> filterDomains(
          ctx.reader.segments().size());
      std::vector<DocSet*> childDomainPtrs(ctx.reader.segments().size());

      if (!filterSources.empty()) {
        for (size_t segnum = 0; segnum < ctx.reader.segments().size(); segnum++) {
          auto* outerDomain = ctx.domainPerSeg.empty() ? nullptr : ctx.domainPerSeg[segnum];
          filterDomains[segnum] = QueryPrep::materializeEffectiveIntersection(
            QueryPrep::preparedSpan(filterSources), ctx.reader,
            ctx.reader.segments()[segnum], outerDomain);
          filterDomains[segnum] = std::move(filterDomains[segnum])
              .pinnedWith(context.filterUses);
          childDomainPtrs[segnum] = filterDomains[segnum].get();
        }
      } else {
        for (size_t segnum = 0; segnum < ctx.reader.segments().size(); segnum++) {
          childDomainPtrs[segnum] = ctx.domainPerSeg.empty() ? nullptr : ctx.domainPerSeg[segnum];
        }
      }

      Query::Weight::PrepareContext childCtx{
        ctx.reader,
        std::span<DocSet* const>(childDomainPtrs.data(), childDomainPtrs.size()),
        ctx.parallel};
      auto mandatorySources = QueryPrep::prepareSources(mandatoryWeights, childCtx);
      auto optionalSources = QueryPrep::prepareSources(optionalWeights, childCtx);
      auto prohibitedSources = QueryPrep::prepareSources(prohibitedWeights, ctx);

      return std::make_unique<BooleanPreparedWeight>(
        std::move(mandatorySources), mandatoryScores,
        std::move(optionalSources),
        std::move(prohibitedSources), std::move(filterDomains),
        !filterSources.empty(), minShouldMatch, needsScores, allowsPruning);
    }


    Query::ScorerSupplier* scorerSupplier(solux::MemPool& targetPool, solux::IndexReader::Segment& segment) override {
      auto mandatorySources = QueryPrep::liveSources(targetPool, mandatoryWeights);
      auto optionalSources = QueryPrep::liveSources(targetPool, optionalWeights);
      auto prohibitedSources = QueryPrep::liveSources(targetPool, prohibitedWeights);
      std::span<Query::ScorerSupplier*> filterSuppliers;
      if (!filterWeights.empty()) {
        filterSuppliers = targetPool.make_span<Query::ScorerSupplier*>(
            filterWeights.size());
        for (size_t i = 0; i < filterWeights.size(); i++) {
          bool exactFilteredDisjunction = !allowsPruning
              && !disableFilteredDisjunctionBatchForTests
              && mandatoryWeights.empty() && optionalWeights.size() >= 2
              && prohibitedWeights.empty() && minShouldMatch == 1;
          bool exhaustiveFilterClause = !allowsPruning
              && (needsScores
                    ? !disableExactFilterCachePolicyForTests
                    : !disableFilterClauseCountForTests);
          QueryPrep::FilterSupplierMode mode =
              exactFilteredDisjunction
                  ? QueryPrep::FilterSupplierMode::SPARSE_BATCH
                  : exhaustiveFilterClause
                        ? QueryPrep::FilterSupplierMode::EXHAUSTIVE_CLAUSE
                        : QueryPrep::FilterSupplierMode::DENSITY_ROUTED;
          filterSuppliers[i] = QueryPrep::filterSupplier(
              targetPool, *filterWeights[i], filterUses[i],
              context.topReader, segment, mode,
              exactFilteredDisjunction
                  ? needsScores
                        ? filteredDisjunctionBatchDensityInverseForTests
                        : QueryPrep::kSparseBatchCountOwnDensityInverse
                  : 0);
        }
      }
      return makeSupplier(targetPool, segment, mandatorySources, mandatoryScores,
                          optionalSources,
                          prohibitedSources, filterSuppliers, minShouldMatch,
                          needsScores, allowsPruning);
    }

    int64_t sparseFilteredTopKCost(
        MemPool& targetPool, IndexReader::Segment& segment) override {
      if (!sparseFilteredTopKEligible) {
        return -1;
      }
      int64_t cost = segment.maxDoc();
      for (auto* weight : filterWeights) {
        auto* supplier = weight->scorerSupplier(targetPool, segment);
        if (supplier == nullptr) {
          return 0;
        }
        cost = std::min(cost, supplier->cost());
      }
      return cost;
    }

    SparseFilteredTopKFamily sparseFilteredTopKFamily()
        const noexcept override {
      return sparseFilteredTopKFamilyKind;
    }

    Scorer* createScorer(solux::MemPool& targetPool, solux::IndexReader::Segment& segment) override {
      Query::ScorerSupplier* supplier = scorerSupplier(targetPool, segment);
      Query::Demand demand = Query::Demand::fromLeadCost(
          std::numeric_limits<int64_t>::max());
      return static_cast<Scorer*>(supplier->resolve(
          targetPool, supplier->makePlanContext(demand))->build(targetPool));
    }

    // Single-clause boolean shapes delegate to the wrapped clause. Filtered
    // count-only conjunctions exhaust the same per-window bulk intersection
    // used by collection, with filter clauses kept as lazy zero-score members.
  };  // BooleanQuery::Weight

  class Scorer final : public Query::Scorer {
  public:
    Scorer() {}

    int32_t next() override {
      return -1;
    }

    /// doc we are positioned on
    int32_t docId() override {
      return -1;
    }

    float score() override {
      return -1;
    }
  };

  class MandOptScorer final : public Query::Scorer {
    struct ApproxSlot {
      Query::Scorer* scorer = nullptr;
      bool twoPhase = false;

      int32_t next() const {
        return twoPhase ? scorer->approximationNext() : scorer->next();
      }

      int32_t advance(int32_t target) const {
        return twoPhase ? scorer->approximationAdvance(target) : scorer->advance(target);
      }

      int32_t docId() const {
        return twoPhase ? scorer->approximationDocId() : scorer->docId();
      }
    };

    ApproxSlot mand;
    ApproxSlot opt;
    int32_t id = -1;
    float minCompetitiveScore = 0.0f;
    float reqMaxScore = std::numeric_limits<float>::infinity();
    int32_t windowUpTo = -1;
    float windowReqMaxScore = std::numeric_limits<float>::infinity();
    float windowMaxScore = std::numeric_limits<float>::infinity();
    bool optIsRequired = false;
    int32_t optCheckedDoc = -1;
    bool optCheckedMatch = false;
#ifndef NDEBUG
    enum class IterationProtocol : uint8_t { NONE, INTERNAL, EXTERNAL };
    IterationProtocol iterationProtocol = IterationProtocol::NONE;
#endif
    // The window walk earns its keep through skips and conjunction windows.
    // When neither happens for a stretch (small corpora, low thresholds), it
    // disables itself and only re-arms once theta has grown past the theta it
    // failed at by the near-bound margin. The global reqMaxScore collapse in
    // setMinCompetitiveScore stays active regardless. Patience must survive a
    // fragment storm: windows split at every optional-side doc, so one coarse
    // competitive group can produce dozens of useless evals before the walk
    // reaches the next group where bounds actually change.
    static constexpr int32_t kWindowWalkPatience = 64;
    int32_t uselessWindowEvals = 0;
    bool windowWalkDisabled = false;
    float windowWalkDisabledAtTheta = 0.0f;

    bool anyTwoPhase() const {
      return mand.twoPhase || opt.twoPhase;
    }

#ifndef NDEBUG
    void markInternalProtocol() {
      assert(iterationProtocol != IterationProtocol::EXTERNAL);
      iterationProtocol = IterationProtocol::INTERNAL;
    }

    void markExternalProtocol() {
      assert(iterationProtocol != IterationProtocol::INTERNAL);
      iterationProtocol = IterationProtocol::EXTERNAL;
    }
#endif

    bool optCanExist(int32_t upTo) const {
      int32_t optDoc = opt.docId();
      return optDoc != solux::PostingsReader::END && optDoc <= upTo;
    }

    bool optMatches() {
      if (!opt.twoPhase) {
        return true;
      }
      int32_t doc = opt.docId();
      if (optCheckedDoc != doc) {
        optCheckedDoc = doc;
        skipCount(SkipStats::mandOptOptionalVerifies);
        optCheckedMatch = opt.scorer->matches();
      }
      return optCheckedMatch;
    }

    float maxScoreAt(int32_t upTo, bool refined) {
      float maxScore = refined ? mand.scorer->refineMaxScore(upTo)
                               : mand.scorer->getMaxScore(upTo);
      if (optCanExist(upTo)) {
        maxScore += optionalUpperBound(
            refined ? opt.scorer->refineMaxScore(upTo)
                    : opt.scorer->getMaxScore(upTo));
      }
      return maxScore;
    }

    ScoreBounds scoreBoundsAt(int32_t upTo, bool refined) {
      ScoreBounds bounds = refined ? mand.scorer->refineScoreBounds(upTo)
                                   : mand.scorer->getScoreBounds(upTo);
      if (optCanExist(upTo)) {
        ScoreBounds optBounds = refined ? opt.scorer->refineScoreBounds(upTo)
                                        : opt.scorer->getScoreBounds(upTo);
        bounds = addScoreBounds(bounds, optionalScoreBounds(optBounds));
      }
      return bounds;
    }

    void refineWindowNearTheta() {
      if (!(minCompetitiveScore > 0.0f)) {
        return;
      }

      if (windowReqMaxScore >= minCompetitiveScore && std::isfinite(windowReqMaxScore)
          && (double) minCompetitiveScore >= kRefineBeta * (double) windowReqMaxScore) {
        float refinedReqMax = mand.scorer->refineMaxScore(windowUpTo);
        if (refinedReqMax < windowReqMaxScore) {
          windowReqMaxScore = refinedReqMax;
        }
      }

      if (windowMaxScore >= minCompetitiveScore && std::isfinite(windowMaxScore)
          && (double) minCompetitiveScore >= kRefineBeta * (double) windowMaxScore) {
        float refinedMax = maxScoreAt(windowUpTo, true);
        if (refinedMax < windowMaxScore) {
          windowMaxScore = refinedMax;
        }
      }
    }

    void moveToNextBlock(int32_t target) {
      skipCount(SkipStats::mandOptWindowEvals);
      windowUpTo = advanceShallow(target);
      windowReqMaxScore = mand.scorer->getMaxScore(windowUpTo);
      windowMaxScore = maxScoreAt(windowUpTo, false);
      refineWindowNearTheta();
      optIsRequired = windowReqMaxScore < minCompetitiveScore;
      if (optIsRequired) {
        skipCount(SkipStats::mandOptConjunctionWindows);
      }
    }

    int32_t advanceImpacts(int32_t target) {
      if (windowWalkDisabled) {
        if (windowWalkDisabledAtTheta >= kRefineBeta * (double) minCompetitiveScore) {
          return target;
        }
        windowWalkDisabled = false;
        uselessWindowEvals = 0;
      }
      bool evaluated = false;
      if (target > windowUpTo) {
        moveToNextBlock(target);
        evaluated = true;
      }

      for (;;) {
        if (windowMaxScore >= minCompetitiveScore) {
          // Patience is charged per window EVALUATION, never per advance:
          // in-window advances answer from the cached bound and carry no
          // signal about whether the walk is paying off.
          if (evaluated) {
            if (optIsRequired) {
              uselessWindowEvals = 0;
            } else if (++uselessWindowEvals > kWindowWalkPatience) {
              windowWalkDisabled = true;
              windowWalkDisabledAtTheta = minCompetitiveScore;
            }
          }
          return target;
        }
        uselessWindowEvals = 0;
        skipCount(SkipStats::mandOptWindowSkips);
        if (windowUpTo >= solux::PostingsReader::END - 1) {
          return solux::PostingsReader::END;
        }
        target = windowUpTo + 1;
        moveToNextBlock(target);
        evaluated = true;
      }
    }

    int32_t advanceInternal(int32_t target) {
      if (target == solux::PostingsReader::END) {
        id = mand.docId() < target ? mand.advance(target) : target;
        return id;
      }

      int32_t reqDoc = target;
      advanceHead:
      for (;;) {
        if (minCompetitiveScore > 0.0f) {
          reqDoc = advanceImpacts(reqDoc);
        }
        if (mand.docId() < reqDoc) {
          reqDoc = mand.advance(reqDoc);
        }
        if (reqDoc == solux::PostingsReader::END || !optIsRequired) {
          id = reqDoc;
          return id;
        }

        int32_t upperBound = reqMaxScore < minCompetitiveScore
          ? solux::PostingsReader::END
          : windowUpTo;
        if (reqDoc > upperBound) {
          continue;
        }

        for (;;) {
          int32_t optDoc = opt.docId();
          if (optDoc < reqDoc) {
            optDoc = opt.advance(reqDoc);
          }
          if (optDoc > upperBound) {
            reqDoc = upperBound >= solux::PostingsReader::END - 1
              ? solux::PostingsReader::END
              : upperBound + 1;
            goto advanceHead;
          }

          if (optDoc != reqDoc) {
            reqDoc = mand.advance(optDoc);
            if (reqDoc > upperBound) {
              goto advanceHead;
            }
          }

          if (reqDoc == solux::PostingsReader::END || optDoc == reqDoc) {
            id = reqDoc;
            return id;
          }
        }
      }
    }

    int32_t nextMatched(int32_t doc) {
      while (doc != solux::PostingsReader::END && !matches()) {
        doc = approximationNext();
      }
      return doc;
    }

  public:
    MandOptScorer(solux::MemPool& targetPool,
                  Scorer* mandScorer, bool mandTwoPhase,
                  Scorer* optScorer, bool optTwoPhase,
                  bool enableTwoPhase)
      : mand{mandScorer, enableTwoPhase && mandTwoPhase},
        opt{optScorer, enableTwoPhase && optTwoPhase} {
      unused(targetPool);
      reqMaxScore = mand.scorer->getMaxScoreForSetup(solux::PostingsReader::END);
    }

    int32_t docId() override {
      return id;
    }

    int32_t next() override {
      assert(id != solux::PostingsReader::END);
      int32_t doc = approximationNext();
      return anyTwoPhase() ? nextMatched(doc) : doc;
    }

    int32_t advance(int32_t docid) override {
      int32_t doc = approximationAdvance(docid);
      return anyTwoPhase() ? nextMatched(doc) : doc;
    }

    int32_t approximationNext() override {
#ifndef NDEBUG
      markInternalProtocol();
#endif
      assert(id != solux::PostingsReader::END);
      // Without a threshold the window walk is inert and optIsRequired can
      // never be set; keep the mandatory clause on its sequential next()
      // (exhaustive consumers like exact counts scan every posting, and the
      // advance path costs measurably more per step).
      if (!(minCompetitiveScore > 0.0f)) {
        id = mand.next();
        return id;
      }
      return advanceInternal(mand.docId() + 1);
    }

    int32_t approximationAdvance(int32_t target) override {
#ifndef NDEBUG
      markInternalProtocol();
#endif
      if (!(minCompetitiveScore > 0.0f)) {
        id = mand.advance(target);
        return id;
      }
      return advanceInternal(target);
    }

    int32_t approximationDocId() override {
      return id;
    }

    std::span<DocsPosEnum*> approximationEnums() override {
      if (!mand.twoPhase) {
        return {};
      }
      // This deliberately projects only the mandatory side. The outer
      // conjunction can globally order a selective filter with a phrase's
      // postings, while matchesAt() retains the dynamic opt-is-required test.
      // The scorer's private approximation remains opaque so its local
      // block-max walk can still require the optional side.
      return mand.scorer->approximationEnums();
    }

    bool matchesAt(int32_t doc) override {
#ifndef NDEBUG
      markExternalProtocol();
#endif
      id = doc;
      if (!mand.scorer->matchesAt(doc)) {
        return false;
      }

      if (optIsRequired) {
        int32_t optDoc = opt.docId();
        if (optDoc < doc) {
          optDoc = opt.advance(doc);
        }
        if (optDoc != doc) {
          return false;
        }
        if (!optMatches()) {
          opt.next();
          return false;
        }
      } else if (opt.docId() == doc && !optMatches()) {
        opt.next();
      }
      return true;
    }

    bool matches() override {
#ifndef NDEBUG
      markInternalProtocol();
#endif
      int32_t reqDoc = mand.docId();
      if (mand.twoPhase && !mand.scorer->matches()) {
        return false;
      }

      if (optIsRequired) {
        int32_t optDoc = opt.docId();
        if (optDoc < reqDoc) {
          optDoc = opt.advance(reqDoc);
        }
        if (optDoc != reqDoc) {
          return false;
        }
        if (!optMatches()) {
          opt.next();
          return false;
        }
      } else if (opt.docId() == reqDoc && !optMatches()) {
        opt.next();
      }
      return true;
    }

    float matchCost() override {
      float cost = 1.0f;
      if (mand.twoPhase) {
        cost += mand.scorer->matchCost();
      }
      if (opt.twoPhase) {
        cost += opt.scorer->matchCost();
      }
      return cost;
    }

    float score() override {
      float score = mand.scorer->score();
      // Consult the optional scorer for this exact doc (Lucene's ReqOptSumScorer
      // style): advance it only if behind, then add its score on an exact hit.
      int32_t optDoc = opt.docId();
      if (optDoc < id) {
        optDoc = opt.advance(id);
      }
      if (optDoc == id && !optMatches()) {
        optDoc = opt.next();
      }
      if (optDoc == id) {
        score += opt.scorer->score();
      }
      return score;
    }

    void setMinCompetitiveScore(float minScore) override {
      // Keep theta at the MandOpt level. Forwarding (theta - optMax) into a
      // term scorer switches its bulk fill from vectorized block scoring to
      // scalar impact hopping, which can cost far more than the skips it adds.
      if (minScore > minCompetitiveScore) {
        minCompetitiveScore = minScore;
        windowUpTo = -1;
      }
      if (reqMaxScore < minScore) {
        optIsRequired = true;
        if (reqMaxScore == 0.0f) {
          opt.scorer->setMinCompetitiveScore(minScore);
        }
      }
    }

    float getMaxScore(int32_t upTo) override {
      return maxScoreAt(upTo, false);
    }

    float refineMaxScore(int32_t upTo) override {
      return maxScoreAt(upTo, true);
    }

    ScoreBounds getScoreBounds(int32_t upTo) override {
      return scoreBoundsAt(upTo, false);
    }

    ScoreBounds refineScoreBounds(int32_t upTo) override {
      return scoreBoundsAt(upTo, true);
    }

    int32_t advanceShallow(int32_t target) override {
      int32_t upTo = mand.scorer->advanceShallow(target);
      int32_t optDoc = opt.docId();
      if (optDoc != solux::PostingsReader::END) {
        if (optDoc <= target) {
          upTo = std::min(upTo, opt.scorer->advanceShallow(target));
        } else {
          upTo = std::min(upTo, optDoc - 1);
        }
      }
      return upTo;
    }
  }; // MandOptScorer

  class FilterOnlyBulkScorer final : public BulkScorer {
    BulkScorer* lead;
    std::span<uint64_t> filterWords;
    std::span<uint64_t> combinedWords;
    DocSet* combinedWith = nullptr;
    int32_t filterCard;
    int32_t combinedCard = 0;
    int32_t maxDoc;
    bool combinedReady = false;

    void combineWith(DocSet* incoming) {
      if (combinedReady && combinedWith == incoming) {
        return;
      }
      combinedWith = incoming;
      combinedReady = true;
      combinedCard = 0;
      if (incoming->type == DocSet::BITSET) {
        const auto& incomingBits = ((BitDocSet*) incoming)->bits();
        for (size_t word = 0; word < filterWords.size(); word++) {
          combinedWords[word] = filterWords[word] & incomingBits.words[word];
          combinedCard += (int32_t) std::popcount(combinedWords[word]);
        }
      } else {
        std::fill(combinedWords.begin(), combinedWords.end(), 0);
        for (int32_t doc : ((ArrDocSet*) incoming)->docs()) {
          uint64_t mask = 1ULL << (doc & 63);
          if ((filterWords[(size_t) doc >> 6] & mask) != 0) {
            combinedWords[(size_t) doc >> 6] |= mask;
            combinedCard++;
          }
        }
      }
    }

  public:
    FilterOnlyBulkScorer(BulkScorer* lead,
                         std::span<uint64_t> filterWords,
                         std::span<uint64_t> combinedWords,
                         int32_t filterCard, int32_t maxDoc)
      : lead(lead), filterWords(filterWords), combinedWords(combinedWords),
        filterCard(filterCard), maxDoc(maxDoc) {}

    bool supportsMatchWindows() const override {
      return lead->supportsMatchWindows();
    }

    int32_t scoreNextWindow(ScoreWindow& out, DocSet* incoming,
                            int32_t min, int32_t max,
                            float minCompetitiveScore) override {
      // Filter clauses are non-scoring: emitted docs carry the Boolean's
      // constant score (0), never the lead clause's own scores, and the
      // lead must not prune on its own impacts.
      unused(minCompetitiveScore);
      int32_t next;
      if (filterWords.empty()) {
        // Single filter clause: the lead already IS the whole filter.
        next = lead->scoreNextWindow(out, incoming, min, max, 0.0f);
      } else if (incoming == nullptr) {
        BitDocSet filter(FixedBitSet(filterWords.data(), maxDoc), filterCard);
        next = lead->scoreNextWindow(out, &filter, min, max, 0.0f);
      } else {
        combineWith(incoming);
        BitDocSet filter(FixedBitSet(combinedWords.data(), maxDoc), combinedCard);
        next = lead->scoreNextWindow(out, &filter, min, max, 0.0f);
      }
      std::fill(out.scores.begin(), out.scores.begin() + out.size, 0.0f);
      return next;
    }

    int32_t matchNextWindow(ScoreWindow& out, DocSet* incoming,
                            int32_t min, int32_t max) override {
      if (filterWords.empty()) {
        return lead->matchNextWindow(out, incoming, min, max);
      }
      if (incoming == nullptr) {
        BitDocSet filter(FixedBitSet(filterWords.data(), maxDoc), filterCard);
        return lead->matchNextWindow(out, &filter, min, max);
      }
      combineWith(incoming);
      BitDocSet filter(FixedBitSet(combinedWords.data(), maxDoc), combinedCard);
      return lead->matchNextWindow(out, &filter, min, max);
    }

    int32_t countNextWindow(int64_t& count, DocSetBuilder* domainOut,
                            DocSet* incoming, int32_t min, int32_t max) override {
      if (filterWords.empty()) {
        return lead->countNextWindow(count, domainOut, incoming, min, max);
      }
      if (incoming == nullptr) {
        BitDocSet filter(FixedBitSet(filterWords.data(), maxDoc), filterCard);
        return lead->countNextWindow(count, domainOut, &filter, min, max);
      }
      combineWith(incoming);
      BitDocSet filter(FixedBitSet(combinedWords.data(), maxDoc), combinedCard);
      return lead->countNextWindow(count, domainOut, &filter, min, max);
    }
  };

  class MandOptBulkScorer final : public BulkScorer {
    constexpr static int32_t kWindowSize = DocsEnumMeta::L1_DOCS;
    constexpr static int32_t kWindowWords = kWindowSize / 64;
    static_assert((kWindowSize % 64) == 0);

    Query::Scorer* mand;
    std::span<Query::Scorer*> opts;
    std::span<int64_t> optCosts;
    std::span<float> optWindowMax;
    std::span<int32_t> optOrder;
    std::span<double> optRemainingMax;
    std::span<uint64_t> windowBits;
    // Valid only for set bits in windowBits; first touch overwrites the row.
    std::span<float> windowScores;
    std::span<int32_t> outDocs;
    std::span<float> outScores;
    WindowFilter* windowFilter = nullptr;

    int32_t maxDoc;
    int64_t mandCost;
    int32_t windowStart = 0;
    int32_t windowEnd = 0;
    int32_t liveOptCount = 0;
    float minCompetitiveScore = std::numeric_limits<float>::lowest();
    double scoreBoundFactor = 1.0;
    float mandWindowMax = std::numeric_limits<float>::infinity();
    double optWindowMaxSum = 0.0;

    static bool lessMaxScore(float a, float b) {
      bool finiteA = std::isfinite(a);
      bool finiteB = std::isfinite(b);
      if (finiteA != finiteB) return finiteA;
      return a < b;
    }

    bool greaterWindowOrder(int32_t a, int32_t b) const {
      float maxA = optWindowMax[(size_t) a];
      float maxB = optWindowMax[(size_t) b];
      if (maxA != maxB) return lessMaxScore(maxB, maxA);
      return a < b;
    }

    bool competitiveEnabled() const {
      return minCompetitiveScore > 0.0f;
    }

    bool canReach(float score, double bound) const {
      return scoreCanReach(score, bound, minCompetitiveScore, scoreBoundFactor);
    }

    void clearWindowBits() {
      std::fill(windowBits.begin(), windowBits.end(), 0);
    }

    void addWindowScore(int32_t index, float score) {
      uint64_t& word = windowBits[(size_t) (index >> 6)];
      uint64_t mask = 1ULL << (index & 63);
      if ((word & mask) == 0) {
        word |= mask;
        windowScores[(size_t) index] = score;
      } else {
        windowScores[(size_t) index] += score;
      }
    }

    bool acceptsDoc(DocSet* filter, int32_t doc) const {
      return (filter == nullptr || filter->get(doc))
          && (windowFilter == nullptr || windowFilter->accepts(doc));
    }

    void setWindowBounds(int32_t start, int32_t max) {
      windowStart = start;
      int32_t requestedEnd = windowStart + kWindowSize;
      if (requestedEnd < windowStart) {
        requestedEnd = max;
      }
      windowEnd = std::min(std::min(requestedEnd, max), maxDoc);
    }

    void prepareOutputWindow(ScoreWindow& out) {
      out.min = windowStart;
      out.max = windowEnd;
      out.size = 0;
      out.docs = outDocs;
      out.scores = outScores;
    }

    int32_t computeSkipUpTo(int32_t target, int32_t max) {
      int32_t upTo = mand->advanceShallow(target);
      if (upTo >= max) {
        upTo = max - 1;
      }
      for (size_t i = 0; i < opts.size(); i++) {
        auto* opt = opts[i];
        int32_t doc = opt->docId();
        if (doc == PostingsReader::END) {
          continue;
        }
        if (doc <= target) {
          upTo = std::min(upTo, opt->advanceShallow(target));
        } else {
          upTo = std::min(upTo, doc - 1);
        }
      }
      if (upTo >= max) {
        upTo = max - 1;
      }
      return std::max(upTo, target);
    }

    double skipMaxScoreAt(int32_t upTo, bool refined) {
      double sum = (double) (refined ? mand->refineMaxScore(upTo)
                                    : mand->getMaxScore(upTo));
      for (size_t i = 0; i < opts.size(); i++) {
        auto* opt = opts[i];
        int32_t doc = opt->docId();
        if (doc != PostingsReader::END && doc <= upTo) {
          sum += (double) optionalUpperBound(
              refined ? opt->refineMaxScore(upTo)
                      : opt->getMaxScore(upTo));
        }
      }
      return sum;
    }

    int32_t skipToCompetitiveWindow(int32_t start, int32_t max) {
      int32_t target = start;
      while (target < max) {
        int32_t upTo = computeSkipUpTo(target, max);
        double maxScore = skipMaxScoreAt(upTo, false);
        if (canReach(0.0f, maxScore) && std::isfinite(maxScore)
            && (double) minCompetitiveScore >= kRefineBeta * maxScore) {
          double refined = skipMaxScoreAt(upTo, true);
          if (refined < maxScore) {
            maxScore = refined;
          }
        }
        if (canReach(0.0f, maxScore)) {
          return target;
        }
        skipCount(SkipStats::mandOptBulkWindowSkips);
        if (upTo >= max - 1 || upTo >= PostingsReader::END - 1) {
          return PostingsReader::END;
        }
        target = upTo + 1;
      }
      return target;
    }

    void sortWindowOrder() {
      for (int32_t i = 1; i < liveOptCount; i++) {
        int32_t idx = optOrder[(size_t) i];
        int32_t j = i;
        while (j > 0 && greaterWindowOrder(idx, optOrder[(size_t) j - 1])) {
          optOrder[(size_t) j] = optOrder[(size_t) j - 1];
          j--;
        }
        optOrder[(size_t) j] = idx;
      }
    }

    void buildRemainingMax() {
      optRemainingMax[(size_t) liveOptCount] = 0.0;
      for (int32_t i = liveOptCount; i-- > 0; ) {
        int32_t idx = optOrder[(size_t) i];
        optRemainingMax[(size_t) i] =
            optRemainingMax[(size_t) i + 1] + (double) optWindowMax[(size_t) idx];
      }
      optWindowMaxSum = optRemainingMax[0];
    }

    void updateProductionMaxScores() {
      int32_t mandDoc = mand->docId();
      if (mandDoc >= windowEnd) {
        mandWindowMax = 0.0f;
      } else {
        mand->advanceShallowForSetup(windowStart);
        mandWindowMax = mand->getMaxScoreForSetup(windowEnd - 1);
      }

      liveOptCount = 0;
      for (size_t i = 0; i < opts.size(); i++) {
        auto* opt = opts[i];
        int32_t doc = opt->docId();
        if (doc >= windowEnd) {
          optWindowMax[i] = 0.0f;
          continue;
        }
        opt->advanceShallowForSetup(windowStart);
        optWindowMax[i] = optionalUpperBound(
            opt->getMaxScoreForSetup(windowEnd - 1));
        optOrder[(size_t) liveOptCount++] = (int32_t) i;
      }
      sortWindowOrder();
      buildRemainingMax();
    }

    void updateLiveOptsWithoutBounds() {
      mandWindowMax = std::numeric_limits<float>::infinity();
      liveOptCount = 0;
      optWindowMaxSum = 0.0;
      for (size_t i = 0; i < opts.size(); i++) {
        optWindowMax[i] = 0.0f;
        if (opts[i]->docId() < windowEnd) {
          optOrder[(size_t) liveOptCount++] = (int32_t) i;
        }
      }
      optRemainingMax[(size_t) liveOptCount] = 0.0;
      for (int32_t i = liveOptCount; i-- > 0; ) {
        optRemainingMax[(size_t) i] = 0.0;
      }
    }

    void setupProductionWindow(int32_t start, int32_t max) {
      setWindowBounds(start, max);
      if (competitiveEnabled()) {
        updateProductionMaxScores();
      } else {
        updateLiveOptsWithoutBounds();
      }
      skipCount(SkipStats::mandOptBulkWindows);
    }

    void recordCompaction(int32_t before, int32_t after) {
      if (SkipStats::enabled && after < before) {
        SkipStats::mandOptBulkCompactions += (int64_t) (before - after);
      }
    }

    int32_t compactCompetitive(ScoreWindow& out, double bound) {
      float threshold = competitiveScoreThreshold(minCompetitiveScore, scoreBoundFactor, bound);
      int32_t write = compactByScoreThreshold(out.docs.data(), out.scores.data(),
                                              out.size, threshold);
      recordCompaction(out.size, write);
      return write;
    }

    void finishCompetitive(ScoreWindow& out) {
      if (competitiveEnabled()) {
        out.size = compactByScoreNotLessThanThreshold(out.docs.data(), out.scores.data(),
                                                      out.size, minCompetitiveScore);
      }
    }

    int64_t liveOptCostSum() const {
      int64_t sum = 0;
      for (int32_t i = 0; i < liveOptCount; i++) {
        int64_t cost = optCosts[(size_t) optOrder[(size_t) i]];
        if (cost <= 0) {
          continue;
        }
        int64_t room = std::numeric_limits<int64_t>::max() - sum;
        if (cost >= room) {
          return std::numeric_limits<int64_t>::max();
        }
        sum += cost;
      }
      return sum;
    }

    bool useOptDrivenFill() const {
      return competitiveEnabled() && !canReach(0.0f, (double) mandWindowMax)
          && liveOptCostSum() < mandCost;
    }

    void applyOptSweeps(ScoreWindow& out) {
      if (competitiveEnabled()) {
        out.size = compactCompetitive(out, optRemainingMax[0]);
      }
      for (int32_t i = 0; i < liveOptCount && out.size > 0; i++) {
        int32_t idx = optOrder[(size_t) i];
        skipCount(SkipStats::mandOptBulkSweeps);
        out.size = opts[(size_t) idx]->applyToCandidates(out.docs.data(), out.scores.data(),
                                                         out.size, false);
        if (competitiveEnabled() && i + 1 < liveOptCount) {
          out.size = compactCompetitive(out, optRemainingMax[(size_t) i + 1]);
        }
      }
      finishCompetitive(out);
    }

    bool fillMandDrivenCandidates(ScoreWindow& out, DocSet* filter) {
      prepareOutputWindow(out);
      if (mand->docId() < windowStart && mand->advance(windowStart) == PostingsReader::END) {
        return false;
      }

      if (filter == nullptr && windowFilter == nullptr) {
        int32_t n;
        while ((n = mand->fillScoreBlock(out.docs.data() + out.size,
                                         out.scores.data() + out.size,
                                         kWindowSize - out.size, windowEnd)) > 0) {
          skipCount(SkipStats::maxScoreDirectFills);
          out.size += n;
          assert(out.size <= kWindowSize);
        }
        applyOptSweeps(out);
        return mand->docId() != PostingsReader::END;
      }

      int32_t blockDocs[Postings::DOCS_BLOCK_SIZE];
      float blockScores[Postings::DOCS_BLOCK_SIZE];
      int32_t n;
      while ((n = mand->fillScoreBlock(blockDocs, blockScores,
                                       Postings::DOCS_BLOCK_SIZE, windowEnd)) > 0) {
        for (int32_t i = 0; i < n; i++) {
          int32_t doc = blockDocs[i];
          if (!acceptsDoc(filter, doc)) {
            continue;
          }
          assert(out.size < kWindowSize);
          out.docs[(size_t) out.size] = doc;
          out.scores[(size_t) out.size] = blockScores[i];
          out.size++;
        }
      }
      applyOptSweeps(out);
      return mand->docId() != PostingsReader::END;
    }

    void extractOptDrivenCandidates(ScoreWindow& out) {
      int32_t innerSize = windowEnd - windowStart;
      for (int32_t word = 0; word < kWindowWords; word++) {
        uint64_t bits = windowBits[(size_t) word];
        while (bits != 0) {
          int32_t bit = (int32_t) std::countr_zero(bits);
          int32_t index = (word << 6) + bit;
          if (index >= innerSize) {
            break;
          }
          out.docs[(size_t) out.size] = windowStart + index;
          out.scores[(size_t) out.size] = windowScores[(size_t) index];
          out.size++;
          bits &= bits - 1;
        }
      }
    }

    bool fillOptDrivenCandidates(ScoreWindow& out, DocSet* filter) {
      skipCount(SkipStats::mandOptBulkOptDrivenWindows);
      prepareOutputWindow(out);
      clearWindowBits();
      int32_t blockDocs[Postings::DOCS_BLOCK_SIZE];
      float blockScores[Postings::DOCS_BLOCK_SIZE];
      for (int32_t i = 0; i < liveOptCount; i++) {
        auto* opt = opts[(size_t) optOrder[(size_t) i]];
        if (opt->docId() < windowStart) {
          opt->advance(windowStart);
        }
        int32_t n;
        while ((n = opt->fillScoreBlock(blockDocs, blockScores,
                                        Postings::DOCS_BLOCK_SIZE, windowEnd)) > 0) {
          for (int32_t j = 0; j < n; j++) {
            int32_t doc = blockDocs[j];
            if (acceptsDoc(filter, doc)) {
              addWindowScore(doc - windowStart, blockScores[j]);
            }
          }
        }
      }

      extractOptDrivenCandidates(out);
      if (competitiveEnabled()) {
        out.size = compactCompetitive(out, (double) mandWindowMax);
      }
      if (out.size == 0) {
        return mand->docId() != PostingsReader::END;
      }
      if (mand->docId() < windowStart && mand->advance(windowStart) == PostingsReader::END) {
        out.size = 0;
        return false;
      }
      int32_t before = out.size;
      out.size = mand->applyToCandidates(out.docs.data(), out.scores.data(),
                                         out.size, true);
      recordCompaction(before, out.size);
      finishCompetitive(out);
      return mand->docId() != PostingsReader::END;
    }

  public:
    MandOptBulkScorer(solux::MemPool& pool, Query::Scorer* mand,
                      std::span<Query::Scorer*> opts, std::span<int64_t> optCosts,
                      int32_t maxDoc, int64_t mandCost)
        : mand(mand),
          opts(opts),
          optCosts(optCosts),
          optWindowMax(pool.make_arr<float>(opts.size()), opts.size()),
          optOrder(pool.make_arr<int32_t>(opts.size()), opts.size()),
          optRemainingMax(pool.make_arr<double>(opts.size() + 1), opts.size() + 1),
          windowBits(pool.make_arr<uint64_t>((size_t) kWindowWords), (size_t) kWindowWords),
          windowScores(pool.make_arr<float>((size_t) kWindowSize), (size_t) kWindowSize),
          outDocs(pool.make_arr<int32_t>((size_t) kWindowSize), (size_t) kWindowSize),
          outScores(pool.make_arr<float>((size_t) kWindowSize), (size_t) kWindowSize),
          maxDoc(maxDoc),
          mandCost(mandCost) {
      assert(mand != nullptr);
      assert(!opts.empty());
      scoreBoundFactor = 1.0 + (double) (opts.size() + 1) * 0x1p-24;
      for (auto* opt : opts) {
        assert(opt != nullptr);
      }
    }

    bool attachWindowFilter(WindowFilter* filter) override {
      assert(windowFilter == nullptr);
      windowFilter = filter;
      return true;
    }

    int32_t scoreNextWindow(ScoreWindow& out, DocSet* filter, int32_t min, int32_t max,
                            float minCompetitiveScore) override {
      out.min = min;
      out.max = min;
      out.size = 0;
      out.docs = outDocs;
      out.scores = outScores;

      max = std::min(max, maxDoc);
      if (min >= max) {
        return PostingsReader::END;
      }
      if (filter != nullptr && filter->card() == 0) {
        return PostingsReader::END;
      }
      if (minCompetitiveScore > this->minCompetitiveScore) {
        this->minCompetitiveScore = minCompetitiveScore;
      }

      int32_t start = min;
      if (competitiveEnabled()) {
        start = skipToCompetitiveWindow(start, max);
        if (start == PostingsReader::END || start >= max) {
          out.max = max;
          return PostingsReader::END;
        }
      }

      setupProductionWindow(start, max);
      // Candidates require an unconsumed mandatory doc inside the window; an
      // empty window must not pay the fill (opt-driven would decode every
      // optional posting only to intersect against nothing).
      int32_t mandDoc = mand->docId();
      if (mandDoc >= windowEnd) {
        out.min = windowStart;
        out.max = windowEnd;
        if (mandDoc == PostingsReader::END) {
          out.max = max;
          return PostingsReader::END;
        }
        return windowEnd >= max ? PostingsReader::END : windowEnd;
      }
      if (windowFilter != nullptr
          && windowFilter->prepare(windowStart, windowEnd) == 0) {
        prepareOutputWindow(out);
        return windowEnd >= max ? PostingsReader::END : windowEnd;
      }
      bool mandMayContinue = useOptDrivenFill()
          ? fillOptDrivenCandidates(out, filter)
          : fillMandDrivenCandidates(out, filter);
      if (!mandMayContinue && out.size == 0) {
        out.max = max;
        return PostingsReader::END;
      }
      return windowEnd >= max ? PostingsReader::END : windowEnd;
    }

    int32_t countNextWindow(int64_t& count, DocSetBuilder* domainOut,
                            DocSet* filter, int32_t min, int32_t max) override {
      max = std::min(max, maxDoc);
      if (min >= max) {
        return PostingsReader::END;
      }
      if (filter != nullptr && filter->card() == 0) {
        return PostingsReader::END;
      }
      if (domainOut != nullptr) {
        skipCount(SkipStats::bulkDomainWindowsFed);
      }
      setWindowBounds(min, max);
      skipCount(SkipStats::mandOptBulkWindows);
      if (windowFilter != nullptr
          && windowFilter->prepare(windowStart, windowEnd) == 0) {
        return windowEnd >= max ? PostingsReader::END : windowEnd;
      }
      if (mand->docId() < windowStart && mand->advance(windowStart) == PostingsReader::END) {
        return PostingsReader::END;
      }

      int32_t blockDocs[Postings::DOCS_BLOCK_SIZE];
      float blockScores[Postings::DOCS_BLOCK_SIZE];
      int32_t n;
      while ((n = mand->fillScoreBlock(blockDocs, blockScores,
                                       Postings::DOCS_BLOCK_SIZE, windowEnd)) > 0) {
        unused(blockScores);
        for (int32_t i = 0; i < n; i++) {
          int32_t doc = blockDocs[i];
          if (acceptsDoc(filter, doc)) {
            count++;
            if (domainOut != nullptr) {
              domainOut->add(doc);
            }
          }
        }
      }
      if (mand->docId() == PostingsReader::END) {
        return PostingsReader::END;
      }
      return windowEnd >= max ? PostingsReader::END : windowEnd;
    }
  }; // MandOptBulkScorer

  // Add rank-only SHOULD scores to an exact required-body bulk stream.
  // Membership and the mandatory score are owned by required; optionals can
  // only add score, so sweeping them over the surviving batch preserves the
  // Boolean match set and score-addition order. This decorator deliberately
  // disables pruning: the required-body bounds do not include the optional
  // contribution.
  class OptionalScoreBulkScorer final : public BulkScorer {
    BulkScorer* required;
    std::span<Query::Scorer*> optional;

  public:
    OptionalScoreBulkScorer(BulkScorer* required,
                            std::span<Query::Scorer*> optional)
        : required(required), optional(optional) {
      assert(required != nullptr);
      assert(!optional.empty());
    }

    void setTopKDepth(int32_t topK, bool allowPruning) override {
      assert(!allowPruning);
      unused(allowPruning);
      required->setTopKDepth(topK, false);
    }

    bool willCountDense() const override {
      return required->willCountDense();
    }

    bool supportsMatchWindows() const override {
      return required->supportsMatchWindows();
    }

    bool attachWindowFilter(WindowFilter* filter) override {
      return required->attachWindowFilter(filter);
    }

    int32_t scoreNextWindow(ScoreWindow& out, DocSet* filter,
                            int32_t min, int32_t max,
                            float minCompetitiveScore) override {
      unused(minCompetitiveScore);
      int32_t next = required->scoreNextWindow(
          out, filter, min, max, std::numeric_limits<float>::lowest());
      for (auto* scorer : optional) {
        int32_t size = scorer->applyToCandidates(
            out.docs.data(), out.scores.data(), out.size, false);
        assert(size == out.size);
        unused(size);
      }
      return next;
    }

    int32_t matchNextWindow(ScoreWindow& out, DocSet* filter,
                            int32_t min, int32_t max) override {
      return required->matchNextWindow(out, filter, min, max);
    }

    int32_t countNextWindow(int64_t& count, DocSetBuilder* domainOut,
                            DocSet* filter, int32_t min,
                            int32_t max) override {
      return required->countNextWindow(
          count, domainOut, filter, min, max);
    }
  };

  class MandNotScorer final : public Query::Scorer {
    Scorer* mandScorer;
    Scorer* notScorer;
    int32_t id = -1;
    int32_t notid = -1;
    bool notTwoPhase;
  public:
    static inline bool disableNotTwoPhaseForTests = false;

    MandNotScorer(solux::MemPool& targetPool,
                  Scorer* mandScorer, Scorer* notScorer,
                  bool prohibitedTwoPhase, bool enableTwoPhase)
      : mandScorer(mandScorer), notScorer(notScorer),
        notTwoPhase(enableTwoPhase && prohibitedTwoPhase) {
      unused(targetPool);
    }

    int32_t docId() override {
      return id;
    }

    int32_t next() override {
      assert(id != solux::PostingsReader::END);
      id = mandScorer->next();
      return doNext();
    }

    int32_t advance(int32_t docid) override {
      id = mandScorer->advance(docid);
      return doNext();
    }

    float score() override {
      return mandScorer->score();
    }

    void setMinCompetitiveScore(float minScore) override {
      mandScorer->setMinCompetitiveScore(minScore);
    }

    float getMaxScore(int32_t upTo) override {
      return mandScorer->getMaxScore(upTo);
    }

    float refineMaxScore(int32_t upTo) override {
      return mandScorer->refineMaxScore(upTo);
    }

    ScoreBounds getScoreBounds(int32_t upTo) override {
      return mandScorer->getScoreBounds(upTo);
    }

    ScoreBounds refineScoreBounds(int32_t upTo) override {
      return mandScorer->refineScoreBounds(upTo);
    }

    int32_t advanceShallow(int32_t target) override {
      return mandScorer->advanceShallow(target);
    }

    float getMaxScoreForSetup(int32_t upTo) override {
      return mandScorer->getMaxScoreForSetup(upTo);
    }

    int32_t advanceShallowForSetup(int32_t target) override {
      return mandScorer->advanceShallowForSetup(target);
    }

  private:

    // mandScorer should be advanced and id set before calling this
    int32_t doNext() {
      while (id != solux::PostingsReader::END) {
        if (notid < id) {
          notid = notTwoPhase ? notScorer->approximationAdvance(id)
                              : notScorer->advance(id);
        }
        if (notid > id) {
          return id;
        }
        if (notTwoPhase && !notScorer->matches()) {
          return id;
        }
        // at this point, notid == id, so we need to try another id by calling next again.
        id = mandScorer->next();
      }
      return id;  // only way to reach here is if we hit the end
    }
  }; // MandNotScorer


  class ConjunctionScorer final : public Query::Scorer {
    struct ApproxSlot {
      enum class Kind : uint8_t {
        SINGLE_PHASE,
        TWO_PHASE,
        DOCS_ENUM
      };

      Query::Scorer* scorer = nullptr;
      DocsPosEnum* docsEnum = nullptr;
      int64_t cost = 0;
      Kind kind = Kind::SINGLE_PHASE;

      int32_t next() const {
        switch (kind) {
          case Kind::SINGLE_PHASE: return scorer->next();
          case Kind::TWO_PHASE: return scorer->approximationNext();
          case Kind::DOCS_ENUM: return docsEnum->next();
        }
        std::unreachable();
      }

      int32_t advance(int32_t target) const {
        switch (kind) {
          case Kind::SINGLE_PHASE: return scorer->advance(target);
          case Kind::TWO_PHASE: return scorer->approximationAdvance(target);
          case Kind::DOCS_ENUM: return docsEnum->advance(target);
        }
        std::unreachable();
      }

      int32_t docId() const {
        switch (kind) {
          case Kind::SINGLE_PHASE: return scorer->docId();
          case Kind::TWO_PHASE: return scorer->approximationDocId();
          case Kind::DOCS_ENUM: return docsEnum->docId();
        }
        std::unreachable();
      }
    };

    struct VerifierSlot {
      Query::Scorer* scorer = nullptr;
      size_t approxIndex = 0;
      float matchCost = 0.0f;
      bool externallyDriven = false;

      bool matches(int32_t doc) const {
        return externallyDriven ? scorer->matchesAt(doc) : scorer->matches();
      }
    };

    enum class ExactApproxKind : uint8_t {
      DOCS_ENUM,
      TERM_SCORER,
      DOC_SET,
    };

    std::span<Query::Scorer*> scorers; // subset of required clauses that contributes to score()
    std::span<Query::Scorer*> conjunctionClauses; // every direct required clause
    std::span<ApproxSlot> approximations; // every required clause, ascending cost (lead first)
    std::span<VerifierSlot> verifiers; // two-phase clauses sorted by matchCost
    std::span<ExactApproxKind> exactApproxKinds;

    int32_t docid = -1;
    float minCompetitiveScore = 0.0f;
    // Lead docs <= competitiveUpTo lie in block ranges whose summed clause
    // bounds passed the threshold check; they skip re-evaluation.  Reset when
    // the threshold rises.
    int32_t competitiveUpTo = -1;
    // Consecutive competitive (non-skipping) evaluations back off: each
    // failure extends the certified horizon geometrically, so hopeless bound
    // checks decay to O(log) per threshold epoch instead of one evaluation
    // per posting block of the densest clause (a stopword-ish phrase member
    // otherwise pays impact walks on every block that never prune).  A
    // successful skip or a threshold rise resets to fine granularity;
    // instances that have proven able to skip (skipSeen) ramp far more
    // gently so intermittent skip opportunities survive.  The policy only
    // changes how often bounds are consulted - admitted ranges are still
    // fully intersected and verified, so results are exact regardless.
    int failStreak = 0;
    bool skipSeen = false;
    bool competitivePruning;
    int64_t skippedRangeCount = 0;

    // Skip past doc-block ranges where the SUM of the scoring clauses' score
    // bounds cannot reach the collector's threshold (Lucene's
    // BlockMaxConjunctionScorer shape).  Returns a possibly-competitive target
    // or END.  A clause without impact data bounds as +infinity; that caches
    // as competitive-through-upTo, so the check stays one compare per lead
    // move rather than a permanent re-evaluation.
    int32_t advanceTarget(int32_t target) {
      if (target <= competitiveUpTo || !(minCompetitiveScore > 0.0f)
          || disablePruningForTests) {
        return target;
      }
      for (;;) {
        if (target >= solux::PostingsReader::END - 1) {
          return solux::PostingsReader::END;
        }
        skipCount(SkipStats::conjRangeEvals);
        int32_t evalStartTarget = target;
        int32_t upTo = solux::PostingsReader::END;
        for (auto* scorer : scorers) {
          upTo = std::min(upTo, scorer->advanceShallow(target));
        }
        double maxScore = 0.0;
        for (auto* scorer : scorers) {
          float scorerMax = scorer->getMaxScore(upTo);
          if (!std::isfinite(scorerMax)) {
            maxScore = std::numeric_limits<double>::infinity();
            break;
          }
          maxScore += (double) scorerMax;
        }
        // The pull conjunction refines near-theta ranges to block granularity.
        // Its members can be two-phase, so a skipped range can avoid position
        // verification. The all-term bulk scorer stays group-granular to avoid
        // refining every candidate window.
        bool competitive = maxScore >= (double) minCompetitiveScore;
        if (competitive && std::isfinite(maxScore)
            && (double) minCompetitiveScore >= kRefineBeta * maxScore) {
          double refinedMax = maxScore;
          for (auto* scorer : scorers) {
            float cheapMax = scorer->getMaxScore(upTo);
            float refined = scorer->refineMaxScore(upTo);
            if (!std::isfinite(refined)) {
              refinedMax = std::numeric_limits<double>::infinity();
              break;
            }
            refinedMax += (double) refined - (double) cheapMax;
            if (refinedMax < (double) minCompetitiveScore) {
              competitive = false;
              break;
            }
          }
          if (competitive) {
            int32_t refinedUpTo = solux::PostingsReader::END;
            for (auto* scorer : scorers) {
              refinedUpTo = std::min(refinedUpTo, scorer->advanceShallow(target));
            }
            if (refinedUpTo < upTo) {
              upTo = refinedUpTo;
              refinedMax = 0.0;
              for (auto* scorer : scorers) {
                float refined = scorer->refineMaxScore(upTo);
                if (!std::isfinite(refined)) {
                  refinedMax = std::numeric_limits<double>::infinity();
                  break;
                }
                refinedMax += (double) refined;
              }
              competitive = refinedMax >= (double) minCompetitiveScore;
            }
          }
        }
        if (competitive) {
          competitiveUpTo = upTo;
          if (failStreak > 0 && upTo < solux::PostingsReader::END - 1) {
            constexpr int kBackoffShiftCap = 15;
            int kBackoffInitialShift = skipSeen ? 15 : 3;
            int shift = failStreak - 1 - kBackoffInitialShift;
            int64_t span = (int64_t) upTo - (int64_t) evalStartTarget + 1;
            int64_t extension = shift >= 0
                ? span << std::min(shift, kBackoffShiftCap)
                : std::max<int64_t>(1, span >> -shift);
            int64_t extended = (int64_t) upTo + extension;
            int64_t maxDoc = (int64_t) solux::PostingsReader::END - 1;
            int32_t backedOffUpTo = (int32_t) std::min(extended, maxDoc);
            if (backedOffUpTo > upTo) {
              competitiveUpTo = backedOffUpTo;
              skipCount(SkipStats::conjEvalBackoffs);
            }
          }
          if (failStreak <= 15 + 15) {
            failStreak++;
          }
          return target;
        }
        skippedRangeCount++;
        skipCount(SkipStats::conjRangeSkips);
        failStreak = 0;
        skipSeen = true;
        if (upTo >= solux::PostingsReader::END - 1) {
          return solux::PostingsReader::END;
        }
        target = upTo + 1;
      }
    }

    // Route a lead landing doc through advanceTarget so every candidate that
    // enters the conjunction loop is in a competitive block range.
    int32_t leadTo(int32_t id) {
      for (;;) {
        int32_t pruned = advanceTarget(id);
        if (pruned == id) {
          return id;
        }
        if (pruned == solux::PostingsReader::END) {
          return solux::PostingsReader::END;
        }
        id = approximations[0].advance(pruned);
      }
    }

    int32_t exactNext(size_t index) {
      const ApproxSlot& approximation = approximations[index];
      switch (exactApproxKinds[index]) {
        case ExactApproxKind::DOCS_ENUM:
          return approximation.docsEnum->next();
        case ExactApproxKind::TERM_SCORER:
          return disableExactFreqOnSurvivalForTests
              ? static_cast<TermQuery::Scorer*>(
                    approximation.scorer)->next()
              : static_cast<TermQuery::Scorer*>(
                    approximation.scorer)->nextScoredProbe();
        case ExactApproxKind::DOC_SET:
          return static_cast<QueryPrep::DocSetScorer*>(
              approximation.scorer)->next();
      }
      std::unreachable();
    }

    int32_t exactAdvance(size_t index, int32_t target) {
      const ApproxSlot& approximation = approximations[index];
      switch (exactApproxKinds[index]) {
        case ExactApproxKind::DOCS_ENUM:
          return approximation.docsEnum->advance(target);
        case ExactApproxKind::TERM_SCORER:
          return disableExactFreqOnSurvivalForTests
              ? static_cast<TermQuery::Scorer*>(
                    approximation.scorer)->advance(target)
              : static_cast<TermQuery::Scorer*>(
                    approximation.scorer)->advanceScoredProbe(target);
        case ExactApproxKind::DOC_SET:
          return static_cast<QueryPrep::DocSetScorer*>(
              approximation.scorer)->advance(target);
      }
      std::unreachable();
    }

    int32_t exactDocId(size_t index) {
      const ApproxSlot& approximation = approximations[index];
      switch (exactApproxKinds[index]) {
        case ExactApproxKind::DOCS_ENUM:
          return approximation.docsEnum->docId();
        case ExactApproxKind::TERM_SCORER:
          return static_cast<TermQuery::Scorer*>(
              approximation.scorer)->docId();
        case ExactApproxKind::DOC_SET:
          return static_cast<QueryPrep::DocSetScorer*>(
              approximation.scorer)->docId();
      }
      std::unreachable();
    }

    int32_t doNextExact(int32_t target) {
      outer:
      for (;;) {
        if (target == solux::PostingsReader::END) {
          docid = target;
          return docid;
        }
        for (size_t j = 1; j < approximations.size(); j++) {
          if (exactDocId(j) < target) {
            int32_t id = exactAdvance(j, target);
            assert(id >= target);
            if (id > target) {
              target = exactAdvance(0, id);
              goto outer;
            }
          }
        }
        for (auto& verifier : verifiers) {
          if (!verifier.matches(target)) {
            int32_t id = exactNext(verifier.approxIndex);
            target = verifier.approxIndex == 0
                ? id : exactAdvance(0, id);
            goto outer;
          }
        }
        docid = target;
        return docid;
      }
    }

    // internal utility method where first approximation has already been advanced to target.
    int32_t doNext(int32_t target) {
      ApproxSlot& first = approximations[0];

      outer:
      for (;;) {
        if (target == solux::PostingsReader::END) {
          docid = target;
          return docid;
        }
        for (int j = 1; j < (int) approximations.size(); j++) {
          // advance() is strict; skip sub-scorers already on target.
          if (approximations[(size_t) j].docId() < target) {
            int32_t id = approximations[(size_t) j].advance(target);
            assert(id >= target);
            if (id > target) {
              target = leadTo(first.advance(id));
              goto outer;  // could perhaps replace with "j=0; continue;" but that seems potentially worse?
            }
          }
        }
        // if we made it through the loop, all approximations matched.
        for (auto& verifier : verifiers) {
          if (!verifier.matches(target)) {
            int32_t id = approximations[verifier.approxIndex].next();
            target = leadTo(verifier.approxIndex == 0 ? id : first.advance(id));
            goto outer;
          }
        }
        docid = target;
        return docid;
      }
      // unreachable
    }

  public:
    // Test hook: disable block-max range skipping. The default evaluates each
    // scorer's natural bound horizon without imposing a minimum stride.
    static inline bool disablePruningForTests = false;
    static inline bool disableApproxFlattenForTests = false;
    static inline bool disableExactDirectApproximationsForTests = false;
    static inline bool disableExactFreqOnSurvivalForTests = false;

    // allCosts contains each allScorers entry's supplier cost. scoringScorers is
    // the subset whose score() contributes to the conjunction score (filter
    // clauses iterate but do not score); every entry must also appear in
    // allScorers.
    // TODO: if any scoring scorer is boosted to 0 it could be dropped from the
    // scoring subset while staying in allScorers.
    ConjunctionScorer(solux::MemPool& pool, std::span<Query::Scorer*> allScorers,
                      std::span<int64_t> allCosts,
                      std::span<const uint8_t> twoPhaseScorers,
                      std::span<Query::Scorer*> scoringScorers,
                      bool competitivePruning,
                      bool enableTwoPhase)
            : scorers(scoringScorers), conjunctionClauses(allScorers),
              competitivePruning(competitivePruning) {
      assert(allScorers.size() == allCosts.size());
      assert(allScorers.size() == twoPhaseScorers.size());
      auto flattened = pool.make_span<std::span<DocsPosEnum*>>(allScorers.size());
      size_t approximationCount = 0;
      size_t verifierCount = 0;
      bool expanded = false;
      for (size_t i = 0; i < allScorers.size(); i++) {
        bool twoPhase = enableTwoPhase && twoPhaseScorers[i] != 0;
        if (twoPhase && !disableApproxFlattenForTests) {
          flattened[i] = allScorers[i]->approximationEnums();
          expanded |= !flattened[i].empty();
        }
        approximationCount += flattened[i].empty() ? 1 : flattened[i].size();
        if (twoPhase) verifierCount++;
      }

      approximations = pool.make_span<ApproxSlot>(approximationCount);
      verifiers = pool.make_span<VerifierSlot>(verifierCount);
      size_t approximationIndex = 0;
      size_t verifierIndex = 0;
      for (size_t i = 0; i < allScorers.size(); i++) {
        Query::Scorer* scorer = allScorers[i];
        bool twoPhase = enableTwoPhase && twoPhaseScorers[i] != 0;
        if (!flattened[i].empty()) {
          for (DocsPosEnum* docsEnum : flattened[i]) {
            approximations[approximationIndex++] = {
              nullptr, docsEnum, docsEnum->numDocs(),
              ApproxSlot::Kind::DOCS_ENUM};
          }
          verifiers[verifierIndex++] = {scorer, 0, scorer->matchCost(), true};
          continue;
        }
        approximations[approximationIndex++] = {
          scorer, nullptr, allCosts[i],
          twoPhase ? ApproxSlot::Kind::TWO_PHASE
                   : ApproxSlot::Kind::SINGLE_PHASE};
      }
      assert(approximationIndex == approximations.size());
      if (expanded) {
        std::stable_sort(approximations.begin(), approximations.end(),
                         [](const ApproxSlot& a, const ApproxSlot& b) {
                           return a.cost < b.cost;
                         });
      }
      for (size_t i = 0; i < approximations.size(); i++) {
        if (approximations[i].kind == ApproxSlot::Kind::TWO_PHASE) {
          Query::Scorer* scorer = approximations[i].scorer;
          verifiers[verifierIndex++] = {scorer, i, scorer->matchCost(), false};
        }
      }
      assert(verifierIndex == verifiers.size());
      std::sort(verifiers.begin(), verifiers.end(),
                [](const VerifierSlot& a, const VerifierSlot& b) {
                  return a.matchCost < b.matchCost;
                });
      if (!competitivePruning
          && !disableExactDirectApproximationsForTests) {
        auto kinds =
            pool.make_span<ExactApproxKind>(approximations.size());
        bool supported = true;
        for (size_t i = 0; i < approximations.size(); i++) {
          const ApproxSlot& approximation = approximations[i];
          if (approximation.kind == ApproxSlot::Kind::DOCS_ENUM) {
            kinds[i] = ExactApproxKind::DOCS_ENUM;
          } else if (approximation.kind == ApproxSlot::Kind::SINGLE_PHASE
                     && dynamic_cast<TermQuery::Scorer*>(
                            approximation.scorer) != nullptr) {
            kinds[i] = ExactApproxKind::TERM_SCORER;
          } else if (approximation.kind == ApproxSlot::Kind::SINGLE_PHASE
                     && dynamic_cast<QueryPrep::DocSetScorer*>(
                            approximation.scorer) != nullptr) {
            kinds[i] = ExactApproxKind::DOC_SET;
          } else {
            supported = false;
            break;
          }
        }
        if (supported) {
          exactApproxKinds = kinds;
          skipCount(SkipStats::conjExactDirectApproxEngagements);
        }
      }
    }

    int32_t next() override {
      assert(docid != solux::PostingsReader::END);
      if (!exactApproxKinds.empty()) {
        return doNextExact(exactNext(0));
      }
      return doNext(leadTo(approximations[0].next()));
    }

    int32_t advance(int32_t docid) override {
      if (!exactApproxKinds.empty()) {
        return doNextExact(exactAdvance(0, docid));
      }
      return doNext(leadTo(approximations[0].advance(docid)));
    }

    /// doc we are positioned on
    int32_t docId() override {
      return docid;
    }

    float score() override {
      float score = 0.0f;
      for (auto* scorer: scorers) {
        assert(scorer->docId() == docid);
        score += scorer->score();
      }
      return score;
    }

    std::span<Query::Scorer*> flatConjunctionScorers() override {
      return conjunctionClauses;
    }

    void setMinCompetitiveScore(float minScore) override {
      // The threshold applies to the conjunction's SUM; children must not see
      // it when multiple scoring children contribute (a child pruning on its
      // own score alone would drop docs whose sum is competitive). With one
      // scoring child its score is the sum, so forwarding is exact. `scorers`
      // excludes every non-scoring required/filter member by construction.
      if (scorers.size() == 1) {
        scorers[0]->setMinCompetitiveScore(minScore);
      }
      if (minScore > minCompetitiveScore) {
        failStreak = 0;
      }
      minCompetitiveScore = minScore;
      competitiveUpTo = -1;  // re-evaluate block ranges under the higher threshold
    }

    // Bounds for a parent compound scorer: sum of clause bounds over the range
    // (infinity propagates from clauses without impact data).
    float getMaxScore(int32_t upTo) override {
      float sum = 0.0f;
      for (auto* scorer : scorers) {
        sum += scorer->getMaxScore(upTo);
      }
      return sum;
    }

    float refineMaxScore(int32_t upTo) override {
      float sum = 0.0f;
      for (auto* scorer : scorers) {
        sum += scorer->refineMaxScore(upTo);
      }
      return sum;
    }

    ScoreBounds getScoreBounds(int32_t upTo) override {
      return sumRequiredScoreBounds(scorers, upTo, false);
    }

    ScoreBounds refineScoreBounds(int32_t upTo) override {
      return sumRequiredScoreBounds(scorers, upTo, true);
    }

    int32_t advanceShallow(int32_t target) override {
      int32_t upTo = solux::PostingsReader::END;
      for (auto* scorer : scorers) {
        upTo = std::min(upTo, scorer->advanceShallow(target));
      }
      return upTo;
    }

    int64_t skippedRanges() const {
      return skippedRangeCount;
    }
  }; // ConjuctionScorer

  struct PostingsCandidateBatch {
    int32_t size;
    int32_t nextDoc;
  };

  // Copy sorted docs from the one-way docs-only stream. nextDoc carries the
  // first unconsumed posting because docsEnum rests on the last consumed one.
  static PostingsCandidateBatch copyPostingsCandidates(
      DocsFreqEnum& docsEnum, std::span<int32_t> outDocs,
      int32_t nextDoc, int32_t min, int32_t upTo) {
    int32_t doc = nextDoc;
    if (doc < min) {
      doc = docsEnum.docId();
      if (doc < min) {
        doc = docsEnum.advanceDocOnly(min);
      }
    }
    if (doc >= upTo) {
      return {0, doc};
    }

    int32_t size = 0;
    while (size < (int32_t) outDocs.size()) {
      std::span<const int32_t> blockDocs =
          docsEnum.peekDocOnlyBlock();
      if (blockDocs.empty()) {
        break;
      }
      assert(blockDocs.front() >= min);
      int32_t available = (int32_t) blockDocs.size();
      const int32_t* rangeEnd = screaming::gallopLowerBound(
          blockDocs.data(), blockDocs.data() + available, upTo);
      int32_t count = std::min(
          (int32_t) (rangeEnd - blockDocs.data()),
          (int32_t) outDocs.size() - size);
      if (count == 0) {
        break;
      }
      std::copy_n(
          blockDocs.begin(), count, outDocs.begin() + size);
      size += count;
      docsEnum.consumeDocOnlyBlock(count);
      if (count < available) {
        break;
      }
    }

    std::span<const int32_t> remaining =
        docsEnum.peekDocOnlyBlock();
    return {
        size,
        remaining.empty() ? PostingsReader::END : remaining.front()};
  }

  static PostingsCandidateBatch copyPostingsCandidates(
      DocsOnlyEnum& docsEnum, std::span<int32_t> outDocs,
      int32_t nextDoc, int32_t min, int32_t upTo) {
    int32_t doc = nextDoc;
    if (doc < min) {
      doc = docsEnum.docId();
      if (doc < min) {
        doc = docsEnum.advance(min);
      }
    }
    if (doc >= upTo) {
      return {0, doc};
    }

    int32_t size = 0;
    while (size < (int32_t) outDocs.size()) {
      std::span<const int32_t> blockDocs = docsEnum.peekDocBlock();
      if (blockDocs.empty()) {
        break;
      }
      assert(blockDocs.front() >= min);
      const int32_t* rangeEnd = screaming::gallopLowerBound(
          blockDocs.data(), blockDocs.data() + blockDocs.size(), upTo);
      int32_t count = std::min(
          (int32_t) (rangeEnd - blockDocs.data()),
          (int32_t) outDocs.size() - size);
      if (count == 0) {
        break;
      }
      std::copy_n(blockDocs.begin(), count, outDocs.begin() + size);
      size += count;
      docsEnum.consumeDocBlock(count);
      if (count < (int32_t) blockDocs.size()) {
        break;
      }
    }

    std::span<const int32_t> remaining = docsEnum.peekDocBlock();
    return {
        size,
        remaining.empty() ? PostingsReader::END : remaining.front()};
  }

  static void applyDomainBitsToWindow(std::span<uint64_t> windowBits,
                                      int32_t windowStart, int32_t windowEnd,
                                      const FixedBitSet* domainBits) {
    assert(domainBits != nullptr);
    int32_t domainWords = (int32_t) FixedBitSet::sizeInWords(domainBits->size());
    for (size_t w = 0; w < windowBits.size(); w++) {
      int32_t firstDoc = windowStart + (int32_t) (w << 6);
      int32_t remaining = windowEnd - firstDoc;
      if (remaining <= 0) {
        windowBits[w] = 0;
        continue;
      }
      uint64_t validMask = remaining >= 64 ? ~0ULL : (1ULL << remaining) - 1ULL;
      int32_t sourceWord = firstDoc >> 6;
      int32_t shift = firstDoc & 63;
      uint64_t domainWord = 0;
      if (sourceWord < domainWords) {
        domainWord = domainBits->words[sourceWord] >> shift;
        if (shift != 0 && sourceWord + 1 < domainWords) {
          domainWord |= domainBits->words[sourceWord + 1] << (64 - shift);
        }
      }
      windowBits[w] &= domainWord & validMask;
    }
  }

  static void applyDocSetToWindow(std::span<uint64_t> windowBits,
                                  int32_t windowStart, int32_t windowEnd,
                                  DocSet* filter) {
    int32_t innerSize = windowEnd - windowStart;
    for (int32_t word = 0; word < (int32_t) windowBits.size(); word++) {
      uint64_t bits = windowBits[(size_t) word];
      while (bits != 0) {
        int32_t bit = (int32_t) std::countr_zero(bits);
        int32_t index = (word << 6) + bit;
        if (index >= innerSize) {
          break;
        }
        if (!filter->get(windowStart + index)) {
          windowBits[(size_t) word] &= ~(1ULL << bit);
        }
        bits &= bits - 1;
      }
    }
  }

  // Exact docs-only conjunction of direct terms. Keeping this separate from
  // the scored conjunction avoids constructing frequency cursors and dense
  // scoring scratch that exact membership never uses.
  class ExactDocsOnlyTermConjunctionBulkScorer final : public BulkScorer {
    static constexpr int32_t kBatchSize = 1024;
    MemPool& pool;
    std::span<DocsOnlyEnum*> terms;
    std::span<int32_t> candidates;
    std::span<float> scores;
    int32_t maxDoc;
    int32_t nextLeadDoc = -1;

    static int32_t retainCandidates(
        DocsOnlyEnum& docsEnum, int32_t* docs, int32_t size) {
      int32_t current = docsEnum.docId();
      int32_t read = 0;
      int32_t write = 0;
      while (read < size) {
        if (current > docs[read]) {
          read = (int32_t) (screaming::gallopLowerBound(
              docs + read, docs + size, current) - docs);
          if (read == size) {
            break;
          }
        }
        int32_t target = docs[read];
        if (current < target) {
          current = docsEnum.advance(target);
        }
        if (current == target) {
          docs[write++] = target;
        }
        read++;
      }
      return write;
    }

    PostingsCandidateBatch nextBatch(DocSet* filter,
                                     int32_t min, int32_t max) {
      skipCount(SkipStats::exactTermCountBatches);
      PostingsCandidateBatch batch = copyPostingsCandidates(
          *terms[0], candidates, nextLeadDoc, min, max);
      nextLeadDoc = batch.nextDoc;
      int32_t size = batch.size;
      if (filter != nullptr) {
        int32_t write = 0;
        for (int32_t i = 0; i < size; i++) {
          int32_t doc = candidates[(size_t) i];
          if (filter->get(doc)) {
            candidates[(size_t) write++] = doc;
          }
        }
        size = write;
      }
      for (size_t clause = 1; clause < terms.size() && size > 0; clause++) {
        size = retainCandidates(*terms[clause], candidates.data(), size);
      }
      batch.size = size;
      return batch;
    }

  public:
    ExactDocsOnlyTermConjunctionBulkScorer(
        MemPool& pool, std::span<DocsOnlyEnum*> terms,
        int32_t maxDoc)
        : pool(pool),
          terms(terms),
          candidates(pool.make_span<int32_t>(kBatchSize)),
          maxDoc(maxDoc) {
      assert(terms.size() >= 3);
      skipCount(SkipStats::exactTermCountEngagements);
    }

    bool supportsMatchWindows() const override {
      return true;
    }

    int32_t scoreNextWindow(ScoreWindow& out, DocSet* filter,
                            int32_t min, int32_t max,
                            float minCompetitiveScore) override {
      unused(minCompetitiveScore);
      int32_t next = matchNextWindow(out, filter, min, max);
      if (scores.empty()) {
        scores = pool.make_span<float>(kBatchSize);
      }
      std::fill_n(scores.begin(), out.size, 0.0f);
      out.scores = scores;
      return next;
    }

    int32_t matchNextWindow(ScoreWindow& out, DocSet* filter,
                            int32_t min, int32_t max) override {
      max = std::min(max, maxDoc);
      out.min = min;
      out.max = max;
      out.docs = candidates;
      out.scores = scores;
      if (min >= max || (filter != nullptr && filter->card() == 0)) {
        out.size = 0;
        return PostingsReader::END;
      }
      PostingsCandidateBatch batch = nextBatch(filter, min, max);
      out.max = std::min(max, batch.nextDoc);
      out.size = batch.size;
      return batch.nextDoc;
    }

    int32_t countNextWindow(int64_t& count, DocSetBuilder* domainOut,
                            DocSet* filter, int32_t min, int32_t max) override {
      max = std::min(max, maxDoc);
      if (min >= max || (filter != nullptr && filter->card() == 0)) {
        return PostingsReader::END;
      }
      if (domainOut != nullptr) {
        skipCount(SkipStats::bulkDomainWindowsFed);
      }
      PostingsCandidateBatch batch = nextBatch(filter, min, max);
      count += batch.size;
      if (domainOut != nullptr) {
        for (int32_t i = 0; i < batch.size; i++) {
          domainOut->add(candidates[(size_t) i]);
        }
      }
      return batch.nextDoc;
    }
  };

  // Bulk execution for pure conjunctions (every clause required, no two-phase
  // members; required-but-non-scoring clauses carry zero score and bounds):
  // windows anchor on the lead's current doc,
  // whole windows are skipped when the summed clause bounds cannot reach the
  // threshold (Lucene BlockMaxConjunctionBulkScorer shape), and surviving
  // windows intersect the lead's decoded blocks against the other clauses in
  // candidate batches - one tight pass per clause with early candidate
  // abandonment, instead of a per-doc virtual leapfrog.
  class ConjunctionBulkScorer final : public BulkScorer {
  public:
    // Dense count windows engage when the lead matches at least
    // maxDoc/kDenseThresholdInverse docs. Word-encoded block fills remain
    // useful at lower lead densities than scalar clause leapfrogging.
    static constexpr int32_t kDenseThresholdInverse = 512;
    static constexpr int32_t kDenseLeapfrogThreshold =
        DocsEnumMeta::L1_DOCS / 32;
    // A DocSet first fill is not enough work to amortize term leapfrogging.
    // Keep filling through the first query clause, then apply the ordinary
    // intermediate-cardinality crossover.
    static constexpr int32_t kDocSetLeadLeapfrogThreshold = 1;
    // Disabled because scalar per-survivor advances cost more than streaming
    // term tail fills. The environment override is a test hook for changes
    // that reduce the per-probe cost.
    static constexpr int32_t kTermLeadLeapfrogThreshold = 0;
    // Direct-term and DocSet tails switch to sparse lead iteration while the
    // lead is still denser than the disjunction-group crossover because their
    // sparse path avoids group construction.
    static constexpr int32_t kTermTailDenseThresholdInverse = 128;
    // Bound filtered-conjunction candidate buffers while amortizing clause
    // cursor setup. Count and scored routes share the same production batch.
    static constexpr int32_t kFilteredConjunctionBatchSize = 1024;
    static constexpr int32_t kCandidateCountSampleSize = 256;
    static constexpr int32_t kIntegratedCountMinClauses = 3;
    // Exact filtered COUNT samples 256 candidates before replacing the dense
    // route, then uses the full batch after admission. Candidate probing wins
    // when the call-time domain and first tail membership together remove
    // nearly all of the raw postings feed; otherwise direct window fills
    // retain better locality. The 5M
    // intersection corpus crosses over between 4.5% and 8.7% survivors;
    // 1/14 admits the measured middle band while retaining margin before the
    // first dense winner.
    static constexpr int32_t kCandidateCountFirstTailDensityInverse = 14;
    static inline int32_t denseThresholdInverseForTests = [] {
      const char* value = std::getenv("SOLUX_DENSE_THRESHOLD_INVERSE");
      return value != nullptr ? std::atoi(value) : kDenseThresholdInverse;
    }();
    static inline int32_t denseLeapfrogThresholdForTests = [] {
      const char* value = std::getenv("SOLUX_DENSE_LEAPFROG_THRESHOLD");
      return value != nullptr ? std::atoi(value) : kDenseLeapfrogThreshold;
    }();
    static inline int32_t docSetLeadLeapfrogThresholdForTests =
        kDocSetLeadLeapfrogThreshold;
    static inline int32_t termLeadLeapfrogThresholdForTests = [] {
      const char* value =
          std::getenv("SOLUX_TERM_LEAD_LEAPFROG_THRESHOLD");
      return value != nullptr ? std::atoi(value) : kTermLeadLeapfrogThreshold;
    }();
    static inline int32_t termTailDenseThresholdInverseForTests = [] {
      const char* value =
          std::getenv("SOLUX_TERM_TAIL_DENSE_THRESHOLD_INVERSE");
      return value != nullptr
          ? std::atoi(value) : kTermTailDenseThresholdInverse;
    }();
    static inline int32_t candidateCountFirstTailDensityInverseForTests = [] {
      const char* value =
          std::getenv("SOLUX_CANDIDATE_COUNT_SURVIVOR_INVERSE");
      return value != nullptr
          ? std::atoi(value) : kCandidateCountFirstTailDensityInverse;
    }();
    // The generic lead fill consumes one postings block at a time.
    static inline int32_t filteredConjunctionBatchSizeForTests =
        kFilteredConjunctionBatchSize;
    static inline bool disableDisjGroupBulkForTests = false;
    static inline bool disableDenseScoredForTests = false;
    static inline bool disableScoredProbeForTests = false;
    static inline bool disableBatchBoundForTests = false;
    static inline bool disableExactScoredBoundsBypassForTests = false;
    static inline bool disableDirectDenseClausesForTests = false;
    static inline bool disableNegatedCountForTests = false;
    static inline bool disableCandidateCountRetainForTests =
        std::getenv("SOLUX_DISABLE_CANDIDATE_COUNT_RETAIN") != nullptr;

  private:
    static constexpr int32_t kChunk = Postings::DOCS_BLOCK_SIZE;
    static constexpr int32_t kWindowSize = DocsEnumMeta::L1_DOCS;
    static constexpr int32_t kWindowWords = kWindowSize / 64;
    static_assert((kWindowSize % 64) == 0);

    struct TermClause {
      std::span<Query::Scorer*> members;
    };

    struct DenseClause {
      std::span<Query::Scorer*> members;
      TermQuery::Scorer* term = nullptr;
      QueryPrep::DocSetScorer* docSet = nullptr;
    };

    struct BufferedTerm {
      TermQuery::Scorer* scorer = nullptr;
      std::span<int32_t> docs;
      std::span<float> scores;
      int32_t index = 0;
      int32_t size = 0;
    };

    std::span<Query::Scorer*> scorers;  // ascending cost; scorers[0] leads
    std::span<Query::Scorer*> prohibitedScorers;
    std::span<TermQuery::Scorer*> candidateProhibitedTerms;
    std::span<const uint8_t> scoringClauses;
    std::span<TermQuery::Scorer*> termScorers; // populated when every scorer is a term
    std::span<TermClause> termClauses;  // direct terms or decomposed flat unions
    std::span<DenseClause> denseClauses; // exact window-fill capable clauses
    std::span<DenseClause> prohibitedDenseClauses;
    std::span<uint8_t> prohibitedExhausted;
    std::span<float> windowMax;         // per-clause bound over the current window
    std::span<double> suffixMax;        // suffixMax[c] = sum of windowMax[c..n)
    std::span<int32_t> candDocs;
    std::span<float> candScores;
    std::span<int32_t> boundDrops;
    std::span<int32_t> outDocs;
    std::span<float> outScores;
    std::span<uint64_t> windowBits;
    std::span<uint64_t> clauseBits;
    std::span<int32_t> denseFreqs;
    std::span<uint64_t> groupMatchBits;
    std::span<float> groupScores;
    std::span<BufferedTerm> leadGroupTerms;
    WindowFilter* windowFilter = nullptr;
    int32_t maxDoc;
    float minCompetitiveScore = std::numeric_limits<float>::lowest();
    double scoreBoundFactor = 1.0;
    int64_t skippedWindowCount = 0;
    int32_t candidateBatchSize;
    bool allTermScorers = false;
    bool allTermClauses = false;
    bool allDenseClauses = false;
    bool hasDisjGroup = false;
    bool denseHasDisjGroup = false;
    bool negatedCountPath = false;
    bool denseCountPath = false;
    bool countLeadIsDocSet = false;
    bool termTailScorers = false;
    bool sparseEligibleTails = false;
    bool denseScoredEligible = false;
    bool denseScoredCostRejected = false;
    bool competitivePruning = true;
    const uint8_t* denseScoredNorms = nullptr;
    TermQuery::Scorer* candidatePostingsLead = nullptr;
    TermQuery::Scorer* candidateLeadScoreScorer = nullptr;
    std::span<uint8_t> candidateFilters;
    std::span<DocSet*> candidateFilterDocSets;
    // Forward membership cursors over the DocSet filter clauses: candidates
    // ascend across batches and windows, so galloping probes replace the
    // per-candidate virtual get() (a from-scratch binary search on ARRAY
    // sets). One cursor per clause, private to this scorer's single scan.
    std::span<DocSetProbe> candidateFilterProbes;
    int32_t nextCandidatePostingsLeadDoc = -1;

    enum class CandidateCountAdmission : uint8_t {
      NONE,
      SAMPLING,
      ADMITTED,
      DENSE
    };
    CandidateCountAdmission candidateCountAdmission =
        CandidateCountAdmission::NONE;

    enum class DenseScoredAdmission : uint8_t {
      SCORE_FIRST,
      SAMPLING,
      ADMITTED
    };
    DenseScoredAdmission denseScoredAdmission =
        DenseScoredAdmission::SCORE_FIRST;
    int32_t denseSampleWindows = 0;
    int64_t denseSampleLead = 0;
    int64_t denseSampleSurvivors = 0;
    int32_t denseScoredTopK = 0;

    // Dense-scored admission considers requested depth, an initial survivor
    // sample, and non-lead cost. Keep the parameters together.
    static constexpr int32_t kDenseScoredMinTopK = 100;
    static constexpr int32_t kDenseAdmissionSampleWindows = 8;
    static constexpr int32_t kDenseAdmissionMinLeadPerWindow = 32;
    static constexpr int32_t kDenseAdmissionMaxNonLeadCostRatio = 8;
    // Minimum filter density (maxDoc/this) for the dense scored path to stay
    // eligible when a window filter is attached; see attachWindowFilter.
    static constexpr int64_t kDenseScoredMinFilterDensityInverse = 16;
    static constexpr int32_t kDenseAdmissionMaxDensityTopK = 1000;
    static constexpr int32_t kDenseAdmissionMinDensityPercent = 10;
    static constexpr int32_t kDenseAdmissionMaxDensityPercent = 60;
    static constexpr int32_t kDenseAdmissionDensityTopKSpan =
        kDenseAdmissionMaxDensityTopK - kDenseScoredMinTopK;
    static constexpr int32_t kDenseAdmissionDensityDenominator =
        100 * kDenseAdmissionDensityTopKSpan;

    static int32_t denseAdmissionDensityNumerator(int32_t topK) {
      int32_t clampedTopK = std::clamp(
          topK, kDenseScoredMinTopK, kDenseAdmissionMaxDensityTopK);
      return kDenseAdmissionMinDensityPercent * kDenseAdmissionDensityTopKSpan
          + (clampedTopK - kDenseScoredMinTopK)
              * (kDenseAdmissionMaxDensityPercent
                  - kDenseAdmissionMinDensityPercent);
    }

    bool acceptsDoc(DocSet* filter, int32_t doc) {
      if (filter != nullptr && !filter->get(doc)) {
        return false;
      }
      return windowFilter == nullptr
          || (windowFilter->probes() ? windowFilter->acceptsProbe(doc)
                                     : windowFilter->accepts(doc));
    }

    TermQuery::Scorer* termClauseMember(size_t clause, size_t member) {
      return static_cast<TermQuery::Scorer*>(
          termClauses[clause].members[member]);
    }

    int32_t termClauseDocId(size_t clause) {
      int32_t doc = PostingsReader::END;
      for (size_t member = 0; member < termClauses[clause].members.size(); member++) {
        doc = std::min(doc, termClauseMember(clause, member)->docsEnum.docId());
      }
      return doc;
    }

    int32_t denseClauseDocId(const DenseClause& clause) {
      if (clause.term != nullptr) {
        return clause.term->docsEnum.docId();
      }
      if (clause.docSet != nullptr) {
        return clause.docSet->docId();
      }
      int32_t doc = PostingsReader::END;
      for (Query::Scorer* member : clause.members) {
        doc = std::min(doc, member->docId());
      }
      return doc;
    }

    int32_t denseClauseCountAdvance(
        const DenseClause& clause, int32_t target) {
      if (clause.term != nullptr) {
        return clause.term->docsEnum.advanceDocOnly(target);
      }
      if (clause.docSet != nullptr) {
        return clause.docSet->advance(target);
      }
      int32_t doc = PostingsReader::END;
      for (Query::Scorer* member : clause.members) {
        DocsFreqEnum* probe = member->windowFilterProbeDocsEnum();
        int32_t memberDoc = probe != nullptr ? probe->docId() : member->docId();
        if (memberDoc < target) {
          memberDoc = probe != nullptr
              ? probe->advanceDocOnly(target)
              : member->advance(target);
        }
        doc = std::min(doc, memberDoc);
      }
      return doc;
    }

    void fillDenseClauseWindowBits(const DenseClause& clause,
                                   std::span<uint64_t> bits,
                                   int32_t windowBase, int32_t windowEnd) {
      if (clause.term != nullptr) {
        clause.term->fillWindowBits(bits, windowBase, windowEnd);
        return;
      }
      if (clause.docSet != nullptr) {
        clause.docSet->fillWindowBits(bits, windowBase, windowEnd);
        return;
      }
      for (Query::Scorer* member : clause.members) {
        member->fillWindowBits(bits, windowBase, windowEnd);
      }
    }

    void applyProhibitedWindowBits(int32_t windowBase, int32_t windowEnd) {
      if (!negatedCountPath) {
        return;
      }
      skipCount(SkipStats::negatedCountWindows);
      for (size_t clause = 0; clause < prohibitedDenseClauses.size(); clause++) {
        if (prohibitedExhausted[clause] != 0) {
          continue;
        }
        const DenseClause& denseClause = prohibitedDenseClauses[clause];
        int32_t doc = denseClauseDocId(denseClause);
        if (doc < windowBase) {
          doc = denseClauseCountAdvance(denseClause, windowBase);
        }
        if (doc == PostingsReader::END) {
          prohibitedExhausted[clause] = 1;
          continue;
        }
        if (doc >= windowEnd) {
          continue;
        }

        clearWindowBits(clauseBits);
        fillDenseClauseWindowBits(
            denseClause, clauseBits, windowBase, windowEnd);
        skipCount(SkipStats::negatedCountExclFills);
        for (int32_t word = 0; word < kWindowWords; word++) {
          windowBits[(size_t) word] &= ~clauseBits[(size_t) word];
        }
        if (denseClauseDocId(denseClause) == PostingsReader::END) {
          prohibitedExhausted[clause] = 1;
        }
      }
    }

    int32_t termClauseAdvanceShallow(size_t clause, int32_t target) {
      int32_t upTo = PostingsReader::END;
      for (size_t member = 0; member < termClauses[clause].members.size(); member++) {
        upTo = std::min(upTo,
            termClauseMember(clause, member)->advanceShallow(target));
      }
      return upTo;
    }

    float termClauseGetMaxScore(size_t clause, int32_t upTo) {
      float sum = 0.0f;
      bool disjunction = termClauses[clause].members.size() > 1;
      for (size_t member = 0; member < termClauses[clause].members.size(); member++) {
        float bound = termClauseMember(clause, member)->getMaxScore(upTo);
        if (!std::isfinite(bound)) return std::numeric_limits<float>::infinity();
        sum += disjunction ? optionalUpperBound(bound) : bound;
      }
      return sum;
    }

    int32_t leadTermGroupDocId() {
      int32_t doc = PostingsReader::END;
      for (auto& term : leadGroupTerms) {
        int32_t termDoc = term.index < term.size
          ? term.docs[(size_t) term.index]
          : term.scorer->docsEnum.docId();
        doc = std::min(doc, termDoc);
      }
      return doc;
    }

    int32_t advanceLeadTermGroup(int32_t target) {
      int32_t doc = PostingsReader::END;
      for (auto& term : leadGroupTerms) {
        while (term.index < term.size && term.docs[(size_t) term.index] < target) {
          term.index++;
        }
        int32_t termDoc;
        if (term.index < term.size) {
          termDoc = term.docs[(size_t) term.index];
        } else {
          term.index = 0;
          term.size = 0;
          termDoc = term.scorer->docsEnum.docId();
          if (termDoc < target) {
            termDoc = term.scorer->advance(target);
          }
        }
        doc = std::min(doc, termDoc);
      }
      return doc;
    }

    int32_t refillLeadGroupTerm(BufferedTerm& term, int32_t upTo) {
      if (term.index < term.size) return term.docs[(size_t) term.index];
      term.index = 0;
      term.size = term.scorer->fillScoreBlock(
          term.docs.data(), term.scores.data(), kChunk, upTo);
      return term.size == 0 ? PostingsReader::END : term.docs[0];
    }

    int32_t fillLeadTermGroupScoreBlock(int32_t* docs, float* scores,
                                        int32_t count, int32_t upTo) {
      int32_t filled = 0;
      while (filled < count) {
        int32_t doc = PostingsReader::END;
        for (auto& term : leadGroupTerms) {
          doc = std::min(doc, refillLeadGroupTerm(term, upTo));
        }
        if (doc >= upTo) break;

        float score = 0.0f;
        for (auto& term : leadGroupTerms) {
          if (term.index < term.size
              && term.docs[(size_t) term.index] == doc) {
            score += term.scores[(size_t) term.index];
            term.index++;
          }
        }
        docs[filled] = doc;
        scores[filled] = score;
        filled++;
      }
      return filled;
    }

    int32_t fillTermClauseLeadScoreBlock(int32_t* docs, float* scores,
                                         int32_t count, int32_t upTo) {
      if (leadGroupTerms.empty()) {
        return termClauseMember(0, 0)->fillScoreBlock(
            docs, scores, count, upTo);
      }
      return fillLeadTermGroupScoreBlock(docs, scores, count, upTo);
    }

    int32_t termClauseScoreDocId(size_t clause) {
      return clause == 0 && !leadGroupTerms.empty()
        ? leadTermGroupDocId()
        : termClauseDocId(clause);
    }

    int32_t termClauseScoreAdvance(size_t clause, int32_t target) {
      if (clause == 0 && !leadGroupTerms.empty()) {
        return advanceLeadTermGroup(target);
      }
      int32_t doc = PostingsReader::END;
      for (size_t member = 0; member < termClauses[clause].members.size(); member++) {
        auto* scorer = termClauseMember(clause, member);
        int32_t memberDoc = scorer->docId();
        if (memberDoc < target) memberDoc = scorer->advance(target);
        doc = std::min(doc, memberDoc);
      }
      return doc;
    }

    int32_t compactByRemainingBound(int32_t size, double remaining) {
      int32_t write = 0;
      for (int32_t i = 0; i < size; i++) {
        if (((double) candScores[(size_t) i] + remaining) * scoreBoundFactor
            < (double) minCompetitiveScore) {
          continue;
        }
        if (write != i) {
          candDocs[(size_t) write] = candDocs[(size_t) i];
          candScores[(size_t) write] = candScores[(size_t) i];
        }
        write++;
      }
      return write;
    }

    bool remainingBoundCanDrop(double remaining) const {
      return minCompetitiveScore > 0.0f
          && remaining * scoreBoundFactor < (double) minCompetitiveScore;
    }

    int32_t batchCompactByRemainingBound(
        int32_t size, double remaining) SOLUX_INLINE {
      for (int32_t i = 0; i < size; i++) {
        boundDrops[(size_t) i] =
            (((double) candScores[(size_t) i] + remaining) * scoreBoundFactor
             < (double) minCompetitiveScore);
      }

      int32_t write = 0;
      for (int32_t i = 0; i < size; i++) {
        if (boundDrops[(size_t) i] != 0) continue;
        if (write != i) {
          candDocs[(size_t) write] = candDocs[(size_t) i];
          candScores[(size_t) write] = candScores[(size_t) i];
        }
        write++;
      }
      return write;
    }

    int32_t applyTermGroupToCandidates(size_t clause, int32_t size) {
      size_t words = ((size_t) size + 63) >> 6;
      std::fill(groupMatchBits.begin(), groupMatchBits.begin() + (ptrdiff_t) words, 0);
      std::fill(groupScores.begin(), groupScores.begin() + size, 0.0f);
      auto matches = groupMatchBits.first(words);
      for (size_t member = 0; member < termClauses[clause].members.size(); member++) {
        termClauseMember(clause, member)->addToCandidates(
            candDocs.data(), groupScores.data(), size, matches);
      }
      int32_t write = 0;
      for (int32_t i = 0; i < size; i++) {
        if ((matches[(size_t) (i >> 6)] & (1ULL << (i & 63))) == 0) continue;
        candDocs[(size_t) write] = candDocs[(size_t) i];
        candScores[(size_t) write] = candScores[(size_t) i] + groupScores[(size_t) i];
        write++;
      }
      return write;
    }

    template <bool TermFast, bool TermTailFast = false>
    int32_t scorerDocId(size_t index) {
      if constexpr (TermFast) {
        return termScorers[index]->docsEnum.docId();
      } else if constexpr (TermTailFast) {
        return index == 0 ? scorers[0]->docId()
                          : termScorers[index]->docsEnum.docId();
      } else {
        return scorers[index]->docId();
      }
    }

    template <bool TermFast, bool TermTailFast = false>
    int32_t scorerAdvance(size_t index, int32_t target) {
      if constexpr (TermFast) {
        return termScorers[index]->docsEnum.advance(target);
      } else if constexpr (TermTailFast) {
        return index == 0 ? scorers[0]->advance(target)
                          : termScorers[index]->docsEnum.advance(target);
      } else {
        return scorers[index]->advance(target);
      }
    }

    template <bool TermFast, bool TermTailFast = false>
    int32_t scorerAdvanceForClauseProbe(size_t index, int32_t target) {
      if constexpr (TermFast) {
        return disableScoredProbeForTests
            ? termScorers[index]->docsEnum.advance(target)
            : termScorers[index]->advanceScoredProbe(target);
      } else if constexpr (TermTailFast) {
        assert(index > 0);
        return disableScoredProbeForTests
            ? termScorers[index]->docsEnum.advance(target)
            : termScorers[index]->advanceScoredProbe(target);
      } else {
        return scorers[index]->advance(target);
      }
    }

    template <bool TermFast, bool TermTailFast = false>
    int32_t scorerAdvanceLead(int32_t target) {
      const int64_t before =
          SkipStats::enabled ? SkipStats::tfreqBlocksDecoded : 0;
      int32_t doc = scorerAdvance<TermFast, TermTailFast>(0, target);
      if (SkipStats::enabled) {
        SkipStats::conjScoredLeadFreqDecodes +=
            SkipStats::tfreqBlocksDecoded - before;
      }
      return doc;
    }

    template <bool TermFast, bool TermTailFast = false>
    int32_t scorerCountDocId(size_t index) {
      if constexpr (TermFast) {
        return termScorers[index]->docsEnum.docId();
      } else if constexpr (TermTailFast) {
        return index == 0 ? scorers[0]->docId()
                          : termScorers[index]->docsEnum.docId();
      } else {
        // Mirror scorerCountAdvance: term clauses in a mixed shape answer
        // from the docs-only enum directly.
        if (termScorers[index] != nullptr) {
          return termScorers[index]->docsEnum.docId();
        }
        return scorers[index]->docId();
      }
    }

    template <bool TermFast, bool TermTailFast = false>
    int32_t scorerCountAdvance(size_t index, int32_t target) {
      if constexpr (TermFast) {
        return termScorers[index]->docsEnum.advanceDocOnly(target);
      } else if constexpr (TermTailFast) {
        return index == 0
            ? scorers[0]->advance(target)
            : termScorers[index]->docsEnum.advanceDocOnly(target);
      } else {
        // A DocSet clause makes allTermScorers false, but it must not demote
        // the remaining terms to generic Scorer::advance (which can decode
        // frequencies). Preserve the docs-only word-probe route per clause.
        if (termScorers[index] != nullptr) {
          return termScorers[index]->docsEnum.advanceDocOnly(target);
        }
        return scorers[index]->advance(target);
      }
    }

    template <bool TermFast, bool TermTailFast = false>
    int32_t scorerAdvanceShallow(size_t index, int32_t target) {
      if constexpr (TermFast) {
        return termScorers[index]->advanceShallow(target);
      } else if constexpr (TermTailFast) {
        return index == 0 ? scorers[0]->advanceShallow(target)
                          : termScorers[index]->advanceShallow(target);
      } else {
        return scorers[index]->advanceShallow(target);
      }
    }

    template <bool TermFast, bool TermTailFast = false>
    float scorerGetMaxScore(size_t index, int32_t upTo) {
      if constexpr (TermFast) {
        return termScorers[index]->getMaxScore(upTo);
      } else if constexpr (TermTailFast) {
        return index == 0 ? scorers[0]->getMaxScore(upTo)
                          : termScorers[index]->getMaxScore(upTo);
      } else {
        return scorers[index]->getMaxScore(upTo);
      }
    }

    template <bool TermFast>
    int32_t scorerFillScoreBlock(size_t index, int32_t* docs, float* scores,
                                 int32_t count, int32_t upTo) {
      if constexpr (TermFast) {
        return termScorers[index]->fillScoreBlock(docs, scores, count, upTo);
      } else {
        return scorers[index]->fillScoreBlock(docs, scores, count, upTo);
      }
    }

    template <bool TermFast, bool TermTailFast = false>
    float scorerScore(size_t index) {
      if constexpr (TermFast) {
        return termScorers[index]->score();
      } else if constexpr (TermTailFast) {
        return index == 0 ? scorers[0]->score()
                          : termScorers[index]->score();
      } else {
        return scorers[index]->score();
      }
    }

    template <bool CheckBound>
    int32_t SOLUX_INLINE applyScoredProbeToCandidates(
        TermQuery::Scorer* scorer, int32_t size,
        double remaining) {
      int32_t write = 0;
      for (int32_t i = 0; i < size; i++) {
        int32_t doc = candDocs[(size_t) i];
        float sum = candScores[(size_t) i];
        if constexpr (CheckBound) {
          if (((double) sum + remaining) * scoreBoundFactor
              < (double) minCompetitiveScore) {
            continue;
          }
        }
        float clauseScore;
        if (!scorer->matchScoredProbe(doc, clauseScore)) continue;
        candDocs[(size_t) write] = doc;
        candScores[(size_t) write] = sum + clauseScore;
        write++;
      }
      return write;
    }

    template <bool CheckBound, bool TermFast, bool TermTailFast = false>
    int32_t SOLUX_INLINE applyScorerToCandidates(
        size_t clause, int32_t size, double remaining) {
      int32_t scorerDoc = scorerDocId<TermFast, TermTailFast>(clause);
      int32_t write = 0;
      for (int32_t i = 0; i < size; i++) {
        int32_t doc = candDocs[(size_t) i];
        float sum = candScores[(size_t) i];
        if constexpr (CheckBound) {
          if (((double) sum + remaining) * scoreBoundFactor
              < (double) minCompetitiveScore) {
            continue;
          }
        }
        if (scorerDoc < doc) {
          scorerDoc = scorerAdvanceForClauseProbe<TermFast, TermTailFast>(
              clause, doc);
        }
        if (scorerDoc != doc) continue;
        candDocs[(size_t) write] = doc;
        candScores[(size_t) write] =
            sum + scorerScore<TermFast, TermTailFast>(clause);
        write++;
      }
      return write;
    }

    int32_t advanceCandidatePostingsLead(int32_t target) {
      int32_t doc = nextCandidatePostingsLeadDoc;
      if (doc < target) {
        auto& docsEnum = candidatePostingsLead->docsEnum;
        doc = docsEnum.docId();
        if (doc < target) {
          doc = docsEnum.advanceDocOnly(target);
        }
        nextCandidatePostingsLeadDoc = doc;
      }
      return doc;
    }

    int32_t gatherCandidatePostingsLead(int32_t min, int32_t upTo) {
      PostingsCandidateBatch batch = copyPostingsCandidates(
          candidatePostingsLead->docsEnum, candDocs,
          nextCandidatePostingsLeadDoc, min, upTo);
      nextCandidatePostingsLeadDoc = batch.nextDoc;
      std::fill(
          candScores.begin(), candScores.begin() + batch.size, 0.0f);
      return batch.size;
    }

    int32_t compactCandidateFilters(int32_t size, DocSet* filter) {
      int32_t write = 0;
      for (int32_t i = 0; i < size; i++) {
        int32_t doc = candDocs[(size_t) i];
        if (filter != nullptr && !filter->get(doc)) {
          continue;
        }
        if (windowFilter != nullptr
            && (windowFilter->probes()
                    ? !windowFilter->acceptsProbe(doc)
                    : !windowFilter->accepts(doc))) {
          continue;
        }
        candDocs[(size_t) write++] = doc;
      }
      size = write;

      assert(candidateFilters.size() == scorers.size());
      assert(candidateFilterDocSets.size() == scorers.size());
      for (size_t clause = 0; clause < scorers.size() && size > 0;
           clause++) {
        if (candidateFilters[clause] == 0) {
          continue;
        }
        DocSet* docSet = candidateFilterDocSets[clause];
        TermQuery::Scorer* term = termScorers[clause];
        assert(docSet != nullptr || term != nullptr);
        write = 0;
        for (int32_t i = 0; i < size; i++) {
          int32_t doc = candDocs[(size_t) i];
          bool accepted;
          if (docSet != nullptr) {
            accepted = candidateFilterProbes[clause].get(doc);
          } else {
            int32_t filterDoc = term->docsEnum.docId();
            if (filterDoc < doc) {
              filterDoc = term->docsEnum.advanceDocOnly(doc);
            }
            accepted = filterDoc == doc;
          }
          if (accepted) {
            candDocs[(size_t) write++] = doc;
          }
        }
        size = write;
      }
      return size;
    }

    int32_t compactCandidateProhibitedTerms(int32_t size) {
      if (candidateProhibitedTerms.empty() || size == 0) {
        return size;
      }
      size_t words = ((size_t) size + 63) >> 6;
      std::fill(clauseBits.begin(), clauseBits.begin() + (ptrdiff_t) words, 0);
      auto matches = clauseBits.first(words);
      for (auto* term : candidateProhibitedTerms) {
        term->addMatchesToCandidates(candDocs.data(), size, matches);
      }
      int32_t write = 0;
      for (int32_t i = 0; i < size; i++) {
        if ((matches[(size_t) (i >> 6)] & (1ULL << (i & 63))) != 0) {
          continue;
        }
        candDocs[(size_t) write] = candDocs[(size_t) i];
        candScores[(size_t) write] = candScores[(size_t) i];
        write++;
      }
      return write;
    }

    int32_t compactCompetitiveCandidates(int32_t size) {
      if (minCompetitiveScore == std::numeric_limits<float>::lowest()) {
        return size;
      }
      int32_t write = 0;
      for (int32_t i = 0; i < size; i++) {
        if (candScores[(size_t) i] < minCompetitiveScore) {
          continue;
        }
        candDocs[(size_t) write] = candDocs[(size_t) i];
        candScores[(size_t) write] = candScores[(size_t) i];
        write++;
      }
      return write;
    }

    template <bool TermFast, bool TermTailFast = false,
              bool CandidatePostingsFeed = false,
              bool CandidateTermFeed = false>
    int32_t scoreNextWindowImpl(ScoreWindow& out, DocSet* filter, int32_t min, int32_t max,
                                float minCompetitiveScore) {
      out.min = min;
      out.max = min;
      out.size = 0;
      out.docs = outDocs;
      out.scores = outScores;

      max = std::min(max, maxDoc);
      if (min >= max) {
        return solux::PostingsReader::END;
      }
      if (minCompetitiveScore > this->minCompetitiveScore) {
        this->minCompetitiveScore = minCompetitiveScore;
      }

      int32_t leadDoc;
      if constexpr (CandidatePostingsFeed) {
        assert(candidatePostingsLead != nullptr);
        leadDoc = advanceCandidatePostingsLead(min);
      } else {
        leadDoc = scorerDocId<TermFast, TermTailFast>(0);
        if (leadDoc < min) {
          leadDoc = scorerAdvanceLead<TermFast, TermTailFast>(min);
        }
      }
      const bool exactDrive =
          !competitivePruning && !disableExactScoredBoundsBypassForTests;
      if constexpr (CandidateTermFeed) {
        assert(CandidatePostingsFeed);
        assert(candidateLeadScoreScorer != nullptr);
      }

      for (;;) {
        if (leadDoc == solux::PostingsReader::END) {
          out.max = max;
          return solux::PostingsReader::END;
        }
        if (leadDoc >= max) {
          out.max = max;
          return leadDoc;
        }

        int32_t upTo = max - 1;
        if (exactDrive) {
          // Exact collection pins theta at the lowest float, so score bounds
          // cannot reject a window or candidate. Keep the same lead batches
          // and scored resident probes without parsing impact headers merely
          // to establish a useless bound horizon.
        } else {
          // Window = [leadDoc, upTo], bounded by the scoring clauses' shallow
          // blocks. A filter is a zero-score required clause: it owns
          // membership and may own the candidate feed, but contributes no
          // score bound and must not parse frequency/impact data here.
          for (size_t c = 0; c < scorers.size(); c++) {
            if (scoringClauses[c] != 0) {
              upTo = std::min(
                  upTo,
                  scorerAdvanceShallow<TermFast, TermTailFast>(c, leadDoc));
            }
          }
          if (upTo < leadDoc) {
            upTo = leadDoc;
          }
          suffixMax[scorers.size()] = 0.0;
          for (size_t c = scorers.size(); c-- > 0; ) {
            windowMax[c] = scoringClauses[c] != 0
                ? scorerGetMaxScore<TermFast, TermTailFast>(c, upTo)
                : 0.0f;
            suffixMax[c] = suffixMax[c + 1] + (double) windowMax[c];
          }
          // Keep all-term bulk bounds group-granular so candidate windows do
          // not pay for block-level refinement.
          if (suffixMax[0] * scoreBoundFactor
              < (double) this->minCompetitiveScore) {
            skippedWindowCount++;
            if (upTo >= max - 1) {
              out.max = max;
              return upTo + 1;
            }
            if constexpr (CandidatePostingsFeed) {
              leadDoc = advanceCandidatePostingsLead(upTo + 1);
            } else {
              leadDoc = scorerAdvanceLead<TermFast, TermTailFast>(upTo + 1);
            }
            continue;
          }
        }

        // Produce this window: chunks of lead docs, one pass per other clause.
        if (windowFilter != nullptr && !windowFilter->probes()
            && upTo - leadDoc >= kWindowSize) {
          upTo = leadDoc + kWindowSize - 1;
        }
        out.max = upTo + 1;
        if (windowFilter != nullptr && !windowFilter->probes()
            && windowFilter->prepare(leadDoc, upTo + 1) == 0) {
          out.min = leadDoc;
          return upTo + 1;
        }
        // Last lead doc fully decided (emitted or rejected).  Resume from this,
        // not from lead->docId(): fill contracts differ on where the lead rests
        // after a fill (TermQuery leaves it ON the last emitted doc; the base
        // Scorer contract has already advanced PAST it), so a docId()-based
        // resume can skip a doc at a buffer boundary.
        int32_t lastDecided = -1;
        for (;;) {
          if (out.size + candidateBatchSize > (int32_t) outDocs.size()) {
            // out is nearly full: end the window early; the next call resumes
            // the rest of this window right after the last decided doc.
            assert(lastDecided >= 0);
            return lastDecided + 1;
          }
          int32_t n;
          if constexpr (CandidatePostingsFeed) {
            n = gatherCandidatePostingsLead(leadDoc, upTo + 1);
          } else {
            const int64_t leadFreqDecodesBefore =
                SkipStats::enabled ? SkipStats::tfreqBlocksDecoded : 0;
            n = scorerFillScoreBlock<TermFast>(
                0, candDocs.data(), candScores.data(), kChunk, upTo + 1);
            if (SkipStats::enabled) {
              SkipStats::conjScoredLeadFreqDecodes +=
                  SkipStats::tfreqBlocksDecoded - leadFreqDecodesBefore;
            }
          }
          if (n == 0) {
            break;
          }
          lastDecided = candDocs[(size_t) n - 1];
          if constexpr (CandidateTermFeed) {
            n = compactCandidateFilters(n, filter);
            if (n > 0) {
              n = applyScoredProbeToCandidates<false>(
                  candidateLeadScoreScorer, n,
                  std::numeric_limits<double>::infinity());
            }
            for (size_t c = 1; c < scorers.size() && n > 0; c++) {
              if (candidateFilters[c] != 0) {
                continue;
              }
              assert(termScorers[c] != nullptr);
              n = applyScoredProbeToCandidates<false>(
                  termScorers[c], n,
                  std::numeric_limits<double>::infinity());
            }
          } else {
            bool fillFilter =
                windowFilter != nullptr && !windowFilter->probes();
            if (filter != nullptr || fillFilter) {
              int32_t w = 0;
              for (int32_t i = 0; i < n; i++) {
                int32_t doc = candDocs[(size_t) i];
                if (filter != nullptr && !filter->get(doc)) continue;
                if (fillFilter && !windowFilter->accepts(doc)) continue;
                candDocs[(size_t) w] = doc;
                candScores[(size_t) w] = candScores[(size_t) i];
                w++;
              }
              n = w;
            }
            for (size_t c = 1; c < scorers.size() && n > 0; c++) {
              const double remaining = exactDrive
                  ? std::numeric_limits<double>::infinity()
                  : suffixMax[c];  // clauses [c, end) add at most this
              const bool batchBound = !disableBatchBoundForTests;
              if (batchBound && remainingBoundCanDrop(remaining)) {
                n = batchCompactByRemainingBound(n, remaining);
                if (n == 0) continue;
              }
              if constexpr (TermFast || TermTailFast) {
                if (!disableScoredProbeForTests) {
                  n = batchBound
                      ? applyScoredProbeToCandidates<false>(
                          termScorers[c], n, remaining)
                      : applyScoredProbeToCandidates<true>(
                          termScorers[c], n, remaining);
                  continue;
                }
              }
              n = batchBound
                  ? applyScorerToCandidates<
                      false, TermFast, TermTailFast>(c, n, remaining)
                  : applyScorerToCandidates<
                      true, TermFast, TermTailFast>(c, n, remaining);
            }
          }
          if (!candidateProhibitedTerms.empty()) {
            n = compactCompetitiveCandidates(n);
            n = compactCandidateProhibitedTerms(n);
          }
          for (int32_t i = 0; i < n; i++) {
            if (candScores[(size_t) i] < this->minCompetitiveScore) continue;
            int32_t doc = candDocs[(size_t) i];
            if constexpr (!CandidateTermFeed) {
              if (windowFilter != nullptr && windowFilter->probes()
                  && !windowFilter->acceptsProbe(doc)) continue;
            }
            outDocs[(size_t) out.size] = doc;
            outScores[(size_t) out.size] = candScores[(size_t) i];
            out.size++;
          }
        }
        return upTo + 1;
      }
    }

    int32_t scoreNextWindowTermClauses(ScoreWindow& out, DocSet* filter,
                                       int32_t min, int32_t max,
                                       float minCompetitiveScore) {
      out.min = min;
      out.max = min;
      out.size = 0;
      out.docs = outDocs;
      out.scores = outScores;

      max = std::min(max, maxDoc);
      if (min >= max) return PostingsReader::END;
      if (minCompetitiveScore > this->minCompetitiveScore) {
        this->minCompetitiveScore = minCompetitiveScore;
      }

      int32_t leadDoc = termClauseScoreDocId(0);
      if (leadDoc < min) leadDoc = termClauseScoreAdvance(0, min);

      for (;;) {
        if (leadDoc == PostingsReader::END) {
          out.max = max;
          return PostingsReader::END;
        }
        if (leadDoc >= max) {
          out.max = max;
          return leadDoc;
        }

        int32_t upTo = max - 1;
        for (size_t c = 0; c < termClauses.size(); c++) {
          upTo = std::min(upTo, termClauseAdvanceShallow(c, leadDoc));
        }
        if (upTo < leadDoc) upTo = leadDoc;
        suffixMax[termClauses.size()] = 0.0;
        for (size_t c = termClauses.size(); c-- > 0; ) {
          windowMax[c] = termClauseGetMaxScore(c, upTo);
          suffixMax[c] = suffixMax[c + 1] + (double) windowMax[c];
        }
        if (suffixMax[0] * scoreBoundFactor
            < (double) this->minCompetitiveScore) {
          skippedWindowCount++;
          if (upTo >= max - 1) {
            out.max = max;
            return upTo + 1;
          }
          leadDoc = termClauseScoreAdvance(0, upTo + 1);
          continue;
        }

        skipCount(SkipStats::conjDisjGroupScoreWindows);
        if (windowFilter != nullptr && !windowFilter->probes()
            && upTo - leadDoc >= kWindowSize) {
          upTo = leadDoc + kWindowSize - 1;
        }
        out.max = upTo + 1;
        if (windowFilter != nullptr && !windowFilter->probes()
            && windowFilter->prepare(leadDoc, upTo + 1) == 0) {
          out.min = leadDoc;
          return upTo + 1;
        }
        int32_t lastDecided = -1;
        for (;;) {
          if (out.size + kChunk > (int32_t) outDocs.size()) {
            assert(lastDecided >= 0);
            return lastDecided + 1;
          }
          int32_t n = fillTermClauseLeadScoreBlock(
              candDocs.data(), candScores.data(), kChunk, upTo + 1);
          if (n == 0) break;
          lastDecided = candDocs[(size_t) n - 1];

          bool fillFilter = windowFilter != nullptr && !windowFilter->probes();
          if (filter != nullptr || fillFilter) {
            int32_t write = 0;
            for (int32_t i = 0; i < n; i++) {
              int32_t doc = candDocs[(size_t) i];
              if (filter != nullptr && !filter->get(doc)) continue;
              if (fillFilter && !windowFilter->accepts(doc)) continue;
              if (write != i) {
                candDocs[(size_t) write] = doc;
                candScores[(size_t) write] = candScores[(size_t) i];
              }
              write++;
            }
            n = write;
          }

          for (size_t c = 1; c < termClauses.size() && n > 0; c++) {
            double remaining = suffixMax[c];
            if (termClauses[c].members.size() > 1) {
              n = compactByRemainingBound(n, remaining);
              if (n > 0) n = applyTermGroupToCandidates(c, n);
              continue;
            }

            auto* scorer = termClauseMember(c, 0);
            int32_t scorerDoc = scorer->docId();
            int32_t write = 0;
            for (int32_t i = 0; i < n; i++) {
              int32_t doc = candDocs[(size_t) i];
              float sum = candScores[(size_t) i];
              if (((double) sum + remaining) * scoreBoundFactor
                  < (double) this->minCompetitiveScore) {
                continue;
              }
              if (scorerDoc < doc) scorerDoc = scorer->advance(doc);
              if (scorerDoc != doc) continue;
              candDocs[(size_t) write] = doc;
              candScores[(size_t) write] = sum + scorer->score();
              write++;
            }
            n = write;
          }

          for (int32_t i = 0; i < n; i++) {
            if (candScores[(size_t) i] < this->minCompetitiveScore) continue;
            int32_t doc = candDocs[(size_t) i];
            if (windowFilter != nullptr && windowFilter->probes()
                && !windowFilter->acceptsProbe(doc)) continue;
            outDocs[(size_t) out.size] = doc;
            outScores[(size_t) out.size] = candScores[(size_t) i];
            out.size++;
          }
        }
        return upTo + 1;
      }
    }

    void clearWindowBits(std::span<uint64_t> bits) {
      std::fill(bits.begin(), bits.end(), 0);
    }

    int64_t popCountWindowBits() const {
      int64_t total = 0;
      for (uint64_t bits : windowBits) {
        total += std::popcount(bits);
      }
      return total;
    }

    int64_t countWindowBitsWithFilter(DocSet* filter, int32_t windowBase,
                                      int32_t windowEnd) const {
      int64_t total = 0;
      int32_t innerSize = windowEnd - windowBase;
      for (int32_t word = 0; word < kWindowWords; word++) {
        uint64_t bits = windowBits[(size_t) word];
        while (bits != 0) {
          int32_t bit = (int32_t) std::countr_zero(bits);
          int32_t index = (word << 6) + bit;
          if (index >= innerSize) {
            break;
          }
          int32_t doc = windowBase + index;
          if (filter->get(doc)) {
            total++;
          }
          bits &= bits - 1;
        }
      }
      return total;
    }

    int32_t ratchetDenseWindow(int32_t min, int32_t max) {
      for (size_t c = 0; c < denseClauses.size(); c++) {
        int32_t doc = denseClauseDocId(denseClauses[c]);
        if (doc < min) {
          doc = denseClauseCountAdvance(denseClauses[c], min);
        }
        if (doc > min) {
          min = doc;
        }
        if (min >= max) {
          break;
        }
      }
      // May be >= max (or END): every doc below it fails some clause, so it
      // is a sound resume point either way.
      return min;
    }

    void leapfrogRemainingDenseClauses(size_t firstClause,
                                       int32_t windowBase, int32_t windowEnd) {
      int32_t innerSize = windowEnd - windowBase;
      for (int32_t word = 0; word < kWindowWords; word++) {
        uint64_t bits = windowBits[(size_t) word];
        while (bits != 0) {
          int32_t bit = (int32_t) std::countr_zero(bits);
          int32_t index = (word << 6) + bit;
          if (index >= innerSize) {
            break;
          }
          int32_t doc = windowBase + index;
          bool matched = true;
          for (size_t c = firstClause; c < denseClauses.size(); c++) {
            int32_t scorerDoc = termScorers[c] != nullptr
                ? termScorers[c]->docsEnum.docId()
                : denseClauseDocId(denseClauses[c]);
            if (scorerDoc < doc) {
              scorerDoc = termScorers[c] != nullptr
                  ? termScorers[c]->docsEnum.advanceDocOnly(doc)
                  : denseClauseCountAdvance(denseClauses[c], doc);
            }
            if (scorerDoc != doc) {
              matched = false;
              break;
            }
          }
          if (!matched) {
            windowBits[(size_t) word] &= ~(1ULL << bit);
          }
          bits &= bits - 1;
        }
      }
    }

    bool fillDenseWindowBits(DocSet* filter, int32_t min, int32_t max,
                             int32_t& windowBase, int32_t& windowEnd) {
      windowBase = ratchetDenseWindow(min, max);
      windowEnd = windowBase;
      if (windowBase >= max) {
        return false;
      }

      int32_t requestedEnd = windowBase + kWindowSize;
      if (requestedEnd < windowBase) {
        requestedEnd = max;
      }
      windowEnd = std::min(requestedEnd, max);

      if (windowFilter != nullptr
          && windowFilter->prepare(windowBase, windowEnd) == 0) {
        return false;
      }
      clearWindowBits(windowBits);
      fillDenseClauseWindowBits(
          denseClauses[0], windowBits, windowBase, windowEnd);
      auto belowLeapfrogThreshold = [&](int32_t threshold) {
        int32_t card = 0;
        for (uint64_t bits : windowBits) {
          card += (int32_t) std::popcount(bits);
        }
        return card < threshold;
      };
      int32_t threshold = countLeadIsDocSet
          ? docSetLeadLeapfrogThresholdForTests
          : termLeadLeapfrogThresholdForTests;
      if (threshold > 0 && denseClauses.size() >= 2
          && belowLeapfrogThreshold(threshold)) {
        if (!countLeadIsDocSet) {
          skipCount(SkipStats::conjTermLeadFirstFillLeapfrogs);
        }
        leapfrogRemainingDenseClauses(1, windowBase, windowEnd);
      } else {
        for (size_t c = 1; c < denseClauses.size(); c++) {
          clearWindowBits(clauseBits);
          fillDenseClauseWindowBits(
              denseClauses[c], clauseBits, windowBase, windowEnd);
          for (int32_t w = 0; w < kWindowWords; w++) {
            windowBits[(size_t) w] &= clauseBits[(size_t) w];
          }
          if (c + 1 < denseClauses.size()
              && belowLeapfrogThreshold(
                  denseLeapfrogThresholdForTests)) {
            leapfrogRemainingDenseClauses(c + 1, windowBase, windowEnd);
            break;
          }
        }
      }

      applyProhibitedWindowBits(windowBase, windowEnd);
      if (filter != nullptr && filter->type == DocSet::BITSET) {
        applyDomainBitsToWindow(windowBits, windowBase, windowEnd,
                                &((BitDocSet*) filter)->bits());
      } else if (filter != nullptr) {
        applyDocSetToWindow(windowBits, windowBase, windowEnd, filter);
      }
      if (windowFilter != nullptr) {
        windowFilter->intersect(windowBits);
      }
      return true;
    }

    void emitWindowBits(ScoreWindow& out, int32_t windowBase,
                        int32_t windowEnd) const {
      int32_t innerSize = windowEnd - windowBase;
      for (int32_t word = 0; word < kWindowWords; word++) {
        uint64_t bits = windowBits[(size_t) word];
        while (bits != 0) {
          int32_t bit = (int32_t) std::countr_zero(bits);
          int32_t index = (word << 6) + bit;
          if (index >= innerSize) {
            break;
          }
          assert(out.size < kWindowSize);
          out.docs[(size_t) out.size] = windowBase + index;
          out.size++;
          bits &= bits - 1;
        }
      }
    }

    std::span<int32_t> denseClauseFreqs(size_t clause) {
      return denseFreqs.subspan(
          clause * (size_t) kWindowSize, (size_t) kWindowSize);
    }

    int32_t ratchetDenseScoredWindow(int32_t min, int32_t max) {
      for (TermQuery::Scorer* scorer : termScorers) {
        int32_t doc = scorer->docId();
        if (doc < min) {
          doc = scorer->advance(min);
        }
        if (doc > min) {
          min = doc;
        }
        if (min >= max) {
          break;
        }
      }
      return min;
    }

    bool fillDenseScoredWindowBits(
        DocSet* filter, int32_t min, int32_t max,
        int32_t& windowBase, int32_t& windowEnd, int64_t& leadPostings) {
      windowBase = ratchetDenseScoredWindow(min, max);
      windowEnd = windowBase;
      leadPostings = 0;
      if (windowBase >= max) {
        return false;
      }

      int32_t requestedEnd = windowBase + kWindowSize;
      if (requestedEnd < windowBase) {
        requestedEnd = max;
      }
      windowEnd = std::min(requestedEnd, max);
      if (windowFilter != nullptr
          && windowFilter->prepare(windowBase, windowEnd) == 0) {
        return false;
      }

      clearWindowBits(windowBits);
      termScorers[0]->fillWindowBitsAndFreqs(
          windowBits, denseClauseFreqs(0), windowBase, windowEnd);
      leadPostings = popCountWindowBits();
      for (size_t c = 1; c < termScorers.size(); c++) {
        clearWindowBits(clauseBits);
        termScorers[c]->fillWindowBitsAndFreqs(
            clauseBits, denseClauseFreqs(c), windowBase, windowEnd);
        for (int32_t w = 0; w < kWindowWords; w++) {
          windowBits[(size_t) w] &= clauseBits[(size_t) w];
        }
      }

      if (filter != nullptr && filter->type == DocSet::BITSET) {
        applyDomainBitsToWindow(windowBits, windowBase, windowEnd,
                                &((BitDocSet*) filter)->bits());
      } else if (filter != nullptr) {
        applyDocSetToWindow(windowBits, windowBase, windowEnd, filter);
      }
      if (windowFilter != nullptr) {
        windowFilter->intersect(windowBits);
      }
      return true;
    }

    void finishDenseAdmissionSample(bool terminal) {
      if (denseScoredAdmission != DenseScoredAdmission::SAMPLING
          || (denseSampleWindows < kDenseAdmissionSampleWindows && !terminal)) {
        return;
      }
      bool leadAccepted = denseSampleLead
          >= (int64_t) kDenseAdmissionMinLeadPerWindow * denseSampleWindows;
      bool densityAccepted =
          denseSampleSurvivors * kDenseAdmissionDensityDenominator
          <= denseSampleLead
              * denseAdmissionDensityNumerator(denseScoredTopK);
      if (!densityAccepted) {
        skipCount(SkipStats::conjDenseScoredDensityRejects);
      }
      // Construction already bounds non-lead fill cost relative to the lead;
      // this sample therefore gates on useful lead volume and survivor density
      // instead of imposing an unrelated absolute survivor cap.
      bool admitted = leadAccepted && densityAccepted;
      denseScoredAdmission = admitted
          ? DenseScoredAdmission::ADMITTED
          : DenseScoredAdmission::SCORE_FIRST;
      skipCount(admitted ? SkipStats::conjDenseScoredAdmits
                         : SkipStats::conjDenseScoredLatchBacks);
    }

    int32_t scoreNextWindowDense(ScoreWindow& out, DocSet* filter,
                                 int32_t min, int32_t max,
                                 float minCompetitiveScore) {
      unused(minCompetitiveScore);
      out.min = min;
      out.max = min;
      out.size = 0;
      out.docs = outDocs;
      out.scores = outScores;

      max = std::min(max, maxDoc);
      if (min >= max) {
        return PostingsReader::END;
      }
      if (filter != nullptr && filter->card() == 0) {
        return PostingsReader::END;
      }

      int32_t windowBase;
      int32_t windowEnd;
      int64_t leadPostings;
      bool windowReady = fillDenseScoredWindowBits(
          filter, min, max, windowBase, windowEnd, leadPostings);
      if (windowBase >= max) {
        out.max = max;
        finishDenseAdmissionSample(true);
        return windowBase;
      }
      out.min = windowBase;
      out.max = windowEnd;
      if (windowReady) {
        emitWindowBits(out, windowBase, windowEnd);
      }

      skipCount(SkipStats::conjDenseScoredWindows);
      if (denseScoredAdmission == DenseScoredAdmission::SAMPLING) {
        denseSampleWindows++;
        denseSampleLead += leadPostings;
        denseSampleSurvivors += out.size;
      }

      for (int32_t i = 0; i < out.size; i++) {
        int32_t doc = out.docs[(size_t) i];
        int32_t offset = doc - windowBase;
        int64_t norm = denseScoredNorms[doc];
        float score = 0.0f;
        for (size_t c = 0; c < termScorers.size(); c++) {
          score += termScorers[c]->scoreFreqWithNorm(
              denseClauseFreqs(c)[(size_t) offset], norm);
        }
        out.scores[(size_t) i] = score;
      }
      bool terminal = windowEnd >= max;
      finishDenseAdmissionSample(terminal);
      return terminal ? PostingsReader::END : windowEnd;
    }

    int32_t countNextWindowDense(int64_t& count, DocSetBuilder* domainOut,
                                 DocSet* filter, int32_t min, int32_t max) {
      int32_t windowBase;
      int32_t windowEnd;
      bool windowReady = fillDenseWindowBits(
          filter, min, max, windowBase, windowEnd);
      if (windowBase >= max) {
        return windowBase;
      }
      skipCount(SkipStats::conjDenseCountWindows);
      if (denseHasDisjGroup) {
        skipCount(SkipStats::conjDisjGroupCountWindows);
      }
      if (!windowReady) {
        return windowEnd >= max ? PostingsReader::END : windowEnd;
      }
      int32_t wordCard = (int32_t) popCountWindowBits();
      if (domainOut != nullptr && wordCard != 0) {
        domainOut->addWindowWords(
            windowBits.data(), windowBase, windowEnd, wordCard);
      }
      count += wordCard;

      return windowEnd >= max ? PostingsReader::END : windowEnd;
    }

    template <bool TermFast, bool TermTailFast = false, typename Consumer>
    int32_t visitNextWindowSparse(DocSet* filter, int32_t min, int32_t max,
                                  int32_t& windowEnd, Consumer consume) {
      windowEnd = max;
      if (windowFilter != nullptr && !windowFilter->probes()) {
        int32_t requestedEnd = min + kWindowSize;
        if (requestedEnd < min) {
          requestedEnd = max;
        }
        windowEnd = std::min(max, requestedEnd);
        if (windowFilter->prepare(min, windowEnd) == 0) {
          return windowEnd;
        }
      }
      int32_t target = min;
      while (target < windowEnd) {
        int32_t doc = scorerCountDocId<TermFast, TermTailFast>(0);
        if (doc < target) {
          doc = scorerCountAdvance<TermFast, TermTailFast>(0, target);
        }
        if (doc >= windowEnd) {
          return doc;  // sound resume point (or END); covers doc == END
        }
        target = doc;

        bool matched = true;
        for (size_t c = 1; c < scorers.size(); c++) {
          int32_t scorerDoc = scorerCountDocId<TermFast, TermTailFast>(c);
          if (scorerDoc < target) {
            scorerDoc =
                scorerCountAdvance<TermFast, TermTailFast>(c, target);
          }
          if (scorerDoc != target) {
            target = scorerDoc;
            matched = false;
            break;
          }
        }
        if (!matched) {
          continue;
        }

        if (acceptsDoc(filter, target)) {
          bool keepGoing = consume(target);
          target++;
          if (!keepGoing) {
            windowEnd = target;
            return target;
          }
        } else {
          target++;
        }
      }
      return target;  // >= windowEnd: failing clause position or matched doc + 1
    }

    template <bool TermFast, bool TermTailFast = false>
    int32_t countNextWindowSparse(int64_t& count, DocSetBuilder* domainOut,
                                  DocSet* filter, int32_t min, int32_t max) {
      skipCount(SkipStats::conjCountFallbacks);
      int32_t windowEnd;
      return visitNextWindowSparse<TermFast, TermTailFast>(
          filter, min, max, windowEnd, [&](int32_t doc) {
            count++;
            if (domainOut != nullptr) {
              domainOut->add(doc);
            }
            return true;
          });
    }

    int32_t countNextWindowCandidateBatch(
        int64_t& count, DocSetBuilder* domainOut, DocSet* filter,
        int32_t min, int32_t max) {
      int32_t size = 0;
      int32_t sampleInput = 0;
      int32_t doc;
      if (candidatePostingsLead != nullptr) {
        size_t gatherSize = candidateCountAdmission
                == CandidateCountAdmission::SAMPLING
            ? std::min<size_t>(
                candDocs.size(), (size_t) kCandidateCountSampleSize)
            : candDocs.size();
        PostingsCandidateBatch batch = copyPostingsCandidates(
            candidatePostingsLead->docsEnum, candDocs.first(gatherSize),
            nextCandidatePostingsLeadDoc, min, max);
        size = batch.size;
        sampleInput = size;
        doc = batch.nextDoc;
        nextCandidatePostingsLeadDoc = doc;
        if (filter != nullptr) {
          int32_t write = 0;
          for (int32_t i = 0; i < size; i++) {
            int32_t candidate = candDocs[(size_t) i];
            if (filter->get(candidate)) {
              candDocs[(size_t) write++] = candidate;
            }
          }
          size = write;
        }
      } else {
        doc = scorers[0]->docId();
        if (doc < min) {
          doc = scorers[0]->advance(min);
        }
        while (doc < max && size < candidateBatchSize) {
          if (filter == nullptr || filter->get(doc)) {
            candDocs[(size_t) size++] = doc;
          }
          doc = scorers[0]->next();
        }
        sampleInput = size;
      }

      int32_t sampleFirstTailSurvivors = size;
      for (size_t clause = 1; clause < scorers.size() && size > 0;
           clause++) {
        if (disableCandidateCountRetainForTests) {
          size_t words = ((size_t) size + 63) >> 6;
          std::fill(
              clauseBits.begin(),
              clauseBits.begin() + (ptrdiff_t) words, 0);
          termScorers[clause]->addMatchesToCandidates(
              candDocs.data(), size, clauseBits.first(words));
          int32_t write = 0;
          for (int32_t i = 0; i < size; i++) {
            if ((clauseBits[(size_t) (i >> 6)]
                 & (1ULL << (i & 63))) != 0) {
              candDocs[(size_t) write++] = candDocs[(size_t) i];
            }
          }
          size = write;
        } else {
          size = termScorers[clause]->retainMatchesToCandidates(
              candDocs.data(), size);
        }
        if (clause == 1) {
          sampleFirstTailSurvivors = size;
        }
      }

      if (candidateCountAdmission == CandidateCountAdmission::SAMPLING
          && sampleInput > 0) {
        bool admitted = (int64_t) sampleFirstTailSurvivors
                * candidateCountFirstTailDensityInverseForTests
            <= sampleInput;
        candidateCountAdmission = admitted
            ? CandidateCountAdmission::ADMITTED
            : CandidateCountAdmission::DENSE;
        if (admitted) {
          skipCount(SkipStats::filteredCountCandidateAdmits);
        } else {
          candidatePostingsLead = nullptr;
          skipCount(SkipStats::filteredCountCandidateDenseLatchBacks);
        }
      }

      count += size;
      if (domainOut != nullptr) {
        for (int32_t i = 0; i < size; i++) {
          domainOut->add(candDocs[(size_t) i]);
        }
      }
      skipCount(SkipStats::filteredConjBatchCountWindows);
      return doc;
    }

  public:
    ConjunctionBulkScorer(solux::MemPool& pool, std::span<Query::Scorer*> scorers,
                          std::span<Query::Scorer*> prohibitedScorers,
                          std::span<TermQuery::Scorer*>
                              candidateProhibitedTerms,
                          std::span<const uint8_t> scoringClauses,
                          int32_t maxDoc, int64_t leadCost,
                          int64_t nonLeadCost, bool scoredConstruction,
                          bool countLeadIsDocSet,
                          TermQuery::Scorer* candidatePostingsLead,
                          TermQuery::Scorer* candidateLeadScoreScorer,
                          std::span<uint8_t> candidateFilters,
                          std::span<DocSet*> candidateFilterDocSets,
                          bool plannedNegatedCount,
                          bool recordRouteCommitment,
                          std::span<const ConjunctionClauseLayout>
                              plannedClauses,
                          std::span<const ConjunctionClauseLayout>
                              plannedProhibitedClauses)
        : scorers(scorers),
          prohibitedScorers(prohibitedScorers),
          candidateProhibitedTerms(candidateProhibitedTerms),
          scoringClauses(scoringClauses),
          termScorers(pool.make_arr<TermQuery::Scorer*>(scorers.size()), scorers.size()),
          termClauses(pool.make_arr<TermClause>(scorers.size()), scorers.size()),
          denseClauses(pool.make_arr<DenseClause>(scorers.size()), scorers.size()),
          prohibitedDenseClauses(
              pool.make_arr<DenseClause>(prohibitedScorers.size()),
              prohibitedScorers.size()),
          prohibitedExhausted(
              pool.make_arr<uint8_t>(prohibitedScorers.size()),
              prohibitedScorers.size()),
          windowMax(pool.make_arr<float>(scorers.size()), scorers.size()),
          suffixMax(pool.make_arr<double>(scorers.size() + 1), scorers.size() + 1),
          candDocs(pool.make_arr<int32_t>(
              (size_t) (countLeadIsDocSet || candidatePostingsLead != nullptr
                  ? filteredConjunctionBatchSizeForTests : kChunk)),
              (size_t) (countLeadIsDocSet || candidatePostingsLead != nullptr
                  ? filteredConjunctionBatchSizeForTests : kChunk)),
          candScores(pool.make_arr<float>(
              (size_t) (countLeadIsDocSet || candidatePostingsLead != nullptr
                  ? filteredConjunctionBatchSizeForTests : kChunk)),
              (size_t) (countLeadIsDocSet || candidatePostingsLead != nullptr
                  ? filteredConjunctionBatchSizeForTests : kChunk)),
          boundDrops(pool.make_arr<int32_t>(
              (size_t) (countLeadIsDocSet || candidatePostingsLead != nullptr
                  ? filteredConjunctionBatchSizeForTests : kChunk)),
              (size_t) (countLeadIsDocSet || candidatePostingsLead != nullptr
                  ? filteredConjunctionBatchSizeForTests : kChunk)),
          outDocs(pool.make_arr<int32_t>((size_t) kWindowSize), (size_t) kWindowSize),
          outScores(pool.make_arr<float>((size_t) kWindowSize), (size_t) kWindowSize),
          windowBits(pool.make_arr<uint64_t>((size_t) kWindowWords), (size_t) kWindowWords),
          clauseBits(pool.make_arr<uint64_t>((size_t) kWindowWords), (size_t) kWindowWords),
          groupMatchBits(
              pool.make_arr<uint64_t>(((size_t) kChunk + 63) >> 6),
              ((size_t) kChunk + 63) >> 6),
          groupScores(pool.make_arr<float>((size_t) kChunk), (size_t) kChunk),
          maxDoc(maxDoc),
          candidateBatchSize(
              countLeadIsDocSet || candidatePostingsLead != nullptr
                  ? filteredConjunctionBatchSizeForTests : kChunk),
          countLeadIsDocSet(countLeadIsDocSet),
          candidatePostingsLead(candidatePostingsLead),
          candidateLeadScoreScorer(candidateLeadScoreScorer),
          candidateFilters(candidateFilters),
          candidateFilterDocSets(candidateFilterDocSets) {
      assert(!scorers.empty());
      assert(scoringClauses.size() == scorers.size());
      assert(plannedClauses.size() == scorers.size());
      assert(plannedProhibitedClauses.size()
             == prohibitedScorers.size());
      assert(candidateBatchSize >= kChunk);
      if (!candidateFilterDocSets.empty()) {
        candidateFilterProbes =
            pool.make_span<DocSetProbe>(candidateFilterDocSets.size());
        for (size_t i = 0; i < candidateFilterDocSets.size(); i++) {
          candidateFilterProbes[i].reset(candidateFilterDocSets[i]);
        }
      }
      std::fill(prohibitedExhausted.begin(), prohibitedExhausted.end(), 0);
      allTermScorers = true;
      termTailScorers = scorers.size() > 1;
      sparseEligibleTails = scorers.size() > 1;
      allTermClauses = true;
      allDenseClauses = true;
      bool hasDirectDenseClause = false;
      size_t scoreAddends = 0;
      for (size_t i = 0; i < scorers.size(); i++) {
        std::span<Query::Scorer*> denseMembers =
            plannedClauses[i].denseMembers;
        denseClauses[i].members = denseMembers;
        if (!scoredConstruction
            && !disableDirectDenseClausesForTests
            && denseMembers.size() == 1) {
          denseClauses[i].term = plannedClauses[i].directTerm;
          denseClauses[i].docSet = plannedClauses[i].directDocSet;
          hasDirectDenseClause |= denseClauses[i].term != nullptr
              || denseClauses[i].docSet != nullptr;
        }
        allDenseClauses &= !denseMembers.empty();
        denseHasDisjGroup |= denseMembers.size() > 1;

        termScorers[i] = plannedClauses[i].directTerm;
        allTermScorers &= termScorers[i] != nullptr;
        if (i > 0) {
          termTailScorers &= termScorers[i] != nullptr;
          sparseEligibleTails &= termScorers[i] != nullptr
              || denseClauses[i].docSet != nullptr;
        }
        if (termScorers[i] != nullptr) {
          termClauses[i].members = scorers.subspan(i, 1);
          scoreAddends += (size_t) (scoringClauses[i] != 0);
          continue;
        }
        std::span<Query::Scorer*> members =
            plannedClauses[i].termMembers;
        termClauses[i].members = members;
        allTermClauses &= !members.empty();
        hasDisjGroup |= !members.empty();
        if (scoringClauses[i] != 0) {
          scoreAddends += members.empty() ? 1 : members.size();
        }
      }
      if (hasDirectDenseClause && recordRouteCommitment) {
        skipCount(SkipStats::conjDirectDenseEngagements);
      }
      bool allDenseProhibited = true;
      for (size_t i = 0; i < prohibitedScorers.size(); i++) {
        std::span<Query::Scorer*> denseMembers =
            plannedProhibitedClauses[i].denseMembers;
        switch (plannedProhibitedClauses[i].windowFillKind) {
          case Query::ClauseShape::DIRECT:
            prohibitedScorers[i]->recordWindowFilterCommit(true);
            break;
          case Query::ClauseShape::FLAT_DISJUNCTION:
            prohibitedScorers[i]->recordWindowFilterCommit(false);
            if (!denseMembers.empty()) {
              for (Query::Scorer* member : denseMembers) {
                member->recordWindowFilterCommit(true);
              }
            }
            break;
          case Query::ClauseShape::FLAT_CONJUNCTION:
            assert(false);
            break;
          case Query::ClauseShape::NONE:
            prohibitedScorers[i]->recordWindowFilterCommit(false);
            break;
          case Query::ClauseShape::UNKNOWN:
            assert(false);
            break;
        }
        prohibitedDenseClauses[i].members = denseMembers;
        allDenseProhibited &= !denseMembers.empty();
      }
      // Bounds cover the same number of float additions as final scoring,
      // including every member contribution inside decomposed unions.
      scoreBoundFactor = 1.0 + (double) scoreAddends * 0x1p-24;
      if (allTermClauses && termClauses[0].members.size() > 1) {
        size_t memberCount = termClauses[0].members.size();
        leadGroupTerms = pool.make_span<BufferedTerm>(memberCount);
        auto docs = pool.make_span<int32_t>(memberCount * (size_t) kChunk);
        auto scores = pool.make_span<float>(memberCount * (size_t) kChunk);
        for (size_t member = 0; member < memberCount; member++) {
          leadGroupTerms[member].scorer = termClauseMember(0, member);
          leadGroupTerms[member].docs = docs.subspan(member * (size_t) kChunk,
                                                     (size_t) kChunk);
          leadGroupTerms[member].scores = scores.subspan(member * (size_t) kChunk,
                                                         (size_t) kChunk);
        }
      }
      assert(plannedNegatedCount
             == (!prohibitedScorers.empty()
                 || !candidateProhibitedTerms.empty()));
      negatedCountPath = plannedNegatedCount;
      int32_t denseThresholdInverse =
          !scoredConstruction && sparseEligibleTails
          ? termTailDenseThresholdInverseForTests
          : denseThresholdInverseForTests;
      denseCountPath = allDenseClauses && allDenseProhibited
          && maxDoc >= kWindowSize
          && leadCost >= std::max<int64_t>(
              1, (int64_t) maxDoc / denseThresholdInverse);
      if (candidatePostingsLead != nullptr && !scoredConstruction) {
        assert(denseCountPath);
        candidateCountAdmission = CandidateCountAdmission::SAMPLING;
      }
      denseScoredEligible = scoredConstruction && denseCountPath
          && !negatedCountPath && allTermScorers
          && candidatePostingsLead == nullptr
          && !disableDenseScoredForTests;
      if (denseScoredEligible) {
        denseScoredNorms = termScorers[0]->flatNormsBase;
        for (TermQuery::Scorer* scorer : termScorers) {
          denseScoredEligible &= scorer->simScorer != nullptr
              && scorer->flatNormsBase != nullptr
              && scorer->flatNormsBase == denseScoredNorms;
        }
        if (denseScoredEligible) {
          denseScoredCostRejected =
              nonLeadCost
                  > leadCost * kDenseAdmissionMaxNonLeadCostRatio;
          denseScoredEligible &= !denseScoredCostRejected;
        }
        if (denseScoredEligible) {
          denseFreqs = pool.make_span<int32_t>(
              scorers.size() * (size_t) kWindowSize);
        }
      }
    }

    void setTopKDepth(int32_t topK, bool allowPruning) override {
      competitivePruning = allowPruning;
      if (!allowPruning && !disableExactScoredBoundsBypassForTests) {
        skipCount(SkipStats::conjExactScoredBoundsBypasses);
      }
      // Unpruned collection has no score skips, so model it at the maximum
      // depth used by the density policy. The requested depth only sizes the
      // heap when pruning is disabled.
      int32_t effectiveTopK = allowPruning
          ? topK : kDenseAdmissionMaxDensityTopK;
      if (effectiveTopK < kDenseScoredMinTopK
          || denseScoredAdmission != DenseScoredAdmission::SCORE_FIRST) {
        return;
      }
      if (denseScoredCostRejected) {
        skipCount(SkipStats::conjDenseScoredCostRejects);
      }
      if (!denseScoredEligible) {
        return;
      }
      denseScoredTopK = effectiveTopK;
      denseScoredAdmission = DenseScoredAdmission::SAMPLING;
    }

    bool willCountDense() const override {
      return denseCountPath && candidatePostingsLead == nullptr;
    }

    bool willSampleCandidateCount() const {
      return candidateCountAdmission == CandidateCountAdmission::SAMPLING;
    }

    bool supportsMatchWindows() const override {
      return true;
    }

    bool attachWindowFilter(WindowFilter* filter) override {
      if (!allTermClauses) {
        return false;
      }
      assert(windowFilter == nullptr);
      windowFilter = filter;
      // Candidate COUNT does not prepare WindowFilter windows. Preserve the
      // BulkScorer filter contract by selecting its exact dense sibling before
      // iteration; sparse execution already checks WindowFilter per match.
      if (candidateCountAdmission == CandidateCountAdmission::SAMPLING) {
        candidateCountAdmission = CandidateCountAdmission::DENSE;
        candidatePostingsLead = nullptr;
      }
      // The dense scored path fills every clause before applying the filter,
      // so a selective filter discards most of that work. Require the filter
      // to match at least maxDoc/kDenseScoredMinFilterDensityInverse docs;
      // sparser filters remain on score-first execution.
      if (filter->cost()
          < (int64_t) maxDoc
              / kDenseScoredMinFilterDensityInverse) {
        if (denseScoredEligible) {
          skipCount(SkipStats::conjDenseScoredFilterRejects);
        }
        denseScoredEligible = false;
      }
      return true;
    }

    int32_t scoreNextWindow(ScoreWindow& out, DocSet* filter, int32_t min, int32_t max,
                            float minCompetitiveScore) override {
      if (denseScoredAdmission != DenseScoredAdmission::SCORE_FIRST) {
        return scoreNextWindowDense(
            out, filter, min, max, minCompetitiveScore);
      }
      if (allTermClauses && hasDisjGroup) {
        return scoreNextWindowTermClauses(
            out, filter, min, max, minCompetitiveScore);
      }
      if ((countLeadIsDocSet || candidatePostingsLead != nullptr)
          && termTailScorers) {
        skipCount(SkipStats::filteredConjBatchScoreWindows);
      }
      if (candidateLeadScoreScorer != nullptr) {
        if (!termTailScorers) {
          skipCount(SkipStats::filteredConjBatchScoreWindows);
        }
        return scoreNextWindowImpl<true, false, true, true>(
            out, filter, min, max, minCompetitiveScore);
      }
      if (candidatePostingsLead != nullptr) {
        return scoreNextWindowImpl<true, false, true>(
            out, filter, min, max, minCompetitiveScore);
      }
      return allTermScorers
          ? scoreNextWindowImpl<true>(out, filter, min, max, minCompetitiveScore)
          : termTailScorers
              ? scoreNextWindowImpl<false, true>(
                  out, filter, min, max, minCompetitiveScore)
              : scoreNextWindowImpl<false>(
                  out, filter, min, max, minCompetitiveScore);
    }

    int32_t matchNextWindow(ScoreWindow& out, DocSet* filter,
                            int32_t min, int32_t max) override {
      out.min = min;
      out.max = min;
      out.size = 0;
      out.docs = outDocs;
      out.scores = outScores;

      max = std::min(max, maxDoc);
      if (min >= max) {
        return PostingsReader::END;
      }
      if (filter != nullptr && filter->card() == 0) {
        return PostingsReader::END;
      }
      if (denseCountPath) {
        int32_t windowBase;
        int32_t windowEnd;
        bool windowReady = fillDenseWindowBits(
            filter, min, max, windowBase, windowEnd);
        if (windowBase >= max) {
          out.max = max;
          return windowBase;
        }
        skipCount(SkipStats::conjDenseMatchWindows);
        out.min = windowBase;
        out.max = windowEnd;
        if (windowReady) {
          emitWindowBits(out, windowBase, windowEnd);
        }
        return windowEnd >= max ? PostingsReader::END : windowEnd;
      }

      skipCount(SkipStats::conjMatchFallbacks);
      int32_t windowEnd;
      auto emit = [&](int32_t doc) {
        assert(out.size < kWindowSize);
        out.docs[(size_t) out.size] = doc;
        out.size++;
        return out.size < kWindowSize;
      };
      int32_t next = allTermScorers
          ? visitNextWindowSparse<true>(filter, min, max, windowEnd, emit)
          : termTailScorers
              ? visitNextWindowSparse<false, true>(
                  filter, min, max, windowEnd, emit)
              : visitNextWindowSparse<false>(
                  filter, min, max, windowEnd, emit);
      out.max = windowEnd;
      return next;
    }

    int32_t countNextWindow(int64_t& count, DocSetBuilder* domainOut,
                            DocSet* filter, int32_t min, int32_t max) override {
      max = std::min(max, maxDoc);
      if (min >= max) {
        return PostingsReader::END;
      }
      if (filter != nullptr && filter->card() == 0) {
        return PostingsReader::END;
      }
      if (domainOut != nullptr) {
        skipCount(SkipStats::bulkDomainWindowsFed);
      }
      if (!disableFilteredConjunctionBatchForTests
          && candidatePostingsLead != nullptr && termTailScorers) {
        return countNextWindowCandidateBatch(
            count, domainOut, filter, min, max);
      }
      if (denseCountPath) {
        return countNextWindowDense(count, domainOut, filter, min, max);
      }
      if (!disableFilteredConjunctionBatchForTests
          && countLeadIsDocSet && termTailScorers) {
        return countNextWindowCandidateBatch(
            count, domainOut, filter, min, max);
      }
      return allTermScorers
          ? countNextWindowSparse<true>(count, domainOut, filter, min, max)
          : termTailScorers
              ? countNextWindowSparse<false, true>(
                  count, domainOut, filter, min, max)
              : countNextWindowSparse<false>(
                  count, domainOut, filter, min, max);
    }

    int64_t skippedWindows() const {
      return skippedWindowCount;
    }

    bool denseScoredLatchedBackForTests() const {
      return denseScoredEligible && denseSampleWindows > 0
          && denseScoredAdmission == DenseScoredAdmission::SCORE_FIRST;
    }

    int32_t denseScoredSampleWindowsForTests() const {
      return denseSampleWindows;
    }

    bool denseScoredCostRejectedForTests() const {
      return denseScoredCostRejected;
    }

    std::span<TermQuery::Scorer*> termScorersForTests() {
      return termScorers;
    }
  }; // ConjunctionBulkScorer


  class DisjunctionScorer final : public Query::Scorer {
    struct ApproxSlot {
      Scorer* scorer = nullptr;
      bool twoPhase = false;
      int32_t doc = -1;

      int32_t next() {
        doc = twoPhase ? scorer->approximationNext() : scorer->next();
        return doc;
      }

      int32_t advance(int32_t target) {
        doc = twoPhase ? scorer->approximationAdvance(target) : scorer->advance(target);
        return doc;
      }

      int32_t docId() const {
        return doc;
      }

      float matchCost() const {
        return twoPhase ? scorer->matchCost() : 0.0f;
      }
    };

    std::span<Scorer*> clauses;  // stable decomposition order
    std::span<ApproxSlot> members;  // stable score order and protocol latches
    std::span<ApproxSlot*> heap;
    std::span<ApproxSlot*> verificationOrder;

    constexpr static auto idComparator = [](ApproxSlot& a, ApproxSlot& b) {
      return b.docId() < a.docId();
    };

    solux::IndirectPQ<ApproxSlot, decltype(idComparator)> pq;

    int32_t docid = -1;
    // Scorers expose no approximation cost for a weighted estimate.
    float verificationCost = 0.0f;
    bool anyTwoPhase = false;
    bool twoWay;

    static std::span<ApproxSlot> makeMembers(
        solux::MemPool& pool, std::span<Scorer*> scorers,
        std::span<const uint8_t> twoPhaseScorers,
        bool enableTwoPhase) {
      assert(scorers.size() == twoPhaseScorers.size());
      auto members = pool.make_span<ApproxSlot>(scorers.size());
      for (size_t i = 0; i < scorers.size(); i++) {
        bool twoPhase = enableTwoPhase && twoPhaseScorers[i] != 0;
        int32_t doc = twoPhase ? scorers[i]->approximationDocId()
                               : scorers[i]->docId();
        members[i] = {scorers[i], twoPhase, doc};
      }
      return members;
    }

    static std::span<ApproxSlot*> makePointers(solux::MemPool& pool,
                                                std::span<ApproxSlot> members) {
      auto pointers = pool.make_span<ApproxSlot*>(members.size());
      for (size_t i = 0; i < members.size(); i++) pointers[i] = &members[i];
      return pointers;
    }

    int32_t nextMatched(int32_t doc) {
      while (doc != solux::PostingsReader::END && !matches()) {
        doc = approximationNext();
      }
      return doc;
    }

  public:
    static inline bool disableDisjTwoPhaseForTests = false;
    static inline bool disableTwoWayForTests = false;

    DisjunctionScorer(solux::MemPool& pool, std::span<Scorer*> scorers,
                      std::span<const uint8_t> twoPhaseScorers,
                      bool enableTwoPhase)
            : clauses(scorers),
              members(makeMembers(
                  pool, scorers, twoPhaseScorers, enableTwoPhase)),
              heap(makePointers(pool, members)),
              verificationOrder(makePointers(pool, members)), pq(heap),
              twoWay(scorers.size() == 2 && !disableTwoWayForTests) {
      for (auto& member : members) {
        if (member.twoPhase) {
          anyTwoPhase = true;
          verificationCost = std::max(verificationCost, member.matchCost());
        }
      }
      std::stable_sort(verificationOrder.begin(), verificationOrder.end(),
                       [](const ApproxSlot* a, const ApproxSlot* b) {
                         return a->matchCost() < b->matchCost();
                       });
    }

    int32_t next() override {
      return nextMatched(approximationNext());
    }

    int32_t advance(int32_t target) override {
      return nextMatched(approximationAdvance(target));
    }

    int32_t approximationNext() override {
      // Contract: callers must not re-poll after END (see Query::Scorer).
      assert(docid != solux::PostingsReader::END);
      if (twoWay) {
        if (members[0].docId() == docid) {
          members[0].next();
        }
        if (members[1].docId() == docid) {
          members[1].next();
        }
        docid = std::min(members[0].docId(), members[1].docId());
        return docid;
      }
      assert(pq.size() > 0);
      int currid = docid;
      assert(pq.top().docId() == docid);

      docid = pq.top().next();
      for (;;) {
        if (docid == solux::PostingsReader::END) {
          pq.removeTop();
          if (pq.size() == 0) {
            break;
          }
        } else {
          bool changed = pq.updateTop();
          if (!changed) {
            // we didn't change the top scorer, so we are done.
            break;
          }
        }

        // OK, heap was changed, so lets look at the lowest id now.
        docid = pq.top().docId();
        if (docid <= currid) {  // really, it should never be less, just equal if multiple scorers matched the same doc
          docid = pq.top().next();
        }
      }

      return docid;
    }

    // Advance every member below target through its latched protocol. On the
    // heap path each member is touched at most once: it surfaces at the top
    // while behind, advances to >= target, and sifts down.
    int32_t approximationAdvance(int32_t target) override {
      assert(docid < target);
      if (twoWay) {
        if (members[0].docId() < target) {
          members[0].advance(target);
        }
        if (members[1].docId() < target) {
          members[1].advance(target);
        }
        docid = std::min(members[0].docId(), members[1].docId());
        return docid;
      }
      while (pq.size() > 0 && pq.top().docId() < target) {
        if (pq.top().advance(target) == solux::PostingsReader::END) {
          pq.removeTop();
        } else {
          pq.updateTop();
        }
      }
      docid = pq.size() > 0 ? pq.top().docId() : solux::PostingsReader::END;
      return docid;
    }

    /// doc we are positioned on
    int32_t docId() override {
      return docid;
    }

    int32_t approximationDocId() override {
      return docid;
    }

    bool matches() override {
      for (ApproxSlot* member : verificationOrder) {
        if (member->docId() != docid) continue;
        if (!member->twoPhase || member->scorer->matches()) return true;
      }
      return false;
    }

    float matchCost() override {
      return verificationCost;
    }

    std::span<Scorer*> flatDisjunctionScorers() override {
      return clauses;
    }

    float score() override {
      assert(docid != solux::PostingsReader::END);
      assert(twoWay || (pq.size() > 0 && docid == pq.top().docId()));
      float score = 0.0f;
      for (auto& member : members) {
        if (member.docId() == docid
            && (!member.twoPhase || member.scorer->matches())) {
          score += member.scorer->score();
        }
      }
      return score;
    }

    bool usesTwoWayMergeForTests() const {
      return twoWay;
    }

    int32_t advanceShallow(int32_t target) override {
      int32_t upTo = solux::PostingsReader::END;
      for (auto& member : members) {
        if (member.docId() != solux::PostingsReader::END) {
          upTo = std::min(upTo, member.scorer->advanceShallow(target));
        }
      }
      return upTo;
    }

    float getMaxScore(int32_t upTo) override {
      float sum = 0.0f;
      for (auto& member : members) {
        int32_t doc = member.docId();
        if (doc != solux::PostingsReader::END && doc <= upTo) {
          sum += optionalUpperBound(member.scorer->getMaxScore(upTo));
        }
      }
      return sum;
    }

    float refineMaxScore(int32_t upTo) override {
      float sum = 0.0f;
      for (auto& member : members) {
        int32_t doc = member.docId();
        if (doc != solux::PostingsReader::END && doc <= upTo) {
          sum += optionalUpperBound(member.scorer->refineMaxScore(upTo));
        }
      }
      return sum;
    }

    ScoreBounds scoreBoundsAt(int32_t upTo, bool refined) {
      ScoreBounds sum = ScoreBounds::exact(0.0f);
      bool any = false;
      for (auto& member : members) {
        int32_t doc = member.docId();
        if (doc == solux::PostingsReader::END || doc > upTo) {
          continue;
        }
        ScoreBounds bounds = refined ? member.scorer->refineScoreBounds(upTo)
                                     : member.scorer->getScoreBounds(upTo);
        bounds = optionalScoreBounds(bounds);
        sum = any ? addScoreBounds(sum, bounds) : bounds;
        any = true;
      }
      return any ? sum : ScoreBounds::exact(0.0f);
    }

    ScoreBounds getScoreBounds(int32_t upTo) override {
      return scoreBoundsAt(upTo, false);
    }

    ScoreBounds refineScoreBounds(int32_t upTo) override {
      return scoreBoundsAt(upTo, true);
    }
  }; // DisjunctionScorer


  class MaxScoreDisjunctionScorer final : public Query::Scorer {
    MemPool& pool;
    std::span<Scorer*> scorers;  // stable global-max order, used for deterministic scoring
    std::span<float> clauseMax;
    std::span<float> windowMax;
    std::span<int32_t> windowOrder;
    std::span<bool> isEssential;
    std::span<Scorer*> essentialPointers;
    // Probe order + cumulative bounds for the demoted (non-essential) clauses:
    // neOrder[0..splitIndex) ascending by bound, nePrefix[s] = sum of bounds
    // over neOrder[0..s] (see scoreCurrentDoc).
    std::span<int32_t> neOrder;
    std::span<double> nePrefix;

    // TODO: OPT: heapifying with virtual methods prob isn't a good idea... pull out and save the docid.
    constexpr static auto idComparator = [](Query::Scorer& a, Query::Scorer& b) { return b.docId() < a.docId(); };

    solux::IndirectPQ<Scorer, decltype(idComparator)>* pq = nullptr;

    int32_t maxDoc;
    int32_t windowSize;
    bool globalMode;
    bool windowReady = false;
    int32_t nextWindowStart = 0;
    int32_t windowStart = 0;
    int32_t windowEnd = 0;
    size_t splitIndex = 0;
    float minCompetitiveScore = std::numeric_limits<float>::lowest();
    int32_t docid = -1;
    float currentScore = 0.0f;
    int64_t visitedCandidates = 0;
    // Inflates the double split bound to cover score()'s float-summation rounding:
    // score() can round above the exact double sum of demoted-clause maxima by up to
    // ~(m-1) ULP for m clauses, so demoting purely on the exact sum could skip a doc
    // whose score() rounds > theta.  No-op at theta == lowest().  (scoreCurrentDoc and
    // getMaxScore accumulate in float, which is already monotonically >= score().)
    double scoreBoundFactor = 1.0;
    int64_t nonEssentialLookups = 0;

    static bool lessMaxScore(float a, float b) {
      bool finiteA = std::isfinite(a);
      bool finiteB = std::isfinite(b);
      if (finiteA != finiteB) return finiteA;
      return a < b;
    }

    static int32_t normalizeWindowSize(int32_t requestedWindowSize) {
      return requestedWindowSize > 0 ? requestedWindowSize : DocsEnumMeta::L1_DOCS;
    }

    void sortByClauseMax() {
      for (size_t i = 1; i < scorers.size(); i++) {
        Scorer* scorer = scorers[i];
        float maxScore = clauseMax[i];
        size_t j = i;
        while (j > 0 && lessMaxScore(maxScore, clauseMax[j - 1])) {
          scorers[j] = scorers[j - 1];
          clauseMax[j] = clauseMax[j - 1];
          j--;
        }
        scorers[j] = scorer;
        clauseMax[j] = maxScore;
      }
    }

    bool lessWindowOrder(int32_t a, int32_t b) const {
      return lessMaxScore(windowMax[(size_t) a], windowMax[(size_t) b]);
    }

    void sortWindowOrder() {
      for (size_t i = 1; i < windowOrder.size(); i++) {
        int32_t idx = windowOrder[i];
        size_t j = i;
        while (j > 0 && lessWindowOrder(idx, windowOrder[j - 1])) {
          windowOrder[j] = windowOrder[j - 1];
          j--;
        }
        windowOrder[j] = idx;
      }
    }

    void clearEssentialFlags() {
      for (size_t i = 0; i < isEssential.size(); i++) {
        isEssential[i] = false;
      }
    }

    void rebuildGlobalHeap() {
      clearEssentialFlags();
      size_t essentialCount = scorers.size() - splitIndex;
      for (size_t i = 0; i < essentialCount; i++) {
        isEssential[splitIndex + i] = true;
        essentialPointers[i] = scorers[splitIndex + i];
      }
      double acc = 0.0;
      for (size_t s = 0; s < splitIndex; s++) {
        neOrder[s] = (int32_t) s;  // global order is already ascending clauseMax
        acc += (double) clauseMax[s];
        nePrefix[s] = acc;
      }
      pq = pool.make<solux::IndirectPQ<Scorer, decltype(idComparator)>>(essentialPointers, essentialCount);
    }

    void updateGlobalSplit() {
      // Accumulate the non-essential bound in double so the partition is conservatively
      // sound: a float running sum could round down below the threshold and demote a clause
      // whose exact max-sum still reaches it.  Summing the float clauseMax values in double
      // is exact for any realistic clause count, so a clause is demoted only when the true
      // sum of clause maxima is strictly below the competitive threshold.
      double sum = 0.0;
      size_t newSplit = 0;
      for (; newSplit < scorers.size(); newSplit++) {
        if (!std::isfinite(clauseMax[newSplit])) break;
        double nextSum = sum + (double) clauseMax[newSplit];
        if (!(nextSum * scoreBoundFactor < (double) minCompetitiveScore)) break;
        sum = nextSum;
      }
      if (newSplit > splitIndex) {
        splitIndex = newSplit;
        rebuildGlobalHeap();
      }
    }

    // Largest non-essential prefix whose cumulative window-max stays under the
    // threshold, accumulated in double (conservative, as in updateGlobalSplit).
    size_t computeWindowSplit() const {
      double sum = 0.0;
      size_t s = 0;
      for (; s < scorers.size(); s++) {
        float maxScore = windowMax[(size_t) windowOrder[s]];
        if (!std::isfinite(maxScore)) break;
        double nextSum = sum + (double) maxScore;
        if (!(nextSum * scoreBoundFactor < (double) minCompetitiveScore)) break;
        sum = nextSum;
      }
      return s;
    }

    // Rebuild the essential heap from the window-essential clauses (windowOrder
    // suffix from splitIndex) at their CURRENT positions.
    void rebuildWindowEssentialHeap() {
      clearEssentialFlags();
      size_t essentialCount = 0;
      for (size_t i = splitIndex; i < scorers.size(); i++) {
        int32_t idx = windowOrder[i];
        isEssential[(size_t) idx] = true;
        essentialPointers[essentialCount++] = scorers[(size_t) idx];
      }
      double acc = 0.0;
      for (size_t s = 0; s < splitIndex; s++) {
        neOrder[s] = windowOrder[s];
        acc += (double) windowMax[(size_t) windowOrder[s]];
        nePrefix[s] = acc;
      }
      pq = pool.make<solux::IndirectPQ<Scorer, decltype(idComparator)>>(essentialPointers, essentialCount);
    }

    void setupWindow(int32_t start) {
      windowStart = start;
      int32_t remaining = maxDoc - windowStart;
      windowEnd = remaining > windowSize ? windowStart + windowSize : maxDoc;

      // Setup never moves clause iterators: lagging clauses are advanced by
      // whoever needs them (the essential heap drive, or a per-candidate
      // probe), so demoted clauses that are never probed never advance.
      for (size_t i = 0; i < scorers.size(); i++) {
        int32_t doc = scorers[i]->docId();
        if (doc >= windowEnd) {
          // Iterators only move forward, so 0 is this clause's exact bound
          // for the window, not just a valid upper bound.
          windowMax[i] = 0.0f;
        } else {
          scorers[i]->advanceShallowForSetup(windowStart);
          windowMax[i] = optionalUpperBound(
              scorers[i]->getMaxScoreForSetup(windowEnd - 1));
        }
        windowOrder[i] = (int32_t) i;
      }
      sortWindowOrder();

      splitIndex = computeWindowSplit();
      rebuildWindowEssentialHeap();
      windowReady = true;
    }

    // The threshold rose mid-window: re-partition against the SAME window maxes so
    // a window does not keep driving clauses the now-higher threshold has demoted.
    // splitIndex only advances within a window (theta is monotonic); the next
    // window boundary recomputes it from scratch via setupWindow.
    void updateWindowSplit() {
      size_t newSplit = computeWindowSplit();
      if (newSplit > splitIndex) {
        splitIndex = newSplit;
        rebuildWindowEssentialHeap();
      }
    }

    void scoreCurrentDoc() {
      // Essential clauses sit on the heap at docid: sum them first, then probe
      // the demoted clauses from the largest bound down, abandoning the doc as
      // soon as the unprobed bounds cannot lift it over the threshold (the
      // bulk scorer's scoreCandidate shape).  An abandoned doc keeps its
      // partial sum, which is below the pushed threshold, so the collector
      // discards it. Summation order varies across execution paths, so score
      // bits are not guaranteed identical.
      float sum = 0.0f;
      for (size_t i = 0; i < scorers.size(); i++) {
        if (isEssential[i] && scorers[i]->docId() == docid) {
          sum += scorers[i]->score();
        }
      }
      for (size_t s = splitIndex; s-- > 0; ) {
        if (((double) sum + nePrefix[s]) * scoreBoundFactor < (double) minCompetitiveScore) {
          break;
        }
        auto* scorer = scorers[(size_t) neOrder[s]];
        nonEssentialLookups++;
        if (scorer->docId() < docid) {
          scorer->advance(docid);
        }
        if (scorer->docId() == docid) {
          sum += scorer->score();
        }
      }
      currentScore = sum;
      visitedCandidates++;
    }

    int32_t nextGlobal() {
      int32_t currid = docid;
      for (;;) {
        if (pq->size() == 0) {
          docid = solux::PostingsReader::END;
          currentScore = 0.0f;
          return docid;
        }
        int32_t topDoc = pq->top().docId();
        if (topDoc == solux::PostingsReader::END) {
          pq->removeTop();
          continue;
        }
        if (topDoc <= currid) {
          int32_t nextDoc = pq->top().next();
          if (nextDoc == solux::PostingsReader::END) {
            pq->removeTop();
          } else {
            pq->updateTop();
          }
          continue;
        }
        docid = pq->top().docId();
        scoreCurrentDoc();
        return docid;
      }
    }

    int32_t nextWindowed() {
      for (;;) {
        if (!windowReady) {
          if (nextWindowStart >= maxDoc) {
            docid = solux::PostingsReader::END;
            currentScore = 0.0f;
            return docid;
          }
          setupWindow(nextWindowStart);
        }

        for (;;) {
          if (pq->size() == 0) {
            nextWindowStart = windowEnd;
            windowReady = false;
            break;
          }
          int32_t topDoc = pq->top().docId();
          if (topDoc == solux::PostingsReader::END) {
            pq->removeTop();
            continue;
          }
          if (topDoc < windowStart) {
            topDoc = pq->top().advance(windowStart);
            if (topDoc == solux::PostingsReader::END) {
              pq->removeTop();
            } else {
              pq->updateTop();
            }
            continue;
          }
          if (topDoc >= windowEnd) {
            nextWindowStart = windowEnd;
            windowReady = false;
            break;
          }
          if (topDoc <= docid) {
            int32_t nextDoc = pq->top().next();
            if (nextDoc == solux::PostingsReader::END) {
              pq->removeTop();
            } else {
              pq->updateTop();
            }
            continue;
          }
          docid = topDoc;
          scoreCurrentDoc();
          return docid;
        }
      }
    }

  public:
    // The passed in span of scorers will be modified (rearranged).
    MaxScoreDisjunctionScorer(solux::MemPool& pool, std::span<Scorer*> scorers,
                              int32_t maxDoc, int32_t windowSize = DocsEnumMeta::L1_DOCS)
            : pool(pool),
              scorers(scorers),
              clauseMax(pool.make_arr<float>(scorers.size()), scorers.size()),
              windowMax(pool.make_arr<float>(scorers.size()), scorers.size()),
              windowOrder(pool.make_arr<int32_t>(scorers.size()), scorers.size()),
              isEssential(pool.make_arr<bool>(scorers.size()), scorers.size()),
              essentialPointers(pool.make_arr<Scorer*>(scorers.size()), scorers.size()),
              neOrder(pool.make_arr<int32_t>(scorers.size()), scorers.size()),
              nePrefix(pool.make_arr<double>(scorers.size()), scorers.size()),
              maxDoc(maxDoc),
              windowSize(normalizeWindowSize(windowSize)),
              globalMode(normalizeWindowSize(windowSize) >= maxDoc) {
      // Float-summation error headroom for the double split bound (see member).
      scoreBoundFactor = 1.0 + (double) scorers.size() * 0x1p-24;
      for (size_t i = 0; i < scorers.size(); i++) {
        clauseMax[i] = optionalUpperBound(
            scorers[i]->getMaxScoreForSetup(PostingsReader::END));
      }
      sortByClauseMax();
      for (size_t i = 0; i < scorers.size(); i++) {
        windowOrder[i] = (int32_t) i;
      }
      rebuildGlobalHeap();
    }

    int32_t next() override {
      assert(docid != solux::PostingsReader::END);
      return globalMode ? nextGlobal() : nextWindowed();
    }

    int32_t docId() override {
      return docid;
    }

    float score() override {
      return currentScore;
    }

    void setMinCompetitiveScore(float minScore) override {
      if (minScore > minCompetitiveScore) {
        minCompetitiveScore = minScore;
        if (globalMode) {
          updateGlobalSplit();
        } else if (windowReady) {
          updateWindowSplit();
        }
      }
    }

    float getMaxScore(int32_t upTo) override {
      float sum = 0.0f;
      for (auto* scorer : scorers) {
        float maxScore = optionalUpperBound(scorer->getMaxScore(upTo));
        if (!std::isfinite(maxScore)) {
          return std::numeric_limits<float>::infinity();
        }
        sum += maxScore;
      }
      return sum;
    }

    float refineMaxScore(int32_t upTo) override {
      float sum = 0.0f;
      for (auto* scorer : scorers) {
        float maxScore = optionalUpperBound(scorer->refineMaxScore(upTo));
        if (!std::isfinite(maxScore)) {
          return std::numeric_limits<float>::infinity();
        }
        sum += maxScore;
      }
      return sum;
    }

    ScoreBounds getScoreBounds(int32_t upTo) override {
      return sumOptionalScoreBounds(scorers, upTo, false);
    }

    ScoreBounds refineScoreBounds(int32_t upTo) override {
      return sumOptionalScoreBounds(scorers, upTo, true);
    }

    int64_t visited() const {
      return visitedCandidates;
    }

    int64_t nonEssentialLookupCount() const {
      return nonEssentialLookups;
    }

    int32_t currentSplitIndex() const {
      return (int32_t) splitIndex;
    }

    std::span<Scorer*> clauseScorersForTests() {
      return scorers;
    }
  }; // MaxScoreDisjunctionScorer


  // Exact filtered term disjunctions gather a batch from the filter feed, then
  // sweep each term clause over those monotone candidates. Clause-at-a-time
  // execution preserves each term's resident-block probe state across adjacent
  // candidates, and scoring decodes frequencies only in blocks with survivors.
  class FilteredDisjunctionBulkScorer final : public BulkScorer {
    // Bound candidate and score buffers while amortizing each clause sweep.
    static constexpr int32_t kBatchSize = DocsEnumMeta::L1_DOCS;
    static constexpr int32_t kMatchWords = (kBatchSize + 63) >> 6;

    Query::Scorer* filterScorer;
    TermQuery::Scorer* postingsScorer;
    std::span<const int32_t> filterDocs;
    std::span<TermQuery::Scorer*> termScorers;
    std::span<uint64_t> matchedWords;
    std::span<int32_t> outDocs;
    std::span<float> outScores;
    int32_t maxDoc;
    int32_t nextFilterDoc = -1;
    size_t nextFilterIndex = 0;
    int32_t candidateCount = 0;

    int32_t gatherPostingsCandidates(int32_t min, int32_t max) {
      int32_t upTo = std::min(max, maxDoc);
      PostingsCandidateBatch batch = copyPostingsCandidates(
          postingsScorer->docsEnum, outDocs, nextFilterDoc, min, upTo);
      candidateCount = batch.size;
      nextFilterDoc = batch.nextDoc;
      return nextFilterDoc >= upTo
          ? PostingsReader::END : nextFilterDoc;
    }

    int32_t gatherScorerCandidates(DocSet* externalFilter, int32_t min,
                                   int32_t max) {
      int32_t doc = nextFilterDoc;
      if (doc < min) {
        doc = filterScorer->docId();
        if (doc < min) {
          doc = filterScorer->advance(min);
        }
      }
      candidateCount = 0;
      while (doc < max && doc < maxDoc && candidateCount < kBatchSize) {
        if (externalFilter == nullptr || externalFilter->get(doc)) {
          outDocs[(size_t) candidateCount++] = doc;
        }
        doc = filterScorer->next();
      }
      nextFilterDoc = doc;
      return doc >= max || doc >= maxDoc
          ? PostingsReader::END : doc;
    }

    int32_t gatherArrayCandidates(DocSet* externalFilter, int32_t min,
                                  int32_t max) {
      const int32_t* begin = filterDocs.data() + nextFilterIndex;
      const int32_t* end = filterDocs.data() + filterDocs.size();
      if (begin != end && *begin < min) {
        begin = screaming::gallopLowerBound(begin, end, min);
      }
      int32_t upTo = std::min(max, maxDoc);
      const int32_t* rangeEnd =
          screaming::gallopLowerBound(begin, end, upTo);
      candidateCount = 0;
      if (externalFilter == nullptr) {
        int32_t count = std::min(
            (int32_t) (rangeEnd - begin), kBatchSize);
        std::copy_n(begin, count, outDocs.begin());
        candidateCount = count;
        begin += count;
      } else {
        while (begin != rangeEnd && candidateCount < kBatchSize) {
          int32_t doc = *begin++;
          if (externalFilter->get(doc)) {
            outDocs[(size_t) candidateCount++] = doc;
          }
        }
      }
      nextFilterIndex = (size_t) (begin - filterDocs.data());
      if (begin == end || (begin != end && *begin >= upTo)) {
        return PostingsReader::END;
      }
      return *begin;
    }

    int32_t gatherCandidates(DocSet* externalFilter, int32_t min,
                             int32_t max) {
      if (externalFilter == nullptr && postingsScorer != nullptr
          && !disableFilteredDisjunctionPostingsBlockGatherForTests) {
        return gatherPostingsCandidates(min, max);
      }
      return filterDocs.empty()
          ? gatherScorerCandidates(externalFilter, min, max)
          : gatherArrayCandidates(externalFilter, min, max);
    }

    void fillMatches(bool withScores) {
      size_t words = ((size_t) candidateCount + 63) >> 6;
      std::fill(matchedWords.begin(),
                matchedWords.begin() + (ptrdiff_t) words, 0);
      if (withScores) {
        std::fill(outScores.begin(),
                  outScores.begin() + candidateCount, 0.0f);
        for (auto* scorer : termScorers) {
          scorer->addToCandidates(
              outDocs.data(), outScores.data(), candidateCount,
              matchedWords.first(words));
        }
      } else {
        for (auto* scorer : termScorers) {
          scorer->addMatchesToCandidates(
              outDocs.data(), candidateCount, matchedWords.first(words));
        }
      }
    }

    bool matched(int32_t index) const {
      return (matchedWords[(size_t) (index >> 6)]
              & (1ULL << (index & 63))) != 0;
    }

    void countMatches(int64_t& count, DocSetBuilder* domainOut) {
      // DocSetBuilder requires globally increasing docs. Clause-at-a-time
      // compaction emits term groups instead, so keep the original candidate
      // order whenever a downstream domain is requested.
      if (disableFilteredDisjunctionCountCompactionForTests
          || domainOut != nullptr) {
        fillMatches(false);
        for (int32_t i = 0; i < candidateCount; i++) {
          if (matched(i)) {
            count++;
            if (domainOut != nullptr) {
              domainOut->add(outDocs[(size_t) i]);
            }
          }
        }
        return;
      }

      for (auto* scorer : termScorers) {
        size_t words = ((size_t) candidateCount + 63) >> 6;
        std::fill(matchedWords.begin(),
                  matchedWords.begin() + (ptrdiff_t) words, 0);
        scorer->addMatchesToCandidates(
            outDocs.data(), candidateCount, matchedWords.first(words));
        int32_t write = 0;
        for (int32_t i = 0; i < candidateCount; i++) {
          if (matched(i)) {
            count++;
            if (domainOut != nullptr) {
              domainOut->add(outDocs[(size_t) i]);
            }
          } else {
            outDocs[(size_t) write++] = outDocs[(size_t) i];
          }
        }
        candidateCount = write;
        if (candidateCount == 0) {
          break;
        }
      }
    }

    void emit(ScoreWindow& out, int32_t min, int32_t max, int32_t next,
              bool withScores, float minCompetitiveScore) {
      int32_t write = 0;
      for (int32_t i = 0; i < candidateCount; i++) {
        if (!matched(i)
            || (withScores
                && outScores[(size_t) i] < minCompetitiveScore)) {
          continue;
        }
        if (write != i) {
          outDocs[(size_t) write] = outDocs[(size_t) i];
          outScores[(size_t) write] = outScores[(size_t) i];
        }
        write++;
      }
      out.min = min;
      out.max = next == PostingsReader::END ? max : next;
      out.size = write;
      out.docs = outDocs.first((size_t) write);
      out.scores = outScores.first((size_t) write);
    }

  public:
    FilteredDisjunctionBulkScorer(
        MemPool& pool, Query::Scorer* filterScorer,
        std::span<const int32_t> filterDocs,
        std::span<TermQuery::Scorer*> termScorers, int32_t maxDoc,
        TermQuery::Scorer* postingsScorer)
        : filterScorer(filterScorer), postingsScorer(postingsScorer),
          filterDocs(filterDocs),
          termScorers(termScorers),
          matchedWords(pool.make_span<uint64_t>((size_t) kMatchWords)),
          outDocs(pool.make_span<int32_t>((size_t) kBatchSize)),
          outScores(pool.make_span<float>((size_t) kBatchSize)),
          maxDoc(maxDoc) {
      assert((filterScorer == nullptr) != filterDocs.empty());
      assert(postingsScorer == nullptr || postingsScorer == filterScorer);
      skipCount(SkipStats::filteredDisjBatchEngagements);
      if (postingsScorer != nullptr) {
        skipCount(
            SkipStats::filteredDisjBatchPostingsFeedEngagements);
      }
    }

    bool supportsMatchWindows() const override {
      return true;
    }

    bool supportsExactCandidateScoring() const override {
      return true;
    }

    void scoreCandidatesExact(std::span<int32_t> docs,
                              std::span<float> scores) override {
      assert(docs.size() == scores.size());
      assert(docs.size() <= outDocs.size());
      std::fill(scores.begin(), scores.end(), 0.0f);
      size_t words = (docs.size() + 63) >> 6;
      std::fill(matchedWords.begin(),
                matchedWords.begin() + (ptrdiff_t) words, 0);
      auto matches = matchedWords.first(words);
      for (auto* scorer : termScorers) {
        scorer->addToCandidates(
            docs.data(), scores.data(), (int32_t) docs.size(), matches);
      }
#ifndef NDEBUG
      for (size_t i = 0; i < docs.size(); i++) {
        assert((matches[i >> 6] & (1ULL << (i & 63))) != 0);
      }
#endif
    }

    int32_t scoreNextWindow(ScoreWindow& out, DocSet* filter,
                            int32_t min, int32_t max,
                            float minCompetitiveScore) override {
      out = {.min = min, .max = min, .docs = outDocs, .scores = outScores};
      max = std::min(max, maxDoc);
      if (min >= max) {
        return PostingsReader::END;
      }
      int32_t next = gatherCandidates(filter, min, max);
      if (candidateCount > 0) {
        skipCount(SkipStats::filteredDisjBatchScoreWindows);
        fillMatches(true);
        emit(out, min, max, next, true, minCompetitiveScore);
      }
      return next;
    }

    int32_t matchNextWindow(ScoreWindow& out, DocSet* filter,
                            int32_t min, int32_t max) override {
      out = {.min = min, .max = min, .docs = outDocs, .scores = outScores};
      max = std::min(max, maxDoc);
      if (min >= max) {
        return PostingsReader::END;
      }
      int32_t next = gatherCandidates(filter, min, max);
      if (candidateCount > 0) {
        skipCount(SkipStats::filteredDisjBatchMatchWindows);
        fillMatches(false);
        emit(out, min, max, next, false,
             std::numeric_limits<float>::lowest());
      }
      return next;
    }

    int32_t countNextWindow(int64_t& count, DocSetBuilder* domainOut,
                            DocSet* filter, int32_t min,
                            int32_t max) override {
      max = std::min(max, maxDoc);
      if (min >= max) {
        return PostingsReader::END;
      }
      int32_t next = gatherCandidates(filter, min, max);
      if (candidateCount > 0) {
        skipCount(SkipStats::filteredDisjBatchCountWindows);
        countMatches(count, domainOut);
        if (domainOut != nullptr) {
          skipCount(SkipStats::bulkDomainWindowsFed);
        }
      }
      return next;
    }
  };

  class MaxScoreBulkScorer final : public BulkScorer {
  public:
    static inline bool disableDisjConjBulkForTests = false;

    constexpr static int32_t kWindowSize = DocsEnumMeta::L1_DOCS;
    constexpr static int32_t kWindowWords = kWindowSize / 64;
    constexpr static size_t kBs1MinClauses = 16;
    // The identity avoids streaming the largest postings list by probing it
    // once per doc from the smaller terms. A df ratio alone misprices that
    // trade because dense word-encoded postings stream cheaply. Bound the
    // absolute probe count as well, while retaining a ratio floor so small,
    // balanced unions use ordinary enumeration.
    constexpr static int64_t kDisjunctionCountIdentityMaxProbes = 1024;
    constexpr static int64_t kDisjunctionCountIdentityMinDfRatio = 32;

  private:
    // Drive from the domain when card*nClauses*W < sum(clause.cost()). W models
    // a scalar advance relative to vectorized block decode and is sensitive to
    // both implementations. W_ARRAY is lower because stream-side membership
    // uses ArrDocSet::get instead of a bit lookup.
    constexpr static int64_t W_BITSET = 32;
    constexpr static int64_t W_ARRAY = 28;
    static_assert((kWindowSize % 64) == 0);

    struct CountClause {
      std::span<Query::Scorer*> terms;
    };

    std::span<Query::Scorer*> scorers;  // stable global-max order
    std::span<int64_t> clauseCosts;     // kept aligned with scorers
    std::span<float> clauseMax;
    std::span<float> windowMax;
    std::span<int32_t> windowOrder;
    std::span<bool> isEssential;
    // nonEssentialPrefixMax[s] = sum of windowMax over windowOrder[0..s]: the
    // most the not-yet-swept non-essential clauses can add when sweeps run from
    // s = splitIndex-1 downward.
    std::span<double> nonEssentialPrefixMax;
    std::span<float> compactThresholds;
    std::span<float> compactThresholdMcs;
    std::span<bool> compactThresholdReady;
    std::span<uint64_t> windowBits;
    // One shared per-window score accumulation row for all fill modes.
    // Never cleared: a slot is valid only under a set window bit, and
    // addWindowScore overwrites on the first touch of a window.
    // Clauses add into it in fill order, so the sum's rounding depends on
    // which clauses are essential in a window. Score bits are not guaranteed
    // identical across execution paths.
    std::span<float> windowScores;
    std::span<int32_t> outDocs;
    std::span<float> outScores;
    std::span<CountClause> countClauses;
    std::span<Query::Scorer*> countTerms;
    std::span<uint64_t> countClauseBits;
    std::span<uint64_t> countTermBits;
    std::span<TermQuery::Scorer*> disjCountIdentityOthers;
    TermQuery::Scorer* disjCountIdentityLargest = nullptr;
    int64_t disjCountIdentityLargestDf = 0;
    std::span<Query::Scorer*> exclusionScorers;  // flattened OR
    std::span<uint64_t> exclusionBits;           // current production window
    WindowFilter* windowFilter = nullptr;

    int32_t maxDoc;
    int64_t aggregateClauseCost;
    DocSet* arrayCursorFilter = nullptr;
    size_t arrayCursor = 0;
    int32_t arrayCursorLastWindowStart = -1;
    int32_t windowStart = 0;
    int32_t windowEnd = 0;
    int32_t outerWindowStart = 0;
    int32_t outerWindowEnd = 0;
    bool outerWindowReady = false;
    int64_t numOuterWindows = 0;
    int64_t numCandidates = 0;
    int32_t minWindowSize = 1;
    size_t splitIndex = 0;
    size_t firstRequired = 0;
    int64_t bs1Windows = 0;
    int64_t domainDriveWindows = 0;
    float minCompetitiveScore = std::numeric_limits<float>::lowest();
    float nextPartitionMcs = std::numeric_limits<float>::infinity();
    double scoreBoundFactor = 1.0;
    bool disjConjCountPath = false;

    void configureDisjunctionCountIdentity(solux::MemPool& pool,
                                           bool enabled,
                                           size_t largestIndex) {
      if (!enabled) return;
      assert(clauseCosts.size() == scorers.size());
      assert(largestIndex < scorers.size());
      assert(dynamic_cast<TermQuery::Scorer*>(
          scorers[largestIndex]) != nullptr);

      disjCountIdentityOthers =
          pool.make_span<TermQuery::Scorer*>(scorers.size() - 1);
      size_t other = 0;
      for (size_t i = 0; i < scorers.size(); i++) {
        if (i != largestIndex) {
          auto* term = dynamic_cast<TermQuery::Scorer*>(scorers[i]);
          assert(term != nullptr);
          disjCountIdentityOthers[other++] = term;
        }
      }
      assert(other == disjCountIdentityOthers.size());
      disjCountIdentityLargest =
          static_cast<TermQuery::Scorer*>(scorers[largestIndex]);
      disjCountIdentityLargestDf = clauseCosts[largestIndex];
    }

    // Exact unscored count decomposition is all-or-nothing. Any opaque or
    // non-term member leaves the existing top-level scorer path in control.
    void configureDisjConjCount(solux::MemPool& pool, bool enable) {
      if (!enable || disableDisjConjBulkForTests) {
        return;
      }

      size_t termCount = 0;
      bool hasConjunction = false;
      for (auto* scorer : scorers) {
        if (dynamic_cast<TermQuery::Scorer*>(scorer) != nullptr) {
          termCount++;
          continue;
        }
        auto terms = scorer->flatConjunctionScorers();
        if (terms.empty()) {
          return;
        }
        for (auto* term : terms) {
          if (dynamic_cast<TermQuery::Scorer*>(term) == nullptr) {
            return;
          }
        }
        termCount += terms.size();
        hasConjunction = true;
      }
      if (!hasConjunction) {
        return;
      }

      countClauses = pool.make_span<CountClause>(scorers.size());
      countTerms = pool.make_span<Query::Scorer*>(termCount);
      size_t termIndex = 0;
      for (size_t i = 0; i < scorers.size(); i++) {
        auto* scorer = scorers[i];
        auto terms = scorer->flatConjunctionScorers();
        if (terms.empty()) {
          countTerms[termIndex] = scorer;
          countClauses[i].terms = countTerms.subspan(termIndex, 1);
          termIndex++;
          continue;
        }
        for (auto* term : terms) {
          countTerms[termIndex++] = term;
        }
        countClauses[i].terms = countTerms.subspan(termIndex - terms.size(), terms.size());
      }
      assert(termIndex == countTerms.size());
      countClauseBits = pool.make_span<uint64_t>((size_t) kWindowWords);
      countTermBits = pool.make_span<uint64_t>((size_t) kWindowWords);
      disjConjCountPath = true;
    }

    static bool lessMaxScore(float a, float b) {
      bool finiteA = std::isfinite(a);
      bool finiteB = std::isfinite(b);
      if (finiteA != finiteB) return finiteA;
      return a < b;
    }

    void sortByClauseMax() {
      for (size_t i = 1; i < scorers.size(); i++) {
        Query::Scorer* scorer = scorers[i];
        int64_t cost = clauseCosts.empty() ? 0 : clauseCosts[i];
        float maxScore = clauseMax[i];
        size_t j = i;
        while (j > 0 && lessMaxScore(maxScore, clauseMax[j - 1])) {
          scorers[j] = scorers[j - 1];
          if (!clauseCosts.empty()) {
            clauseCosts[j] = clauseCosts[j - 1];
          }
          clauseMax[j] = clauseMax[j - 1];
          j--;
        }
        scorers[j] = scorer;
        if (!clauseCosts.empty()) {
          clauseCosts[j] = cost;
        }
        clauseMax[j] = maxScore;
      }
    }

    bool canReach(float score, double bound) const {
      return scoreCanReach(score, bound, minCompetitiveScore, scoreBoundFactor);
    }

    static float firstFloatGreaterThan(double value) {
      if (std::isnan(value)) {
        return std::numeric_limits<float>::infinity();
      }
      if (value < (double) std::numeric_limits<float>::lowest()) {
        return std::numeric_limits<float>::lowest();
      }
      if (value == std::numeric_limits<double>::infinity()) {
        return std::numeric_limits<float>::infinity();
      }
      float candidate = (float) value;
      if ((double) candidate <= value) {
        candidate = std::nextafter(candidate, std::numeric_limits<float>::infinity());
      }
      return candidate;
    }

    bool lessWindowOrder(int32_t a, int32_t b) const {
      // Lucene's score-per-cost order prevents a boosted dense clause from
      // becoming the union driver ahead of much cheaper sparse clauses. Keep
      // raw-max order for two and three clauses: there it preserves Solux's
      // single-essential direct-fill path and is faster despite extra probes.
      if (scorers.size() >= kCostAwareOrderMinClauses) {
        float maxA = windowMax[(size_t) a];
        float maxB = windowMax[(size_t) b];
        bool finiteA = std::isfinite(maxA);
        bool finiteB = std::isfinite(maxB);
        if (finiteA != finiteB) return finiteA;
        double ratioA = (double) maxA
          / (double) std::max<int64_t>(1, clauseCosts[(size_t) a]);
        double ratioB = (double) maxB
          / (double) std::max<int64_t>(1, clauseCosts[(size_t) b]);
        return ratioA < ratioB;
      }
      return lessMaxScore(windowMax[(size_t) a], windowMax[(size_t) b]);
    }

    void sortWindowOrder() {
      for (size_t i = 1; i < windowOrder.size(); i++) {
        int32_t idx = windowOrder[i];
        size_t j = i;
        while (j > 0 && lessWindowOrder(idx, windowOrder[j - 1])) {
          windowOrder[j] = windowOrder[j - 1];
          j--;
        }
        windowOrder[j] = idx;
      }
    }

    void clearEssentialFlags() {
      for (size_t i = 0; i < isEssential.size(); i++) {
        isEssential[i] = false;
      }
    }

    size_t computeWindowSplit() const {
      double sum = 0.0;
      size_t s = 0;
      for (; s < scorers.size(); s++) {
        float maxScore = windowMax[(size_t) windowOrder[s]];
        if (!std::isfinite(maxScore)) break;
        double nextSum = sum + (double) maxScore;
        if (canReach(0.0f, nextSum)) break;
        sum = nextSum;
      }
      return s;
    }

    float computeNextPartitionMcs() const {
      double sum = 0.0;
      for (size_t s = 0; s < scorers.size(); s++) {
        float maxScore = windowMax[(size_t) windowOrder[s]];
        if (!std::isfinite(maxScore)) {
          break;
        }
        double nextSum = sum + (double) maxScore;
        if (canReach(0.0f, nextSum)) {
          return firstFloatGreaterThan(nextSum * scoreBoundFactor);
        }
        sum = nextSum;
      }
      return std::numeric_limits<float>::infinity();
    }

    void markEssentialScorers() {
      clearEssentialFlags();
      for (size_t i = splitIndex; i < scorers.size(); i++) {
        isEssential[(size_t) windowOrder[i]] = true;
      }
    }

    bool useBs1ForWindow() const {
      // Dense shared-row fill is useful only when every clause is essential.
      // Once a clause is non-essential, the ordinary path can leave its
      // postings untouched and probe only surviving candidates.
      return scorers.size() >= kBs1MinClauses && splitIndex == 0;
    }

    void updateMaxWindowScores(int32_t start, int32_t end) {
      for (size_t i = 0; i < scorers.size(); i++) {
        int32_t doc = scorers[i]->docId();
        if (doc >= end) {
          windowMax[i] = 0.0f;
        } else {
          scorers[i]->advanceShallowForSetup(start);
          windowMax[i] = optionalUpperBound(
              scorers[i]->getMaxScoreForSetup(end - 1));
        }
        windowOrder[i] = (int32_t) i;
      }
    }

    void promoteRequiredScorers() {
      assert(scorers.size() - splitIndex == 1);
      firstRequired = splitIndex;
      double maxRequiredScore = (double) windowMax[(size_t) windowOrder[splitIndex]];
      // Required promotion is only a sweep-time intersection, not an essential
      // fill change. The invariant is: after promoting windowOrder[firstRequired],
      // any buffer doc that misses that clause cannot reach theta even if it
      // takes every lower non-essential clause. That makes dropping it from the
      // union buffer safe for top-k scoring; count/domain paths do not use this
      // scorer loop.
      while (firstRequired > 0) {
        double bound = maxRequiredScore;
        if (firstRequired > 1) {
          bound += nonEssentialPrefixMax[firstRequired - 2];
        }
        if (canReach(0.0f, bound)) {
          break;
        }
        firstRequired--;
        maxRequiredScore += (double) windowMax[(size_t) windowOrder[firstRequired]];
      }
    }

    void partitionWindow() {
      sortWindowOrder();
      splitIndex = computeWindowSplit();
      nextPartitionMcs = computeNextPartitionMcs();
      markEssentialScorers();
      std::fill(compactThresholdReady.begin(), compactThresholdReady.end(), false);
      double acc = 0.0;
      for (size_t s = 0; s < splitIndex; s++) {
        acc += (double) windowMax[(size_t) windowOrder[s]];
        nonEssentialPrefixMax[s] = acc;
      }
      firstRequired = splitIndex;
      if (scorers.size() - splitIndex == 1) {
        promoteRequiredScorers();
      }
    }

    int32_t computeOuterWindowMax(int32_t start, int32_t max) {
      if (scorers.empty()) return std::min(max, maxDoc);
      size_t firstWindowLead = std::min(splitIndex, scorers.size() - 1);
      int32_t end = PostingsReader::END;
      for (size_t i = firstWindowLead; i < scorers.size(); i++) {
        auto* scorer = scorers[(size_t) windowOrder[i]];
        if (scorer->docId() >= max) continue;
        int32_t target = std::max(scorer->docId(), start);
        int32_t upTo = scorer->advanceShallowForSetup(target);
        if (upTo != PostingsReader::END) {
          end = std::min(end, upTo >= PostingsReader::END - 1 ? PostingsReader::END : upTo + 1);
        }
      }

      if (scorers.size() - firstWindowLead > 1) {
        int64_t threshold = numOuterWindows * 32LL * (int64_t) scorers.size();
        if (numCandidates < threshold) {
          minWindowSize = std::min(minWindowSize << 1, kWindowSize);
        } else {
          minWindowSize = 1;
        }
        int32_t minWindowEnd = start + minWindowSize;
        if (minWindowEnd < start) {
          minWindowEnd = max;
        }
        end = std::max(end, minWindowEnd);
      }

      end = std::min(std::min(end, max), maxDoc);
      if (end <= start) {
        int32_t requestedEnd = start + kWindowSize;
        if (requestedEnd < start) {
          requestedEnd = max;
        }
        end = std::min(std::min(requestedEnd, max), maxDoc);
      }
      return end;
    }

    void setupOuterWindow(int32_t start, int32_t max) {
      skipCount(SkipStats::maxScoreOuterWindows);
      outerWindowStart = start;
      outerWindowEnd = computeOuterWindowMax(start, max);
      for (;;) {
        updateMaxWindowScores(start, outerWindowEnd);
        partitionWindow();
        int32_t nextEnd = computeOuterWindowMax(start, max);
        if (nextEnd >= outerWindowEnd) {
          break;
        }
        outerWindowEnd = nextEnd;
        skipCount(SkipStats::maxScoreOuterWindowRefines);
      }
      outerWindowReady = true;
      numOuterWindows++;
    }

    void setupWindow(int32_t start, int32_t max) {
      if (!outerWindowReady || start < outerWindowStart || start >= outerWindowEnd) {
        setupOuterWindow(start, max);
      }
      windowStart = start;
      int32_t requestedEnd = windowStart + kWindowSize;
      if (requestedEnd < windowStart) {
        requestedEnd = max;
      }
      windowEnd = std::min(std::min(std::min(requestedEnd, outerWindowEnd), max), maxDoc);
      skipCount(SkipStats::maxScoreInnerWindows);
    }

    int32_t consumeOuterWindow(ScoreWindow& out, int32_t max) {
      windowEnd = outerWindowEnd;
      out.min = windowStart;
      out.max = outerWindowEnd;
      out.size = 0;
      return outerWindowEnd >= max ? PostingsReader::END : outerWindowEnd;
    }

    bool positionEssentialScorers(int32_t& top1, int32_t& top2, int32_t& top1Index) {
      top1 = PostingsReader::END;
      top2 = PostingsReader::END;
      top1Index = -1;
      for (size_t i = splitIndex; i < scorers.size(); i++) {
        int32_t scorerIndex = windowOrder[i];
        auto* scorer = scorers[(size_t) scorerIndex];
        int32_t doc = scorer->docId();
        if (doc < windowStart) {
          doc = scorer->advance(windowStart);
        }
        if (doc == PostingsReader::END) {
          continue;
        }
        if (doc < top1) {
          top2 = top1;
          top1 = doc;
          top1Index = scorerIndex;
        } else if (doc < top2) {
          top2 = doc;
        }
      }
      return top1Index >= 0;
    }

    void anchorWindowAt(int32_t top1, int32_t max) {
      if (top1 <= windowStart) {
        return;
      }
      windowStart = top1;
      int32_t requestedEnd = windowStart + kWindowSize;
      if (requestedEnd < windowStart) {
        requestedEnd = max;
      }
      windowEnd = std::min(std::min(std::min(requestedEnd, outerWindowEnd), max), maxDoc);
      skipCount(SkipStats::maxScoreAnchorJumps);
    }

    void clearWindowBits() {
      std::fill(windowBits.begin(), windowBits.end(), 0);
    }

    void prepareExclusionWindow() {
      if (exclusionScorers.empty()) {
        return;
      }
      // This is intentionally not a WindowFilter complement. Exclusions are
      // sparse OR bits consumed with AND-NOT at candidate formation.
      assert(windowEnd >= windowStart);
      assert(windowEnd - windowStart <= kWindowSize);
      std::fill(exclusionBits.begin(), exclusionBits.end(), 0);
      skipCount(SkipStats::bulkExclusionWindows);
      for (Query::Scorer* scorer : exclusionScorers) {
        scorer->fillWindowBits(exclusionBits, windowStart, windowEnd);
        skipCount(SkipStats::bulkExclusionFills);
      }
    }

    bool isExcluded(int32_t doc) const {
      if (exclusionBits.empty()) {
        return false;
      }
      assert(doc >= windowStart && doc < windowEnd);
      int32_t index = doc - windowStart;
      return (exclusionBits[(size_t) (index >> 6)]
              & (1ULL << (index & 63))) != 0;
    }

    void removeExcludedWindowBits(std::span<uint64_t> bits) const {
      if (exclusionBits.empty()) {
        return;
      }
      assert(bits.size() == exclusionBits.size());
      for (size_t word = 0; word < bits.size(); word++) {
        bits[word] &= ~exclusionBits[word];
      }
    }

    static void clearCountBits(std::span<uint64_t> bits) {
      std::fill(bits.begin(), bits.end(), 0);
    }

    int32_t nextDisjunctionCountIdentityDoc(int32_t target) {
      int32_t next = PostingsReader::END;
      for (auto* scorer : disjCountIdentityOthers) {
        int32_t doc = scorer->docId();
        if (doc < target) {
          // Docs-only: the window loop also fills these scorers' bits, which
          // switches their enums to docs-only consumption; a scored advance
          // may not follow that.
          doc = scorer->advanceDocOnly(target);
        }
        next = std::min(next, doc);
      }
      return next;
    }

    int32_t countDisjunctionIdentity(int64_t& count) {
      skipCount(SkipStats::disjCountIdentityEngagements);
      count += disjCountIdentityLargestDf;

      int32_t cursor = 0;
      while (cursor < maxDoc) {
        int32_t windowBase = nextDisjunctionCountIdentityDoc(cursor);
        if (windowBase == PostingsReader::END) {
          break;
        }
        int32_t requestedEnd = windowBase + kWindowSize;
        if (requestedEnd < windowBase) {
          requestedEnd = maxDoc;
        }
        int32_t identityWindowEnd = std::min(requestedEnd, maxDoc);

        clearWindowBits();
        for (auto* scorer : disjCountIdentityOthers) {
          scorer->fillWindowBits(windowBits, windowBase, identityWindowEnd);
        }

        int32_t candidates = 0;
        int32_t innerSize = identityWindowEnd - windowBase;
        for (int32_t word = 0; word < kWindowWords; word++) {
          uint64_t bits = windowBits[(size_t) word];
          while (bits != 0) {
            int32_t bit = (int32_t) std::countr_zero(bits);
            int32_t index = (word << 6) + bit;
            if (index >= innerSize) {
              break;
            }
            outDocs[(size_t) candidates++] = windowBase + index;
            bits &= bits - 1;
          }
        }

        std::fill_n(outScores.data(), candidates, 0.0f);
        int32_t members = disjCountIdentityLargest->applyToCandidates(
            outDocs.data(), outScores.data(), candidates, true);
        count += candidates - members;
        cursor = identityWindowEnd;
      }
      return PostingsReader::END;
    }

    void fillDisjConjCountBits() {
      clearWindowBits();
      for (auto& clause : countClauses) {
        if (clause.terms.size() == 1) {
          clause.terms[0]->fillWindowBits(windowBits, windowStart, windowEnd);
          continue;
        }

        clearCountBits(countClauseBits);
        clause.terms[0]->fillWindowBits(countClauseBits, windowStart, windowEnd);
        for (size_t term = 1; term < clause.terms.size(); term++) {
          clearCountBits(countTermBits);
          clause.terms[term]->fillWindowBits(countTermBits, windowStart, windowEnd);
          for (size_t word = 0; word < countClauseBits.size(); word++) {
            countClauseBits[word] &= countTermBits[word];
          }
        }
        for (size_t word = 0; word < windowBits.size(); word++) {
          windowBits[word] |= countClauseBits[word];
        }
      }
    }

    bool prepareExhaustiveWindow(int32_t min, int32_t max) {
      setWindowBounds(min, max);
      if (windowFilter != nullptr
          && windowFilter->prepare(windowStart, windowEnd) == 0) {
        return false;
      }
      prepareExclusionWindow();
      return true;
    }

    bool fillDisjConjWindowBits(DocSet* filter, int32_t min, int32_t max) {
      if (!prepareExhaustiveWindow(min, max)) {
        return false;
      }
      skipCount(SkipStats::disjConjGroupCountWindows);
      fillDisjConjCountBits();
      if (filter != nullptr && filter->type == DocSet::BITSET) {
        applyDomainBits(&((BitDocSet*) filter)->bits());
      } else if (filter != nullptr) {
        applyDocSetToWindow(windowBits, windowStart, windowEnd, filter);
      }
      if (windowFilter != nullptr) {
        windowFilter->intersect(windowBits);
      }
      removeExcludedWindowBits(windowBits);
      return true;
    }

    bool fillArrayFilterWindowBits(DocSet* filter, int32_t min, int32_t max) {
      assert(filter != nullptr && filter->type == DocSet::ARRAY);
      if (!prepareExhaustiveWindow(min, max)) {
        return false;
      }
      clearWindowBits();
      int32_t blockDocs[Postings::DOCS_BLOCK_SIZE];
      float blockScores[Postings::DOCS_BLOCK_SIZE];
      for (size_t i = 0; i < scorers.size(); i++) {
        auto* scorer = scorers[i];
        if (scorer->docId() < windowStart) {
          scorer->advance(windowStart);
        }
        int32_t n;
        while ((n = scorer->fillScoreBlock(blockDocs, blockScores,
                                           Postings::DOCS_BLOCK_SIZE, windowEnd)) > 0) {
          for (int32_t j = 0; j < n; j++) {
            if (acceptsDoc(filter, nullptr, blockDocs[j])) {
              setWindowBit(blockDocs[j] - windowStart);
            }
          }
        }
      }
      return true;
    }

    bool fillPlainWindowBits(DocSet* filter, int32_t min, int32_t max) {
      assert(filter == nullptr || filter->type == DocSet::BITSET);
      if (!prepareExhaustiveWindow(min, max)) {
        return false;
      }
      clearWindowBits();
      for (size_t i = 0; i < scorers.size(); i++) {
        scorers[i]->fillWindowBits(windowBits, windowStart, windowEnd);
      }
      if (filter != nullptr) {
        applyDomainBits(&((BitDocSet*) filter)->bits());
      }
      if (windowFilter != nullptr) {
        windowFilter->intersect(windowBits);
      }
      removeExcludedWindowBits(windowBits);
      return true;
    }

    bool fillExhaustiveWindowBits(DocSet* filter, int32_t min, int32_t max) {
      if (disjConjCountPath) {
        return fillDisjConjWindowBits(filter, min, max);
      }
      if (filter != nullptr && filter->type == DocSet::ARRAY) {
        return fillArrayFilterWindowBits(filter, min, max);
      }
      return fillPlainWindowBits(filter, min, max);
    }

    void emitWindowBits(ScoreWindow& out) {
      int32_t innerSize = windowEnd - windowStart;
      for (int32_t word = 0; word < kWindowWords; word++) {
        uint64_t bits = windowBits[(size_t) word];
        while (bits != 0) {
          int32_t bit = (int32_t) std::countr_zero(bits);
          int32_t index = (word << 6) + bit;
          if (index >= innerSize) {
            break;
          }
          out.docs[(size_t) out.size] = windowStart + index;
          out.size++;
          bits &= bits - 1;
        }
      }
    }

    void setWindowBit(int32_t index) {
      windowBits[(size_t) (index >> 6)] |= 1ULL << (index & 63);
    }

    void addWindowScore(int32_t index, float score) {
      uint64_t& word = windowBits[(size_t) (index >> 6)];
      uint64_t mask = 1ULL << (index & 63);
      if ((word & mask) == 0) {
        word |= mask;
        windowScores[(size_t) index] = score;
      } else {
        windowScores[(size_t) index] += score;
      }
    }

    bool acceptsDoc(DocSet* filter, const FixedBitSet* domainBits, int32_t doc) const {
      if (windowFilter != nullptr && !windowFilter->accepts(doc)) {
        return false;
      }
      if (isExcluded(doc)) {
        return false;
      }
      if (filter == nullptr) {
        return true;
      }
      if (domainBits != nullptr) {
        return domainBits->get(doc);
      }
      // ARRAY correctness fallback. Domain-driven execution consumes its
      // sorted span directly; other routes may use this binary-search lookup.
      return filter->get(doc);
    }

    void applyDomainBits(const FixedBitSet* domainBits) {
      applyDomainBitsToWindow(windowBits, windowStart, windowEnd, domainBits);
    }

    bool verifyMatch(Query::Scorer* scorer, int32_t doc) const {
      unused(scorer, doc);
      // Reserved for two-phase: approximation hits will call matches() here.
      return true;
    }

    bool shouldDriveFromDomain(DocSet* filter) {
      if (disableBulkDomainDriveForTests || filter == nullptr) {
        return false;
      }
      int64_t card = (int64_t) filter->card();
      if (card == 0 || aggregateClauseCost <= 0) {
        return false;
      }
      int64_t weight = filter->type == DocSet::BITSET ? W_BITSET : W_ARRAY;
      int64_t nClauses = (int64_t) scorers.size();
      if (nClauses <= 0 || nClauses > std::numeric_limits<int64_t>::max() / weight) {
        return false;
      }
      int64_t scale = nClauses * weight;
      // Drive cost is roughly card*nClauses scalar advances. Stream cost is
      // roughly sum(clause.cost()) vectorized decodes. ARRAY gets a lower W
      // because stream-side membership is an ArrDocSet::get binary search,
      // while BITSET membership is a bit lookup.
      return card <= (aggregateClauseCost - 1) / scale;
    }

    // Window bounds only - none of setupWindow's max-score machinery. Used by
    // the domain-driven and counting paths, which never consult impacts.
    void setWindowBounds(int32_t start, int32_t max) {
      windowStart = start;
      int32_t requestedEnd = windowStart + kWindowSize;
      if (requestedEnd < windowStart) {
        requestedEnd = max;
      }
      windowEnd = std::min(std::min(requestedEnd, max), maxDoc);
    }

    bool scoreDomainDoc(int32_t doc, float& score) {
      float sum = 0.0f;
      bool matched = false;
      for (size_t i = 0; i < scorers.size(); i++) {
        auto* scorer = scorers[i];
        if (scorer->docId() < doc) {
          scorer->advance(doc);
        }
        if (scorer->docId() == doc && verifyMatch(scorer, doc)) {
          sum += scorer->score();
          matched = true;
        }
      }
      score = sum;
      return matched;
    }

    void resetArrayCursorIfNeeded(DocSet* filter) {
      if (filter != arrayCursorFilter || windowStart <= arrayCursorLastWindowStart) {
        arrayCursorFilter = filter;
        arrayCursor = 0;
      }
      arrayCursorLastWindowStart = windowStart;
    }

    void collectDomainDoc(ScoreWindow& out, int32_t doc) {
      float score = 0.0f;
      if (!scoreDomainDoc(doc, score)) {
        return;
      }
      if (score >= minCompetitiveScore) {
        assert(out.size < kWindowSize);
        out.docs[(size_t) out.size] = doc;
        out.scores[(size_t) out.size] = score;
        out.size++;
      }
    }

    // Visit every domain doc in the current window in order.
    template <typename PerDoc>
    void forEachDomainDoc(DocSet* filter, PerDoc&& perDoc) {
      if (filter->type == DocSet::ARRAY) {
        ArrDocSet* arrDocs = (ArrDocSet*) filter;
        auto docs = arrDocs->docs();
        resetArrayCursorIfNeeded(filter);
        if (arrayCursor < docs.size()) {
          const int32_t* base = docs.data();
          const int32_t* it = screaming::gallopLowerBound(
            base + arrayCursor, base + docs.size(), windowStart);
          arrayCursor = (size_t)(it - base);
        }
        while (arrayCursor < docs.size() && docs[arrayCursor] < windowEnd) {
          int32_t doc = docs[arrayCursor];
          if ((windowFilter == nullptr || windowFilter->accepts(doc))
              && !isExcluded(doc)) {
            perDoc(doc);
          }
          arrayCursor++;
        }
        return;
      }

      assert(filter->type == DocSet::BITSET);
      const FixedBitSet& bits = ((BitDocSet*) filter)->bits();
      int32_t doc = windowStart - 1;
      while (doc + 1 < windowEnd) {
        doc = bits.nextSetBit(doc + 1);
        if (doc >= windowEnd) {
          break;
        }
        if ((windowFilter == nullptr || windowFilter->accepts(doc))
            && !isExcluded(doc)) {
          perDoc(doc);
        }
      }
    }

    void fillDomainDrivenCandidates(ScoreWindow& out, DocSet* filter) {
      prepareOutputWindow(out);
      forEachDomainDoc(filter, [&](int32_t doc) { collectDomainDoc(out, doc); });
    }

    // Membership-only variant of scoreDomainDoc: stops at the first matching
    // clause instead of advancing and scoring all of them.
    bool matchesAnyClause(int32_t doc) {
      for (size_t i = 0; i < scorers.size(); i++) {
        auto* scorer = scorers[i];
        if (scorer->docId() < doc) {
          scorer->advance(doc);
        }
        if (scorer->docId() == doc && verifyMatch(scorer, doc)) {
          return true;
        }
      }
      return false;
    }

    // Essential clauses drive via the block-fill API rather than per-doc
    // next()/score(): whole decoded blocks land at once and text terms score
    // through the vectorized flat-norms kernel. Scores may differ from the
    // doc-at-a-time paths in the last bit due to vectorized versus scalar
    // rounding.
    // No clause-level threshold is ever pushed by this bulk scorer, so the
    // block fills below cannot skip docs.
    void fillEssentialCandidates(DocSet* filter, const FixedBitSet* domainBits) {
      clearWindowBits();
      int32_t blockDocs[Postings::DOCS_BLOCK_SIZE];
      float blockScores[Postings::DOCS_BLOCK_SIZE];
      for (size_t i = 0; i < scorers.size(); i++) {
        if (!isEssential[i]) {
          continue;
        }
        auto* scorer = scorers[i];
        if (scorer->docId() < windowStart) {
          scorer->advance(windowStart);
        }
        int32_t n;
        while ((n = scorer->fillScoreBlock(blockDocs, blockScores,
                                           Postings::DOCS_BLOCK_SIZE, windowEnd)) > 0) {
          for (int32_t j = 0; j < n; j++) {
            int32_t doc = blockDocs[j];
            if (acceptsDoc(filter, domainBits, doc)) {
              int32_t index = doc - windowStart;
              addWindowScore(index, blockScores[j]);
            }
          }
        }
      }
    }

    void fillBs1Candidates() {
      clearWindowBits();
      int32_t blockDocs[Postings::DOCS_BLOCK_SIZE];
      float blockScores[Postings::DOCS_BLOCK_SIZE];
      for (size_t i = 0; i < scorers.size(); i++) {
        auto* scorer = scorers[i];
        int32_t doc = scorer->docId();
        if (doc < windowStart) {
          scorer->advance(windowStart);
        }
        int32_t count = 0;
        while ((count = scorer->fillScoreBlock(
                    blockDocs, blockScores, Postings::DOCS_BLOCK_SIZE, windowEnd)) > 0) {
          for (int32_t j = 0; j < count; j++) {
            int32_t index = blockDocs[j] - windowStart;
            addWindowScore(index, blockScores[j]);
          }
        }
      }
    }

    void prepareOutputWindow(ScoreWindow& out) {
      out.min = windowStart;
      out.max = windowEnd;
      out.size = 0;
      out.docs = outDocs;
      out.scores = outScores;
    }

    bool prepareFilterWindow(ScoreWindow& out) {
      if (windowFilter != nullptr
          && windowFilter->prepare(windowStart, windowEnd) == 0) {
        prepareOutputWindow(out);
        return false;
      }
      prepareExclusionWindow();
      return true;
    }

    void recordBufferDrops(int32_t before, int32_t after) {
      if (SkipStats::enabled && after < before) {
        SkipStats::maxScoreBufferCompactions += (int64_t) (before - after);
      }
    }

    float cachedCompetitiveThreshold(size_t sweepLevel) {
      bool refresh = compactThresholdReady[sweepLevel];
      if (!refresh || compactThresholdMcs[sweepLevel] != minCompetitiveScore) {
        // A stale threshold from a lower mcs in this partition is the ideal
        // bisection seed: the exact result moves at most a few ulps.
        compactThresholds[sweepLevel] =
            refresh
              ? competitiveScoreThreshold(minCompetitiveScore, scoreBoundFactor,
                                          nonEssentialPrefixMax[sweepLevel],
                                          (double) compactThresholds[sweepLevel])
              : competitiveScoreThreshold(minCompetitiveScore, scoreBoundFactor,
                                          nonEssentialPrefixMax[sweepLevel]);
        if (refresh) {
          skipCount(SkipStats::maxScoreThresholdRefreshes);
        }
        compactThresholdMcs[sweepLevel] = minCompetitiveScore;
        compactThresholdReady[sweepLevel] = true;
      }
      return compactThresholds[sweepLevel];
    }

    int32_t compactCompetitive(ScoreWindow& out, size_t sweepLevel) {
      float threshold = cachedCompetitiveThreshold(sweepLevel);
      int32_t write = compactByScoreThreshold(out.docs.data(), out.scores.data(),
                                              out.size, threshold);
      if (SkipStats::enabled) {
        SkipStats::maxScoreCompactionInput += out.size;
        SkipStats::maxScoreCompactionKept += write;
        if (sweepLevel < SkipStats::MAX_SWEEP_LEVELS) {
          SkipStats::maxScoreCompactionInputByLevel[sweepLevel] += out.size;
          SkipStats::maxScoreCompactionKeptByLevel[sweepLevel] += write;
        }
      }
      recordBufferDrops(out.size, write);
      return write;
    }

    void finishCompetitive(ScoreWindow& out) {
      int32_t before = out.size;
      out.size = compactByScoreNotLessThanThreshold(out.docs.data(), out.scores.data(),
                                                    out.size, minCompetitiveScore);
      if (SkipStats::enabled && splitIndex > 0) {
        SkipStats::maxScoreFinalCompactionInput += before;
        SkipStats::maxScoreFinalCompactionKept += out.size;
      }
    }

    void applyNonEssentialSweeps(ScoreWindow& out) {
      if (splitIndex > 0) {
        skipCount(SkipStats::maxScoreSweepWindows);
        if (SkipStats::enabled) {
          SkipStats::maxScoreSweepCalls += 1;
          SkipStats::maxScoreSweepEntryCandidates += out.size;
        }
      }
      // The buffer is sorted and already contains only accepted essential
      // candidates. Sweeping one non-essential clause at a time keeps each child
      // scorer monotone through the window; compactCompetitive runs before each
      // sweep and guarantees every removed doc cannot reach theta even if all
      // remaining unswept clauses hit their window maxima.
      for (size_t s = splitIndex; s-- > 0; ) {
        out.size = compactCompetitive(out, s);
        if (out.size == 0) {
          return;
        }
        bool required = s >= firstRequired;
        if (required) {
          skipCount(SkipStats::maxScoreRequiredSweeps);
        }
        int32_t before = out.size;
        auto* scorer = scorers[(size_t) windowOrder[s]];
        const int64_t advancesBefore = SkipStats::applyToCandidatesAdvances;
        const int64_t matchesBefore = SkipStats::applyToCandidatesMatches;
        if (SkipStats::enabled) {
          SkipStats::maxScoreProbeCandidates += out.size;
          if (required) {
            SkipStats::maxScoreRequiredProbeCandidates += out.size;
          } else {
            SkipStats::maxScoreOptionalProbeCandidates += out.size;
          }
          if (s < SkipStats::MAX_SWEEP_LEVELS) {
            SkipStats::maxScoreProbeCandidatesByLevel[s] += out.size;
          }
        }
        out.size = scorer->applyToCandidates(out.docs.data(), out.scores.data(),
                                             out.size, required);
        if (SkipStats::enabled) {
          const int64_t advances = SkipStats::applyToCandidatesAdvances - advancesBefore;
          const int64_t matches = SkipStats::applyToCandidatesMatches - matchesBefore;
          if (required) {
            SkipStats::maxScoreRequiredMatches += matches;
          } else {
            SkipStats::maxScoreOptionalMatches += matches;
          }
          if (s < SkipStats::MAX_SWEEP_LEVELS) {
            SkipStats::maxScoreAdvancesByLevel[s] += advances;
            SkipStats::maxScoreMatchesByLevel[s] += matches;
          }
        }
        if (required) {
          recordBufferDrops(before, out.size);
        }
      }
      finishCompetitive(out);
    }

    void finalizeCandidates(ScoreWindow& out) {
      prepareOutputWindow(out);

      int32_t innerSize = windowEnd - windowStart;
      for (int32_t word = 0; word < kWindowWords; word++) {
        uint64_t bits = windowBits[(size_t) word];
        while (bits != 0) {
          int32_t bit = (int32_t) std::countr_zero(bits);
          int32_t index = (word << 6) + bit;
          if (index >= innerSize) {
            break;
          }
          int32_t doc = windowStart + index;
          out.docs[(size_t) out.size] = doc;
          out.scores[(size_t) out.size] = windowScores[(size_t) index];
          out.size++;
          bits &= bits - 1;
        }
      }
      applyNonEssentialSweeps(out);
    }

    // With one essential clause each candidate surfaces exactly once, so its
    // blocks stream straight to the candidate buffer with no bitset/score-row
    // accumulation pass. The non-essential side is still swept clause-at-a-time
    // over the whole buffer rather than probed per candidate.
    void fillSingleEssentialCandidates(ScoreWindow& out, DocSet* filter,
                                       const FixedBitSet* domainBits,
                                       int32_t essentialIdx) {
      prepareOutputWindow(out);
      auto* scorer = scorers[(size_t) essentialIdx];
      if (scorer->docId() < windowStart) {
        scorer->advance(windowStart);
      }

      if (filter == nullptr && windowFilter == nullptr
          && exclusionScorers.empty()) {
        int32_t n;
        while ((n = scorer->fillScoreBlock(out.docs.data() + out.size,
                                           out.scores.data() + out.size,
                                           kWindowSize - out.size, windowEnd)) > 0) {
          skipCount(SkipStats::maxScoreDirectFills);
          out.size += n;
          assert(out.size <= kWindowSize);
        }
        applyNonEssentialSweeps(out);
        return;
      }

      int32_t blockDocs[Postings::DOCS_BLOCK_SIZE];
      float blockScores[Postings::DOCS_BLOCK_SIZE];
      int32_t n;
      while ((n = scorer->fillScoreBlock(blockDocs, blockScores,
                                         Postings::DOCS_BLOCK_SIZE, windowEnd)) > 0) {
        for (int32_t j = 0; j < n; j++) {
          int32_t doc = blockDocs[j];
          if (!acceptsDoc(filter, domainBits, doc)) {
            continue;
          }
          assert(out.size < kWindowSize);
          out.docs[(size_t) out.size] = doc;
          out.scores[(size_t) out.size] = blockScores[j];
          out.size++;
        }
      }
      applyNonEssentialSweeps(out);
    }

    void finalizeBs1Candidates(ScoreWindow& out, DocSet* filter, const FixedBitSet* domainBits) {
      prepareOutputWindow(out);

      int32_t innerSize = windowEnd - windowStart;
      for (int32_t word = 0; word < kWindowWords; word++) {
        uint64_t bits = windowBits[(size_t) word];
        while (bits != 0) {
          int32_t bit = (int32_t) std::countr_zero(bits);
          int32_t index = (word << 6) + bit;
          if (index >= innerSize) {
            break;
          }
          int32_t doc = windowStart + index;
          // Shared-row fill defers filter membership to one check per candidate.
          // Selective filters may accumulate rejected docs, but this mode is
          // restricted to dense, all-essential windows.
          if (acceptsDoc(filter, domainBits, doc)) {
            float score = windowScores[(size_t) index];
            if (score >= minCompetitiveScore) {
              out.docs[(size_t) out.size] = doc;
              out.scores[(size_t) out.size] = score;
              out.size++;
            }
          }
          bits &= bits - 1;
        }
      }
    }

  public:
    // The passed in span of scorers will be modified (rearranged).
    MaxScoreBulkScorer(solux::MemPool& pool, std::span<Query::Scorer*> scorers,
                       std::span<int64_t> clauseCosts,
                       std::span<Query::Scorer*> exclusionScorers,
                       int32_t maxDoc, int64_t aggregateClauseCost,
                       bool enableDisjConjCount,
                       bool enableDisjunctionCountIdentity,
                       size_t identityLargestIndex)
            : scorers(scorers),
              clauseCosts(clauseCosts),
              clauseMax(pool.make_arr<float>(scorers.size()), scorers.size()),
              windowMax(pool.make_arr<float>(scorers.size()), scorers.size()),
              windowOrder(pool.make_arr<int32_t>(scorers.size()), scorers.size()),
              isEssential(pool.make_arr<bool>(scorers.size()), scorers.size()),
              nonEssentialPrefixMax(pool.make_arr<double>(scorers.size()), scorers.size()),
              compactThresholds(pool.make_arr<float>(scorers.size()), scorers.size()),
              compactThresholdMcs(pool.make_arr<float>(scorers.size()), scorers.size()),
              compactThresholdReady(pool.make_arr<bool>(scorers.size()), scorers.size()),
              windowBits(pool.make_arr<uint64_t>((size_t) kWindowWords), (size_t) kWindowWords),
              windowScores(pool.make_arr<float>((size_t) kWindowSize), (size_t) kWindowSize),
              outDocs(pool.make_arr<int32_t>((size_t) kWindowSize), (size_t) kWindowSize),
              outScores(pool.make_arr<float>((size_t) kWindowSize), (size_t) kWindowSize),
              exclusionScorers(exclusionScorers),
              exclusionBits(
                  exclusionScorers.empty()
                    ? std::span<uint64_t>{}
                    : pool.make_span<uint64_t>((size_t) kWindowWords)),
              maxDoc(maxDoc),
              aggregateClauseCost(aggregateClauseCost) {
      assert(clauseCosts.empty() || scorers.size() == clauseCosts.size());
      scoreBoundFactor = 1.0 + (double) scorers.size() * 0x1p-24;
      for (size_t i = 0; i < scorers.size(); i++) {
        clauseMax[i] = optionalUpperBound(
            scorers[i]->getMaxScoreForSetup(PostingsReader::END));
      }
      sortByClauseMax();
      for (size_t i = 0; i < scorers.size(); i++) {
        windowOrder[i] = (int32_t) i;
      }
      configureDisjConjCount(pool, enableDisjConjCount);
      configureDisjunctionCountIdentity(
          pool, enableDisjunctionCountIdentity, identityLargestIndex);
    }

    bool supportsMatchWindows() const override {
      return true;
    }

    bool attachWindowFilter(WindowFilter* filter) override {
      assert(windowFilter == nullptr);
      windowFilter = filter;
      return true;
    }

    int32_t scoreNextWindow(ScoreWindow& out, DocSet* filter, int32_t min, int32_t max,
                            float minCompetitiveScore) override {
      out.min = min;
      out.max = min;
      out.size = 0;
      out.docs = outDocs;
      out.scores = outScores;

      max = std::min(max, maxDoc);
      if (min >= max) {
        return PostingsReader::END;
      }
      if (minCompetitiveScore > this->minCompetitiveScore) {
        this->minCompetitiveScore = minCompetitiveScore;
        bool insideOuterWindow = outerWindowReady && min >= outerWindowStart && min < outerWindowEnd;
        if (!insideOuterWindow || disablePartitionLatchForTests
            || minCompetitiveScore >= nextPartitionMcs) {
          if (insideOuterWindow && !disablePartitionLatchForTests) {
            skipCount(SkipStats::maxScorePartitionLatchBreaks);
          }
          outerWindowReady = false;
        } else {
          skipCount(SkipStats::maxScorePartitionLatchReuses);
        }
      }

      if (filter != nullptr && filter->card() == 0) {
        return PostingsReader::END;
      }
      if (shouldDriveFromDomain(filter)) {
        setWindowBounds(min, max);
        domainDriveWindows++;
        if (!prepareFilterWindow(out)) {
          return windowEnd >= max ? PostingsReader::END : windowEnd;
        }
        fillDomainDrivenCandidates(out, filter);
        return windowEnd >= max ? PostingsReader::END : windowEnd;
      }

      const FixedBitSet* domainBits = nullptr;
      if (filter != nullptr && filter->type == DocSet::BITSET) {
        domainBits = &((BitDocSet*) filter)->bits();
      }

      setupWindow(min, max);
      if (splitIndex < scorers.size() && useBs1ForWindow()) {
        if (!prepareFilterWindow(out)) {
          return windowEnd >= max ? PostingsReader::END : windowEnd;
        }
        bs1Windows++;
        fillBs1Candidates();
        finalizeBs1Candidates(out, filter, domainBits);
      } else if (!disableWindowDispatchForTests && splitIndex == scorers.size()) {
        skipCount(SkipStats::maxScoreDeadOuterJumps);
        int32_t next = consumeOuterWindow(out, max);
        numCandidates += out.size;
        return next;
      } else if (splitIndex < scorers.size()) {
        if (!disableWindowDispatchForTests) {
          int32_t top1 = PostingsReader::END;
          int32_t top2 = PostingsReader::END;
          int32_t top1Index = -1;
          if (!positionEssentialScorers(top1, top2, top1Index) || top1 >= outerWindowEnd) {
            int32_t next = consumeOuterWindow(out, max);
            numCandidates += out.size;
            return next;
          }
          anchorWindowAt(top1, max);
          if (scorers.size() - splitIndex == 1) {
            if (!prepareFilterWindow(out)) {
              return windowEnd >= max ? PostingsReader::END : windowEnd;
            }
            fillSingleEssentialCandidates(out, filter, domainBits, top1Index);
          } else if (top2 >= windowEnd) {
            skipCount(SkipStats::maxScoreTop2Conversions);
            if (!prepareFilterWindow(out)) {
              return windowEnd >= max ? PostingsReader::END : windowEnd;
            }
            fillSingleEssentialCandidates(out, filter, domainBits, top1Index);
          } else if (top2 - kWindowSize / 2 >= top1) {
            windowEnd = std::min(windowEnd, top2);
            skipCount(SkipStats::maxScoreHalfWindowClips);
            if (!prepareFilterWindow(out)) {
              return windowEnd >= max ? PostingsReader::END : windowEnd;
            }
            fillSingleEssentialCandidates(out, filter, domainBits, top1Index);
          } else {
            if (!prepareFilterWindow(out)) {
              return windowEnd >= max ? PostingsReader::END : windowEnd;
            }
            fillEssentialCandidates(filter, domainBits);
            finalizeCandidates(out);
          }
        } else if (scorers.size() - splitIndex == 1) {
          if (!prepareFilterWindow(out)) {
            return windowEnd >= max ? PostingsReader::END : windowEnd;
          }
          fillSingleEssentialCandidates(out, filter, domainBits, windowOrder[splitIndex]);
        } else {
          if (!prepareFilterWindow(out)) {
            return windowEnd >= max ? PostingsReader::END : windowEnd;
          }
          fillEssentialCandidates(filter, domainBits);
          finalizeCandidates(out);
        }
      } else {
        clearWindowBits();
        out.min = windowStart;
        out.max = windowEnd;
      }

      numCandidates += out.size;
      return windowEnd >= max ? PostingsReader::END : windowEnd;
    }

    int32_t matchNextWindow(ScoreWindow& out, DocSet* filter,
                            int32_t min, int32_t max) override {
      out.min = min;
      out.max = min;
      out.size = 0;
      out.docs = outDocs;
      out.scores = outScores;

      max = std::min(max, maxDoc);
      if (min >= max) {
        return PostingsReader::END;
      }
      if (filter != nullptr && filter->card() == 0) {
        return PostingsReader::END;
      }
      if (shouldDriveFromDomain(filter)) {
        setWindowBounds(min, max);
        domainDriveWindows++;
        prepareOutputWindow(out);
        if (windowFilter != nullptr
            && windowFilter->prepare(windowStart, windowEnd) == 0) {
          return windowEnd >= max ? PostingsReader::END : windowEnd;
        }
        forEachDomainDoc(filter, [&](int32_t doc) {
          if (matchesAnyClause(doc)) {
            assert(out.size < kWindowSize);
            out.docs[(size_t) out.size] = doc;
            out.size++;
          }
        });
        return windowEnd >= max ? PostingsReader::END : windowEnd;
      }

      bool windowReady = fillExhaustiveWindowBits(filter, min, max);
      prepareOutputWindow(out);
      if (windowReady) {
        emitWindowBits(out);
      }
      return windowEnd >= max ? PostingsReader::END : windowEnd;
    }

    // Exhaustive window count: per-clause block drives OR into the window
    // bitset, then popcount. No score rows, no impact bookkeeping, no
    // candidate materialization - counting needs none of them. Only used for
    // count-only collection, where nothing ever raises a clause's competitive
    // threshold, so the block fills below cannot skip.
    int32_t countNextWindow(int64_t& count, DocSetBuilder* domainOut,
                            DocSet* filter, int32_t min, int32_t max) override {
      max = std::min(max, maxDoc);
      if (min >= max) {
        return PostingsReader::END;
      }
      if (filter != nullptr && filter->card() == 0) {
        return PostingsReader::END;
      }
      if (disjCountIdentityLargest != nullptr
          && (filter != nullptr || domainOut != nullptr)) {
        if (filter != nullptr) {
          skipCount(SkipStats::disjCountIdentityFilterFallbacks);
        }
        if (domainOut != nullptr) {
          skipCount(SkipStats::disjCountIdentityDomainOutputFallbacks);
        }
        // The caller will resume this scorer window by window. Latch onto the
        // ordinary enumeration path so each fallback is counted once.
        disjCountIdentityLargest = nullptr;
      }
      if (disjCountIdentityLargest != nullptr
          && min == 0 && max == maxDoc) {
        return countDisjunctionIdentity(count);
      }
      if (domainOut != nullptr) {
        skipCount(SkipStats::bulkDomainWindowsFed);
      }
      if (shouldDriveFromDomain(filter)) {
        setWindowBounds(min, max);
        domainDriveWindows++;
        if (windowFilter != nullptr
            && windowFilter->prepare(windowStart, windowEnd) == 0) {
          return windowEnd >= max ? PostingsReader::END : windowEnd;
        }
        forEachDomainDoc(filter, [&](int32_t doc) {
          if (matchesAnyClause(doc)) {
            count++;
            if (domainOut != nullptr) {
              domainOut->add(doc);
            }
          }
        });
        return windowEnd >= max ? PostingsReader::END : windowEnd;
      }
      if (!fillExhaustiveWindowBits(filter, min, max)) {
        return windowEnd >= max ? PostingsReader::END : windowEnd;
      }
      int32_t wordCard = 0;
      for (size_t w = 0; w < windowBits.size(); w++) {
        wordCard += (int32_t) std::popcount(windowBits[w]);
      }
      if (domainOut != nullptr && wordCard != 0) {
        domainOut->addWindowWords(
            windowBits.data(), windowStart, windowEnd, wordCard);
      }
      count += wordCard;
      return windowEnd >= max ? PostingsReader::END : windowEnd;
    }

    int64_t bs1WindowCount() const {
      return bs1Windows;
    }

    int64_t domainDriveWindowCount() const {
      return domainDriveWindows;
    }

    float nextPartitionMcsForTests() const {
      return nextPartitionMcs;
    }

    int32_t outerWindowEndForTests() const {
      return outerWindowEnd;
    }

    bool usesCostAwareWindowOrderForTests() const {
      return scorers.size() >= kCostAwareOrderMinClauses;
    }
  }; // MaxScoreBulkScorer


  class MinShouldMatchWandScorer final : public Query::Scorer {
    std::span<Scorer*> scorers;
    std::span<float> clauseMax;
    std::span<int32_t> head;
    std::span<int32_t> tail;
    std::span<int32_t> lead;
    int32_t minMatch;
    int32_t headSize = 0;
    int32_t tailSize = 0;
    int32_t leadSize = 0;
    double tailMaxScore = 0.0;
    double leadMaxScore = 0.0;
    float minCompetitiveScore = std::numeric_limits<float>::lowest();
    int32_t docid = -1;
    float currentScore = 0.0f;
    bool scoreReady = false;
    int64_t visitedCandidates = 0;
    // Inflates the pivot's double max-sum so it is a true UPPER bound on the float
    // score() returned for a candidate.  score() is a sequential float accumulation
    // of matching clause scores, which can round above the exact double sum of the
    // per-clause maxima by up to ~(m-1) ULP for m clauses; comparing the exact sum
    // against theta could otherwise skip a doc whose score() rounds > theta.
    double scoreBoundFactor = 1.0;

    // True when no doc the (lead+tail) maxima could produce can be competitive,
    // accounting for score()'s float-summation rounding.  +inf maxima (non-impact
    // clauses) and theta == lowest() both make this false, so pruning never engages
    // there - the scorer degrades to exhaustive.
    bool maxScoreBelowThreshold(double maxSum) const {
      return maxSum * scoreBoundFactor < (double) minCompetitiveScore;
    }

    bool headLess(int32_t a, int32_t b) const {
      int32_t docA = scorers[(size_t) a]->docId();
      int32_t docB = scorers[(size_t) b]->docId();
      if (docA != docB) return docA > docB;
      return a > b;
    }

    bool tailLess(int32_t a, int32_t b) const {
      float maxA = clauseMax[(size_t) a];
      float maxB = clauseMax[(size_t) b];
      if (maxA != maxB) return maxA < maxB;
      return a > b;
    }

    bool tailGreater(int32_t a, int32_t b) const {
      float maxA = clauseMax[(size_t) a];
      float maxB = clauseMax[(size_t) b];
      if (maxA != maxB) return maxA > maxB;
      return a < b;
    }

    void heapifyTail() {
      auto comp = [this](int32_t a, int32_t b) { return tailLess(a, b); };
      std::make_heap(tail.begin(), tail.begin() + tailSize, comp);
    }

    void recomputeTailMaxScore() {
      double sum = 0.0;
      for (int32_t i = 0; i < tailSize; i++) {
        sum += (double) clauseMax[(size_t) tail[(size_t) i]];
      }
      tailMaxScore = sum;
    }

    void addHead(int32_t idx) {
      if (scorers[(size_t) idx]->docId() == PostingsReader::END) return;
      auto comp = [this](int32_t a, int32_t b) { return headLess(a, b); };
      head[(size_t) headSize++] = idx;
      std::push_heap(head.begin(), head.begin() + headSize, comp);
    }

    int32_t popHead() {
      auto comp = [this](int32_t a, int32_t b) { return headLess(a, b); };
      std::pop_heap(head.begin(), head.begin() + headSize, comp);
      return head[(size_t) --headSize];
    }

    void addTail(int32_t idx) {
      auto comp = [this](int32_t a, int32_t b) { return tailLess(a, b); };
      tail[(size_t) tailSize++] = idx;
      std::push_heap(tail.begin(), tail.begin() + tailSize, comp);
      tailMaxScore += (double) clauseMax[(size_t) idx];
    }

    int32_t popTail() {
      auto comp = [this](int32_t a, int32_t b) { return tailLess(a, b); };
      std::pop_heap(tail.begin(), tail.begin() + tailSize, comp);
      int32_t idx = tail[(size_t) --tailSize];
      recomputeTailMaxScore();
      return idx;
    }

    // Keep the tail unable to produce a competitive min-should-match hit by
    // itself. If adding idx would break that invariant, evict the highest-max
    // tail clause so it can be advanced into the head.
    int32_t insertTailWithOverflow(int32_t idx) {
      bool scorePrunable = maxScoreBelowThreshold(
          tailMaxScore + (double) clauseMax[(size_t) idx]);
      if (scorePrunable || tailSize + 1 < minMatch) {
        if (scorePrunable) {
          skipCount(SkipStats::wandAdvancePrunes);
        }
        addTail(idx);
        return -1;
      }
      if (tailSize == 0) {
        return idx;
      }

      int32_t top = tail[0];
      if (!tailGreater(top, idx)) {
        return idx;
      }
      tail[0] = idx;
      heapifyTail();
      recomputeTailMaxScore();
      return top;
    }

    void advanceToHead(int32_t idx, int32_t target) {
      auto* scorer = scorers[(size_t) idx];
      if (scorer->docId() < target) {
        scorer->advance(target);
      }
      addHead(idx);
    }

    void clearLead() {
      leadSize = 0;
      leadMaxScore = 0.0;
    }

    void addLead(int32_t idx) {
      lead[(size_t) leadSize++] = idx;
      leadMaxScore += (double) clauseMax[(size_t) idx];
    }

    void pushBackLeads(int32_t target) {
      for (int32_t i = 0; i < leadSize; i++) {
        int32_t evicted = insertTailWithOverflow(lead[(size_t) i]);
        if (evicted >= 0) {
          advanceToHead(evicted, target);
        }
      }
      clearLead();
    }

    void advanceHead(int32_t target) {
      while (headSize > 0 && scorers[(size_t) head[0]]->docId() < target) {
        int32_t idx = popHead();
        int32_t evicted = insertTailWithOverflow(idx);
        if (evicted >= 0) {
          advanceToHead(evicted, target);
        }
      }
    }

    void resetForTarget(int32_t target) {
      headSize = 0;
      tailSize = 0;
      clearLead();
      tailMaxScore = 0.0;
      scoreReady = false;
      for (size_t i = 0; i < scorers.size(); i++) {
        if (scorers[i]->docId() == PostingsReader::END) {
          continue;
        }
        if (scorers[i]->docId() < target) {
          int32_t evicted = insertTailWithOverflow((int32_t) i);
          if (evicted >= 0) {
            advanceToHead(evicted, target);
          }
        } else {
          addHead((int32_t) i);
        }
      }
    }

    void moveHeadToLead() {
      clearLead();
      if (headSize == 0) return;
      docid = scorers[(size_t) head[0]]->docId();
      while (headSize > 0 && scorers[(size_t) head[0]]->docId() == docid) {
        addLead(popHead());
      }
    }

    bool candidateMatchesPivot() {
      while (maxScoreBelowThreshold(leadMaxScore) || leadSize < minMatch) {
        if (maxScoreBelowThreshold(leadMaxScore + tailMaxScore)
            || leadSize + tailSize < minMatch) {
          skipCount(SkipStats::wandCandidatePrunes);
          return false;
        }
        int32_t idx = popTail();
        auto* scorer = scorers[(size_t) idx];
        if (scorer->docId() < docid) {
          scorer->advance(docid);
        }
        if (scorer->docId() == docid) {
          addLead(idx);
        } else {
          addHead(idx);
        }
      }
      return true;
    }

    int32_t doAdvance(int32_t target) {
      resetForTarget(target);
      for (;;) {
        if (headSize == 0) {
          docid = PostingsReader::END;
          currentScore = 0.0f;
          scoreReady = true;
          return docid;
        }
        moveHeadToLead();
        if (candidateMatchesPivot()) {
          visitedCandidates++;
          currentScore = 0.0f;
          scoreReady = false;
          return docid;
        }
        target = docid + 1;
        pushBackLeads(target);
        advanceHead(target);
      }
    }

  public:
    MinShouldMatchWandScorer(solux::MemPool& pool, std::span<Scorer*> scorers, int32_t minMatch)
            : scorers(scorers),
              clauseMax(pool.make_arr<float>(scorers.size()), scorers.size()),
              head(pool.make_arr<int32_t>(scorers.size()), scorers.size()),
              tail(pool.make_arr<int32_t>(scorers.size()), scorers.size()),
              lead(pool.make_arr<int32_t>(scorers.size()), scorers.size()),
              minMatch(minMatch) {
      assert(minMatch >= 1);
      assert((int32_t)scorers.size() > minMatch);
      // Worst-case relative error of summing scorers.size() non-negative floats in
      // float arithmetic is bounded by (m-1)*u, u = 2^-24; round the double max-sum
      // up by that so it bounds score()'s float accumulation from above.
      scoreBoundFactor = 1.0 + (double) scorers.size() * 0x1p-24;
      for (size_t i = 0; i < scorers.size(); i++) {
        clauseMax[i] = optionalUpperBound(
            scorers[i]->getMaxScore(PostingsReader::END));
      }
    }

    int32_t next() override {
      assert(docid != solux::PostingsReader::END);
      return doAdvance(docid + 1);
    }

    int32_t advance(int32_t target) override {
      assert(docid < target);
      return doAdvance(target);
    }

    int32_t docId() override {
      return docid;
    }

    float score() override {
      if (scoreReady) return currentScore;
      float sum = 0.0f;
      for (size_t i = 0; i < scorers.size(); i++) {
        if (scorers[i]->docId() < docid) {
          scorers[i]->advance(docid);
        }
        if (scorers[i]->docId() == docid) {
          sum += scorers[i]->score();
        }
      }
      currentScore = sum;
      scoreReady = true;
      return currentScore;
    }

    void setMinCompetitiveScore(float minScore) override {
      if (minScore > minCompetitiveScore) {
        minCompetitiveScore = minScore;
      }
    }

    int32_t advanceShallow(int32_t target) override {
      int32_t upTo = PostingsReader::END;
      for (auto* scorer : scorers) {
        if (scorer->docId() != PostingsReader::END) {
          upTo = std::min(upTo, scorer->advanceShallow(target));
        }
      }
      return upTo;
    }

    float getMaxScore(int32_t upTo) override {
      double sum = 0.0;
      for (auto* scorer : scorers) {
        int32_t doc = scorer->docId();
        if (doc == PostingsReader::END || doc > upTo) {
          continue;
        }
        float maxScore = optionalUpperBound(scorer->getMaxScore(upTo));
        if (!std::isfinite(maxScore)) {
          return std::numeric_limits<float>::infinity();
        }
        sum += (double) maxScore;
      }
      // Same float-accumulation headroom as the pivot bound, so this is a true upper
      // bound on score() when the scorer nests under another impact scorer.
      sum *= scoreBoundFactor;
      if (!std::isfinite(sum) || sum > (double) std::numeric_limits<float>::max()) {
        return std::numeric_limits<float>::infinity();
      }
      float ret = (float) sum;
      if ((double) ret < sum) {
        ret = std::nextafter(ret, std::numeric_limits<float>::infinity());
      }
      return ret;
    }

    float refineMaxScore(int32_t upTo) override {
      double sum = 0.0;
      for (auto* scorer : scorers) {
        int32_t doc = scorer->docId();
        if (doc == PostingsReader::END || doc > upTo) {
          continue;
        }
        float maxScore = optionalUpperBound(scorer->refineMaxScore(upTo));
        if (!std::isfinite(maxScore)) {
          return std::numeric_limits<float>::infinity();
        }
        sum += (double) maxScore;
      }
      sum *= scoreBoundFactor;
      if (!std::isfinite(sum) || sum > (double) std::numeric_limits<float>::max()) {
        return std::numeric_limits<float>::infinity();
      }
      float ret = (float) sum;
      if ((double) ret < sum) {
        ret = std::nextafter(ret, std::numeric_limits<float>::infinity());
      }
      return ret;
    }

    ScoreBounds getScoreBounds(int32_t upTo) override {
      return sumOptionalScoreBounds(scorers, upTo, false);
    }

    ScoreBounds refineScoreBounds(int32_t upTo) override {
      return sumOptionalScoreBounds(scorers, upTo, true);
    }

    int64_t visited() const {
      return visitedCandidates;
    }
  }; // MinShouldMatchWandScorer


  // Matches docs where at least `minMatch` of the sub-scorers match (the
  // OR..AND middle ground). Called only for 1 < minMatch < scorers.size().
  //
  // With scorers sorted by ascending cost, the first N - minMatch + 1 are leads.
  // Any match must hit at least one lead, so leads generate candidates and the
  // tail only confirms them.
  //
  // TODO: OPT lead with a doc-ordered priority queue (like DisjunctionScorer)
  // and rebalance lead/tail once block-max scoring exists.
  class MinShouldMatchScorer final : public Query::Scorer {
    std::span<Scorer*> scorers;  // ascending cost: leads first, then tail
    int32_t minMatch;
    int32_t leadCount;           // scorers[0, leadCount) lead; [leadCount, N) tail
    int32_t docid = -1;

    std::span<Scorer*> leads() { return scorers.subspan(0, (size_t)leadCount); }
    std::span<Scorer*> tail() { return scorers.subspan((size_t)leadCount); }

    // First doc >= target carrying at least minMatch matching sub-scorers.
    int32_t findNext(int32_t target) {
      for (;;) {
        // Advance lagging leads and use their minimum as the candidate.
        int32_t candidate = PostingsReader::END;
        for (auto* s : leads()) {
          if (s->docId() < target) s->advance(target);
          if (s->docId() < candidate) candidate = s->docId();
        }
        if (candidate == PostingsReader::END) {
          return docid = PostingsReader::END;
        }

        int32_t freq = 0;
        for (auto* s : leads()) {
          if (s->docId() == candidate) freq++;
        }

        // Top up from the tail, advancing onto the candidate only as needed.
        auto t = tail();
        for (size_t i = 0; i < t.size(); i++) {
          if (freq >= minMatch) break;                            // confirmed match
          if (freq + (int32_t)(t.size() - i) < minMatch) break;   // can't reach it
          if (t[i]->docId() < candidate) t[i]->advance(candidate);
          if (t[i]->docId() == candidate) freq++;
        }

        if (freq >= minMatch) {
          return docid = candidate;
        }
        target = candidate + 1;  // candidate fell short; look past it
      }
    }

  public:
    MinShouldMatchScorer(solux::MemPool& pool, std::span<Scorer*> costAscendingScorers, int32_t minMatch)
            : scorers(costAscendingScorers), minMatch(minMatch),
              leadCount((int32_t)costAscendingScorers.size() - minMatch + 1) {
      unused(pool);
      assert(minMatch >= 2);
      assert((int32_t)scorers.size() >= minMatch);
      assert(leadCount >= 1);
    }

    int32_t next() override {
      assert(docid != solux::PostingsReader::END);
      return findNext(docid + 1);
    }

    int32_t advance(int32_t target) override {
      assert(docid < target);  // strict, and (END < target) is never true: also latches END
      return findNext(target);
    }

    int32_t docId() override {
      return docid;
    }

    float score() override {
      // findNext may stop before every matching tail scorer has advanced.
      float score = 0.0f;
      for (auto* s : leads()) {
        if (s->docId() == docid) score += s->score();
      }
      for (auto* s : tail()) {
        if (s->docId() < docid) s->advance(docid);
        if (s->docId() == docid) score += s->score();
      }
      return score;
    }

    ScoreBounds getScoreBounds(int32_t upTo) override {
      return sumOptionalScoreBounds(scorers, upTo, false);
    }

    ScoreBounds refineScoreBounds(int32_t upTo) override {
      return sumOptionalScoreBounds(scorers, upTo, true);
    }
  }; // MinShouldMatchScorer


};

} // namespace solux
