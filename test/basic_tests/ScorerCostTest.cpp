#include <gtest/gtest.h>

#include "test/SoluxTest.h"
#include "test/CollectionHelper.h"
#include "solux/query/TermQuery.h"
#include "solux/query/PhraseQuery.h"
#include "solux/query/BooleanQuery.h"
#include "solux/query/ConstantScoreQuery.h"

using namespace solux;
using namespace solux::test;

// Locks the cardinality cost() contract across query types. cost() only drives
// planning (lead selection), so a value regression - e.g. a phrase or nested
// boolean silently reverting to the DefaultScorerSupplier's maxDoc - would not
// change results and the other suites would not catch it.
//
// One segment of 6 docs over body_w (whitespace, case-sensitive). Per-term doc
// counts: a=6, b=3, c=1; maxDoc=6.
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

  // Cost of a query's supplier on the single segment.
  int64_t cost(Query* q) {
    Query::Context ctx(pool, *reader);
    auto* weight = q->createWeight(ctx);
    auto* supplier = weight->scorerSupplier(pool, reader->segments()[0]);
    return supplier->cost();
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
  // min(a=6, b=3); an upper bound, not the 2 docs the phrase actually matches.
  EXPECT_EQ(3, cost(pool.make<PhraseQuery>("body_w", terms, pos)));
}

TEST_F(ScorerCostTest, disjunctionCostIsSumCapped) {
  EXPECT_EQ(4, cost(boolq({}, clauses({term("b"), term("c")}))));   // 3 + 1
  EXPECT_EQ(6, cost(boolq({}, clauses({term("a"), term("b")}))));   // 6 + 3 capped at maxDoc 6
}

TEST_F(ScorerCostTest, conjunctionCostIsRarest) {
  EXPECT_EQ(3, cost(boolq(clauses({term("a"), term("b")}), {})));   // min(6, 3)
}

TEST_F(ScorerCostTest, minShouldMatchCostIsCheapestSubset) {
  // optional a(6) b(3) c(1), mm=2 -> sum of the n-mm+1 = 2 cheapest: 1 + 3.
  EXPECT_EQ(4, cost(boolq({}, clauses({term("a"), term("b"), term("c")}), 2)));
}

TEST_F(ScorerCostTest, constantScoreCostDelegatesToChild) {
  EXPECT_EQ(1, cost(pool.make<ConstantScoreQuery>(term("c"), 1.0f)));
}

TEST_F(ScorerCostTest, nestedBooleanCompositeCost) {
  // Required a(6) AND (b OR c) [disjunction cost 3+1=4] -> conjunction min(6, 4) = 4.
  auto* inner = boolq({}, clauses({term("b"), term("c")}));
  EXPECT_EQ(4, cost(boolq(clauses({term("a"), inner}), {})));
}
