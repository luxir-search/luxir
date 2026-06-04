#pragma once

#include <memory>
#include <utility>

#include "Query.h"
#include "QueryPrep.h"

namespace solux {

// Wrapper that preserves a child query's matching docs but assigns the same
// score to each hit. This is also a small example of the compound-query
// prepare contract: create a child Weight, forward needsPrepare(), and wrap
// either the live child Weight or its PreparedWeight at scorer creation time.
class ConstantScoreQuery final : public solux::Query {
  Query* child;
  float constantScore;

  class Scorer final : public Query::Scorer {
    Query::Scorer* child;
    float constantScore;

  public:
    Scorer(Query::Scorer* child, float constantScore) : child(child), constantScore(constantScore) {}

    int32_t next() override {
      return child->next();
    }

    int32_t advance(int32_t docid) override {
      return child->advance(docid);
    }

    bool advanceExact(int32_t docid) override {
      return child->advanceExact(docid);
    }

    int32_t docId() override {
      return child->docId();
    }

    float score() override {
      return constantScore;
    }
  };

public:
  ConstantScoreQuery(Query* child, float constantScore = 1.0f) : child(child), constantScore(constantScore) {}

  Weight* createWeight(Context& context) override {
    return context.pool.make<ConstantScoreQuery::Weight>(context, *this);
  }

  class Weight final : public Query::Weight {
    Query::Weight* childWeight = nullptr;
    float constantScore;
    // TODO: once Weight creation gets a NEED_SCORES flag, create the child
    // scorer without scores so filters like TermQuery can skip sim/norm setup.

    class Prepared final : public Query::Weight::PreparedWeight {
      QueryPrep::PreparedSource child;
      float constantScore;

    public:
      Prepared(QueryPrep::PreparedSource&& child, float constantScore)
        : child(std::move(child)), constantScore(constantScore) {}

      Query::Scorer* createScorer(MemPool& targetPool, IndexReader::Segment& segment) override {
        auto* childScorer = QueryPrep::createScorer(targetPool, segment, child.segmentSource());
        if (childScorer == nullptr) return nullptr;
        return targetPool.make<ConstantScoreQuery::Scorer>(childScorer, constantScore);
      }

      bool outputIsSubsetOfDomain() const noexcept override {
        return child.prepared != nullptr && child.prepared->outputIsSubsetOfDomain();
      }
    };

  public:
    Weight(Context& context, ConstantScoreQuery& query)
      : Query::Weight(context), constantScore(query.constantScore) {
      childWeight = query.child->createWeight(context);
    }

    bool needsPrepare() const noexcept override {
      return childWeight->needsPrepare();
    }

    std::unique_ptr<Query::Weight::PreparedWeight> prepare(Query::Weight::PrepareContext& ctx) override {
      QueryPrep::PreparedSource source;
      source.weight = childWeight;
      if (childWeight->needsPrepare()) {
        source.prepared = childWeight->prepare(ctx);
      }
      return std::make_unique<Prepared>(std::move(source), constantScore);
    }

    Query::Scorer* createScorer(MemPool& targetPool, IndexReader::Segment& segment) override {
      auto* childScorer = QueryPrep::createScorer(targetPool, segment, *childWeight);
      if (childScorer == nullptr) return nullptr;
      return targetPool.make<ConstantScoreQuery::Scorer>(childScorer, constantScore);
    }
  };
};

} // namespace solux
