#pragma once

#include <bit>
#include <cmath>

#include "Query.h"
#include "QueryPrep.h"

namespace solux {

class BooleanQuery final : public solux::Query {
  std::span<Query*> mandatory;
  std::span<Query*> optional;
  std::span<Query*> prohibited;
  std::span<Query*> filter;
  // Minimum number of `optional` clauses a doc must match. 0 (or 1) is the
  // plain disjunction (any optional). > 1 selects the min-should-match scorer.
  // Only supported for optional-only queries (no mandatory/filter) for now.
  int minShouldMatch;

public:
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
      // Whether a scoring-required (mandatory) clause exists decides how optionals
      // combine: with a mandatory clause they are a pure score add (ReqOpt); with
      // only filters they must match (conjunction). This mirrors the prior
      // mandScorer-vs-filter discriminator.
      bool hasMandatory = !mandatorySources.empty();

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
        // optional terms are absent, or fewer survive than minShouldMatch). With
        // no mandatory clause the optionals are required (conjoined with the
        // filters), so that is no match here - returning reqScorer would wrongly
        // emit filter-only docs. With no optional clauses at all, the filters
        // stand alone, which is correct.
        if (!hasMandatory && !optionalSources.empty()) return nullptr;
        boolScorer = reqScorer;
      } else if (hasMandatory) {
        boolScorer = targetPool.make<BooleanQuery::MandOptScorer>(targetPool, reqScorer, optScorer);
      } else {
        // Filters + optionals, no mandatory: the optional side must match, so
        // conjoin it with the required filters; only the optional side scores.
        std::span<Query::Scorer*> allSpan(targetPool.make_arr<Query::Scorer*>(2), 2);
        allSpan[0] = reqScorer;
        allSpan[1] = optScorer;
        std::span<Query::Scorer*> scoringSpan(targetPool.make_arr<Query::Scorer*>(1), 1);
        scoringSpan[0] = optScorer;
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
        // A mandatory clause makes the optional side score-only (MandOpt), so it
        // doesn't constrain. With only filters, the optional side is conjoined
        // (it must match) and can tighten the estimate.
        if (!hasMandatory && !optionalSources.empty()) {
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

      BulkScorer* bulkScorer(MemPool& targetPool) override {
        if (!needsScores || !mandatorySources.empty() || !prohibitedSources.empty()
            || !filterSuppliers.empty() || minShouldMatch > 1 || optionalSources.size() < 2) {
          return nullptr;
        }
        auto optionalScorers = QueryPrep::createScorers(targetPool, segment, optionalSources);
        if (optionalScorers.size() < 2) {
          return nullptr;
        }
        return targetPool.make<BooleanQuery::MaxScoreBulkScorer>(
          targetPool, optionalScorers, segment.maxDoc());
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
      mandatoryWeights = createWeights(context.pool, context, query.mandatory, flags);
      optionalWeights = createWeights(context.pool, context, query.optional, flags);
      prohibitedWeights = createWeights(context.pool, context, query.prohibited, noScore);
      filterWeights = createWeights(context.pool, context, query.filter, noScore);
      minShouldMatch = query.minShouldMatch;

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
    Scorer* mandScorer;
    Scorer* optScorer;
    int32_t id = -1;
  public:
    MandOptScorer(solux::MemPool& targetPool, Scorer* mandScorer, Scorer* optScorer) : mandScorer(mandScorer),
                                                                                       optScorer(optScorer) {
      unused(targetPool);
    }

    int32_t docId() override {
      return id;
    }

    int32_t next() override {
      assert(id != solux::PostingsReader::END);
      id = mandScorer->next();
      return id;
    }

    int32_t advance(int32_t docid) override {
      id = mandScorer->advance(docid);
      return id;
    }

    float score() override {
      float score = mandScorer->score();
      // Consult the optional scorer for this exact doc (Lucene's ReqOptSumScorer
      // style): advance it only if behind, then add its score on an exact hit.
      if (optScorer->docId() < id) {
        optScorer->advance(id);
      }
      if (optScorer->docId() == id) {
        score += optScorer->score();
      }
      return score;
    }
  }; // MandOptScorer

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
    std::span<Scorer*> scorers;    // subset of allScorers that contributes to score()
    std::span<Scorer*> allScorers; // every required iterator, ascending cost (lead first)

    // TODO: OPT: heapifying with virtual methods prob isn't a good idea... pull out and save the docid.
    constexpr static auto idComparator = [](Query::Scorer& a, Query::Scorer& b) { return b.docId() < a.docId(); };

    // with a ton of clauses, a maxHeap could help with quickly finding the largest number to skip to.

    int32_t docid = -1;


    // internal utility method where first scorer has already been advanced and is equal to the target.
    int32_t doNext(int32_t target) {
      auto* firstScorer = allScorers[0];

      outer:
      for (;;) {
        for (int j = 1; j < allScorers.size(); j++) {
          // advance() is strict; skip sub-scorers already on target.
          if (allScorers[j]->docId() < target) {
            int32_t id = allScorers[j]->advance(target);
            assert(id >= target);
            if (id > target) {
              target = firstScorer->advance(id);
              goto outer;  // could perhaps replace with "j=0; continue;" but that seems potentially worse?
            }
          }
        }
        // if we made it through the loop, all scorers matched (maybe at END)
        docid = target;
        return docid;
      }
      // unreachable
    }

  public:
    // allScorers: every required iterator, ordered by ascending cost so
    // allScorers[0] is the sparsest and leads the matching. scoringScorers: the
    // subset whose score() contributes to the conjunction score (filter clauses
    // iterate but do not score); every entry must also appear in allScorers.
    // TODO: if any scoring scorer is boosted to 0 it could be dropped from the
    // scoring subset while staying in allScorers.
    ConjunctionScorer(solux::MemPool& pool, std::span<Scorer*> allScorers, std::span<Scorer*> scoringScorers)
            : scorers(scoringScorers), allScorers(allScorers) {
      unused(pool);
    }

    int32_t next() override {
      assert(docid != solux::PostingsReader::END);
      return doNext(allScorers[0]->next());
    }

    int32_t advance(int32_t docid) override {
      return doNext(allScorers[0]->advance(docid));
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
  }; // ConjuctionScorer


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
  }; // DisjunctionScorer


  class MaxScoreDisjunctionScorer final : public Query::Scorer {
    MemPool& pool;
    std::span<Scorer*> scorers;  // stable global-max order, used for deterministic scoring
    std::span<float> clauseMax;
    std::span<float> windowMax;
    std::span<int32_t> windowOrder;
    std::span<bool> isEssential;
    std::span<Scorer*> essentialPointers;

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
      pq = pool.make<solux::IndirectPQ<Scorer, decltype(idComparator)>>(essentialPointers, essentialCount);
    }

    void setupWindow(int32_t start) {
      windowStart = start;
      int32_t remaining = maxDoc - windowStart;
      windowEnd = remaining > windowSize ? windowStart + windowSize : maxDoc;

      for (size_t i = 0; i < scorers.size(); i++) {
        if (scorers[i]->docId() < windowStart) {
          scorers[i]->advance(windowStart);
        }
        scorers[i]->advanceShallow(windowStart);
        windowMax[i] = scorers[i]->getMaxScore(windowEnd - 1);
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
      // Sum every matching clause in a single fixed pass over the (clauseMax-sorted) array,
      // independent of splitIndex.  This keeps a doc's score a deterministic function of the
      // doc (not of when the threshold happened to advance), so the pruned and exhaustive
      // runs of this scorer produce bit-identical scores regardless of clause count.
      float sum = 0.0f;
      for (size_t i = 0; i < scorers.size(); i++) {
        if (!isEssential[i]) {
          // Non-essential: not driven by the essential union, so seek it to docid.
          nonEssentialLookups++;
          if (scorers[i]->docId() < docid) {
            scorers[i]->advance(docid);
          }
        }
        if (scorers[i]->docId() == docid) {
          sum += scorers[i]->score();
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
              maxDoc(maxDoc),
              windowSize(normalizeWindowSize(windowSize)),
              globalMode(normalizeWindowSize(windowSize) >= maxDoc) {
      // Float-summation error headroom for the double split bound (see member).
      scoreBoundFactor = 1.0 + (double) scorers.size() * 0x1p-24;
      for (size_t i = 0; i < scorers.size(); i++) {
        clauseMax[i] = scorers[i]->getMaxScore(PostingsReader::END);
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

    int64_t visited() const {
      return visitedCandidates;
    }

    int64_t nonEssentialLookupCount() const {
      return nonEssentialLookups;
    }

    int32_t currentSplitIndex() const {
      return (int32_t) splitIndex;
    }
  }; // MaxScoreDisjunctionScorer


  class MaxScoreBulkScorer final : public BulkScorer {
    constexpr static int32_t kWindowSize = DocsEnum::L1_DOCS;
    constexpr static int32_t kWindowWords = kWindowSize / 64;
    static_assert((kWindowSize % 64) == 0);

    std::span<Query::Scorer*> scorers;  // stable global-max order, used for deterministic scoring
    std::span<float> clauseMax;
    std::span<float> windowMax;
    std::span<int32_t> windowOrder;
    std::span<bool> isEssential;
    std::span<uint64_t> windowBits;
    std::span<float> essentialScores;
    std::span<int32_t> outDocs;
    std::span<float> outScores;

    int32_t maxDoc;
    int32_t windowStart = 0;
    int32_t windowEnd = 0;
    size_t splitIndex = 0;
    float minCompetitiveScore = std::numeric_limits<float>::lowest();
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
        if (!(nextSum * scoreBoundFactor < (double) minCompetitiveScore)) break;
        sum = nextSum;
      }
      return s;
    }

    void markEssentialScorers() {
      clearEssentialFlags();
      for (size_t i = splitIndex; i < scorers.size(); i++) {
        isEssential[(size_t) windowOrder[i]] = true;
      }
    }

    void setupWindow(int32_t start, int32_t max) {
      windowStart = start;
      int32_t requestedEnd = windowStart + kWindowSize;
      if (requestedEnd < windowStart) {
        requestedEnd = max;
      }
      windowEnd = std::min(std::min(requestedEnd, max), maxDoc);

      for (size_t i = 0; i < scorers.size(); i++) {
        if (scorers[i]->docId() < windowStart) {
          scorers[i]->advance(windowStart);
        }
        scorers[i]->advanceShallow(windowStart);
        windowMax[i] = scorers[i]->getMaxScore(windowEnd - 1);
        windowOrder[i] = (int32_t) i;
      }
      sortWindowOrder();
      splitIndex = computeWindowSplit();
      markEssentialScorers();
    }

    float* essentialScoreRow(size_t scorerIndex) {
      return essentialScores.data() + scorerIndex * (size_t) kWindowSize;
    }

    void clearWindowBits() {
      std::fill(windowBits.begin(), windowBits.end(), 0);
    }

    void setWindowBit(int32_t index) {
      windowBits[(size_t) (index >> 6)] |= 1ULL << (index & 63);
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

    bool verifyMatch(Query::Scorer* scorer, int32_t doc) const {
      unused(scorer, doc);
      // Reserved for two-phase: approximation hits will call matches() here.
      return true;
    }

    void fillEssentialCandidates(DocSet* filter, const FixedBitSet* domainBits) {
      clearWindowBits();
      for (size_t i = 0; i < scorers.size(); i++) {
        if (!isEssential[i]) {
          continue;
        }
        float* row = essentialScoreRow(i);
        std::fill(row, row + kWindowSize, 0.0f);
        auto* scorer = scorers[i];
        int32_t doc = scorer->docId();
        if (doc < windowStart) {
          doc = scorer->advance(windowStart);
        }
        while (doc < windowEnd) {
          if (acceptsDoc(filter, domainBits, doc)) {
            int32_t index = doc - windowStart;
            setWindowBit(index);
            row[(size_t) index] = scorer->score();
          }
          doc = scorer->next();
        }
      }
    }

    float scoreCandidate(int32_t doc, int32_t index) {
      float sum = 0.0f;
      for (size_t i = 0; i < scorers.size(); i++) {
        if (isEssential[i]) {
          sum += essentialScoreRow(i)[(size_t) index];
          continue;
        }
        auto* scorer = scorers[i];
        if (scorer->docId() < doc) {
          scorer->advance(doc);
        }
        if (scorer->docId() == doc && verifyMatch(scorer, doc)) {
          sum += scorer->score();
        }
      }
      return sum;
    }

    void finalizeCandidates(ScoreWindow& out) {
      out.min = windowStart;
      out.max = windowEnd;
      out.size = 0;
      out.docs = outDocs;
      out.scores = outScores;

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
          float score = scoreCandidate(doc, index);
          if (score >= minCompetitiveScore) {
            out.docs[(size_t) out.size] = doc;
            out.scores[(size_t) out.size] = score;
            out.size++;
          }
          bits &= bits - 1;
        }
      }
    }

  public:
    // The passed in span of scorers will be modified (rearranged).
    MaxScoreBulkScorer(solux::MemPool& pool, std::span<Query::Scorer*> scorers,
                       int32_t maxDoc)
            : scorers(scorers),
              clauseMax(pool.make_arr<float>(scorers.size()), scorers.size()),
              windowMax(pool.make_arr<float>(scorers.size()), scorers.size()),
              windowOrder(pool.make_arr<int32_t>(scorers.size()), scorers.size()),
              isEssential(pool.make_arr<bool>(scorers.size()), scorers.size()),
              windowBits(pool.make_arr<uint64_t>((size_t) kWindowWords), (size_t) kWindowWords),
              essentialScores(pool.make_arr<float>(scorers.size() * (size_t) kWindowSize),
                              scorers.size() * (size_t) kWindowSize),
              outDocs(pool.make_arr<int32_t>((size_t) kWindowSize), (size_t) kWindowSize),
              outScores(pool.make_arr<float>((size_t) kWindowSize), (size_t) kWindowSize),
              maxDoc(maxDoc) {
      scoreBoundFactor = 1.0 + (double) scorers.size() * 0x1p-24;
      for (size_t i = 0; i < scorers.size(); i++) {
        clauseMax[i] = scorers[i]->getMaxScore(PostingsReader::END);
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
      }

      const FixedBitSet* domainBits = nullptr;
      if (filter != nullptr && filter->type == DocSet::BITSET) {
        domainBits = &((BitDocSet*) filter)->bits();
      }

      setupWindow(min, max);
      if (splitIndex < scorers.size()) {
        fillEssentialCandidates(filter, domainBits);
        finalizeCandidates(out);
      } else {
        clearWindowBits();
        out.min = windowStart;
        out.max = windowEnd;
      }

      return windowEnd >= max ? PostingsReader::END : windowEnd;
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
