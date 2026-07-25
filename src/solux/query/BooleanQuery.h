#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <typeindex>

#include "Query.h"
#include "AllQuery.h"
#include "BoostQuery.h"
#include "ConstantScoreQuery.h"
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
        // If an exclusion is itself a pure-negative Boolean it deliberately
        // stays opaque here. Prohibited-list normalization is not part of R1.
        plan.prohibited.insert(plan.prohibited.end(), inner->prohibited.begin(),
                               inner->prohibited.end());
        parents.erase(parents.begin() + (ptrdiff_t) i);
        plan.ruleMask |= R1_REQUIRED_INLINE;
        // Re-examine the clause shifted into this slot.
        continue;
      }
      bool hasRequired = inner != nullptr
        && (!inner->mandatory.empty() || !inner->filter.empty());
      bool optionalsAllowed = parentIsFilter || (inner != nullptr && inner->optional.empty());
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

  // R5: a bare match-all in a required list is the conjunction's identity.
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
  // A/B toggle: send scored TOP_k with direct window-capable filters to the pull
  // ConjunctionScorer instead of the window-filter bulk route.
  static inline bool disableFilteredScoredBulkForTests = false;
  // A/B toggle for exact probe-vs-fill equivalence tests.
  static inline bool disableFilterMaskProbeForTests = false;
  // A/B toggle for the exact df(base) + non-member count path used by
  // count-only skewed term disjunctions.
  static inline bool disableDisjunctionCountIdentityForTests = false;
  // A/B toggle: retain the plain pull DisjunctionScorer for sparse filtered
  // term unions instead of using head/tail WAND candidate formation.
  static inline bool disableFilteredUnionWandForTests = false;
  // A/B toggle: keep prohibited scored disjunctions on the pull MandNot path.
  static inline bool disableBulkExclusionForTests = false;
  // A/B toggle: force the conjunction onto the eager single-phase path (each
  // clause verifies inside its own advance) instead of two-phase (defer matches
  // until the approximations agree). For benchmarking the two-phase win only.
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
  // Scored TOP_k filter routing: the window-mask bulk needs the collector
  // floor to rise, and that tracks how densely the filter accepts docs, not
  // filter-vs-body cost. Below maxDoc/kMaskFilterDensityInverse the mask
  // route degenerates toward an unpruned scored scan and the pull conjunction
  // (filter leads) wins. Measured crossover on the 5M sweep: 1% filter
  // (journalist) wants pull, 10% (city) wants mask.
  static constexpr int64_t kMaskFilterDensityInverse = solux::kMaskFilterDensityInverse;
  // Relative cost of one monotonic filter advance vs streaming one filter
  // posting. Probe when leadCost * this weight is below filterCost.
  static constexpr int64_t kMaskProbeAdvanceWeight = 6;
  // Probing checks candidates one at a time instead of filling the filter's
  // bits for the window, so it only pays when FILLING is the expensive side -
  // i.e. on a FAT filter. A thin filter is cheap to fill (few bits, sparse
  // postings) while probing still costs a scalar advance per candidate, so
  // the body/filter ratio alone routes wrongly once thin filters reach the
  // mask. Measured on the 300-query intersection class at TOP_100 (forced
  // fill / probe, so >1 means probing WINS):
  //   0.50% 0.938 | 0.99% 0.942 | 1.99% 0.912 | 3.12% 0.959 | 4.98% 0.748
  //   9.9% 0.989 (tie) | 19.2% 1.072 | 29.9% 1.148 | 42.6% 1.204
  // so probing additionally requires the filter to cover at least maxDoc/8
  // (12.5%), which puts every measured point on its winning side.
  static constexpr int64_t kMaskProbeMinFilterDensityInverse = 8;

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

  class Weight final : public Query::Weight {
    std::span<Query::Weight*> mandatoryWeights;
    std::span<uint8_t> mandatoryScores;
    std::span<Query::Weight*> optionalWeights;
    std::span<Query::Weight*> prohibitedWeights;
    std::span<Query::Weight*> filterWeights;
    std::span<FilterCache::Use*> filterUses;
    int minShouldMatch = 0;
    bool needsScores = false;

    static bool lessMaxScore(float a, float b) {
      bool finiteA = std::isfinite(a);
      bool finiteB = std::isfinite(b);
      if (finiteA != finiteB) return finiteA;
      return a < b;
    }

    static bool filterDensityRoutesToPull(int64_t filterCost,
                                          int32_t maxDoc) {
      return filterCost < maxDoc / kMaskFilterDensityInverse;
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
    };

    // Build the required-clause conjunction. Mandatory clauses score; filter
    // clauses only constrain iteration. Required clauses are ordered by ascending
    // supplier cost so the sparsest leads the conjunction (the highest-leverage
    // win for a "+rare +common" or "+term filter:x" query), and the sparsest cost
    // is passed as leadCost to every child get() per the supplier planning
    // contract. Because a Solux Scorer exposes no cost(), this ordering has to
    // happen here at the supplier layer, before any scorer is constructed.
    static Required assembleRequired(
        MemPool& targetPool,
        IndexReader::Segment& segment,
        std::span<Query::SegmentSource* const> mandatorySources,
        std::span<const uint8_t> mandatoryScores,
        std::span<Query::ScorerSupplier* const> filterSuppliers) {
      assert(mandatorySources.size() == mandatoryScores.size());
      struct Entry {
        int64_t cost;
        Query::ScorerSupplier* supplier;
        bool scoring;
      };
      boost::container::small_vector<Entry, 16> entries;
      size_t scoringCapacity = 0;
      for (size_t i = 0; i < mandatorySources.size(); i++) {
        auto* source = mandatorySources[i];
        auto* supplier = source->scorerSupplier(targetPool, segment);
        if (supplier == nullptr) return {nullptr, true, 0, 0};
        bool scores = mandatoryScores[i] != 0;
        scoringCapacity += scores ? 1 : 0;
        entries.push_back({supplier->cost(), supplier, scores});
      }
      for (auto* supplier : filterSuppliers) {
        if (supplier == nullptr) return {nullptr, true, 0, 0};
        entries.push_back({supplier->cost(), supplier, false});
      }
      if (entries.empty()) return {nullptr, false, 0, 0};

      // leadCost is the cost of the sparsest required clause: it bounds how often
      // the others get driven, so each may plan eager vs lazy setup off it.
      int64_t leadCost = std::numeric_limits<int64_t>::max();
      for (auto& e : entries) leadCost = std::min(leadCost, e.cost);

      // Sparsest first so allScorers[0] leads the conjunction.
      std::sort(entries.begin(), entries.end(),
                [](const Entry& a, const Entry& b) { return a.cost < b.cost; });

      auto* all = targetPool.make_arr<Query::Scorer*>(entries.size());
      auto* costs = targetPool.make_arr<int64_t>(entries.size());
      Query::Scorer** scoring = scoringCapacity == 0
        ? nullptr
        : targetPool.make_arr<Query::Scorer*>(scoringCapacity);
      size_t allCount = 0;
      size_t scoringCount = 0;
      for (auto& e : entries) {
        auto* scorer = e.supplier->get(targetPool, leadCost);
        if (scorer == nullptr) return {nullptr, true, 0, 0};
        all[allCount] = scorer;
        costs[allCount++] = e.cost;
        if (e.scoring) scoring[scoringCount++] = scorer;
      }

      // A lone scoring clause (one mandatory, no filters) needs no wrapper.
      if (allCount == 1 && scoringCount == 1) return {all[0], false, leadCost, 1};
      return {targetPool.make<BooleanQuery::ConjunctionScorer>(
                targetPool, std::span<Query::Scorer*>(all, allCount),
                std::span<int64_t>(costs, allCount),
                std::span<Query::Scorer*>(scoring, scoringCount)),
              false, leadCost, scoringCount};
    }

    // Keep clause wiring in one place so prepared and non-prepared execution
    // cannot diverge on filter/prohibited semantics.
    static Query::Scorer* assembleScorer(
        MemPool& targetPool,
        IndexReader::Segment& segment,
        std::span<Query::SegmentSource* const> mandatorySources,
        std::span<const uint8_t> mandatoryScores,
        std::span<Query::SegmentSource* const> optionalSources,
        std::span<Query::SegmentSource* const> prohibitedSources,
        std::span<Query::ScorerSupplier* const> filterSuppliers,
        int minShouldMatch,
        bool needsScores,
        bool externallyDriven) {
      Required req = assembleRequired(
          targetPool, segment, mandatorySources, mandatoryScores,
          filterSuppliers);
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
      QueryPrep::CostedScorers optional;
      if (minShouldMatch > 1) {
        optional = QueryPrep::createScorersByCost(targetPool, segment, optionalSources);
      } else if (minShouldMatch == 1) {
        optional = QueryPrep::createScorersWithCosts(targetPool, segment, optionalSources);
      } else {
        optional.scorers = QueryPrep::createScorers(targetPool, segment, optionalSources);
      }
      auto optionalScorers = optional.scorers;
      auto optionalCosts = optional.costs;
      Query::Scorer* optScorer = nullptr;
      if (!optionalScorers.empty()) {
        int optCount = (int)optionalScorers.size();
        bool directWindowFilters = false;
        if (mandatorySources.empty() && !filterSuppliers.empty()
            && reqScorer != nullptr) {
          auto filterScorers = reqScorer->flatConjunctionScorers();
          directWindowFilters = filterScorers.size() == filterSuppliers.size()
              && std::all_of(filterScorers.begin(), filterScorers.end(),
                             [](Query::Scorer* scorer) {
                               return scorer->supportsWindowFilter();
                             });
        }
        bool flatTermDisjunction = optCount >= 2
            && std::all_of(optionalScorers.begin(), optionalScorers.end(),
                           [](Query::Scorer* scorer) {
                             return dynamic_cast<TermQuery::Scorer*>(scorer)
                                 != nullptr;
                           });
        // The filtered scored-bulk density gate has already selected pull for
        // this exact shape. The filter remains the conjunction lead; WAND only
        // replaces the sole scoring disjunction member.
        bool useFilteredUnionWand = !disableFilteredUnionWandForTests
            && needsScores && minShouldMatch == 1
            && mandatorySources.empty() && prohibitedSources.empty()
            && directWindowFilters && flatTermDisjunction
            && req.scoringCount == 0
            && filterDensityRoutesToPull(req.cost, segment.maxDoc());
        bool plainExternalDisjunction = externallyDriven && needsScores
          && reqScorer == nullptr && prohibitedSources.empty()
          && minShouldMatch <= 1 && optCount >= 2;
        if (plainExternalDisjunction) {
          sortByMaxScore(targetPool, optionalScorers);
        }
        bool useMaxScoreDisjunction = needsScores && reqScorer == nullptr
          && prohibitedSources.empty() && minShouldMatch <= 1 && optCount >= 2
          && !plainExternalDisjunction;
        // minShouldMatch applies to optional scorers that exist in this segment.
        if (minShouldMatch <= 1) {
          if (optCount == 1) {
            optScorer = optionalScorers[0];
          } else if (useFilteredUnionWand) {
            optScorer = targetPool.make<BooleanQuery::MinShouldMatchWandScorer>(
              targetPool, optionalScorers, 1);
          } else if (useMaxScoreDisjunction) {
            optScorer = targetPool.make<BooleanQuery::MaxScoreDisjunctionScorer>(
              targetPool, optionalScorers, segment.maxDoc());
          } else {
            optScorer = targetPool.make<BooleanQuery::DisjunctionScorer>(targetPool, optionalScorers);
          }
        } else if (optCount == minShouldMatch) {
          // Every surviving optional clause is required and scores.
          optScorer = targetPool.make<BooleanQuery::ConjunctionScorer>(
            targetPool, optionalScorers, optionalCosts, optionalScorers);
        } else if (optCount > minShouldMatch) {
          bool useWand = needsScores && reqScorer == nullptr && prohibitedSources.empty();
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
        if (optionalsConstrain && !optionalSources.empty()) return nullptr;
        boolScorer = reqScorer;
      } else if (!optionalsConstrain) {
        // min_match unset with required/filter clauses: optionals rank
        // coincident matches but never decide them.
        boolScorer = targetPool.make<BooleanQuery::MandOptScorer>(targetPool, reqScorer, optScorer);
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
        boolScorer = targetPool.make<BooleanQuery::ConjunctionScorer>(
            targetPool, allSpan, allCosts, scoringSpan);
      }

      auto prohibitedScorers = QueryPrep::createScorers(targetPool, segment, prohibitedSources);
      if (!prohibitedScorers.empty()) {
        Query::Scorer* prohibitedScorer = prohibitedScorers.size() == 1
          ? prohibitedScorers[0]
          : targetPool.make<BooleanQuery::DisjunctionScorer>(targetPool, prohibitedScorers);
        boolScorer = targetPool.make<BooleanQuery::MandNotScorer>(targetPool, boolScorer, prohibitedScorer);
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
      for (int i = 0; i < take; i++) sum += sorted[(size_t) i];
      return std::min(sum, maxDoc);
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

    // TODO: OPT share child suppliers between cost() and get().
    class Supplier final : public Query::ScorerSupplier {
      MemPool& pool;
      IndexReader::Segment& segment;
      std::span<Query::SegmentSource* const> mandatorySources;
      std::span<const uint8_t> mandatoryScores;
      std::span<Query::SegmentSource* const> optionalSources;
      std::span<Query::SegmentSource* const> prohibitedSources;
      std::span<Query::ScorerSupplier* const> filterSuppliers;
      int minShouldMatch;
      bool needsScores;
    public:
      Supplier(MemPool& pool, IndexReader::Segment& segment,
               std::span<Query::SegmentSource* const> mandatorySources,
               std::span<const uint8_t> mandatoryScores,
               std::span<Query::SegmentSource* const> optionalSources,
               std::span<Query::SegmentSource* const> prohibitedSources,
               std::span<Query::ScorerSupplier* const> filterSuppliers,
               int minShouldMatch,
               bool needsScores)
        : pool(pool), segment(segment), mandatorySources(mandatorySources),
          mandatoryScores(mandatoryScores),
          optionalSources(optionalSources), prohibitedSources(prohibitedSources),
          filterSuppliers(filterSuppliers), minShouldMatch(minShouldMatch),
          needsScores(needsScores) {}

      int64_t cost() override {
        return compositeCost(pool, segment, mandatorySources, optionalSources, filterSuppliers, minShouldMatch);
      }

      Query::Scorer* get(MemPool& targetPool, int64_t leadCost) override {
        return assembleScorer(targetPool, segment, mandatorySources,
                              mandatoryScores, optionalSources,
                              prohibitedSources, filterSuppliers, minShouldMatch, needsScores,
                              leadCost != std::numeric_limits<int64_t>::max());
      }

      // Build a bulk conjunction from the required clause boundaries. During
      // exhaustive unscored execution, filters are ordinary zero-scoring
      // conjuncts and a constraining optional group is one disjunction
      // conjunct. The scored path keeps its existing pure-mandatory and
      // single-phase gate.
      BulkScorer* conjunctionBulkScorer(MemPool& targetPool,
                                        bool exhaustive) {
        struct Entry {
          int64_t cost;
          Query::ScorerSupplier* supplier;
          bool optionalGroup;
        };
        boost::container::small_vector<Entry, 16> entries;
        for (auto* source : mandatorySources) {
          auto* supplier = source->scorerSupplier(targetPool, segment);
          if (supplier == nullptr) {
            return nullptr;  // a required clause cannot match this segment
          }
          entries.push_back({supplier->cost(), supplier, false});
        }
        if (exhaustive) {
          for (auto* supplier : filterSuppliers) {
            if (supplier == nullptr) {
              return nullptr;
            }
            entries.push_back({supplier->cost(), supplier, false});
          }
        }

        boost::container::small_vector<Query::ScorerSupplier*, 16>
            optionalGroupSuppliers;
        boost::container::small_vector<int64_t, 16> optionalGroupCosts;
        bool hasOptionalGroup = exhaustive && minShouldMatch >= 1
            && !optionalSources.empty();
        if (hasOptionalGroup) {
          for (auto* source : optionalSources) {
            auto* supplier = source->scorerSupplier(targetPool, segment);
            if (supplier == nullptr) {
              continue;
            }
            optionalGroupSuppliers.push_back(supplier);
            optionalGroupCosts.push_back(supplier->cost());
          }
          if (optionalGroupSuppliers.empty()) {
            return nullptr;
          }
          entries.push_back({optionalCost(optionalGroupCosts, 1, segment.maxDoc()),
                             nullptr, true});
        }
        if (entries.empty()
            || (entries.size() < 2
                && (!exhaustive || prohibitedSources.empty()))) {
          return nullptr;
        }
        std::sort(entries.begin(), entries.end(),
                  [](const Entry& a, const Entry& b) { return a.cost < b.cost; });
        int64_t leadCost = entries[0].cost;
        int64_t nonLeadCost = 0;
        for (size_t i = 1; i < entries.size(); i++) {
          nonLeadCost += entries[i].cost;
        }
        auto* arr = targetPool.make_arr<Query::Scorer*>(entries.size());
        for (size_t i = 0; i < entries.size(); i++) {
          Query::Scorer* scorer;
          if (entries[i].optionalGroup) {
            auto* members = targetPool.make_arr<Query::Scorer*>(
                optionalGroupSuppliers.size());
            size_t memberCount = 0;
            for (auto* supplier : optionalGroupSuppliers) {
              auto* member = supplier->get(targetPool, leadCost);
              if (member != nullptr) {
                members[memberCount++] = member;
              }
            }
            if (memberCount == 0) {
              return nullptr;
            }
            scorer = memberCount == 1
              ? members[0]
              : targetPool.make<BooleanQuery::DisjunctionScorer>(
                  targetPool,
                  std::span<Query::Scorer*>(members, memberCount));
          } else {
            scorer = entries[i].supplier->get(targetPool, leadCost);
          }
          if (scorer == nullptr || (!exhaustive && scorer->hasTwoPhase())) {
            return nullptr;
          }
          arr[i] = scorer;
        }

        auto* prohibitedArr =
            targetPool.make_arr<Query::Scorer*>(prohibitedSources.size());
        size_t prohibitedCount = 0;
        if (exhaustive) {
          for (auto* source : prohibitedSources) {
            auto* supplier = source->scorerSupplier(targetPool, segment);
            if (supplier == nullptr) {
              continue;
            }
            auto* scorer = supplier->get(targetPool, leadCost);
            if (scorer != nullptr) {
              prohibitedArr[prohibitedCount++] = scorer;
            }
          }
        }
        return targetPool.make<BooleanQuery::ConjunctionBulkScorer>(
            targetPool, std::span<Query::Scorer*>(arr, entries.size()),
            std::span<Query::Scorer*>(prohibitedArr, prohibitedCount),
            segment.maxDoc(), leadCost, nonLeadCost, !exhaustive);
      }

      BulkScorer* mandOptBulkScorer(MemPool& targetPool) {
        auto* mandSupplier = mandatorySources[0]->scorerSupplier(targetPool, segment);
        if (mandSupplier == nullptr) {
          return nullptr;
        }
        int64_t mandCost = mandSupplier->cost();
        auto* mandScorer = mandSupplier->get(targetPool, mandCost);
        if (mandScorer == nullptr || mandScorer->hasTwoPhase()) {
          return nullptr;
        }

        auto& optScorers = *targetPool.make_vec<Query::Scorer*>();
        auto& optCosts = *targetPool.make_vec<int64_t>();
        optScorers.reserve(optionalSources.size());
        optCosts.reserve(optionalSources.size());
        for (auto* source : optionalSources) {
          auto* supplier = source->scorerSupplier(targetPool, segment);
          if (supplier == nullptr) {
            continue;
          }
          int64_t cost = supplier->cost();
          auto* scorer = supplier->get(targetPool, mandCost);
          if (scorer == nullptr) {
            continue;
          }
          if (scorer->hasTwoPhase()) {
            return nullptr;
          }
          optScorers.push_back(scorer);
          optCosts.push_back(cost);
        }
        if (optScorers.empty()) {
          return nullptr;
        }

        return targetPool.make<BooleanQuery::MandOptBulkScorer>(
            targetPool, mandScorer,
            std::span<Query::Scorer*>(optScorers.data(), optScorers.size()),
            std::span<int64_t>(optCosts.data(), optCosts.size()),
            segment.maxDoc(), mandCost);
      }

      BulkScorer* maxScoreBulkScorer(MemPool& targetPool,
                                     bool withBulkExclusion = false) {
        auto& optionalScorersVec = *targetPool.make_vec<Query::Scorer*>();
        optionalScorersVec.reserve(optionalSources.size());
        // Count-only identity routing needs every per-segment term df even for
        // the common two-clause case. Scored execution keeps its existing
        // four-clause threshold for retaining costs.
        auto* optionalCostsVec =
          (!needsScores || optionalSources.size() >= kCostAwareOrderMinClauses)
            ? targetPool.make_vec<int64_t>() : nullptr;
        if (optionalCostsVec != nullptr) {
          optionalCostsVec->reserve(optionalSources.size());
        }
        int64_t aggregateClauseCost = 0;
        for (auto* source : optionalSources) {
          auto* supplier = source->scorerSupplier(targetPool, segment);
          if (supplier == nullptr) {
            continue;
          }
          int64_t cost = supplier->cost();
          auto* scorer = supplier->get(
              targetPool, std::numeric_limits<int64_t>::max());
          if (scorer == nullptr) {
            continue;
          }
          optionalScorersVec.push_back(scorer);
          if (optionalCostsVec != nullptr) {
            optionalCostsVec->push_back(cost);
          }
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
        std::span<Query::Scorer*> optionalScorers(
            optionalScorersVec.data(), optionalScorersVec.size());
        std::span<int64_t> optionalCosts;
        if (optionalCostsVec != nullptr) {
          optionalCosts = {optionalCostsVec->data(), optionalCostsVec->size()};
        }
        if (optionalScorers.size() < 2) {
          if (withBulkExclusion) {
            skipCount(SkipStats::bulkExclusionPositiveSegmentFallbacks);
          }
          return nullptr;
        }

        std::span<Query::Scorer*> exclusionScorers;
        if (withBulkExclusion) {
          auto& exclusions = *targetPool.make_vec<Query::Scorer*>();
          for (auto* source : prohibitedSources) {
            auto* supplier = source->scorerSupplier(targetPool, segment);
            if (supplier == nullptr) {
              continue;
            }
            auto* scorer = supplier->get(targetPool, aggregateClauseCost);
            if (scorer == nullptr) {
              continue;
            }
            if (scorer->supportsWindowFilter()) {
              exclusions.push_back(scorer);
              continue;
            }
            // A prohibited Boolean is decomposable only as a flat OR whose
            // members each satisfy the same exact window-fill contract.
            auto members = scorer->flatDisjunctionScorers();
            if (members.empty()
                || !std::all_of(
                    members.begin(), members.end(), [](Query::Scorer* member) {
                      return member->supportsWindowFilter();
                    })) {
              skipCount(SkipStats::bulkExclusionUnsupportedFallbacks);
              return nullptr;
            }
            exclusions.insert(exclusions.end(), members.begin(), members.end());
          }
          exclusionScorers = {exclusions.data(), exclusions.size()};
          skipCount(SkipStats::bulkExclusionEngagements);
        }
        return targetPool.make<BooleanQuery::MaxScoreBulkScorer>(
            targetPool, optionalScorers, optionalCosts, exclusionScorers,
            segment.maxDoc(), aggregateClauseCost, !needsScores,
            segment.liveDocs() != nullptr);
      }

      BulkScorer* attachDirectFilters(MemPool& targetPool,
                                      BulkScorer* bulk,
                                      int64_t bodyCost,
                                      int64_t filterCost) {
        if (bulk == nullptr || filterSuppliers.empty()) {
          return nullptr;
        }

        auto filterScorers = targetPool.make_span<Query::Scorer*>(
            filterSuppliers.size());
        for (size_t i = 0; i < filterSuppliers.size(); i++) {
          auto* scorer = filterSuppliers[i]->get(targetPool, bodyCost);
          if (scorer == nullptr || !scorer->supportsWindowFilter()) {
            return nullptr;
          }
          filterScorers[i] = scorer;
        }
        bool probe = !disableFilterMaskProbeForTests && filterCost > 0
            && filterCost >= (int64_t) segment.maxDoc()
                                 / kMaskProbeMinFilterDensityInverse
            && bodyCost <= (filterCost - 1) / kMaskProbeAdvanceWeight;
        auto* windowFilter = targetPool.make<WindowFilter>(
            targetPool, filterScorers, probe, filterCost);
        return bulk->attachWindowFilter(windowFilter) ? bulk : nullptr;
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

      BulkScorer* filteredScoredBulkScorer(MemPool& targetPool) {
        enum class BodyShape : uint8_t {
          NONE,
          CHILD,
          CONJUNCTION,
          MAND_OPT,
          MAX_SCORE,
        };

        if (!needsScores || filterSuppliers.empty()
            || !prohibitedSources.empty() || minShouldMatch > 1) {
          return nullptr;
        }
        int64_t filterCost = minFilterCost();
        if (filterCost < 0) {
          return nullptr;
        }

        BodyShape shape = BodyShape::NONE;
        Query::ScorerSupplier* childSupplier = nullptr;
        int64_t bodyCost = 0;

        // A folded filter prevents makeSupplier's single-mandatory unwrap, so
        // recover the child's own bulk contract here (term and nested MandOpt
        // are the common cases).
        if (mandatorySources.size() == 1 && optionalSources.empty()
            && minShouldMatch < 1) {
          childSupplier = mandatorySources[0]->scorerSupplier(
              targetPool, segment);
          shape = BodyShape::CHILD;
        } else if (mandatorySources.empty() && optionalSources.size() == 1
                   && minShouldMatch == 1) {
          // Rank-only optionals (minShouldMatch == 0 beside filters) do not
          // own membership and therefore cannot use this route.
          childSupplier = optionalSources[0]->scorerSupplier(
              targetPool, segment);
          shape = BodyShape::CHILD;
        } else if (optionalSources.empty() && mandatorySources.size() >= 2) {
          bodyCost = std::numeric_limits<int64_t>::max();
          for (auto* source : mandatorySources) {
            auto* supplier = source->scorerSupplier(targetPool, segment);
            if (supplier == nullptr) {
              return nullptr;
            }
            bodyCost = std::min(bodyCost, supplier->cost());
          }
          shape = BodyShape::CONJUNCTION;
        } else if (!disableMandOptBulkForTests
                   && mandatorySources.size() == 1
                   && !optionalSources.empty() && minShouldMatch < 1) {
          childSupplier = mandatorySources[0]->scorerSupplier(
              targetPool, segment);
          shape = BodyShape::MAND_OPT;
        } else if (mandatorySources.empty() && optionalSources.size() >= 2
                   && minShouldMatch == 1) {
          for (auto* source : optionalSources) {
            auto* supplier = source->scorerSupplier(targetPool, segment);
            if (supplier == nullptr) {
              continue;
            }
            int64_t cost = supplier->cost();
            if (cost > 0
                && bodyCost < std::numeric_limits<int64_t>::max()) {
              int64_t room = std::numeric_limits<int64_t>::max() - bodyCost;
              bodyCost = cost >= room
                  ? std::numeric_limits<int64_t>::max()
                  : bodyCost + cost;
            }
          }
          shape = BodyShape::MAX_SCORE;
        }

        if (childSupplier == nullptr
            && (shape == BodyShape::CHILD || shape == BodyShape::MAND_OPT)) {
          return nullptr;
        }
        if (childSupplier != nullptr) {
          bodyCost = childSupplier->cost();
        }
        if (shape == BodyShape::NONE
            || filterDensityRoutesToPull(filterCost, segment.maxDoc())) {
          return nullptr;
        }

        BulkScorer* bulk = nullptr;
        switch (shape) {
          case BodyShape::CHILD:
            bulk = childSupplier->bulkScorer(targetPool);
            break;
          case BodyShape::CONJUNCTION:
            bulk = conjunctionBulkScorer(targetPool, false);
            break;
          case BodyShape::MAND_OPT:
            bulk = mandOptBulkScorer(targetPool);
            break;
          case BodyShape::MAX_SCORE:
            bulk = maxScoreBulkScorer(targetPool);
            break;
          case BodyShape::NONE:
            std::unreachable();
        }
        return attachDirectFilters(targetPool, bulk, bodyCost, filterCost);
      }

      BulkScorer* filterOnlyBulkScorer(MemPool& targetPool) {
        struct Entry {
          int64_t cost;
          BulkScorer* bulk;
        };
        boost::container::small_vector<Entry, 16> entries;
        entries.reserve(filterSuppliers.size());
        for (auto* supplier : filterSuppliers) {
          if (supplier == nullptr) {
            return nullptr;
          }
          auto* bulk = supplier->bulkScorer(targetPool);
          if (bulk == nullptr) {
            return nullptr;
          }
          entries.push_back({supplier->cost(), bulk});
        }
        std::sort(entries.begin(), entries.end(),
                  [](const Entry& a, const Entry& b) { return a.cost < b.cost; });
        if (entries.size() == 1) {
          // Wrap even the single-clause case: the wrapper owns the
          // non-scoring contract (constant-0 score windows, no impact
          // pruning by the lead).
          return targetPool.make<BooleanQuery::FilterOnlyBulkScorer>(
              entries[0].bulk, std::span<uint64_t>{}, std::span<uint64_t>{},
              0, segment.maxDoc());
        }

        int32_t maxDoc = segment.maxDoc();
        size_t wordCount = FixedBitSet::sizeInWords(maxDoc);
        auto filterWords = targetPool.make_span<uint64_t>(wordCount);
        auto clauseWords = targetPool.make_span<uint64_t>(wordCount);
        bool first = true;
        for (size_t i = 1; i < entries.size(); i++) {
          DocSetBuilder builder(maxDoc);
          int64_t count = 0;
          for (int32_t cursor = 0;
               cursor != PostingsReader::END && cursor < maxDoc; ) {
            int32_t next = entries[i].bulk->countNextWindow(
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
            entries[0].bulk, filterWords, clauseWords, filterCard, maxDoc);
      }

      BulkScorer* bulkScorer(MemPool& targetPool) override {
        if (!needsScores && optionalSources.size() >= 2) {
          if (!mandatorySources.empty()) {
            skipCount(SkipStats::disjCountIdentityRequiredFallbacks);
          }
          if (!prohibitedSources.empty()) {
            skipCount(SkipStats::disjCountIdentityProhibitedFallbacks);
          }
          if (!filterSuppliers.empty()) {
            skipCount(SkipStats::disjCountIdentityFilterFallbacks);
          }
          if (minShouldMatch > 1) {
            skipCount(SkipStats::disjCountIdentityMinMatchFallbacks);
          }
        }
        if (mandatorySources.empty() && optionalSources.empty()
            && prohibitedSources.empty() && !filterSuppliers.empty()) {
          return filterOnlyBulkScorer(targetPool);
        }
        if (needsScores && !prohibitedSources.empty()) {
          // Normalization hoists +(a b) -c to this exact shape. Required,
          // filtered, min-match, and single-positive forms retain their
          // existing pull/conjunction routing.
          bool maxScoreShape = mandatorySources.empty()
              && filterSuppliers.empty() && minShouldMatch <= 1
              && optionalSources.size() >= 2;
          if (!maxScoreShape) {
            skipCount(SkipStats::bulkExclusionShapeFallbacks);
            return nullptr;
          }
          if (disableBulkExclusionForTests) {
            skipCount(SkipStats::bulkExclusionDisabledFallbacks);
            return nullptr;
          }
          return maxScoreBulkScorer(targetPool, true);
        }
        // Filtered and negated counts use the windowed intersection only when
        // the positive body and every exclusion support exact dense fills.
        // Sparse positive bodies stay on the pull scorer.
        bool hasFilteredCountBody = !mandatorySources.empty()
            || (minShouldMatch >= 1 && !optionalSources.empty());
        if (!needsScores && hasFilteredCountBody
            && optionalSources.empty() == (minShouldMatch < 1)
            && (!prohibitedSources.empty() || !filterSuppliers.empty())
            && minShouldMatch <= 1) {
          if (!prohibitedSources.empty()
              && ConjunctionBulkScorer::disableNegatedCountForTests) {
            return nullptr;
          }
          auto* bulk = conjunctionBulkScorer(targetPool, true);
          return bulk != nullptr && bulk->willCountDense() ? bulk : nullptr;
        }
        if (needsScores && !filterSuppliers.empty()) {
          return disableFilteredScoredBulkForTests
              ? nullptr : filteredScoredBulkScorer(targetPool);
        }
        if (optionalSources.empty() && prohibitedSources.empty()
            && filterSuppliers.empty() && mandatorySources.size() >= 2) {
          // Two-phase clauses stay on the pull conjunction even unscored: it
          // flattens phrase approximations into the doc-level leapfrog and
          // verifies positions only on full agreement, while an opaque
          // phrase advance() verifies eagerly and loses badly (5x on a
          // dense-phrase + term conjunction).
          return conjunctionBulkScorer(targetPool, false);
        }
        if (!disableMandOptBulkForTests && mandatorySources.size() == 1
            && !optionalSources.empty() && prohibitedSources.empty()
            && filterSuppliers.empty() && minShouldMatch < 1) {
          return mandOptBulkScorer(targetPool);
        }
        // Shape gate only - scoring is not required. In count-only mode the
        // window loop exhaustively ORs each clause into the window bitset,
        // which is far cheaper than a doc-at-a-time heap disjunction.
        if (!mandatorySources.empty() || !prohibitedSources.empty()
            || !filterSuppliers.empty() || minShouldMatch > 1 || optionalSources.size() < 2) {
          return nullptr;
        }
        return maxScoreBulkScorer(targetPool);
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
        bool needsScores) {
      // Weight-time optional removal can expose a Boolean child that
      // normalization could not unwrap while the optionals were still present.
      // Preserve the child's complete supplier contract, including bulkScorer.
      if (minShouldMatch < 1 && mandatorySources.size() == 1
          && optionalSources.empty()
          && prohibitedSources.empty() && filterSuppliers.empty()) {
        auto* child = mandatorySources[0]->scorerSupplier(targetPool, segment);
        if (child != nullptr) return child;
      }
      return targetPool.make<Supplier>(
        targetPool, segment, mandatorySources, mandatoryScores, optionalSources,
        prohibitedSources, filterSuppliers, minShouldMatch, needsScores);
    }

    class BooleanPreparedWeight final : public Query::Weight::PreparedWeight {
      std::vector<QueryPrep::PreparedSource> mandatorySources;
      std::vector<uint8_t> mandatoryScores;
      std::vector<QueryPrep::PreparedSource> optionalSources;
      std::vector<QueryPrep::PreparedSource> prohibitedSources;
      std::vector<QueryPrep::MaterializedFilter> filterDomains;
      bool hasFilters = false;
      int minShouldMatch = 0;
      bool needsScores = false;

    public:
      BooleanPreparedWeight(std::vector<QueryPrep::PreparedSource>&& mandatorySources,
                            std::span<const uint8_t> mandatoryScores,
                            std::vector<QueryPrep::PreparedSource>&& optionalSources,
                            std::vector<QueryPrep::PreparedSource>&& prohibitedSources,
                            std::vector<QueryPrep::MaterializedFilter>&& filterDomains,
                            bool hasFilters, int minShouldMatch, bool needsScores)
        : mandatorySources(std::move(mandatorySources)),
          mandatoryScores(mandatoryScores.begin(), mandatoryScores.end()),
          optionalSources(std::move(optionalSources)),
          prohibitedSources(std::move(prohibitedSources)),
          filterDomains(std::move(filterDomains)),
          hasFilters(hasFilters), minShouldMatch(minShouldMatch),
          needsScores(needsScores) {}

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
          filterSuppliers, minShouldMatch, needsScores);
      }

      Query::Scorer* createScorer(MemPool& targetPool, IndexReader::Segment& segment) override {
        return scorerSupplier(targetPool, segment)->get(targetPool, std::numeric_limits<int64_t>::max());
      }

      bool outputIsSubsetOfDomain() const noexcept override {
        // Filter domains are materialized inside the outer prepare domains and
        // installed as a required supplier, so every emitted doc is a member
        // of the domain this weight was prepared against.
        return hasFilters;
      }
    };


  public:
    static inline bool disableUnscoredOptionalDropForTests = false;

    Weight(Context& context, NormalizedBoolean& query, int32_t flags, float multiplier)
      : Query::Weight(context, flags) {
      needsScores = (flags & Query::NEED_SCORES) != 0;
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
      //   decrements min_match (floored at 1), keeping the budget constant -
      //   a doc containing the duplicated term matches exactly as before,
      //   and a doc missing it is no longer charged for one absent term
      //   more than once.
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
    }

    std::unique_ptr<Query::Weight::PreparedWeight> prepare(Query::Weight::PrepareContext& ctx) override {
      auto filterSources = QueryPrep::prepareFilterSources(
          filterWeights, filterUses, ctx);
      std::vector<QueryPrep::MaterializedFilter> filterDomains(
          ctx.reader.segments().size());
      std::vector<DocSet*> childDomainPtrs(ctx.reader.segments().size());

      if (!filterSources.empty()) {
        for (size_t segnum = 0; segnum < ctx.reader.segments().size(); segnum++) {
          auto* outerDomain = ctx.domainPerSeg.empty() ? nullptr : ctx.domainPerSeg[segnum];
          filterDomains[segnum] = QueryPrep::materializeEffectiveIntersection(
            QueryPrep::preparedSpan(filterSources), filterUses, ctx.reader,
            ctx.reader.segments()[segnum], outerDomain);
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
        !filterSources.empty(), minShouldMatch, needsScores);
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
          filterSuppliers[i] = QueryPrep::filterSupplier(
              targetPool, *filterWeights[i], nullptr, filterUses[i],
              context.topReader, segment);
        }
      }
      return makeSupplier(targetPool, segment, mandatorySources, mandatoryScores,
                          optionalSources,
                          prohibitedSources, filterSuppliers, minShouldMatch, needsScores);
    }

    Scorer* createScorer(solux::MemPool& targetPool, solux::IndexReader::Segment& segment) override {
      return scorerSupplier(targetPool, segment)->get(targetPool, std::numeric_limits<int64_t>::max());
    }

    // Single-clause boolean shapes delegate to the wrapped clause. Filtered
    // count-only conjunctions exhaust the same per-window bulk intersection
    // used by collection, with filter clauses kept as lazy zero-score members.
    int64_t count(solux::IndexReader::Segment& segment) override {
      if (!prohibitedWeights.empty() || minShouldMatch > 1) {
        return -1;
      }
      bool hasFilteredCountBody = !mandatoryWeights.empty()
          || (minShouldMatch >= 1 && !optionalWeights.empty());
      if (!filterWeights.empty()) {
        if (!hasFilteredCountBody || needsScores || segment.liveDocs() != nullptr) {
          return -1;
        }
        auto guard = MemPool::threadLocalPoolGuard();
        auto* supplier = scorerSupplier(guard.pool(), segment);
        if (supplier == nullptr) {
          return 0;
        }
        auto* bulk = supplier->bulkScorer(guard.pool());
        if (bulk == nullptr) {
          return -1;
        }
        int64_t count = 0;
        for (int32_t cursor = 0;
             cursor != PostingsReader::END && cursor < segment.maxDoc(); ) {
          int32_t next = bulk->countNextWindow(
              count, nullptr, nullptr, cursor, segment.maxDoc());
          if (next == PostingsReader::END) {
            break;
          }
          assert(next > cursor);
          cursor = next;
        }
        return count;
      }
      if (mandatoryWeights.size() == 1 && optionalWeights.empty()) {
        return mandatoryWeights[0]->count(segment);
      }
      if (mandatoryWeights.empty() && optionalWeights.size() == 1) {
        return optionalWeights[0]->count(segment);
      }
      return -1;
    }
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

    bool optCanExist(int32_t upTo) const {
      int32_t optDoc = opt.docId();
      return optDoc != solux::PostingsReader::END && optDoc <= upTo;
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
    MandOptScorer(solux::MemPool& targetPool, Scorer* mandScorer, Scorer* optScorer)
      : mand{mandScorer, !disableTwoPhaseForTests && mandScorer->hasTwoPhase()},
        opt{optScorer, !disableTwoPhaseForTests && optScorer->hasTwoPhase()} {
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

    bool hasTwoPhase() const override {
      return anyTwoPhase();
    }

    int32_t approximationNext() override {
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
      if (!(minCompetitiveScore > 0.0f)) {
        id = mand.advance(target);
        return id;
      }
      return advanceInternal(target);
    }

    int32_t approximationDocId() override {
      return id;
    }

    bool matches() override {
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
        if (opt.twoPhase && !opt.scorer->matches()) {
          opt.next();
          return false;
        }
      } else if (opt.twoPhase && opt.docId() == reqDoc && !opt.scorer->matches()) {
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
      if (opt.twoPhase && optDoc == id && !opt.scorer->matches()) {
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
      assert(!mand->hasTwoPhase());
      assert(!opts.empty());
      scoreBoundFactor = 1.0 + (double) (opts.size() + 1) * 0x1p-24;
      for (auto* opt : opts) {
        assert(opt != nullptr);
        assert(!opt->hasTwoPhase());
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

  class MandNotScorer final : public Query::Scorer {
    Scorer* mandScorer;
    Scorer* notScorer;
    int32_t id = -1;
    int32_t notid = -1;
    bool notTwoPhase;
  public:
    static inline bool disableNotTwoPhaseForTests = false;

    MandNotScorer(solux::MemPool& targetPool, Scorer* mandScorer, Scorer* notScorer)
      : mandScorer(mandScorer), notScorer(notScorer),
        notTwoPhase(!disableTwoPhaseForTests && !disableNotTwoPhaseForTests
                    && notScorer->hasTwoPhase()) {
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

    std::span<Query::Scorer*> scorers; // subset of required clauses that contributes to score()
    std::span<Query::Scorer*> conjunctionClauses; // every direct required clause
    std::span<ApproxSlot> approximations; // every required clause, ascending cost (lead first)
    std::span<VerifierSlot> verifiers; // two-phase clauses sorted by matchCost

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
        // The pull conjunction refines near-theta ranges to block granularity:
        // its members can be two-phase (phrases), so a skipped range saves
        // position verification, unlike the all-term bulk scorer where block
        // refinement measured as a net loss and stays group-granular.
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
    // A/B hook: turn off block-max range skipping entirely.  (A minimum
    // evaluation stride like Lucene's window minimum was tried and measured
    // neutral-to-worse at 5M - the natural per-block range prunes best at low
    // k; revisit only with fresh profiles.)
    static inline bool disablePruningForTests = false;
    static inline bool disableApproxFlattenForTests = false;

    // allCosts contains each allScorers entry's supplier cost. scoringScorers is
    // the subset whose score() contributes to the conjunction score (filter
    // clauses iterate but do not score); every entry must also appear in
    // allScorers.
    // TODO: if any scoring scorer is boosted to 0 it could be dropped from the
    // scoring subset while staying in allScorers.
    ConjunctionScorer(solux::MemPool& pool, std::span<Query::Scorer*> allScorers,
                      std::span<int64_t> allCosts,
                      std::span<Query::Scorer*> scoringScorers)
            : scorers(scoringScorers), conjunctionClauses(allScorers) {
      assert(allScorers.size() == allCosts.size());
      auto flattened = pool.make_span<std::span<DocsPosEnum*>>(allScorers.size());
      size_t approximationCount = 0;
      size_t verifierCount = 0;
      bool expanded = false;
      for (size_t i = 0; i < allScorers.size(); i++) {
        bool twoPhase = !disableTwoPhaseForTests && allScorers[i]->hasTwoPhase();
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
        bool twoPhase = !disableTwoPhaseForTests && scorer->hasTwoPhase();
        if (!flattened[i].empty()) {
          for (DocsPosEnum* docsEnum : flattened[i]) {
            approximations[approximationIndex++] = {
              nullptr, docsEnum, docsEnum->numDocs(), ApproxSlot::Kind::DOCS_ENUM};
          }
          verifiers[verifierIndex++] = {scorer, 0, scorer->matchCost(), true};
          continue;
        }
        approximations[approximationIndex++] = {
          scorer, nullptr, allCosts[i], twoPhase ? ApproxSlot::Kind::TWO_PHASE
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
    }

    int32_t next() override {
      assert(docid != solux::PostingsReader::END);
      return doNext(leadTo(approximations[0].next()));
    }

    int32_t advance(int32_t docid) override {
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
    // maxDoc / kDenseThresholdInverse docs.  Far below Lucene's 32: word-
    // encoded blocks make clause window fills cheap, so windows beat the
    // docs-only leapfrog until the lead gets quite sparse (measured minimum
    // on the 5M benchmark corpus; 32 and 16384 are both ~20% slower).
    static constexpr int32_t kDenseThresholdInverse = 512;
    static inline bool disableDisjGroupBulkForTests = false;
    static inline bool disableDenseScoredForTests = false;
    static inline bool disableNegatedCountForTests = false;

  private:
    static constexpr int32_t kChunk = Postings::DOCS_BLOCK_SIZE;
    static constexpr int32_t kWindowSize = DocsEnumMeta::L1_DOCS;
    static constexpr int32_t kWindowWords = kWindowSize / 64;
    static constexpr int32_t kDenseLeapfrogThreshold = kWindowSize / 32;
    static_assert((kWindowSize % 64) == 0);

    struct TermClause {
      std::span<Query::Scorer*> members;
    };

    struct DenseClause {
      std::span<Query::Scorer*> members;
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
    std::span<TermQuery::Scorer*> termScorers; // populated when every scorer is a term
    std::span<TermClause> termClauses;  // direct terms or decomposed flat unions
    std::span<DenseClause> denseClauses; // exact window-fill capable clauses
    std::span<DenseClause> prohibitedDenseClauses;
    std::span<uint8_t> prohibitedExhausted;
    std::span<float> windowMax;         // per-clause bound over the current window
    std::span<double> suffixMax;        // suffixMax[c] = sum of windowMax[c..n)
    std::span<int32_t> candDocs;
    std::span<float> candScores;
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
    bool allTermScorers = false;
    bool allTermClauses = false;
    bool allDenseClauses = false;
    bool hasDisjGroup = false;
    bool denseHasDisjGroup = false;
    bool negatedCountPath = false;
    bool denseCountPath = false;
    bool denseScoredEligible = false;
    bool denseScoredCostRejected = false;
    const uint8_t* denseScoredNorms = nullptr;

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

    // Dense-scored admission thresholds were calibrated on the 5M benchmark
    // corpus. Keep them together so the measured policy remains legible.
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
      int32_t doc = PostingsReader::END;
      for (Query::Scorer* member : clause.members) {
        doc = std::min(doc, member->docId());
      }
      return doc;
    }

    int32_t denseClauseCountAdvance(
        const DenseClause& clause, int32_t target) {
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

    template <bool TermFast>
    int32_t scorerDocId(size_t index) {
      if constexpr (TermFast) {
        return termScorers[index]->docsEnum.docId();
      } else {
        return scorers[index]->docId();
      }
    }

    template <bool TermFast>
    int32_t scorerAdvance(size_t index, int32_t target) {
      if constexpr (TermFast) {
        return termScorers[index]->docsEnum.advance(target);
      } else {
        return scorers[index]->advance(target);
      }
    }

    template <bool TermFast>
    int32_t scorerCountAdvance(size_t index, int32_t target) {
      if constexpr (TermFast) {
        return termScorers[index]->docsEnum.advanceDocOnly(target);
      } else {
        return scorers[index]->advance(target);
      }
    }

    template <bool TermFast>
    int32_t scorerAdvanceShallow(size_t index, int32_t target) {
      if constexpr (TermFast) {
        return termScorers[index]->advanceShallow(target);
      } else {
        return scorers[index]->advanceShallow(target);
      }
    }

    template <bool TermFast>
    float scorerGetMaxScore(size_t index, int32_t upTo) {
      if constexpr (TermFast) {
        return termScorers[index]->getMaxScore(upTo);
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

    template <bool TermFast>
    float scorerScore(size_t index) {
      if constexpr (TermFast) {
        return termScorers[index]->score();
      } else {
        return scorers[index]->score();
      }
    }

    template <bool TermFast>
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

      int32_t leadDoc = scorerDocId<TermFast>(0);
      if (leadDoc < min) {
        leadDoc = scorerAdvance<TermFast>(0, min);
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

        // Window = [leadDoc, upTo], bounded by every clause's shallow block.
        int32_t upTo = max - 1;
        for (size_t c = 0; c < scorers.size(); c++) {
          upTo = std::min(upTo, scorerAdvanceShallow<TermFast>(c, leadDoc));
        }
        if (upTo < leadDoc) {
          upTo = leadDoc;
        }
        suffixMax[scorers.size()] = 0.0;
        for (size_t c = scorers.size(); c-- > 0; ) {
          windowMax[c] = scorerGetMaxScore<TermFast>(c, upTo);
          suffixMax[c] = suffixMax[c + 1] + (double) windowMax[c];
        }
        // Group-granular bounds only (see ConjunctionScorer::advanceCompetitive
        // note): block-level refinement measured as a net loss here - the
        // parse plus the 32x window shrink cost more than the added skips.
        if (suffixMax[0] * scoreBoundFactor < (double) this->minCompetitiveScore) {
          // Nothing in this window can compete: hop the lead without touching
          // the other clauses or any scoring.
          skippedWindowCount++;
          if (upTo >= max - 1) {
            out.max = max;
            return upTo + 1;
          }
          leadDoc = scorerAdvance<TermFast>(0, upTo + 1);
          continue;
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
          if (out.size + kChunk > (int32_t) outDocs.size()) {
            // out is nearly full: end the window early; the next call resumes
            // the rest of this window right after the last decided doc.
            assert(lastDecided >= 0);
            return lastDecided + 1;
          }
          int32_t n = scorerFillScoreBlock<TermFast>(
              0, candDocs.data(), candScores.data(), kChunk, upTo + 1);
          if (n == 0) {
            break;
          }
          lastDecided = candDocs[(size_t) n - 1];
          bool fillFilter = windowFilter != nullptr && !windowFilter->probes();
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
            const double remaining = suffixMax[c];  // clauses [c, end) add at most this
            int32_t scorerDoc = scorerDocId<TermFast>(c);
            int32_t w = 0;
            for (int32_t i = 0; i < n; i++) {
              int32_t doc = candDocs[(size_t) i];
              float sum = candScores[(size_t) i];
              if (((double) sum + remaining) * scoreBoundFactor
                  < (double) this->minCompetitiveScore) {
                continue;  // cannot compete no matter what the rest contribute
              }
              if (scorerDoc < doc) {
                scorerDoc = scorerAdvance<TermFast>(c, doc);
              }
              if (scorerDoc != doc) {
                continue;  // not in the conjunction
              }
              candDocs[(size_t) w] = doc;
              candScores[(size_t) w] = sum + scorerScore<TermFast>(c);
              w++;
            }
            n = w;
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
            int32_t scorerDoc = denseClauseDocId(denseClauses[c]);
            if (scorerDoc < doc) {
              scorerDoc = denseClauseCountAdvance(denseClauses[c], doc);
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
      for (size_t c = 1; c < denseClauses.size(); c++) {
        clearWindowBits(clauseBits);
        fillDenseClauseWindowBits(
            denseClauses[c], clauseBits, windowBase, windowEnd);
        for (int32_t w = 0; w < kWindowWords; w++) {
          windowBits[(size_t) w] &= clauseBits[(size_t) w];
        }
        if (denseClauses.size() >= 3 && c + 1 < denseClauses.size()) {
          int32_t card = 0;
          for (uint64_t bits : windowBits) {
            card += (int32_t) std::popcount(bits);
          }
          if (card < kDenseLeapfrogThreshold) {
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
      // Construction bounds summed non-lead cost at 8x lead, so total fill is
      // about 9x lead work; density bounds survivor scoring. An absolute cap
      // priced neither side and blocked high-lead/deep-k wins: american south
      // (82.6 survivors/window, density 0.184, ratio 0.489) and to be or not
      // to be (401.9 survivors/window, density 0.473).
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
      if (domainOut != nullptr) {
        domainOut->addWindowWords(windowBits.data(), windowBase, windowEnd);
      }
      count += popCountWindowBits();

      return windowEnd >= max ? PostingsReader::END : windowEnd;
    }

    template <bool TermFast, typename Consumer>
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
        int32_t doc = scorerDocId<TermFast>(0);
        if (doc < target) {
          doc = scorerCountAdvance<TermFast>(0, target);
        }
        if (doc >= windowEnd) {
          return doc;  // sound resume point (or END); covers doc == END
        }
        target = doc;

        bool matched = true;
        for (size_t c = 1; c < scorers.size(); c++) {
          int32_t scorerDoc = scorerDocId<TermFast>(c);
          if (scorerDoc < target) {
            scorerDoc = scorerCountAdvance<TermFast>(c, target);
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

    template <bool TermFast>
    int32_t countNextWindowSparse(int64_t& count, DocSetBuilder* domainOut,
                                  DocSet* filter, int32_t min, int32_t max) {
      skipCount(SkipStats::conjCountFallbacks);
      int32_t windowEnd;
      return visitNextWindowSparse<TermFast>(
          filter, min, max, windowEnd, [&](int32_t doc) {
            count++;
            if (domainOut != nullptr) {
              domainOut->add(doc);
            }
            return true;
          });
    }

  public:
    ConjunctionBulkScorer(solux::MemPool& pool, std::span<Query::Scorer*> scorers,
                          std::span<Query::Scorer*> prohibitedScorers,
                          int32_t maxDoc, int64_t leadCost,
                          int64_t nonLeadCost, bool scoredConstruction)
        : scorers(scorers),
          prohibitedScorers(prohibitedScorers),
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
          candDocs(pool.make_arr<int32_t>((size_t) kChunk), (size_t) kChunk),
          candScores(pool.make_arr<float>((size_t) kChunk), (size_t) kChunk),
          outDocs(pool.make_arr<int32_t>((size_t) kWindowSize), (size_t) kWindowSize),
          outScores(pool.make_arr<float>((size_t) kWindowSize), (size_t) kWindowSize),
          windowBits(pool.make_arr<uint64_t>((size_t) kWindowWords), (size_t) kWindowWords),
          clauseBits(pool.make_arr<uint64_t>((size_t) kWindowWords), (size_t) kWindowWords),
          groupMatchBits(
              pool.make_arr<uint64_t>(((size_t) kChunk + 63) >> 6),
              ((size_t) kChunk + 63) >> 6),
          groupScores(pool.make_arr<float>((size_t) kChunk), (size_t) kChunk),
          maxDoc(maxDoc) {
      assert(!scorers.empty());
      std::fill(prohibitedExhausted.begin(), prohibitedExhausted.end(), 0);
      allTermScorers = true;
      allTermClauses = true;
      allDenseClauses = true;
      size_t scoreAddends = 0;
      for (size_t i = 0; i < scorers.size(); i++) {
        std::span<Query::Scorer*> denseMembers;
        if (scorers[i]->supportsWindowFilter()) {
          denseMembers = scorers.subspan(i, 1);
        } else if (!disableDisjGroupBulkForTests) {
          denseMembers = scorers[i]->flatDisjunctionScorers();
          for (Query::Scorer* member : denseMembers) {
            if (!member->supportsWindowFilter()) {
              denseMembers = {};
              break;
            }
          }
        }
        denseClauses[i].members = denseMembers;
        allDenseClauses &= !denseMembers.empty();
        denseHasDisjGroup |= denseMembers.size() > 1;

        termScorers[i] = dynamic_cast<TermQuery::Scorer*>(scorers[i]);
        allTermScorers &= termScorers[i] != nullptr;
        if (termScorers[i] != nullptr) {
          termClauses[i].members = scorers.subspan(i, 1);
          scoreAddends++;
          continue;
        }
        std::span<Query::Scorer*> members;
        if (!disableDisjGroupBulkForTests) {
          members = scorers[i]->flatDisjunctionScorers();
        }
        for (auto* member : members) {
          if (dynamic_cast<TermQuery::Scorer*>(member) == nullptr) {
            members = {};
            break;
          }
        }
        termClauses[i].members = members;
        allTermClauses &= !members.empty();
        hasDisjGroup |= !members.empty();
        scoreAddends += members.empty() ? 1 : members.size();
      }
      bool allDenseProhibited = true;
      for (size_t i = 0; i < prohibitedScorers.size(); i++) {
        std::span<Query::Scorer*> denseMembers;
        if (prohibitedScorers[i]->supportsWindowFilter()) {
          denseMembers = prohibitedScorers.subspan(i, 1);
        } else if (!disableDisjGroupBulkForTests) {
          denseMembers = prohibitedScorers[i]->flatDisjunctionScorers();
          for (Query::Scorer* member : denseMembers) {
            if (!member->supportsWindowFilter()) {
              denseMembers = {};
              break;
            }
          }
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
      negatedCountPath = !prohibitedScorers.empty();
      denseCountPath = allDenseClauses && allDenseProhibited
          && maxDoc >= kWindowSize
          && leadCost >= std::max<int64_t>(
              1, (int64_t) maxDoc / kDenseThresholdInverse);
      denseScoredEligible = scoredConstruction && denseCountPath
          && !negatedCountPath && allTermScorers
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
              nonLeadCost > leadCost * kDenseAdmissionMaxNonLeadCostRatio;
          denseScoredEligible &= !denseScoredCostRejected;
        }
        if (denseScoredEligible) {
          denseFreqs = pool.make_span<int32_t>(
              scorers.size() * (size_t) kWindowSize);
        }
      }
    }

    void setTopKDepth(int32_t topK, bool allowPruning) override {
      // Unpruned collection (an exact total count pins the threshold at its
      // lowest) leaves score-first with no skipping at all, which is the
      // deep-k limit of the density bar. Price it at the deepest calibrated
      // depth: the curve was measured to k=1000, so that is the honest
      // ceiling even though nothing prunes here. The shallow-k entry gate
      // exists for the same reason - shallow requests prune hardest - so it
      // reads the effective depth too: without pruning, requested depth only
      // sizes the heap, it does not change the work either path must do.
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
      return denseCountPath;
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
      // The dense scored path fills EVERY clause across the window, so its
      // cost does not shrink when a filter rejects most docs - only its output
      // does. Its admission criteria were calibrated unfiltered, where a low
      // survivor density means the intersection itself is selective and the
      // fill is cheap per result; under a selective filter the same low
      // density instead means the filter threw the fill away. Measured on the
      // 300-query intersection class at TOP_100 (dense-scored off/on, so >1
      // means the dense path is LOSING):
      //   unfiltered 1.051 | 42.6% 1.017 | 9.9% 1.038   <- keep dense
      //   4.98% 0.926 | 3.12% 0.959 | 1.99% 0.897 | 0.99% 0.872 | 0.50% 0.837
      // so require the filter to be at least maxDoc/16 (6.25%) dense, which
      // puts every measured point on its winning side.
      if (filter->cost()
          < (int64_t) maxDoc / kDenseScoredMinFilterDensityInverse) {
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
      return allTermScorers
          ? scoreNextWindowImpl<true>(out, filter, min, max, minCompetitiveScore)
          : scoreNextWindowImpl<false>(out, filter, min, max, minCompetitiveScore);
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
          : visitNextWindowSparse<false>(filter, min, max, windowEnd, emit);
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
      if (denseCountPath) {
        return countNextWindowDense(count, domainOut, filter, min, max);
      }
      return allTermScorers
          ? countNextWindowSparse<true>(count, domainOut, filter, min, max)
          : countNextWindowSparse<false>(count, domainOut, filter, min, max);
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
  }; // ConjunctionBulkScorer


  class DisjunctionScorer final : public Query::Scorer {
    struct ApproxSlot {
      Scorer* scorer = nullptr;
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

      float matchCost() const {
        return twoPhase ? scorer->matchCost() : 0.0f;
      }
    };

    std::span<Scorer*> clauses;  // stable decomposition order
    std::span<ApproxSlot> members;  // stable score order and protocol latches
    std::span<ApproxSlot*> heap;
    std::span<ApproxSlot*> verificationOrder;

    // TODO: OPT: heapifying with virtual methods prob isn't a good idea... pull out and save the docid.
    constexpr static auto idComparator = [](ApproxSlot& a, ApproxSlot& b) {
      return b.docId() < a.docId();
    };

    solux::IndirectPQ<ApproxSlot, decltype(idComparator)> pq;

    int32_t docid = -1;
    // Scorers expose no approximation cost for a weighted estimate.
    float verificationCost = 0.0f;
    bool anyTwoPhase = false;

    static std::span<ApproxSlot> makeMembers(solux::MemPool& pool,
                                              std::span<Scorer*> scorers) {
      auto members = pool.make_span<ApproxSlot>(scorers.size());
      bool enableTwoPhase = !disableTwoPhaseForTests && !disableDisjTwoPhaseForTests;
      for (size_t i = 0; i < scorers.size(); i++) {
        members[i] = {scorers[i], enableTwoPhase && scorers[i]->hasTwoPhase()};
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

    DisjunctionScorer(solux::MemPool& pool, std::span<Scorer*> scorers)
            : clauses(scorers), members(makeMembers(pool, scorers)),
              heap(makePointers(pool, members)),
              verificationOrder(makePointers(pool, members)), pq(heap) {
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

    bool hasTwoPhase() const override {
      return anyTwoPhase;
    }

    int32_t approximationNext() override {
      // Contract: callers must not re-poll after END (see Query::Scorer).
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

    // Advance every member below target through its latched protocol. Members
    // are touched at most once: each surfaces at the top while behind,
    // advances to >= target, and sifts down.
    int32_t approximationAdvance(int32_t target) override {
      assert(docid < target);
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
      assert(pq.size() > 0 && docid == pq.top().docId());
      float score = 0.0f;
      for (auto& member : members) {
        if (member.docId() == docid
            && (!member.twoPhase || member.scorer->matches())) {
          score += member.scorer->score();
        }
      }
      return score;
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
      // discards it.  Summation order is NOT bit-stable across execution
      // paths; only match sets are (accepted policy).
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


  class MaxScoreBulkScorer final : public BulkScorer {
  public:
    static inline bool disableDisjConjBulkForTests = false;

  private:
    constexpr static int32_t kWindowSize = DocsEnumMeta::L1_DOCS;
    constexpr static int32_t kWindowWords = kWindowSize / 64;
    constexpr static size_t kBs1MinClauses = 16;
    // The identity trades streaming the largest postings list for one scalar
    // membership probe per doc of the smaller terms. A df RATIO gate misprices
    // it: the largest clause in a skewed union is dense and word-encoded, so
    // streaming it is nearly free per doc, and there is far less to save than a
    // ratio implies. Measured on 5M (smaller-side df -> time vs enumeration):
    // <100 0.15x, 100-1k 0.29x, 1k-10k 1.01x, 10k-100k 1.15x. So the gate is an
    // ABSOLUTE probe budget, plus a ratio floor so we only pay it where there is
    // something to save (tiny-OR-tiny unions are already fast and just fall
    // back). A ratio-only gate at 32 admitted 95k probes on "the globe
    // newspaper" and lost 4.6x.
    constexpr static int64_t kDisjunctionCountIdentityMaxProbes = 1024;
    constexpr static int64_t kDisjunctionCountIdentityMinDfRatio = 32;
    // Domain-drive gate weights: drive only when card*nClauses*W < SUM(clause.cost()).
    // W is the per-advance penalty (advance cost vs a vectorized block decode). HARDWARE
    // SENSITIVE (vector throughput vs scalar skip cost; ISA): calibrated on an Intel hybrid
    // 2P+8E laptop to a ~1% crossover; RE-VALIDATE on uniform desktop / cloud / ARM (NEON/
    // SVE shifts the ratio). W_ARRAY < W_BITSET because array stream membership is an
    // O(log card) ArrDocSet::get binary search vs bitset O(1) get (W_ARRAY reasoned from the
    // ~15% delta, not swept).
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
    // which clauses are essential in a window - accepted policy (execution
    // paths are not required to be bit-identical).
    std::span<float> windowScores;
    std::span<int32_t> outDocs;
    std::span<float> outScores;
    std::span<CountClause> countClauses;
    std::span<Query::Scorer*> countTerms;
    std::span<uint64_t> countClauseBits;
    std::span<uint64_t> countTermBits;
    std::span<Query::Scorer*> disjCountIdentityOthers;
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
                                           bool countOnly,
                                           bool hasDeletes) {
      if (!countOnly || disableDisjunctionCountIdentityForTests) {
        return;
      }
      if (hasDeletes) {
        skipCount(SkipStats::disjCountIdentityDeleteFallbacks);
        return;
      }
      assert(clauseCosts.size() == scorers.size());

      size_t largestIndex = 0;
      int64_t largestDf = -1;
      int64_t totalDf = 0;
      for (size_t i = 0; i < scorers.size(); i++) {
        if (dynamic_cast<TermQuery::Scorer*>(scorers[i]) == nullptr) {
          skipCount(SkipStats::disjCountIdentityNonTermFallbacks);
          return;
        }
        int64_t df = clauseCosts[i];
        assert(df >= 0);
        if (df > largestDf) {
          largestDf = df;
          largestIndex = i;
        }
        if (df >= std::numeric_limits<int64_t>::max() - totalDf) {
          totalDf = std::numeric_limits<int64_t>::max();
        } else {
          totalDf += df;
        }
      }

      int64_t otherDf = totalDf == std::numeric_limits<int64_t>::max()
          ? totalDf : totalDf - largestDf;
      if (otherDf <= 0
          || otherDf > kDisjunctionCountIdentityMaxProbes
          || otherDf > largestDf / kDisjunctionCountIdentityMinDfRatio) {
        skipCount(SkipStats::disjCountIdentityProfitabilityFallbacks);
        return;
      }

      disjCountIdentityOthers =
          pool.make_span<Query::Scorer*>(scorers.size() - 1);
      size_t other = 0;
      for (size_t i = 0; i < scorers.size(); i++) {
        if (i != largestIndex) {
          disjCountIdentityOthers[other++] = scorers[i];
        }
      }
      assert(other == disjCountIdentityOthers.size());
      disjCountIdentityLargest =
          static_cast<TermQuery::Scorer*>(scorers[largestIndex]);
      disjCountIdentityLargestDf = largestDf;
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
      // BS1 wins when every clause streams exhaustively into the shared score
      // row. Once a clause is non-essential, the ordinary path can leave its
      // postings untouched and probe only surviving candidates; BS1 instead
      // decodes and scores that clause across the whole window.
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
          doc = scorer->advance(target);
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
      // Correctness fallback for ARRAY filters. Step 3 wiring keeps the hot path on
      // null/BITSET filters until this is measured.
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
      // Drive cost is roughly card*nClauses advances. Stream cost is roughly
      // sum(clause.cost()) vectorized decodes. W is the measured advance/decode
      // ratio. ARRAY gets a lower W because stream-side membership is a binary
      // search in ArrDocSet::get, while BITSET stream membership is bits.get().
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
    // doc-at-a-time paths in the last bit (vectorized vs scalar rounding) -
    // accepted policy, execution paths are not required to be bit-identical.
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
          // BS1 intentionally defers filter membership to one check per candidate.
          // Selective filters may accumulate rejected docs, but dense windows are the
          // path this mode is for.
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
                       bool enableDisjConjCount, bool hasDeletes)
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
          pool, enableDisjConjCount, hasDeletes);
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
      if (domainOut != nullptr) {
        domainOut->addWindowWords(windowBits.data(), windowStart, windowEnd);
      }
      for (size_t w = 0; w < windowBits.size(); w++) {
        count += std::popcount(windowBits[w]);
      }
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
