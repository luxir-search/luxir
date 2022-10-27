#pragma once

#include <solux/util/heap.h>
#include "solux/util/MemPool.h"
#include "solux/search/IndexReader.h"
#include "solux/search/Similarity.h"

namespace solux {

//
// The nature of dynamic (not known ahead of time) nested queries is such that they can't  be practically templated.
// I think a rational decision is to start with the same polymorphic model as lucene and then see what falls out
// and how we can improve common cases.  We may need codegen to do a really good job of it.
// Standard virtual method polymorphism will be easiest to work with and can be used to set up faster
// execution strategies.
//
// Re: variant / visit:
// https://www.reddit.com/r/cpp/comments/kst2pu/with_stdvariant_you_choose_either_performance_or/
// https://www.reddit.com/r/cpp/comments/ktyxqa/variants_suck_but_you_can_get_good_performance/
//
// Perhaps something like variant *could* make sense over the lowest-level doc iterators.
//

// Overview:
// - Query represents a user query.
// - Weight is created by a Query for a specific index
// - Scorer is created by a Weight for a specific segment?
//

// Since we will normally be starting with a Protobuf Query, can we drive things from that
// and directly create Weight objects?  Still, we perhaps need a parsing-like phase where we
// figure out what a match on a field means (i.e. if we need to tokenize, lowercase, etc...)
// We also eventually want a simple string parser (to directly handle user queries) and maybe
// even a Lucene-compatible parser.  If so, we're probably still going to want the Query
// hierarchy unless we translate everything into Protobuf classes.
//

/// A map from KeyType to a vector of pointers to ValType.
/// The map internals, the vector, and the instances of ValType are all pool allocated.
/// The pointers to ValType are unique_ptr with a custom deleter that only calls the destructor.
template <typename KeyType, typename ValType>
class PoolMapVec {
public:
  // This is mostly a helper class since it was hard to get the types right the first time.

  using key_type = KeyType;
  using value_type = ValType;
  using valptr = u_ptr<value_type>;
  using vec_type = std::vector<valptr, MemPool::allocator<valptr>>;
  using mapped_type = vec_type;
  using pair_type = std::pair<const key_type, vec_type>;
  using map_type = gtl::node_hash_map<key_type, vec_type, std::hash<std::string_view>, std::equal_to<>, MemPool::allocator<pair_type>>;
  // Using a node_hash_map in a pool will lead to less memory wasted if the map is resized.


  MemPool& pool;
  map_type map;

  PoolMapVec(MemPool& pool, size_t initialMapSize) : pool(pool), map(initialMapSize, pool.getAllocator()) {}

  vec_type& insertOrGet(const key_type& key) {
    return map.try_emplace(key, pool.getAllocator()).first->second;
  }
};

// NOTE: no virtual destructor, so subclasses of Query should be made trivially destructible
class Query {
public:
  class Context;
  class Weight;
  class Scorer;

  /// Returns a non-owning pointer to the created weight.  The Query::Context
  /// is responsible for the lifecycle of the created Weight.
  // TODO: pass down flags like NEED_SCORES, etc
  virtual Query::Weight* createWeight(Query::Context& context) = 0;

  /// Gives context to a Query (i.e. what index it's being used on amongst other things) when creating weights
  class Context {
  public:
    MemPool& pool;
    IndexReader& topReader;
    Weight* top = nullptr;

    // TODO: put in pool
    std::vector<FieldReader> fieldReaders;
    using SegFieldInfoMap = PoolMapVec<std::string_view, SegFieldInfo>;
    SegFieldInfoMap segFieldInfoMap;


    Context(MemPool& pool, IndexReader& topReader)
    : pool(pool), topReader(topReader), segFieldInfoMap(pool, 4) {

      for (auto& segment : topReader.segments()) {
        fieldReaders.emplace_back(pool, segment.postingsReader());
      }
    }

    // return number of segments
    int numSegments() const noexcept {
      return topReader.segments().size();
    }

    std::span<u_ptr<SegFieldInfo>> getSegFieldInfos(std::string_view fieldName) {
      auto numSegs = numSegments();

      // TODO: what if this is a non-existant field? We probably should not cache that (esp somewhere that could
      // lead to unbounded growth, like if we moved this segFieldInfo cache to the IndexReader

      auto &vec = segFieldInfoMap.insertOrGet(fieldName);
      if (vec.size() == 0) {
        vec.resize(numSegs);
        for (int i = 0; i < numSegs; ++i) {
          if (fieldReaders[i].seek(fieldName)) {
            vec[i] = pool.make_unique<SegFieldInfo>();
            fieldReaders[i].readFieldInfo(*vec[i]);
          } else {
            vec[i] = nullptr;
          }
        }
      }

      return vec;
    }
  };

  // A weight is created by a query for a specific index
  class Weight {
  protected:
    Query::Context& context;
  public:
    Weight(Query::Context& context) : context(context) {}

    // Create a scorer for a specific segment in the specific MemPool
    virtual Query::Scorer* createScorer(MemPool& target, IndexReader::Segment& segment) = 0;

    // NOTE: no virtual destructor, so subclasses should be made trivially destructible
  };

  class Scorer {
  public:
    virtual int32_t next() = 0;
    virtual int32_t advance(int32_t docid) {
      int32_t doc;
      while ((doc = next()) < docid) {}
      return doc;
    }
    virtual bool advanceExact (int32_t docid) {
      return advance(docid) == docid;
    }
    /// doc we are positioned on
    virtual int32_t docId() = 0;
    /// term frequency for current doc
    virtual float score() = 0;

    // NOTE: no virtual destructor, so subclasses should be made trivially destructible
  };
};


class TermQuery final : public Query {
protected:
  std::string_view field;
  std::string_view term;
  float boost;
public:
  // TermQuery constructor
  TermQuery(std::string_view field, std::string_view term, float boost=1.0f) : field(field), term(term), boost(boost) {}

  std::string_view getField() const {
    return field;
  }
  std::string_view getTerm() const {
    return term;
  }

  TermQuery::Weight* createWeight(Query::Context& context) override {
    TermQuery::Weight* weight = context.pool.make<TermQuery::Weight>(context, *this);
    return weight;
  }

  class Weight final : public Query::Weight {
  protected:
    TermQuery &query;
    std::span<u_ptr<SegFieldInfo>> segFieldInfos;
  public:
    Similarity::FieldStats fieldStats;
    Similarity::TermStats termStats;

    Similarity::BM25Scorer* simScorer;

    Weight(Query::Context& context, TermQuery& query) : Query::Weight(context), query(query) {
      calcFieldStats(fieldStats, termStats);
      simScorer = context.pool.make<Similarity::BM25Scorer>(Similarity().getScorer(1.0f, fieldStats, termStats));
    }


    // calculate aggregate field and term statistics
    // (TODO: share fieldStats)
    void calcFieldStats(Similarity::FieldStats& fieldStats, Similarity::TermStats& termStats) {
      fieldStats.maxDoc = context.topReader.numDocs();

      auto numSegs = context.numSegments();
      segFieldInfos = context.getSegFieldInfos(query.getField());

      for (int i = 0; i < numSegs; ++i) {
        auto& segFieldInfo = segFieldInfos[i];
        if (segFieldInfo) {
          TermsEnum termsEnum(context.pool, context.topReader.segments()[i].postingsReader(), *segFieldInfo);
          // add the stats from this termsEnum to fieldStats
          fieldStats.sumTotalTermFreq += termsEnum.sumTotalTermFreq();
          fieldStats.sumDocFreq += termsEnum.sumDocFreq();
          fieldStats.docsWithField += termsEnum.docsWithField();

          // We currently need DocsEnum to get the termStats
          if (termsEnum.seek(query.getTerm())) {
            DocsEnum docsEnum(context.pool, context.topReader.segments()[i].postingsReader(), termsEnum);
            termStats.docFreq += docsEnum.numDocs();
            termStats.totalTermFreq += docsEnum.totalTermFreq();
          }
        }
      }
    }

    TermQuery::Scorer* createScorer(MemPool &targetPool, IndexReader::Segment &segment) override {
      // TODO: cache the DocsEnum from when we had to calculate the term stats?
      auto* segFieldInfo = segFieldInfos[segment.ord].get();
      assert(segFieldInfo != nullptr);  // we should never get this far if the field doesn't exist in this segment... but perhaps as a general mechanism we should return nullptr here?
      TermsEnum termsEnum(context.pool, segment.postingsReader(), *segFieldInfo);
      if (!termsEnum.seek(query.getTerm())) {
        return nullptr;
      }
      // If we create a new DocsEnum each time, then we can put it in the targetPool.  If we cache it, it should be
      // cached elsewhere (like the context pool?)
      DocsEnum* docsEnum = targetPool.make<DocsEnum>(targetPool, segment.postingsReader(), termsEnum);
      IntColReader* normsReader = targetPool.make<IntColReader>(targetPool, segment.postingsReader(), *segFieldInfo);
      return targetPool.make<TermQuery::Scorer>(*docsEnum, *normsReader, *simScorer);
    }

  };

  class Scorer final : public Query::Scorer {
  public:
    DocsEnum& docsEnum;
    // IntColReader normsReader; // prob not necessary?
    IntColReader::Iterator normsIter;
    Similarity::BM25Scorer& simScorer; // todo: pass this in, it could be shared
    // point back to weight?  Require TermWeight? Or what if we want to use this from other types of queries though?
    // pass in the ord of this segment? or the actual IndexReader::Segment& seg;

    Scorer(DocsEnum& docsEnum,  IntColReader& normsReader, Similarity::BM25Scorer& simScorer)
    : docsEnum(docsEnum), normsIter(normsReader), simScorer(simScorer)
    {
    }

    int32_t next() override {
      return docsEnum.nextDoc();
    }

    /// doc we are positioned on
    int32_t docId() override {
      return docsEnum.docId();
    }

    float score() override {
      auto docid = docsEnum.docId();
      int32_t tf = docsEnum.termFreq();
      int32_t normDoc = normsIter.advance(docid);
      assert(normDoc == docid);
      auto encodedNorm = normsIter.value();
      return simScorer.score((float)tf, encodedNorm);
    }

    /// term frequency for current doc
    int termFreq()  {
      return docsEnum.termFreq();
    }

    // make a pusher / visitor for term scorer?

  };

};



class BooleanQuery : public Query {
  std::span<Query*> mandatory;
  std::span<Query*> optional;
  std::span<Query*> prohibited;
  std::span<Query*> filter;

public:
  BooleanQuery(std::span<Query*> mandatory, std::span<Query*> optional, std::span<Query*> prohibited, std::span<Query*> filter)
  : mandatory(mandatory), optional(optional), prohibited(prohibited), filter(filter)
  {
  }

  Weight *createWeight(Context &context) override {
    return context.pool.make<BooleanQuery::Weight>(context, *this);
  }

  class Weight : public Query::Weight {
    BooleanQuery &query;
    std::span<Query::Weight*> mandatoryWeights;
    std::span<Query::Weight*> optionalWeights;

    // TODO: make these static (and refactor to query) so other queries can use them?
    // Returns a span of Weights, corresponding to the given span of Queries. Some weights can be null.
    std::span<Query::Weight*> createWeights(MemPool &targetPool, Query::Context& context, std::span<Query*> queries) {
      if (queries.size() == 0) {
        return {};
      }
      auto weights = targetPool.make_arr<Query::Weight*>(queries.size());
      for (int i=0; i<queries.size(); ++i) {
        weights[i] = queries[i]->createWeight(context);
      }
      return {weights, queries.size()};
    }

    std::span<Query::Scorer*> createScorers(MemPool &targetPool, IndexReader::Segment &segment, std::span<Query::Weight*> weights) {
      if (weights.size() == 0) {
        return {};
      }
      // could optimize for 1 as well, but not a big deal.
      auto& scorers = *targetPool.make_vec<Query::Scorer*>();
      scorers.reserve(weights.size());
      for (auto* weight : weights) {
        auto* scorer = weight->createScorer(targetPool, segment);
        if (scorer != nullptr) {
          scorers.push_back(scorer);
        }
      }
      return scorers;
    }


  public:
    Weight(Query::Context &context, BooleanQuery &query) : Query::Weight(context), query(query)
    {
      mandatoryWeights = createWeights(context.pool, context, query.mandatory);
      optionalWeights = createWeights(context.pool, context, query.optional);
    }



    Query::Scorer *createScorer(MemPool &targetPool, IndexReader::Segment &segment) override {
      auto mandatoryScorers = createScorers(targetPool, segment, mandatoryWeights);
      // if some mandatory scorers are missing then it's impossible to match this segment
      if (mandatoryScorers.size() < query.mandatory.size()) {
        return nullptr;
      }

      Query::Scorer* mandScorer = nullptr;
      if (mandatoryScorers.size() > 0) {
        if (mandatoryScorers.size() == 1) {
          mandScorer = mandatoryScorers[0];
        } else {
          mandScorer = targetPool.make<BooleanQuery::ConjunctionScorer>(targetPool, mandatoryScorers);
        }
      }

      auto optionalScorers = createScorers(targetPool, segment, optionalWeights);
      Query::Scorer* optScorer = nullptr;
      if (optionalScorers.size() > 0) {
        if (optionalScorers.size() == 1) {
          optScorer = optionalScorers[0];
        } else {
          optScorer = targetPool.make<DisjunctionScorer>(targetPool, optionalScorers);
        }
      }

      if (mandScorer == nullptr) {
        return optScorer;
      } else if (optScorer == nullptr) {
        return mandScorer;
      } else {
        // TODO: combine mandatory and optional
        return nullptr;
      }
    }
  };

  class Scorer : public Query::Scorer {
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


  class ConjunctionScorer : public Query::Scorer {
    std::span<Query::Scorer*> scorers;

    // TODO: OPT: heapifying with virtual methods prob isn't a good idea... pull out and save the docid.
    constexpr static auto idComparator = [](Query::Scorer& a, Query::Scorer& b) { return b.docId() < a.docId(); };

    // with a ton of clauses, a maxHeap could help with quickly finding the largest number to skip to.

    int32_t docid = -1;


    // internal utility method where first scorer has already been advanced and is equal to the target.
    int32_t doNext(int32_t target) {
      auto* firstScorer = scorers[0];

      outer:
      for (;;) {
        for (int j = 1; j < scorers.size(); j++) {
          int32_t id = scorers[j]->advance(target);
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
    ConjunctionScorer(MemPool& pool, std::span<Query::Scorer*> scorers)
            : scorers(scorers)
    {
    }

    int32_t next() override {
      return doNext(scorers[0]->next());
    }

    int32_t advance(int32_t docid) override {
      return doNext(scorers[0]->advance(docid));
    }

    /// doc we are positioned on
    int32_t docId() override {
      return docid;
    }

    float score() override {
      float score = 0.0f;
      for (auto* scorer : scorers) {
        assert(scorer->docId() == docid);
        score += scorer->score();
      }
      return score;
    }
  };



  class DisjunctionScorer : public Query::Scorer {
    std::span<Query::Scorer*> scorers;

    // TODO: OPT: heapifying with virtual methods prob isn't a good idea... pull out and save the docid.
    constexpr static auto idComparator = [](Query::Scorer& a, Query::Scorer& b) { return b.docId() < a.docId(); };

    IndirectPQ<Query::Scorer, decltype(idComparator)> pq;

    int32_t docid = -1;
  public:
    // The passed in span of scorers will be modified (rearranged).
    DisjunctionScorer(MemPool& pool, std::span<Query::Scorer*> scorers)
    : scorers(scorers), pq(scorers)
    {
    }

    int32_t next() override {
      int currid = docid;

      // if pq.size()==0, then should have previously returned END and so next() should not be called after that.
      assert(pq.size() > 0);
      assert(pq.top().docId() == docid);

      docid = pq.top().next();
      for (;;) {
        if (docid == PostingsReader::END) {
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

    int32_t advance(int32_t docid) override {
      return -1;
    }

    bool advanceExact (int32_t docid) override {
      return -1;
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
      for (int i=1; i<pq.size(); i++) {
        auto id = scorers[i]->docId();
        assert(id >= docid);
        if (id == docid) {
          score += scorers[i]->score();
        }
      }
      return score;
    }
  };


};



} // end namespace