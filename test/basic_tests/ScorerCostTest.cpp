#include <gtest/gtest.h>

#include "test/SoluxTest.h"
#include "test/CollectionHelper.h"
#include "solux/query/TermQuery.h"
#include "solux/query/PhraseQuery.h"
#include "solux/query/BooleanQuery.h"
#include "solux/query/ConstantScoreQuery.h"
#include "solux/query/ForcePrepareQuery.h"

using namespace solux;
using namespace solux::test;

// Single-segment corpus: df(a)=6, df(b)=3, df(c)=1, maxDoc=6.
class ScorerCostTest : public SoluxTest {
public:
  CollectionHelper helper;
  std::shared_ptr<IndexReader> reader;
  MemPool pool;

  ScorerCostTest() {
    helper.clear();
    helper.index(flatdoc("id", "d0", "body_w", "a b c"), UpdateMessage::NO_COMMIT);
    helper.index(flatdoc("id", "d1", "body_w", "a b"), UpdateMessage::NO_COMMIT);
    helper.index(flatdoc("id", "d2", "body_w", "a"), UpdateMessage::NO_COMMIT);
    helper.index(flatdoc("id", "d3", "body_w", "a"), UpdateMessage::NO_COMMIT);
    helper.index(flatdoc("id", "d4", "body_w", "a"), UpdateMessage::NO_COMMIT);
    helper.index(flatdoc("id", "d5", "body_w", "b a"), UpdateMessage::COMMIT);
    reader = helper.getIndexWriter()->getIndexReader();
  }

  int64_t cost(Query* q) {
    Query::Context ctx(pool, *reader);
    auto* weight = q->createWeight(ctx);
    auto& seg = reader->segments()[0];
    // Mirror execution: queries that need a whole-index pass (ForcePrepare, or a
    // boolean that contains one) are scored through their PreparedWeight, so read
    // cost off the prepared supplier - that is the path materialized filters take.
    if (weight->needsPrepare()) {
      Query::Weight::PrepareContext pctx{*reader, std::span<DocSet* const>{}, false};
      auto prepared = weight->prepare(pctx);
      return prepared->scorerSupplier(pool, seg)->cost();
    }
    return weight->scorerSupplier(pool, seg)->cost();
  }

  TermQuery* term(std::string_view t) { return pool.make<TermQuery>("body_w", t); }

  std::span<Query*> clauses(std::initializer_list<Query*> qs) {
    auto span = pool.make_span<Query*>(qs.size());
    size_t i = 0;
    for (auto* q : qs) span[i++] = q;
    return span;
  }

  BooleanQuery* boolq(std::span<Query*> mandatory, std::span<Query*> optional, int minShouldMatch = 0) {
    std::span<Query*> none{};
    return pool.make<BooleanQuery>(mandatory, optional, none, none, minShouldMatch);
  }

  BooleanQuery* filterBoolq(std::span<Query*> filter, std::span<Query*> optional) {
    std::span<Query*> none{};
    return pool.make<BooleanQuery>(none, optional, none, filter, 0);
  }
};

TEST_F(ScorerCostTest, termCostIsDocFreq) {
  EXPECT_EQ(6, cost(term("a")));
  EXPECT_EQ(3, cost(term("b")));
  EXPECT_EQ(1, cost(term("c")));
}

TEST_F(ScorerCostTest, phraseCostIsRarestTerm) {
  auto terms = pool.make_span<std::string_view>(2);
  terms[0] = "a";
  terms[1] = "b";
  auto pos = pool.make_span<int32_t>(2);
  pos[0] = 0;
  pos[1] = 1;
  EXPECT_EQ(3, cost(pool.make<PhraseQuery>("body_w", terms, pos)));
}

TEST_F(ScorerCostTest, disjunctionCostIsSumCapped) {
  EXPECT_EQ(4, cost(boolq({}, clauses({term("b"), term("c")}))));
  EXPECT_EQ(6, cost(boolq({}, clauses({term("a"), term("b")}))));
}

TEST_F(ScorerCostTest, conjunctionCostIsRarest) {
  EXPECT_EQ(3, cost(boolq(clauses({term("a"), term("b")}), {})));
}

TEST_F(ScorerCostTest, minShouldMatchCostIsCheapestSubset) {
  EXPECT_EQ(4, cost(boolq({}, clauses({term("a"), term("b"), term("c")}), 2)));
}

TEST_F(ScorerCostTest, constantScoreCostDelegatesToChild) {
  EXPECT_EQ(1, cost(pool.make<ConstantScoreQuery>(term("c"), 1.0f)));
}

TEST_F(ScorerCostTest, nestedBooleanCompositeCost) {
  auto* inner = boolq({}, clauses({term("b"), term("c")}));
  EXPECT_EQ(4, cost(boolq(clauses({term("a"), inner}), {})));
}

TEST_F(ScorerCostTest, filterPlusOptionalCapsByOptional) {
  // No mandatory: filter and optional are conjoined, so the rare optional caps
  // the estimate. filter a(6) AND optional c(1) -> 1, not the filter's 6.
  EXPECT_EQ(1, cost(filterBoolq(clauses({term("a")}), clauses({term("c")}))));
}

TEST_F(ScorerCostTest, forcePrepareCostDelegatesToChild) {
  // Exercises the prepared path: ForcePrepare always prepares, and its prepared
  // supplier is the child's.
  EXPECT_EQ(1, cost(pool.make<ForcePrepareQuery>(term("c"))));
}

TEST_F(ScorerCostTest, preparedBooleanFilterUsesDocSetCardinality) {
  // A ForcePrepare optional clause forces the boolean through prepare(), so the
  // filter c materializes into a DocSet (card 1) exposed via DocSetSupplier.
  // filter c(1) AND optional a(6) -> 1, proving the filter cost is its
  // cardinality, not maxDoc (which would give 6).
  auto* opt = pool.make<ForcePrepareQuery>(term("a"));
  EXPECT_EQ(1, cost(filterBoolq(clauses({term("c")}), clauses({opt}))));
}
