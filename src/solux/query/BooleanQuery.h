#pragma once

#include "Query.h"

namespace solux {

class BooleanQuery final : public solux::Query {
  std::span<Query*> mandatory;
  std::span<Query*> optional;
  std::span<Query*> prohibited;
  std::span<Query*> filter;

public:
  BooleanQuery(std::span<Query*> mandatory, std::span<Query*> optional, std::span<Query*> prohibited,
               std::span<Query*> filter)
          : mandatory(mandatory), optional(optional), prohibited(prohibited), filter(filter) {
  }

  Weight* createWeight(Context& context) override {
    return context.pool.make<BooleanQuery::Weight>(context, *this);
  }

  class Weight final : public Query::Weight {
    BooleanQuery& query;
    std::span<Query::Weight*> mandatoryWeights;
    std::span<Query::Weight*> optionalWeights;
    std::span<Query::Weight*> prohibitedWeights;
    std::span<Query::Weight*> filterWeights;

    // TODO: make these static (and refactor to query) so other queries can use them?
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

    std::span<Scorer*> createScorers(solux::MemPool& targetPool, solux::IndexReader::Segment& segment,
                                     std::span<Query::Weight*> weights) {
      if (weights.size() == 0) {
        return {};
      }
      // could optimize for 1 as well, but not a big deal.
      auto& scorers = *targetPool.make_vec<Query::Scorer*>();
      scorers.reserve(weights.size());
      for (auto* weight: weights) {
        auto* scorer = weight->createScorer(targetPool, segment);
        if (scorer != nullptr) {
          scorers.push_back(scorer);
        }
      }
      return scorers;
    }


  public:
    Weight(Context& context, BooleanQuery& query) : Query::Weight(context), query(query) {
      mandatoryWeights = createWeights(context.pool, context, query.mandatory);
      optionalWeights = createWeights(context.pool, context, query.optional);
      prohibitedWeights = createWeights(context.pool, context, query.prohibited);
      filterWeights = createWeights(context.pool, context, query.filter);
    }


    Scorer* createScorer(solux::MemPool& targetPool, solux::IndexReader::Segment& segment) override {
      auto mandatoryScorers = createScorers(targetPool, segment, mandatoryWeights);
      if (mandatoryScorers.size() < query.mandatory.size()) {
        // if any mandatory scorers are missing for this segment, then it's impossible to match.
        return nullptr;
      }

      auto filterScorers = createScorers(targetPool, segment, filterWeights);
      if (filterScorers.size() < query.filter.size()) {
        // if any filters are missing for this segment, then it's impossible to match
        return nullptr;
      }

      Query::Scorer* mandScorer = nullptr;
      if (mandatoryScorers.size() > 0) {
        if (mandatoryScorers.size() == 1) {
          mandScorer = mandatoryScorers[0];
        } else {
          mandScorer = targetPool.make<BooleanQuery::ConjunctionScorer>(targetPool, mandatoryScorers, filterScorers);
        }
      }

      auto optionalScorers = createScorers(targetPool, segment, optionalWeights);
      Query::Scorer* optScorer = nullptr;
      if (optionalScorers.size() > 0) {
        if (optionalScorers.size() == 1) {
          optScorer = optionalScorers[0];
        } else {
          optScorer = targetPool.make<BooleanQuery::DisjunctionScorer>(targetPool, optionalScorers);
        }
      }

      // Find the current top scorer... mandatory, optional, or a combination.
      Query::Scorer* boolScorer = nullptr;
      if (mandScorer == nullptr) {
        if (optScorer == nullptr) {
          return nullptr;
        }
        boolScorer = optScorer;
        // if there were no mandatory clauses then we still need to handle any filter clauses.
        if (filterScorers.size() > 0) {
          std::span<Query::Scorer*> optSpan(targetPool.make_arr<Query::Scorer*>(1), 1);
          optSpan[0] = optScorer;
          boolScorer = targetPool.make<BooleanQuery::ConjunctionScorer>(targetPool, optSpan, filterScorers);
        }
      } else if (optScorer == nullptr) {
        boolScorer = mandScorer;
      } else {
        boolScorer = targetPool.make<BooleanQuery::MandOptScorer>(targetPool, mandScorer, optScorer);
      }

      // Now apply prohibited clauses
      auto prohibitedScorers = createScorers(targetPool, segment, prohibitedWeights);
      Query::Scorer* prohibitedScorer = nullptr;
      if (prohibitedScorers.size() > 0) {
        if (prohibitedScorers.size() == 1) {
          prohibitedScorer = prohibitedScorers[0];
        } else {
          prohibitedScorer = targetPool.make<BooleanQuery::DisjunctionScorer>(targetPool, prohibitedScorers);
        }
        boolScorer = targetPool.make<BooleanQuery::MandNotScorer>(targetPool, boolScorer, prohibitedScorer);
      }

      return boolScorer;
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
    int32_t optId = -1;
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

    bool advanceExact(int32_t docid) override {
      if (mandScorer->advanceExact(docid)) {
        id = docid;
      }
      return id;
    }

    float score() override {
      float score = mandScorer->score();
      if (optId < id) {
        if (optScorer->advanceExact(id)) {
          optId = id;
        }
      }
      if (optId == id) {
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

    bool advanceExact(int32_t docid) override {
      return advance(docid) == docid;
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
          int32_t id = allScorers[j]->advance(target);
          assert(id >= target);
          if (id > target) {
            target = firstScorer->advance(target);
            goto outer;  // could perhaps replace with "j=0; continue;" but that seems potentially worse?
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
      int currid = docid;

      // if pq.size()==0, then should have previously returned END and so next() should not be called after that.
      assert(pq.size() > 0);
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
      assert(docid = pq.top().docId());
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


};

} // namespace solux