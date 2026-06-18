#pragma once

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

  Weight* createWeight(Context& context) override {
    return context.pool.make<BooleanQuery::Weight>(context, *this);
  }

  class Weight final : public Query::Weight {
    std::span<Query::Weight*> mandatoryWeights;
    std::span<Query::Weight*> optionalWeights;
    std::span<Query::Weight*> prohibitedWeights;
    std::span<Query::Weight*> filterWeights;
    int minShouldMatch = 0;

    // Returns a span of Weights, corresponding to the given span of Queries. Some weights can be null.
    std::span<Query::Weight*> createWeights(solux::MemPool& targetPool, Context& context, std::span<Query*> queries) {
      if (queries.size() == 0) {
        return {};
      }
      auto weights = targetPool.make_arr<Query::Weight*>(queries.size());
      for (int i = 0; i < queries.size(); ++i) {
        weights[i] = queries[i]->createWeight(context);
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
        int minShouldMatch) {
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
        // minShouldMatch applies to optional scorers that exist in this segment.
        if (minShouldMatch <= 1) {
          optScorer = optCount == 1
            ? optionalScorers[0]
            : targetPool.make<BooleanQuery::DisjunctionScorer>(targetPool, optionalScorers);
        } else if (optCount == minShouldMatch) {
          // Every surviving optional clause is required and scores.
          optScorer = targetPool.make<BooleanQuery::ConjunctionScorer>(
            targetPool, optionalScorers, optionalScorers);
        } else if (optCount > minShouldMatch) {
          optScorer = targetPool.make<BooleanQuery::MinShouldMatchScorer>(
            targetPool, optionalScorers, minShouldMatch);
        }
      }

      Query::Scorer* boolScorer = nullptr;
      if (reqScorer == nullptr) {
        // No required clauses: the optional side stands alone.
        if (optScorer == nullptr) return nullptr;
        boolScorer = optScorer;
      } else if (optScorer == nullptr) {
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
    public:
      Supplier(MemPool& pool, IndexReader::Segment& segment,
               std::span<Query::SegmentSource* const> mandatorySources,
               std::span<Query::SegmentSource* const> optionalSources,
               std::span<Query::SegmentSource* const> prohibitedSources,
               std::span<Query::ScorerSupplier* const> filterSuppliers,
               int minShouldMatch)
        : pool(pool), segment(segment), mandatorySources(mandatorySources),
          optionalSources(optionalSources), prohibitedSources(prohibitedSources),
          filterSuppliers(filterSuppliers), minShouldMatch(minShouldMatch) {}

      int64_t cost() override {
        return compositeCost(pool, segment, mandatorySources, optionalSources, filterSuppliers, minShouldMatch);
      }

      Query::Scorer* get(MemPool& targetPool, int64_t leadCost) override {
        unused(leadCost);
        return assembleScorer(targetPool, segment, mandatorySources, optionalSources,
                              prohibitedSources, filterSuppliers, minShouldMatch);
      }
    };

    class BooleanPreparedWeight final : public Query::Weight::PreparedWeight {
      std::vector<QueryPrep::PreparedSource> mandatorySources;
      std::vector<QueryPrep::PreparedSource> optionalSources;
      std::vector<QueryPrep::PreparedSource> prohibitedSources;
      std::vector<std::unique_ptr<DocSet>> filterDomains;
      bool hasFilters = false;
      int minShouldMatch = 0;

    public:
      BooleanPreparedWeight(std::vector<QueryPrep::PreparedSource>&& mandatorySources,
                            std::vector<QueryPrep::PreparedSource>&& optionalSources,
                            std::vector<QueryPrep::PreparedSource>&& prohibitedSources,
                            std::vector<std::unique_ptr<DocSet>>&& filterDomains,
                            bool hasFilters, int minShouldMatch)
        : mandatorySources(std::move(mandatorySources)),
          optionalSources(std::move(optionalSources)),
          prohibitedSources(std::move(prohibitedSources)),
          filterDomains(std::move(filterDomains)),
          hasFilters(hasFilters), minShouldMatch(minShouldMatch) {}

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
          filterSuppliers, minShouldMatch);
      }

      Query::Scorer* createScorer(MemPool& targetPool, IndexReader::Segment& segment) override {
        return scorerSupplier(targetPool, segment)->get(targetPool, std::numeric_limits<int64_t>::max());
      }
    };


  public:
    Weight(Context& context, BooleanQuery& query) : Query::Weight(context) {
      mandatoryWeights = createWeights(context.pool, context, query.mandatory);
      optionalWeights = createWeights(context.pool, context, query.optional);
      prohibitedWeights = createWeights(context.pool, context, query.prohibited);
      filterWeights = createWeights(context.pool, context, query.filter);
      minShouldMatch = query.minShouldMatch;
    }

    bool needsPrepare() const noexcept override {
      return QueryPrep::anyNeedsPrepare(mandatoryWeights) ||
             QueryPrep::anyNeedsPrepare(optionalWeights) ||
             QueryPrep::anyNeedsPrepare(prohibitedWeights) ||
             QueryPrep::anyNeedsPrepare(filterWeights);
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
        !filterSources.empty(), minShouldMatch);
    }


    Query::ScorerSupplier* scorerSupplier(solux::MemPool& targetPool, solux::IndexReader::Segment& segment) override {
      auto mandatorySources = QueryPrep::liveSources(targetPool, mandatoryWeights);
      auto optionalSources = QueryPrep::liveSources(targetPool, optionalWeights);
      auto prohibitedSources = QueryPrep::liveSources(targetPool, prohibitedWeights);
      auto filterSources = QueryPrep::liveSources(targetPool, filterWeights);
      auto filterSuppliers = QueryPrep::collectSuppliers(targetPool, segment, filterSources);
      return targetPool.make<Supplier>(targetPool, segment, mandatorySources, optionalSources,
                                       prohibitedSources, filterSuppliers, minShouldMatch);
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
      // A parent's advance() (which loops on next()) can call us again after we
      // returned END - e.g. MandOptScorer scoring a required doc past the last
      // optional match. Stay idempotent at END.
      if (pq.size() == 0) {
        return docid = solux::PostingsReader::END;
      }
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
      return findNext(docid + 1);
    }

    int32_t advance(int32_t target) override {
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
