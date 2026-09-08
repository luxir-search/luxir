// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <limits>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "luxir/query/BooleanQuery.h"
#include "luxir/query/PhraseQuery.h"
#include "luxir/query/QueryShape.h"
#include "luxir/query/TermQuery.h"
#include "test/CollectionHelper.h"
#include "test/LuxirTest.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

class PhraseCapabilityShapeTest : public LuxirTest {
public:
  CollectionHelper helper;
  std::shared_ptr<IndexReader> reader;
  MemPool pool;

  PhraseCapabilityShapeTest() {
    EXPECT_TRUE(helper.indexAll(
        {flatdoc("id", "0", "body_w", "common rare r x r"),
         flatdoc("id", "1", "body_w", "common"),
         flatdoc("id", "2", "body_w", "common"),
         flatdoc("id", "3", "body_w", "common")},
        UpdateMessage::COMMIT).success);
    reader = helper.getIndexWriter()->getIndexReader();
  }

  Query::ScorerShape shape(
      std::span<std::string_view> terms,
      std::span<const int32_t> positions, int32_t slop = 0,
      int32_t flags = 0,
      int64_t leadCost = std::numeric_limits<int64_t>::max(),
      bool disableSort = false) {
    PhraseQuery query("body_w", terms, positions, slop);
    Query::Context context(pool, *reader);
    auto* weight = query.createWeight(context, flags);
    auto* supplier = weight->scorerSupplier(pool, reader->segments()[0]);
    Query::PlanContext buildContext{
      .demand = Query::Demand::fromLeadCost(leadCost),
      .phraseDisableSortForTests = disableSort,
    };
    return supplier->describeScorer(buildContext);
  }
};

TEST_F(PhraseCapabilityShapeTest, presentAndMissingTermsAreNonemptyAndEmpty) {
  std::array<std::string_view, 2> presentTerms{"common", "rare"};
  std::array<std::string_view, 2> missingTerms{"common", "missing"};
  std::array<int32_t, 2> positions{0, 1};

  Query::ScorerShape present = shape(presentTerms, positions);
  EXPECT_EQ(Query::MatchState::NONEMPTY, present.matchState);
  EXPECT_EQ(Query::DirectScorerKind::OTHER, present.directKind);
  EXPECT_EQ(Query::ReportedTwoPhase::YES, present.reportedTwoPhase);
  EXPECT_EQ(Query::ClauseShape::NONE, present.windowFillClause);
  EXPECT_EQ(Query::ClauseShape::NONE, present.termDisjunctionClause);
  EXPECT_EQ(Query::IndependentTermAccess::UNSUPPORTED,
            present.independentTerm);
  EXPECT_EQ(Query::DocsOnlyAccess::UNSUPPORTED, present.docsOnly);
  EXPECT_EQ(Query::DirectDocSetAccess::UNSUPPORTED, present.directDocSet);

  EXPECT_EQ(Query::MatchState::EMPTY,
            shape(missingTerms, positions).matchState);
}

TEST_F(PhraseCapabilityShapeTest,
       compoundKeepsNestedVerificationSeparateFromOuterProtocol) {
  std::array<std::string_view, 2> phraseTerms{"common", "rare"};
  std::array<int32_t, 2> positions{0, 1};
  PhraseQuery phrase("body_w", phraseTerms, positions);
  TermQuery rare("body_w", "rare");
  std::array<Query*, 2> phraseRequired{&phrase, &rare};
  BooleanQuery phraseConjunction(phraseRequired, {}, {}, {});
  EXPECT_EQ(Query::VerificationWork::PRESENT,
            membershipVerificationWork(phraseConjunction));

  Query::Context phraseContext(pool, *reader);
  auto* phraseWeight = phraseConjunction.createWeight(phraseContext, 0);
  auto* phraseSupplier = phraseWeight->scorerSupplier(
      pool, reader->segments()[0]);
  ASSERT_NE(nullptr, phraseSupplier);
  auto phrasePlanContext = phraseSupplier->makePlanContext(
      Query::Demand::fromLeadCost(phraseSupplier->cost()));
  EXPECT_EQ(Query::ReportedTwoPhase::NO,
            phraseSupplier->describeScorer(
                phrasePlanContext).reportedTwoPhase);
  EXPECT_EQ(Query::VerificationWork::PRESENT,
            phraseSupplier->verificationWork(phrasePlanContext));

  std::array<Query*, 2> phraseOptional{&phrase, &rare};
  BooleanQuery phraseDisjunction({}, phraseOptional, {}, {});
  EXPECT_EQ(Query::VerificationWork::PARTIAL,
            membershipVerificationWork(phraseDisjunction));
  Query::Context disjunctionContext(pool, *reader);
  auto* disjunctionWeight = phraseDisjunction.createWeight(
      disjunctionContext, 0);
  auto* disjunctionSupplier = disjunctionWeight->scorerSupplier(
      pool, reader->segments()[0]);
  ASSERT_NE(nullptr, disjunctionSupplier);
  auto disjunctionPlanContext = disjunctionSupplier->makePlanContext(
      Query::Demand::fromLeadCost(disjunctionSupplier->cost()));
  EXPECT_EQ(Query::VerificationWork::PARTIAL,
            disjunctionSupplier->verificationWork(
                disjunctionPlanContext));

  TermQuery common("body_w", "common");
  std::array<Query*, 2> termRequired{&common, &rare};
  BooleanQuery termConjunction(termRequired, {}, {}, {});
  EXPECT_EQ(Query::VerificationWork::ABSENT,
            membershipVerificationWork(termConjunction));
  Query::Context termContext(pool, *reader);
  auto* termWeight = termConjunction.createWeight(termContext, 0);
  auto* termSupplier = termWeight->scorerSupplier(
      pool, reader->segments()[0]);
  ASSERT_NE(nullptr, termSupplier);
  auto termPlanContext = termSupplier->makePlanContext(
      Query::Demand::fromLeadCost(termSupplier->cost()));
  EXPECT_EQ(Query::ReportedTwoPhase::NO,
            termSupplier->describeScorer(
                termPlanContext).reportedTwoPhase);
  EXPECT_EQ(Query::ClauseShape::FLAT_CONJUNCTION,
            termSupplier->describeScorer(
                termPlanContext).termConjunctionClause);
  EXPECT_EQ(Query::VerificationWork::ABSENT,
            termSupplier->verificationWork(termPlanContext));
}

TEST_F(PhraseCapabilityShapeTest,
       exclusionFillUsesLeadThresholdAndSnapshottedSortControl) {
  std::array<std::string_view, 2> terms{"common", "rare"};
  std::array<int32_t, 2> positions{0, 1};
  int32_t flags = Query::EXCLUSION_WINDOW_FILL;

  EXPECT_EQ(Query::ClauseShape::NONE,
            shape(terms, positions, 0, flags, 1).windowFillClause);
  EXPECT_EQ(Query::ClauseShape::DIRECT,
            shape(terms, positions, 0, flags, 2).windowFillClause);
  EXPECT_EQ(Query::ClauseShape::NONE,
            shape(terms, positions, 0, flags, 4, true).windowFillClause);
  EXPECT_EQ(Query::ClauseShape::DIRECT,
            shape(terms, positions, 0, flags, 8, true).windowFillClause);
}

TEST_F(PhraseCapabilityShapeTest, sloppyAndExactFillCostsDiverge) {
  std::array<std::string_view, 2> terms{"common", "rare"};
  std::array<int32_t, 2> positions{0, 1};
  int32_t flags = Query::EXCLUSION_WINDOW_FILL;

  EXPECT_EQ(Query::ClauseShape::DIRECT,
            shape(terms, positions, 0, flags, 3).windowFillClause);
  EXPECT_EQ(Query::ClauseShape::NONE,
            shape(terms, positions, 1, flags, 3).windowFillClause);
}

TEST_F(PhraseCapabilityShapeTest, sloppyRepeatGroupIsIncludedInFillCost) {
  std::array<std::string_view, 3> terms{"r", "x", "r"};
  std::array<int32_t, 3> positions{0, 1, 2};
  int32_t flags = Query::EXCLUSION_WINDOW_FILL;

  EXPECT_EQ(Query::ClauseShape::NONE,
            shape(terms, positions, 1, flags, 17).windowFillClause);
  EXPECT_EQ(Query::ClauseShape::DIRECT,
            shape(terms, positions, 1, flags, 18).windowFillClause);
}

TEST_F(PhraseCapabilityShapeTest,
       constructionMatchesEstimatedSortedRepeatLayout) {
  std::array<std::string_view, 3> terms{"r", "x", "r"};
  std::array<int32_t, 3> positions{0, 1, 2};
  PhraseQuery query("body_w", terms, positions, 1);
  Query::Context context(pool, *reader);
  auto* weight = query.createWeight(
      context, Query::EXCLUSION_WINDOW_FILL);
  auto* supplier = weight->scorerSupplier(pool, reader->segments()[0]);

  ASSERT_NE(nullptr, buildScorerForTests(pool, *supplier, 18));
}

TEST_F(PhraseCapabilityShapeTest,
       estimateDedupUsesMinimumOrdinalAndCostTermOrder) {
  std::array<std::string_view, 6> terms{
      "common", "r", "rare", "r", "x", "common"};
  std::array<int32_t, 6> positions{0, 1, 2, 3, 4, 5};
  PhraseQuery query("body_w", terms, positions);
  Query::Context context(pool, *reader);
  auto* weight = dynamic_cast<PhraseQuery::Weight*>(
      query.createWeight(context, 0));
  ASSERT_NE(nullptr, weight);
  auto& segment = reader->segments()[0];

  EXPECT_EQ((std::vector<int32_t>{0, 1, 2, 1, 4, 0}),
            weight->querySlotSourcesForTests(segment));
  EXPECT_EQ((std::vector<int32_t>{2, 4, 1, 0}),
            weight->conjunctionOrderForTests(segment));

  // The disabled-sort fallback retains query order while choosing the same
  // minimum original ordinal for every repeated term.
  EXPECT_EQ((std::vector<int32_t>{0, 1, 2, 1, 4, 0}),
            weight->querySlotSourcesForTests(segment, true));
  EXPECT_EQ((std::vector<int32_t>{0, 1, 2, 4}),
            weight->conjunctionOrderForTests(segment, true));
}
