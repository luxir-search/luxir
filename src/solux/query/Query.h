#pragma once

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


class Query {
public:
  class Context;
  class Weight;
  class Scorer;

  /// Returns a non-owning pointer to the created weight.  The Query::Context
  /// is responsible for the lifecycle of the created Weight.
  virtual Query::Weight* createWeight(Query::Context context, float boost=1.0f) = 0;
  virtual ~Query() = default;


  /// Gives context to a Query (i.e. what index it's being used on amongst other things) when creating weights
  class Context {
  public:
    MemPool& pool;
    IndexReader& topReader;
    Weight* top;

    // OPT: may want to cache SegFieldInfo instances, FieldStats, TermStats,
    // TermsEnum, DocsEnum, SimScorers, etc in case terms and fields are used more than once?
  };

  // A weight is created by a query for a specific index
  class Weight {
  public:


    virtual ~Weight() = default;
  };

  class Scorer {
  public:

    virtual ~Scorer() = default;
  };
};



class TermQuery final : public Query {
public:

  TermQuery::Weight *createWeight(Query::Context context, float boost) override {
    return new TermQuery::Weight();
  }

  ~TermQuery() override = default;


  class Weight final : public Query::Weight {
  public:

    virtual ~Weight() = default;
  };

  class Scorer final : public Query::Scorer {
  public:
    DocsEnum& docsEnum;
    // IntColReader normsReader; // prob not necessary?
    IntColReader::Iterator normsIter;
    Similarity::BM25Scorer& simScorer; // todo: pass this in, it could be shared

    Scorer(DocsEnum& docsEnum,  IntColReader& normsReader, Similarity::BM25Scorer& simScorer)
    : docsEnum(docsEnum), normsIter(normsReader), simScorer(simScorer)
    {
    }

    int32_t next() {
      return docsEnum.nextDoc();
    }

    int32_t advance(int32_t docid) {
      return -1;
    }

    bool advanceExact (int32_t docid) {
      return -1;
    }

    /// doc we are positioned on
    int32_t docId() {
      return docsEnum.docId();
    }

    /// term frequency for current doc
    int termFreq() {
      return docsEnum.termFreq();
    }

    float score() {
      auto docid = docsEnum.docId();
      int32_t tf = docsEnum.termFreq();
      int32_t normDoc = normsIter.advance(docid);
      assert(normDoc == docid);
      auto encodedNorm = normsIter.value();
      return simScorer.score((float)tf, encodedNorm);
    }

    // make a pusher / visitor for term scorer?


    virtual ~Scorer() = default;
  };

};




} // end namespace