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

    // Keep clause wiring in one place so prepared and non-prepared execution
    // cannot diverge on filter/prohibited semantics.
    static Query::Scorer* assembleScorer(
        MemPool& targetPool,
        IndexReader::Segment& segment,
        std::span<Query::SegmentSource* const> mandatorySources,
        std::span<Query::SegmentSource* const> optionalSources,
        std::span<Query::SegmentSource* const> prohibitedSources,
        std::span<Query::Scorer*> filterScorers,
        int minShouldMatch) {
      auto mandatoryScorers = QueryPrep::createScorers(targetPool, segment, mandatorySources);
      if (mandatoryScorers.size() < mandatorySources.size()) return nullptr;

      Query::Scorer* mandScorer = nullptr;
      if (!mandatoryScorers.empty()) {
        mandScorer = mandatoryScorers.size() == 1 && filterScorers.empty()
          ? mandatoryScorers[0]
          : targetPool.make<BooleanQuery::ConjunctionScorer>(targetPool, mandatoryScorers, filterScorers);
      }

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
          // Every surviving optional clause is required.
          optScorer = targetPool.make<BooleanQuery::ConjunctionScorer>(
            targetPool, optionalScorers, std::span<Query::Scorer*>());
        } else if (optCount > minShouldMatch) {
          optScorer = targetPool.make<BooleanQuery::MinShouldMatchScorer>(
            targetPool, optionalScorers, minShouldMatch);
        }
      }

      Query::Scorer* boolScorer = nullptr;
      if (mandScorer == nullptr) {
        if (optScorer == nullptr) {
          if (filterScorers.empty()) return nullptr;
          boolScorer = targetPool.make<BooleanQuery::ConjunctionScorer>(
            targetPool, std::span<Query::Scorer*>(), filterScorers);
        } else {
          boolScorer = optScorer;
          if (!filterScorers.empty()) {
            std::span<Query::Scorer*> optSpan(targetPool.make_arr<Query::Scorer*>(1), 1);
            optSpan[0] = optScorer;
            boolScorer = targetPool.make<BooleanQuery::ConjunctionScorer>(targetPool, optSpan, filterScorers);
          }
        }
      } else if (optScorer == nullptr) {
        boolScorer = mandScorer;
      } else {
        boolScorer = targetPool.make<BooleanQuery::MandOptScorer>(targetPool, mandScorer, optScorer);
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

      Query::Scorer* createScorer(MemPool& targetPool, IndexReader::Segment& segment) override {
        std::span<Query::Scorer*> filterScorers;
        if (hasFilters) {
          auto* filterDomain = filterDomains[(size_t)segment.ord].get();
          auto* filterScorer = QueryPrep::createDocSetScorer(targetPool, filterDomain, segment);
          if (filterScorer == nullptr) return nullptr;
          filterScorers = {targetPool.make_arr<Query::Scorer*>(1), 1};
          filterScorers[0] = filterScorer;
        }

        return assembleScorer(
          targetPool,
          segment,
          QueryPrep::segmentSources(targetPool, QueryPrep::preparedSpan(mandatorySources)),
          QueryPrep::segmentSources(targetPool, QueryPrep::preparedSpan(optionalSources)),
          QueryPrep::segmentSources(targetPool, QueryPrep::preparedSpan(prohibitedSources)),
          filterScorers, minShouldMatch);
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


    Scorer* createScorer(solux::MemPool& targetPool, solux::IndexReader::Segment& segment) override {
      auto mandatorySources = QueryPrep::liveSources(targetPool, mandatoryWeights);
      auto optionalSources = QueryPrep::liveSources(targetPool, optionalWeights);
      auto prohibitedSources = QueryPrep::liveSources(targetPool, prohibitedWeights);
      auto filterSources = QueryPrep::liveSources(targetPool, filterWeights);
      auto filterScorers = QueryPrep::createScorers(targetPool, segment, filterSources);

      // If any filter scorer is missing for this segment, no document can match.
      if (filterScorers.size() < filterSources.size()) return nullptr;

      return assembleScorer(
        targetPool, segment, mandatorySources, optionalSources, prohibitedSources, filterScorers,
        minShouldMatch);
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
    std::span<Scorer*> scorers;    // just the mandatory scorers
    std::span<Scorer*> allScorers; // mandatory scorers combined with filter scorers

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
    // The passed in span of scorers will be modified (rearranged).
    ConjunctionScorer(solux::MemPool& pool, std::span<Scorer*> scorers, std::span<Scorer*> filterScorers)
            : scorers(scorers) {
      // combine the filterScorers with the mandatory scorers
      if (filterScorers.size() != 0) {
        allScorers = {pool.make_arr<Scorer*>(scorers.size() + filterScorers.size()),
                      scorers.size() + filterScorers.size()};
        std::ranges::copy(filterScorers, allScorers.begin());
        std::ranges::copy(scorers, allScorers.begin() + filterScorers.size());
        // TODO: if any mandatory scorers are boosted to 0, we could remove them from scorers (keeping them in allScorers) for when score() is called.
      } else {
        allScorers = scorers;
      }
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
