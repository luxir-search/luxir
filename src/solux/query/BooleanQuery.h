#pragma once

#include <algorithm>
#include <bit>
#include <cmath>

#include "Query.h"
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

  static TermQuery* dedupableTerm(Query* query) {
    auto* term = dynamic_cast<TermQuery*>(query);
    if (term == nullptr || term->hasInjectedStats()) return nullptr;
    return term;
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
      if (auto* term = dedupableTerm(query)) {
        float boost = term->getBoost();
        bool merged = false;
        for (size_t j = i + 1; j < clauses.size(); j++) {
          if (consumed[j]) continue;
          auto* other = dedupableTerm(clauses[j]);
          if (other == nullptr || !sameTermIdentity(*term, *other)) continue;
          boost += other->getBoost();
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
      auto* term = dedupableTerm(clauses[i]);
      bool duplicate = false;
      if (term != nullptr) {
        for (Query* prior : out) {
          auto* priorTerm = dedupableTerm(prior);
          if (priorTerm != nullptr && sameTermIdentity(*priorTerm, *term)) {
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
  // A/B toggle: force the conjunction onto the eager single-phase path (each
  // clause verifies inside its own advance) instead of two-phase (defer matches
  // until the approximations agree). For benchmarking the two-phase win only.
  static inline bool disableTwoPhaseForTests = false;
  // Pull-conjunction refinement band: refine bounds to block granularity when
  // theta reaches this fraction of the group-granular range bound.
  static constexpr double kRefineBeta = 0.75;

  BooleanQuery(std::span<Query*> mandatory, std::span<Query*> optional, std::span<Query*> prohibited,
               std::span<Query*> filter, int minShouldMatch = 0)
          : mandatory(mandatory), optional(optional), prohibited(prohibited), filter(filter),
            minShouldMatch(minShouldMatch) {
  }

  Weight* createWeight(Context& context, int32_t flags) override {
    return context.pool.make<BooleanQuery::Weight>(context, *this, flags);
  }

  class Weight final : public Query::Weight {
    std::span<Query::Weight*> mandatoryWeights;
    std::span<Query::Weight*> optionalWeights;
    std::span<Query::Weight*> prohibitedWeights;
    std::span<Query::Weight*> filterWeights;
    int minShouldMatch = 0;
    bool needsScores = false;

    // Returns a span of Weights, corresponding to the given span of Queries. Some weights can be null.
    std::span<Query::Weight*> createWeights(solux::MemPool& targetPool, Context& context,
                                            std::span<Query*> queries, int32_t flags) {
      if (queries.size() == 0) {
        return {};
      }
      auto weights = targetPool.make_arr<Query::Weight*>(queries.size());
      for (int i = 0; i < queries.size(); ++i) {
        weights[i] = queries[i]->createWeight(context, flags);
      }
      return {weights, queries.size()};
    }

    // Outcome of building the required (mandatory + filter) conjunction for a
    // segment. `scorer` is null when there are no required clauses at all;
    // `unsatisfiable` is true when a required clause cannot match this segment,
    // so the whole boolean cannot match it.
    struct Required {
      Query::Scorer* scorer;
      bool unsatisfiable;
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
        std::span<Query::ScorerSupplier* const> filterSuppliers) {
      struct Entry {
        int64_t cost;
        Query::ScorerSupplier* supplier;
        bool scoring;
      };
      boost::container::small_vector<Entry, 16> entries;
      for (auto* source : mandatorySources) {
        auto* supplier = source->scorerSupplier(targetPool, segment);
        if (supplier == nullptr) return {nullptr, true};
        entries.push_back({supplier->cost(), supplier, true});
      }
      for (auto* supplier : filterSuppliers) {
        if (supplier == nullptr) return {nullptr, true};
        entries.push_back({supplier->cost(), supplier, false});
      }
      if (entries.empty()) return {nullptr, false};

      // leadCost is the cost of the sparsest required clause: it bounds how often
      // the others get driven, so each may plan eager vs lazy setup off it.
      int64_t leadCost = std::numeric_limits<int64_t>::max();
      for (auto& e : entries) leadCost = std::min(leadCost, e.cost);

      // Sparsest first so allScorers[0] leads the conjunction.
      std::sort(entries.begin(), entries.end(),
                [](const Entry& a, const Entry& b) { return a.cost < b.cost; });

      auto* all = targetPool.make_arr<Query::Scorer*>(entries.size());
      Query::Scorer** scoring = mandatorySources.empty()
        ? nullptr
        : targetPool.make_arr<Query::Scorer*>(mandatorySources.size());
      size_t allCount = 0;
      size_t scoringCount = 0;
      for (auto& e : entries) {
        auto* scorer = e.supplier->get(targetPool, leadCost);
        if (scorer == nullptr) return {nullptr, true};
        all[allCount++] = scorer;
        if (e.scoring) scoring[scoringCount++] = scorer;
      }

      // A lone scoring clause (one mandatory, no filters) needs no wrapper.
      if (allCount == 1 && scoringCount == 1) return {all[0], false};
      return {targetPool.make<BooleanQuery::ConjunctionScorer>(
                targetPool, std::span<Query::Scorer*>(all, allCount),
                std::span<Query::Scorer*>(scoring, scoringCount)),
              false};
    }

    // Keep clause wiring in one place so prepared and non-prepared execution
    // cannot diverge on filter/prohibited semantics.
    static Query::Scorer* assembleScorer(
        MemPool& targetPool,
        IndexReader::Segment& segment,
        std::span<Query::SegmentSource* const> mandatorySources,
        std::span<Query::SegmentSource* const> optionalSources,
        std::span<Query::SegmentSource* const> prohibitedSources,
        std::span<Query::ScorerSupplier* const> filterSuppliers,
        int minShouldMatch,
        bool needsScores) {
      Required req = assembleRequired(targetPool, segment, mandatorySources, filterSuppliers);
      if (req.unsatisfiable) return nullptr;
      Query::Scorer* reqScorer = req.scorer;
      bool hasMandatory = !mandatorySources.empty();
      // Whether the optional group CONSTRAINS matching (Lucene bool
      // semantics): min_match >= 1 makes it a real constraint; otherwise
      // optionals only rank, provided a required or filter clause already
      // carries the match (reqScorer non-null iff such clauses exist).  With
      // nothing else, at least one optional must match - the classic
      // should-only boolean.
      bool optionalsConstrain = minShouldMatch >= 1 || reqScorer == nullptr;

      // For min-should-match (> 1) order the optional scorers by cost so the
      // pigeonhole lead/tail split leads with the cheapest (sparsest) iterators.
      auto optionalScorers = minShouldMatch > 1
        ? QueryPrep::createScorersByCost(targetPool, segment, optionalSources)
        : QueryPrep::createScorers(targetPool, segment, optionalSources);
      Query::Scorer* optScorer = nullptr;
      if (!optionalScorers.empty()) {
        int optCount = (int)optionalScorers.size();
        bool useMaxScoreDisjunction = needsScores && reqScorer == nullptr
          && prohibitedSources.empty() && minShouldMatch <= 1 && optCount >= 2;
        // minShouldMatch applies to optional scorers that exist in this segment.
        if (minShouldMatch <= 1) {
          if (optCount == 1) {
            optScorer = optionalScorers[0];
          } else if (useMaxScoreDisjunction) {
            optScorer = targetPool.make<BooleanQuery::MaxScoreDisjunctionScorer>(
              targetPool, optionalScorers, segment.maxDoc());
          } else {
            optScorer = targetPool.make<BooleanQuery::DisjunctionScorer>(targetPool, optionalScorers);
          }
        } else if (optCount == minShouldMatch) {
          // Every surviving optional clause is required and scores.
          optScorer = targetPool.make<BooleanQuery::ConjunctionScorer>(
            targetPool, optionalScorers, optionalScorers);
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
        size_t nscoring = hasMandatory ? 2 : 1;
        std::span<Query::Scorer*> scoringSpan(targetPool.make_arr<Query::Scorer*>(nscoring), nscoring);
        scoringSpan[nscoring - 1] = optScorer;
        if (hasMandatory) scoringSpan[0] = reqScorer;
        boolScorer = targetPool.make<BooleanQuery::ConjunctionScorer>(targetPool, allSpan, scoringSpan);
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
      int n = (int)costs.size();
      if (n == 0) return 0;
      if (minShouldMatch >= n) {  // every optional clause required -> rarest
        int64_t m = maxDoc;
        for (auto c : costs) m = std::min(m, c);
        return m;
      }
      int take = minShouldMatch <= 1 ? n : n - minShouldMatch + 1;
      if (take < n) std::sort(costs.begin(), costs.end());
      int64_t sum = 0;
      for (int i = 0; i < take; i++) sum += costs[i];
      return std::min(sum, maxDoc);
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
      std::span<Query::SegmentSource* const> optionalSources;
      std::span<Query::SegmentSource* const> prohibitedSources;
      std::span<Query::ScorerSupplier* const> filterSuppliers;
      int minShouldMatch;
      bool needsScores;
    public:
      Supplier(MemPool& pool, IndexReader::Segment& segment,
               std::span<Query::SegmentSource* const> mandatorySources,
               std::span<Query::SegmentSource* const> optionalSources,
               std::span<Query::SegmentSource* const> prohibitedSources,
               std::span<Query::ScorerSupplier* const> filterSuppliers,
               int minShouldMatch,
               bool needsScores)
        : pool(pool), segment(segment), mandatorySources(mandatorySources),
          optionalSources(optionalSources), prohibitedSources(prohibitedSources),
          filterSuppliers(filterSuppliers), minShouldMatch(minShouldMatch),
          needsScores(needsScores) {}

      int64_t cost() override {
        return compositeCost(pool, segment, mandatorySources, optionalSources, filterSuppliers, minShouldMatch);
      }

      Query::Scorer* get(MemPool& targetPool, int64_t leadCost) override {
        unused(leadCost);
        return assembleScorer(targetPool, segment, mandatorySources, optionalSources,
                              prohibitedSources, filterSuppliers, minShouldMatch, needsScores);
      }

      // Bulk path for pure scored conjunctions (>= 2 mandatory clauses, nothing
      // else).  Clauses are created in ascending cost order (the sparsest
      // leads, as in assembleRequired); any two-phase member bails to the pull
      // ConjunctionScorer, which owns the verifier machinery.
      BulkScorer* conjunctionBulkScorer(MemPool& targetPool) {
        struct Entry {
          int64_t cost;
          Query::ScorerSupplier* supplier;
        };
        boost::container::small_vector<Entry, 16> entries;
        for (auto* source : mandatorySources) {
          auto* supplier = source->scorerSupplier(targetPool, segment);
          if (supplier == nullptr) {
            return nullptr;  // a required clause cannot match this segment
          }
          entries.push_back({supplier->cost(), supplier});
        }
        int64_t leadCost = std::numeric_limits<int64_t>::max();
        for (auto& e : entries) leadCost = std::min(leadCost, e.cost);
        std::sort(entries.begin(), entries.end(),
                  [](const Entry& a, const Entry& b) { return a.cost < b.cost; });
        auto* arr = targetPool.make_arr<Query::Scorer*>(entries.size());
        for (size_t i = 0; i < entries.size(); i++) {
          auto* scorer = entries[i].supplier->get(targetPool, leadCost);
          if (scorer == nullptr || scorer->hasTwoPhase()) {
            return nullptr;
          }
          arr[i] = scorer;
        }
        return targetPool.make<BooleanQuery::ConjunctionBulkScorer>(
            targetPool, std::span<Query::Scorer*>(arr, entries.size()), segment.maxDoc(), leadCost);
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

      BulkScorer* bulkScorer(MemPool& targetPool) override {
        // Scored or not: unscored clauses bound as +infinity, so the window
        // skip never fires and the bulk intersection runs exhaustively - the
        // right execution for exact conjunction counts too.
        if (optionalSources.empty() && prohibitedSources.empty()
            && filterSuppliers.empty() && mandatorySources.size() >= 2) {
          return conjunctionBulkScorer(targetPool);
        }
        if (!disableMandOptBulkForTests && mandatorySources.size() == 1
            && !optionalSources.empty() && prohibitedSources.empty()
            && filterSuppliers.empty() && minShouldMatch < 1) {
          return mandOptBulkScorer(targetPool);
        }
        // Shape gate only - scoring is not required. Without scores the clause
        // scorers report score()=0 / getMaxScore()=+inf, the window split stays
        // at zero (every clause essential), and the window loop degenerates to
        // an exhaustive per-clause OR into the window bitset - the right
        // execution for unscored counting, far cheaper than the doc-at-a-time
        // heap disjunction.
        if (!mandatorySources.empty() || !prohibitedSources.empty()
            || !filterSuppliers.empty() || minShouldMatch > 1 || optionalSources.size() < 2) {
          return nullptr;
        }
        auto& optionalScorersVec = *targetPool.make_vec<Query::Scorer*>();
        optionalScorersVec.reserve(optionalSources.size());
        int64_t aggregateClauseCost = 0;
        for (auto* source : optionalSources) {
          auto* supplier = source->scorerSupplier(targetPool, segment);
          if (supplier == nullptr) {
            continue;
          }
          int64_t cost = supplier->cost();
          auto* scorer = supplier->get(targetPool, std::numeric_limits<int64_t>::max());
          if (scorer == nullptr) {
            continue;
          }
          optionalScorersVec.push_back(scorer);
          if (cost > 0
              && aggregateClauseCost < std::numeric_limits<int64_t>::max()) {
            int64_t room = std::numeric_limits<int64_t>::max() - aggregateClauseCost;
            if (cost >= room) {
              aggregateClauseCost = std::numeric_limits<int64_t>::max();
            } else {
              aggregateClauseCost += cost;
            }
          }
        }
        std::span<Query::Scorer*> optionalScorers(optionalScorersVec.data(), optionalScorersVec.size());
        if (optionalScorers.size() < 2) {
          return nullptr;
        }
        return targetPool.make<BooleanQuery::MaxScoreBulkScorer>(
          targetPool, optionalScorers, segment.maxDoc(), aggregateClauseCost);
      }
    };

    class BooleanPreparedWeight final : public Query::Weight::PreparedWeight {
      std::vector<QueryPrep::PreparedSource> mandatorySources;
      std::vector<QueryPrep::PreparedSource> optionalSources;
      std::vector<QueryPrep::PreparedSource> prohibitedSources;
      std::vector<std::unique_ptr<DocSet>> filterDomains;
      bool hasFilters = false;
      int minShouldMatch = 0;
      bool needsScores = false;

    public:
      BooleanPreparedWeight(std::vector<QueryPrep::PreparedSource>&& mandatorySources,
                            std::vector<QueryPrep::PreparedSource>&& optionalSources,
                            std::vector<QueryPrep::PreparedSource>&& prohibitedSources,
                            std::vector<std::unique_ptr<DocSet>>&& filterDomains,
                            bool hasFilters, int minShouldMatch, bool needsScores)
        : mandatorySources(std::move(mandatorySources)),
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
        return targetPool.make<BooleanQuery::Weight::Supplier>(
          targetPool, segment,
          QueryPrep::segmentSources(targetPool, QueryPrep::preparedSpan(mandatorySources)),
          QueryPrep::segmentSources(targetPool, QueryPrep::preparedSpan(optionalSources)),
          QueryPrep::segmentSources(targetPool, QueryPrep::preparedSpan(prohibitedSources)),
          filterSuppliers, minShouldMatch, needsScores);
      }

      Query::Scorer* createScorer(MemPool& targetPool, IndexReader::Segment& segment) override {
        return scorerSupplier(targetPool, segment)->get(targetPool, std::numeric_limits<int64_t>::max());
      }
    };


  public:
    Weight(Context& context, BooleanQuery& query, int32_t flags) : Query::Weight(context, flags) {
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
      mandatoryWeights = createWeights(context.pool, context, mandatoryClauses, flags);
      // With a mandatory clause and minShouldMatch unset, optional clauses are
      // a pure score add (MandOpt) - they never affect membership.  Without
      // scores they contribute nothing, so skip building their weights entirely
      // (Lucene's BooleanWeight scorer simplification).  This also exposes
      // "+a b" count-only requests to the single-clause count() shortcut.
      // minShouldMatch >= 1 makes the optional group a membership constraint
      // even under a mandatory clause, so it must be kept.
      bool dropOptional = !needsScores && !mandatoryClauses.empty() && query.minShouldMatch < 1;
      optionalWeights = dropOptional
        ? std::span<Query::Weight*>{}
        : createWeights(context.pool, context, optionalClauses, flags);
      prohibitedWeights = createWeights(context.pool, context, prohibitedClauses, noScore);
      filterWeights = createWeights(context.pool, context, filterClauses, noScore);
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
      bool constant = optionalWeights.empty();
      for (auto* w : mandatoryWeights) {
        if (!w->isConstantScoring()) constant = false;
      }
      if (constant) traits |= IS_CONSTANT_SCORING;
    }

    std::unique_ptr<Query::Weight::PreparedWeight> prepare(Query::Weight::PrepareContext& ctx) override {
      auto filterSources = QueryPrep::prepareSources(filterWeights, ctx);
      std::vector<std::unique_ptr<DocSet>> filterDomains(ctx.reader.segments().size());
      std::vector<DocSet*> childDomainPtrs(ctx.reader.segments().size());

      if (!filterSources.empty()) {
        for (size_t segnum = 0; segnum < ctx.reader.segments().size(); segnum++) {
          auto* outerDomain = ctx.domainPerSeg.empty() ? nullptr : ctx.domainPerSeg[segnum];
          filterDomains[segnum] = QueryPrep::materializeIntersection(
            QueryPrep::preparedSpan(filterSources), ctx.reader.segments()[segnum], outerDomain);
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
        std::move(mandatorySources), std::move(optionalSources),
        std::move(prohibitedSources), std::move(filterDomains),
        !filterSources.empty(), minShouldMatch, needsScores);
    }


    Query::ScorerSupplier* scorerSupplier(solux::MemPool& targetPool, solux::IndexReader::Segment& segment) override {
      auto mandatorySources = QueryPrep::liveSources(targetPool, mandatoryWeights);
      auto optionalSources = QueryPrep::liveSources(targetPool, optionalWeights);
      auto prohibitedSources = QueryPrep::liveSources(targetPool, prohibitedWeights);
      auto filterSources = QueryPrep::liveSources(targetPool, filterWeights);
      auto filterSuppliers = QueryPrep::collectSuppliers(targetPool, segment, filterSources);
      return targetPool.make<Supplier>(targetPool, segment, mandatorySources, optionalSources,
                                       prohibitedSources, filterSuppliers, minShouldMatch, needsScores);
    }

    Scorer* createScorer(solux::MemPool& targetPool, solux::IndexReader::Segment& segment) override {
      return scorerSupplier(targetPool, segment)->get(targetPool, std::numeric_limits<int64_t>::max());
    }

    // Single-clause boolean shapes delegate to the wrapped clause; compound
    // shapes have no cheap exact count (clause overlap is unknown).
    int64_t count(solux::IndexReader::Segment& segment) override {
      if (!prohibitedWeights.empty() || !filterWeights.empty() || minShouldMatch > 1) {
        return -1;
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
        maxScore += refined ? opt.scorer->refineMaxScore(upTo)
                            : opt.scorer->getMaxScore(upTo);
      }
      return maxScore;
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

    // The optional side can add at most its global max: docs whose required
    // score cannot reach (threshold - optMax) cannot compete, so the required
    // side may prune with that reduced threshold (Lucene ReqOptSumScorer's
    // setMinCompetitiveScore shape).  An unbounded optional forwards nothing.
    void setMinCompetitiveScore(float minScore) override {
      float optMax = opt.scorer->getMaxScore(solux::PostingsReader::END);
      if (std::isfinite(optMax)) {
        // Round the reduced threshold DOWN so float rounding can only make the
        // required side less aggressive, never skip a doc whose sum could
        // still reach minScore.
        mand.scorer->setMinCompetitiveScore(
            std::nextafter(minScore - optMax, -std::numeric_limits<float>::infinity()));
      }
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

  class MandOptBulkScorer final : public BulkScorer {
    constexpr static int32_t kWindowSize = DocsEnum::L1_DOCS;
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

    int32_t maxDoc;
    int64_t mandCost;
    int32_t windowStart = 0;
    int32_t windowEnd = 0;
    int32_t liveOptCount = 0;
    float minCompetitiveScore = std::numeric_limits<float>::lowest();
    double scoreBoundFactor = 1.0;
    double optGlobalMax = 0.0;
    bool optGlobalMaxFinite = true;
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

    void pushMandMinCompetitiveScore(float minScore) {
      if (!optGlobalMaxFinite) {
        return;
      }
      float reduced = (float) ((double) minScore - optGlobalMax);
      mand->setMinCompetitiveScore(
          std::nextafter(reduced, -std::numeric_limits<float>::infinity()));
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
      return filter == nullptr || filter->get(doc);
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
          sum += (double) (refined ? opt->refineMaxScore(upTo)
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
        optWindowMax[i] = opt->getMaxScoreForSetup(windowEnd - 1);
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

      if (filter == nullptr) {
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
        float maxScore = opt->getMaxScoreForSetup(PostingsReader::END);
        if (!std::isfinite(maxScore) || !optGlobalMaxFinite) {
          optGlobalMaxFinite = false;
        } else {
          optGlobalMax += (double) maxScore;
        }
      }
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
        pushMandMinCompetitiveScore(minCompetitiveScore);
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
  public:
    MandNotScorer(solux::MemPool& targetPool, Scorer* mandScorer, Scorer* notScorer) : mandScorer(mandScorer),
                                                                                       notScorer(notScorer) {
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

  private:

    // mandScorer should be advanced and id set before calling this
    int32_t doNext() {
      while (id != solux::PostingsReader::END) {
        if (notid < id) {
          notid = notScorer->advance(id);
        }
        if (notid > id) {
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

    struct VerifierSlot {
      Query::Scorer* scorer = nullptr;
      size_t approxIndex = 0;
      float matchCost = 0.0f;
    };

    std::span<Query::Scorer*> scorers; // subset of required clauses that contributes to score()
    std::span<ApproxSlot> approximations; // every required clause, ascending cost (lead first)
    std::span<VerifierSlot> verifiers; // two-phase clauses sorted by matchCost

    int32_t docid = -1;
    float minCompetitiveScore = 0.0f;
    // Lead docs <= competitiveUpTo lie in block ranges whose summed clause
    // bounds passed the threshold check; they skip re-evaluation.  Reset when
    // the threshold rises.
    int32_t competitiveUpTo = -1;
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
          return target;
        }
        skippedRangeCount++;
        skipCount(SkipStats::conjRangeSkips);
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
          if (!verifier.scorer->matches()) {
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

    // allScorers: every required iterator, ordered by ascending cost so
    // allScorers[0] is the sparsest and leads the matching. scoringScorers: the
    // subset whose score() contributes to the conjunction score (filter clauses
    // iterate but do not score); every entry must also appear in allScorers.
    // TODO: if any scoring scorer is boosted to 0 it could be dropped from the
    // scoring subset while staying in allScorers.
    ConjunctionScorer(solux::MemPool& pool, std::span<Query::Scorer*> allScorers,
                      std::span<Query::Scorer*> scoringScorers)
            : scorers(scoringScorers),
              approximations(pool.make_arr<ApproxSlot>(allScorers.size()), allScorers.size()) {
      size_t verifierCount = 0;
      for (size_t i = 0; i < allScorers.size(); i++) {
        bool twoPhase = !disableTwoPhaseForTests && allScorers[i]->hasTwoPhase();
        approximations[i] = {allScorers[i], twoPhase};
        if (twoPhase) verifierCount++;
      }
      verifiers = {pool.make_arr<VerifierSlot>(verifierCount), verifierCount};
      size_t verifierIndex = 0;
      for (size_t i = 0; i < allScorers.size(); i++) {
        if (!approximations[i].twoPhase) {
          continue;
        }
        auto* scorer = approximations[i].scorer;
        verifiers[verifierIndex++] = {scorer, i, scorer->matchCost()};
      }
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

    void setMinCompetitiveScore(float minScore) override {
      // The threshold applies to the conjunction's SUM; children must not see
      // it (a child pruning on its own score alone would drop docs whose sum
      // is competitive).
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

  // Bulk execution for pure scored conjunctions (every clause required AND
  // scoring, no two-phase members): windows anchor on the lead's current doc,
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

  private:
    static constexpr int32_t kChunk = Postings::DOCS_BLOCK_SIZE;
    static constexpr int32_t kWindowSize = DocsEnum::L1_DOCS;
    static constexpr int32_t kWindowWords = kWindowSize / 64;
    static constexpr int32_t kDenseLeapfrogThreshold = kWindowSize / 32;
    static_assert((kWindowSize % 64) == 0);

    std::span<Query::Scorer*> scorers;  // ascending cost; scorers[0] leads
    std::span<TermQuery::Scorer*> termScorers; // populated when every scorer is a term
    std::span<float> windowMax;         // per-clause bound over the current window
    std::span<double> suffixMax;        // suffixMax[c] = sum of windowMax[c..n)
    std::span<int32_t> candDocs;
    std::span<float> candScores;
    std::span<int32_t> outDocs;
    std::span<float> outScores;
    std::span<uint64_t> windowBits;
    std::span<uint64_t> clauseBits;
    int32_t maxDoc;
    float minCompetitiveScore = std::numeric_limits<float>::lowest();
    double scoreBoundFactor = 1.0;
    int64_t skippedWindowCount = 0;
    bool allTermScorers = false;
    bool denseCountPath = false;

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
        out.max = upTo + 1;
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
          if (filter != nullptr) {
            int32_t w = 0;
            for (int32_t i = 0; i < n; i++) {
              if (filter->get(candDocs[(size_t) i])) {
                candDocs[(size_t) w] = candDocs[(size_t) i];
                candScores[(size_t) w] = candScores[(size_t) i];
                w++;
              }
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
            if (candScores[(size_t) i] >= this->minCompetitiveScore) {
              outDocs[(size_t) out.size] = candDocs[(size_t) i];
              outScores[(size_t) out.size] = candScores[(size_t) i];
              out.size++;
            }
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

    void applyDocSetFilterToWindow(DocSet* filter, int32_t windowBase,
                                   int32_t windowEnd) {
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
          if (!filter->get(doc)) {
            windowBits[(size_t) word] &= ~(1ULL << bit);
          }
          bits &= bits - 1;
        }
      }
    }

    int32_t ratchetDenseWindow(int32_t min, int32_t max) {
      for (size_t c = 0; c < termScorers.size(); c++) {
        int32_t doc = termScorers[c]->docsEnum.docId();
        if (doc < min) {
          doc = termScorers[c]->docsEnum.advanceDocOnly(min);
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
          for (size_t c = firstClause; c < termScorers.size(); c++) {
            int32_t scorerDoc = termScorers[c]->docsEnum.docId();
            if (scorerDoc < doc) {
              scorerDoc = termScorers[c]->docsEnum.advanceDocOnly(doc);
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

    int32_t countNextWindowDense(int64_t& count, DocSetBuilder* domainOut,
                                 DocSet* filter, int32_t min, int32_t max) {
      int32_t windowBase = ratchetDenseWindow(min, max);
      if (windowBase >= max) {
        return windowBase;
      }

      int32_t requestedEnd = windowBase + kWindowSize;
      if (requestedEnd < windowBase) {
        requestedEnd = max;
      }
      int32_t windowEnd = std::min(requestedEnd, max);

      skipCount(SkipStats::conjDenseCountWindows);
      clearWindowBits(windowBits);
      termScorers[0]->fillWindowBits(windowBits, windowBase, windowEnd);
      for (size_t c = 1; c < termScorers.size(); c++) {
        clearWindowBits(clauseBits);
        termScorers[c]->fillWindowBits(clauseBits, windowBase, windowEnd);
        for (int32_t w = 0; w < kWindowWords; w++) {
          windowBits[(size_t) w] &= clauseBits[(size_t) w];
        }
        if (termScorers.size() >= 3 && c + 1 < termScorers.size()) {
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

      if (filter != nullptr && filter->type == DocSet::BITSET) {
        applyDomainBitsToWindow(windowBits, windowBase, windowEnd,
                                &((BitDocSet*) filter)->bits());
      } else if (filter != nullptr) {
        applyDocSetFilterToWindow(filter, windowBase, windowEnd);
      }

      if (domainOut != nullptr) {
        domainOut->addWindowWords(windowBits.data(), windowBase, windowEnd);
      }
      count += popCountWindowBits();

      return windowEnd >= max ? PostingsReader::END : windowEnd;
    }

    template <bool TermFast>
    int32_t countNextWindowSparse(int64_t& count, DocSetBuilder* domainOut,
                                  DocSet* filter, int32_t min, int32_t max) {
      skipCount(SkipStats::conjCountFallbacks);
      int32_t target = min;
      while (target < max) {
        int32_t doc = scorerDocId<TermFast>(0);
        if (doc < target) {
          doc = scorerCountAdvance<TermFast>(0, target);
        }
        if (doc >= max) {
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

        if (filter == nullptr || filter->get(target)) {
          count++;
          if (domainOut != nullptr) {
            domainOut->add(target);
          }
        }
        target++;
      }
      return target;  // >= max: a failing clause's position or matched doc + 1
    }

  public:
    ConjunctionBulkScorer(solux::MemPool& pool, std::span<Query::Scorer*> scorers,
                          int32_t maxDoc, int64_t leadCost)
        : scorers(scorers),
          termScorers(pool.make_arr<TermQuery::Scorer*>(scorers.size()), scorers.size()),
          windowMax(pool.make_arr<float>(scorers.size()), scorers.size()),
          suffixMax(pool.make_arr<double>(scorers.size() + 1), scorers.size() + 1),
          candDocs(pool.make_arr<int32_t>((size_t) kChunk), (size_t) kChunk),
          candScores(pool.make_arr<float>((size_t) kChunk), (size_t) kChunk),
          outDocs(pool.make_arr<int32_t>((size_t) kWindowSize), (size_t) kWindowSize),
          outScores(pool.make_arr<float>((size_t) kWindowSize), (size_t) kWindowSize),
          windowBits(pool.make_arr<uint64_t>((size_t) kWindowWords), (size_t) kWindowWords),
          clauseBits(pool.make_arr<uint64_t>((size_t) kWindowWords), (size_t) kWindowWords),
          maxDoc(maxDoc) {
      assert(scorers.size() >= 2);
      // Float-summation error headroom for the double bounds (see the MaxScore
      // scorers' scoreBoundFactor).
      scoreBoundFactor = 1.0 + (double) scorers.size() * 0x1p-24;
      allTermScorers = true;
      for (size_t i = 0; i < scorers.size(); i++) {
        termScorers[i] = dynamic_cast<TermQuery::Scorer*>(scorers[i]);
        allTermScorers &= termScorers[i] != nullptr;
      }
      denseCountPath = allTermScorers && maxDoc >= kWindowSize
          && leadCost >= std::max<int64_t>(1, (int64_t) maxDoc / kDenseThresholdInverse);
    }

    int32_t scoreNextWindow(ScoreWindow& out, DocSet* filter, int32_t min, int32_t max,
                            float minCompetitiveScore) override {
      return allTermScorers
          ? scoreNextWindowImpl<true>(out, filter, min, max, minCompetitiveScore)
          : scoreNextWindowImpl<false>(out, filter, min, max, minCompetitiveScore);
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
  }; // ConjunctionBulkScorer


  class DisjunctionScorer final : public Query::Scorer {
    std::span<Scorer*> scorers;

    // TODO: OPT: heapifying with virtual methods prob isn't a good idea... pull out and save the docid.
    constexpr static auto idComparator = [](Query::Scorer& a, Query::Scorer& b) { return b.docId() < a.docId(); };

    solux::IndirectPQ<Scorer, decltype(idComparator)> pq;

    int32_t docid = -1;
  public:
    // The passed in span of scorers will be modified (rearranged).
    DisjunctionScorer(solux::MemPool& pool, std::span<Scorer*> scorers)
            : scorers(scorers), pq(scorers) {
    }

    int32_t next() override {
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

    // Advance every member below target through its own advance (block
    // skips) instead of the base linear next() walk. Members are touched at
    // most once: each surfaces at the top while behind, advances to >=
    // target, and sifts down. Without this, per-candidate probes of a
    // disjunction (the MandNot exclusion side, the MandOpt rank-only side)
    // walk every doc of every member between candidates.
    int32_t advance(int32_t target) override {
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

    float score() override {
      assert(docid == pq.top().docId());
      float score = pq.top().score();
      int increments = 0;
      // If we ever had a huge disjunction, we don't really need to look at all of them for matches.  But equal
      // ids could be on the left or the right of the heap, so just loop over all scorers for now.
      for (int i = 1; i < pq.size(); i++) {
        auto id = scorers[i]->docId();
        assert(id >= docid);
        if (id == docid) {
          score += scorers[i]->score();
        }
      }
      return score;
    }

    int32_t advanceShallow(int32_t target) override {
      int32_t upTo = solux::PostingsReader::END;
      for (int32_t i = 0; i < (int32_t) pq.size(); i++) {
        upTo = std::min(upTo, scorers[(size_t) i]->advanceShallow(target));
      }
      return upTo;
    }

    float getMaxScore(int32_t upTo) override {
      float sum = 0.0f;
      for (int32_t i = 0; i < (int32_t) pq.size(); i++) {
        auto* scorer = scorers[(size_t) i];
        int32_t doc = scorer->docId();
        if (doc != solux::PostingsReader::END && doc <= upTo) {
          sum += scorer->getMaxScore(upTo);
        }
      }
      return sum;
    }

    float refineMaxScore(int32_t upTo) override {
      float sum = 0.0f;
      for (int32_t i = 0; i < (int32_t) pq.size(); i++) {
        auto* scorer = scorers[(size_t) i];
        int32_t doc = scorer->docId();
        if (doc != solux::PostingsReader::END && doc <= upTo) {
          sum += scorer->refineMaxScore(upTo);
        }
      }
      return sum;
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
      return requestedWindowSize > 0 ? requestedWindowSize : DocsEnum::L1_DOCS;
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
          windowMax[i] = scorers[i]->getMaxScoreForSetup(windowEnd - 1);
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
                              int32_t maxDoc, int32_t windowSize = DocsEnum::L1_DOCS)
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
        clauseMax[i] = scorers[i]->getMaxScoreForSetup(PostingsReader::END);
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
        float maxScore = scorer->getMaxScore(upTo);
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
        float maxScore = scorer->refineMaxScore(upTo);
        if (!std::isfinite(maxScore)) {
          return std::numeric_limits<float>::infinity();
        }
        sum += maxScore;
      }
      return sum;
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
    constexpr static int32_t kWindowSize = DocsEnum::L1_DOCS;
    constexpr static int32_t kWindowWords = kWindowSize / 64;
    constexpr static size_t kBs1MinClauses = 16;
    // Domain-drive gate weights: drive only when card*nClauses*W < SUM(clause.cost()).
    // W is the per-advance penalty (advance cost vs a vectorized block decode). HARDWARE
    // SENSITIVE (vector throughput vs scalar skip cost; ISA): calibrated on an Intel hybrid
    // 2P+8E laptop to a ~1% crossover; RE-VALIDATE on uniform desktop / cloud / ARM (NEON/
    // SVE shifts the ratio). W_ARRAY < W_BITSET because array stream membership is an
    // O(log card) ArrDocSet::get binary search vs bitset O(1) get (W_ARRAY reasoned from the
    // ~15% delta, not swept). See solux-private/tuning-constants.md + domain-pushdown.md.
    constexpr static int64_t W_BITSET = 32;
    constexpr static int64_t W_ARRAY = 28;
    static_assert((kWindowSize % 64) == 0);

    std::span<Query::Scorer*> scorers;  // stable global-max order
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

    static bool lessMaxScore(float a, float b) {
      bool finiteA = std::isfinite(a);
      bool finiteB = std::isfinite(b);
      if (finiteA != finiteB) return finiteA;
      return a < b;
    }

    void sortByClauseMax() {
      for (size_t i = 1; i < scorers.size(); i++) {
        Query::Scorer* scorer = scorers[i];
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
      return scorers.size() >= kBs1MinClauses && splitIndex * 2 <= scorers.size();
    }

    void updateMaxWindowScores(int32_t start, int32_t end) {
      for (size_t i = 0; i < scorers.size(); i++) {
        int32_t doc = scorers[i]->docId();
        if (doc >= end) {
          windowMax[i] = 0.0f;
        } else {
          scorers[i]->advanceShallowForSetup(start);
          windowMax[i] = scorers[i]->getMaxScoreForSetup(end - 1);
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
        auto* scorer = scorers[i];
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
          perDoc(docs[arrayCursor]);
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
        perDoc(doc);
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

      if (filter == nullptr) {
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
                       int32_t maxDoc, int64_t aggregateClauseCost)
            : scorers(scorers),
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
              maxDoc(maxDoc),
              aggregateClauseCost(aggregateClauseCost) {
      scoreBoundFactor = 1.0 + (double) scorers.size() * 0x1p-24;
      for (size_t i = 0; i < scorers.size(); i++) {
        clauseMax[i] = scorers[i]->getMaxScoreForSetup(PostingsReader::END);
      }
      sortByClauseMax();
      for (size_t i = 0; i < scorers.size(); i++) {
        windowOrder[i] = (int32_t) i;
      }
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
        fillDomainDrivenCandidates(out, filter);
        return windowEnd >= max ? PostingsReader::END : windowEnd;
      }

      const FixedBitSet* domainBits = nullptr;
      if (filter != nullptr && filter->type == DocSet::BITSET) {
        domainBits = &((BitDocSet*) filter)->bits();
      }

      setupWindow(min, max);
      if (splitIndex < scorers.size() && useBs1ForWindow()) {
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
            fillSingleEssentialCandidates(out, filter, domainBits, top1Index);
          } else if (top2 >= windowEnd) {
            skipCount(SkipStats::maxScoreTop2Conversions);
            fillSingleEssentialCandidates(out, filter, domainBits, top1Index);
          } else if (top2 - kWindowSize / 2 >= top1) {
            windowEnd = std::min(windowEnd, top2);
            skipCount(SkipStats::maxScoreHalfWindowClips);
            fillSingleEssentialCandidates(out, filter, domainBits, top1Index);
          } else {
            fillEssentialCandidates(filter, domainBits);
            finalizeCandidates(out);
          }
        } else if (scorers.size() - splitIndex == 1) {
          fillSingleEssentialCandidates(out, filter, domainBits, windowOrder[splitIndex]);
        } else {
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
      if (domainOut != nullptr) {
        skipCount(SkipStats::bulkDomainWindowsFed);
      }
      if (shouldDriveFromDomain(filter)) {
        setWindowBounds(min, max);
        domainDriveWindows++;
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

      const FixedBitSet* domainBits = nullptr;
      if (filter != nullptr && filter->type == DocSet::BITSET) {
        domainBits = &((BitDocSet*) filter)->bits();
      } else if (filter != nullptr) {
        setWindowBounds(min, max);
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
        for (size_t w = 0; w < windowBits.size(); w++) {
          count += std::popcount(windowBits[w]);
        }
        if (domainOut != nullptr) {
          domainOut->addWindowWords(windowBits.data(), windowStart, windowEnd);
        }
        return windowEnd >= max ? PostingsReader::END : windowEnd;
      }

      setWindowBounds(min, max);
      clearWindowBits();
      for (size_t i = 0; i < scorers.size(); i++) {
        scorers[i]->fillWindowBits(windowBits, windowStart, windowEnd);
      }
      if (domainBits != nullptr) {
        applyDomainBits(domainBits);
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
      if (maxScoreBelowThreshold(tailMaxScore + (double) clauseMax[(size_t) idx])
          || tailSize + 1 < minMatch) {
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
      while (leadMaxScore < (double) minCompetitiveScore || leadSize < minMatch) {
        if (maxScoreBelowThreshold(leadMaxScore + tailMaxScore)
            || leadSize + tailSize < minMatch) {
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
      assert(minMatch >= 2);
      assert((int32_t)scorers.size() > minMatch);
      // Worst-case relative error of summing scorers.size() non-negative floats in
      // float arithmetic is bounded by (m-1)*u, u = 2^-24; round the double max-sum
      // up by that so it bounds score()'s float accumulation from above.
      scoreBoundFactor = 1.0 + (double) scorers.size() * 0x1p-24;
      for (size_t i = 0; i < scorers.size(); i++) {
        clauseMax[i] = scorers[i]->getMaxScore(PostingsReader::END);
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

    float getMaxScore(int32_t upTo) override {
      double sum = 0.0;
      for (auto* scorer : scorers) {
        float maxScore = scorer->getMaxScore(upTo);
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
        float maxScore = scorer->refineMaxScore(upTo);
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
  }; // MinShouldMatchScorer


};

} // namespace solux
