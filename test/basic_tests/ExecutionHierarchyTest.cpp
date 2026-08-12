#include <array>
#include <limits>
#include <type_traits>

#include <gtest/gtest.h>

#include "solux/query/AllQuery.h"
#include "solux/query/BoostQuery.h"
#include "solux/query/ConstantScoreQuery.h"
#include "solux/query/MatchNoDocsQuery.h"
#include "solux/query/PhraseQuery.h"
#include "solux/query/TermQuery.h"
#include "test/CollectionHelper.h"
#include "test/SoluxTest.h"

using namespace solux;
using namespace solux::test;

static_assert(!std::is_copy_constructible_v<Query::ScorerPlan>);
static_assert(!std::is_move_constructible_v<Query::ScorerPlan>);

namespace {

class DenseClauseGuard {
  bool saved = AllQuery::disableDenseClauseForTests;

public:
  ~DenseClauseGuard() {
    AllQuery::disableDenseClauseForTests = saved;
  }
};

class ExecutionHierarchyTest : public SoluxTest {
public:
  CollectionHelper helper;
  std::shared_ptr<IndexReader> reader;
  MemPool pool;

  ExecutionHierarchyTest() {
    EXPECT_TRUE(helper.indexAll(
        {flatdoc("id", "0", "body_w", "common rare"),
         flatdoc("id", "1", "body_w", "common")},
        UpdateMessage::COMMIT).success);
    reader = helper.getIndexWriter()->getIndexReader();
  }
};

TEST_F(ExecutionHierarchyTest, legacyDemandAdapterPreservesScalarAndUse) {
  Query::PlanContext context = Query::PlanContext::fromLeadCost(
      17, Query::ExecutionUse::COUNT_WINDOWS);
  EXPECT_EQ(17, context.demand.candidates);
  EXPECT_EQ(17, context.demand.span);
  EXPECT_EQ(Query::ExecutionUse::COUNT_WINDOWS, context.demand.horizon);
}

TEST_F(ExecutionHierarchyTest, termPlanBuildsAllRecordedProducts) {
  TermQuery query("body_w", "rare");
  Query::Context context(pool, *reader);
  auto* weight = query.createWeight(context, Query::NEED_SCORES);
  auto* supplier = weight->scorerSupplier(pool, reader->segments()[0]);
  ASSERT_NE(nullptr, supplier);

  Query::PlanContext planContext = Query::PlanContext::fromLeadCost(
      7, Query::ExecutionUse::MATCH_WINDOWS);
  auto* scorerPlan = supplier->resolve(pool, planContext);
  EXPECT_EQ(1, scorerPlan->cost());
  EXPECT_EQ(7, scorerPlan->demand().candidates);
  EXPECT_EQ(7, scorerPlan->demand().span);
  EXPECT_EQ(Query::ExecutionUse::MATCH_WINDOWS,
            scorerPlan->demand().horizon);
  EXPECT_EQ(Query::DirectScorerKind::TERM,
            scorerPlan->shape().directKind);
  auto* scorer = scorerPlan->build(pool);
  ASSERT_NE(nullptr, scorer);
  EXPECT_EQ(0, scorer->next());

  auto* independentPlan = supplier->resolve(pool, planContext);
  auto* independent = independentPlan->buildIndependent(pool);
  ASSERT_NE(nullptr, independent);
  EXPECT_EQ(0, independent->next());

  auto* docsPlan = supplier->resolve(pool, planContext);
  auto* docs = docsPlan->buildDocsOnly(pool);
  ASSERT_NE(nullptr, docs);
  EXPECT_EQ(0, docs->nextDoc());
}

TEST_F(ExecutionHierarchyTest, allPlanSnapshotsDenseControlAtSupplierCreation) {
  DenseClauseGuard guard;
  AllQuery::disableDenseClauseForTests = false;
  AllQuery::Supplier supplier(reader->segments()[0], 0.0f);
  Query::PlanContext context = Query::PlanContext::fromLeadCost(
      std::numeric_limits<int64_t>::max());
  auto* plan = supplier.resolve(pool, context);

  AllQuery::disableDenseClauseForTests = true;
  auto* scorer = plan->build(pool);
  ASSERT_NE(nullptr, scorer);
  EXPECT_EQ(Query::ClauseShape::DIRECT,
            plan->shape().windowFillClause);
}

TEST_F(ExecutionHierarchyTest, constantScorePlanRetainsChildPlan) {
  std::array<std::string_view, 2> terms{"common", "rare"};
  std::array<int32_t, 2> positions{0, 1};
  PhraseQuery phrase("body_w", terms, positions);
  ConstantScoreQuery query(&phrase, 4.0f);
  Query::Context context(pool, *reader);
  auto* weight = query.createWeight(context, Query::NEED_SCORES);
  auto* supplier = weight->scorerSupplier(pool, reader->segments()[0]);
  ASSERT_NE(nullptr, supplier);

  Query::PlanContext planContext = Query::PlanContext::fromLeadCost(
      3, Query::ExecutionUse::SCORED_WINDOWS);
  auto* plan = supplier->resolve(pool, planContext);
  auto* scorer = plan->build(pool);
  ASSERT_NE(nullptr, scorer);
  EXPECT_EQ(0, scorer->next());
  EXPECT_FLOAT_EQ(4.0f, scorer->score());
}

TEST_F(ExecutionHierarchyTest, boostIsPlanTransparentAndNoDocsHasNoSupplier) {
  TermQuery term("body_w", "rare");
  BoostQuery boost(&term, 2.0f);
  MatchNoDocsQuery none;
  Query::Context context(pool, *reader);
  auto& segment = reader->segments()[0];

  auto* boostedSupplier = boost.createWeight(
      context, Query::NEED_SCORES)->scorerSupplier(pool, segment);
  ASSERT_NE(nullptr, boostedSupplier);
  EXPECT_NE(nullptr,
            dynamic_cast<TermQuery::Weight::Supplier*>(boostedSupplier));
  auto* plan = boostedSupplier->resolve(
      pool, Query::PlanContext::fromLeadCost(1));
  EXPECT_EQ(Query::DirectScorerKind::TERM, plan->shape().directKind);

  auto* noneSupplier = none.createWeight(
      context, 0)->scorerSupplier(pool, segment);
  EXPECT_EQ(nullptr, noneSupplier);
}

} // namespace
