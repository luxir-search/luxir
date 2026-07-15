#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <span>
#include <string_view>
#include <typeindex>
#include <utility>
#include <vector>

#include "gtest/gtest.h"
#include "test/SoluxTest.h"
#include "test/TestIndex.h"
#include "solux/query/AllQuery.h"
#include "solux/query/BooleanQuery.h"
#include "solux/query/BoostQuery.h"
#include "solux/query/ConstantScoreQuery.h"
#include "solux/query/ForcePrepareQuery.h"
#include "solux/query/TermQuery.h"

using namespace solux;
using namespace solux::test;

class BooleanNormalizeTest : public SoluxTest {
protected:
  using ScoreMap = std::map<std::pair<int32_t, int32_t>, float>;

  static void buildBodyIndex(TestIndex& testIndex,
                             std::span<const std::string_view> bodies) {
    TestField field(testIndex, "body_w");
    field.startIndexing();
    for (size_t i = 0; i < bodies.size(); i++) {
      field.add((int32_t) i, bodies[i]);
    }
    testIndex.flush();
    field.startReading();
  }

  static int64_t countHits(IndexReader& reader, Query& query) {
    MemPool pool;
    Query::Context context(pool, reader);
    auto* weight = query.createWeight(context, 0);
    int64_t count = 0;
    for (auto& segment : context.topReader.segments()) {
      auto* scorer = weight->createScorer(pool, segment);
      if (scorer == nullptr) continue;
      for (int32_t doc = scorer->next(); doc != PostingsReader::END;
           doc = scorer->next()) {
        count++;
      }
    }
    return count;
  }

  static ScoreMap collectScores(IndexReader& reader, Query& query,
                                int32_t flags = Query::NEED_SCORES) {
    MemPool pool;
    Query::Context context(pool, reader);
    auto* weight = query.createWeight(context, flags);
    ScoreMap scores;
    for (auto& segment : context.topReader.segments()) {
      auto* scorer = weight->createScorer(pool, segment);
      if (scorer == nullptr) continue;
      for (int32_t doc = scorer->next(); doc != PostingsReader::END;
           doc = scorer->next()) {
        scores[{segment.ord, doc}] = scorer->score();
      }
    }
    return scores;
  }

  static void expectScoresNear(const ScoreMap& expected, const ScoreMap& actual) {
    ASSERT_EQ(expected.size(), actual.size());
    auto expectedIt = expected.begin();
    auto actualIt = actual.begin();
    for (; expectedIt != expected.end(); expectedIt++, actualIt++) {
      ASSERT_EQ(expectedIt->first, actualIt->first);
      float scale = std::max({std::fabs(expectedIt->second),
                              std::fabs(actualIt->second), 1.0f});
      EXPECT_NEAR(expectedIt->second, actualIt->second, 1e-6f * scale);
    }
  }

  static BooleanQuery::NormalizeTestView shape(BooleanQuery& query) {
    MemPool pool;
    return query.normalizationForTest(pool);
  }
};

TEST_F(BooleanNormalizeTest, requiredPureNegativeChildInlinesAsComplement) {
  TestIndex testIndex;
  const std::string_view bodies[] = {"a", "a b", "b", "c"};
  buildBodyIndex(testIndex, bodies);

  TermQuery a("body_w", "a");
  TermQuery b("body_w", "b");
  Query* prohibited[] = {&b};
  BooleanQuery pureNegative({}, {}, prohibited, {});
  BoostQuery boost1(&pureNegative, 2.0f);
  BoostQuery boost2(&boost1, 3.0f);
  Query* mandatory[] = {&a, &boost2};
  BooleanQuery outer(mandatory, {}, {}, {});

  auto view = shape(outer);
  EXPECT_EQ(1, view.mandatoryCount);
  EXPECT_EQ(1, view.prohibitedCount);
  EXPECT_EQ(std::type_index(typeid(TermQuery)), view.prohibitedTypes[0]);
  EXPECT_EQ(BooleanQuery::R1_REQUIRED_INLINE,
            view.ruleMask & BooleanQuery::R1_REQUIRED_INLINE);
  EXPECT_EQ(1, countHits(*testIndex.reader, outer));

  Query* flatMandatory[] = {&a};
  BooleanQuery flat(flatMandatory, {}, prohibited, {});
  expectScoresNear(collectScores(*testIndex.reader, flat),
                   collectScores(*testIndex.reader, outer));

  Query* filter[] = {&boost2};
  BooleanQuery filterParent(flatMandatory, {}, {}, filter);
  auto filterView = shape(filterParent);
  EXPECT_EQ(1, filterView.mandatoryCount);
  EXPECT_EQ(0, filterView.filterCount);
  EXPECT_EQ(1, filterView.prohibitedCount);
  EXPECT_EQ(BooleanQuery::R1_REQUIRED_INLINE,
            filterView.ruleMask & BooleanQuery::R1_REQUIRED_INLINE);
  expectScoresNear(collectScores(*testIndex.reader, flat),
                   collectScores(*testIndex.reader, filterParent));
}

TEST_F(BooleanNormalizeTest, standalonePureNegativeSeedsAllAndStaysBoolean) {
  TestIndex testIndex;
  const std::string_view bodies[] = {"a", "a b", "b", "c"};
  buildBodyIndex(testIndex, bodies);

  TermQuery b("body_w", "b");
  Query* prohibited[] = {&b};
  BooleanQuery pureNegative({}, {}, prohibited, {});

  auto view = shape(pureNegative);
  EXPECT_EQ(0, view.mandatoryCount);
  EXPECT_EQ(1, view.optionalCount);
  EXPECT_EQ(1, view.prohibitedCount);
  EXPECT_EQ(std::type_index(typeid(AllQuery)), view.optionalTypes[0]);
  EXPECT_EQ(0u, view.ruleMask & BooleanQuery::R4_SINGLE_CLAUSE_UNWRAP);
  EXPECT_EQ(2, countHits(*testIndex.reader, pureNegative));

  MemPool pool;
  Query::Context context(pool, *testIndex.reader);
  auto* weight = pureNegative.createWeight(context, Query::NEED_SCORES);
  EXPECT_NE(dynamic_cast<BooleanQuery::Weight*>(weight), nullptr);
  EXPECT_TRUE(weight->isConstantScoring());
  EXPECT_FALSE(weight->needsPrepare());
  EXPECT_EQ(-1, weight->count(context.topReader.segments()[0]));
  for (const auto& [doc, score] : collectScores(*testIndex.reader, pureNegative)) {
    unused(doc);
    EXPECT_FLOAT_EQ(0.0f, score);
  }

  ForcePrepareQuery forced(&b);
  Query* forcedProhibited[] = {&forced};
  BooleanQuery preparingComplement({}, {}, forcedProhibited, {});
  MemPool preparePool;
  Query::Context prepareContext(preparePool, *testIndex.reader);
  EXPECT_TRUE(preparingComplement.createWeight(
      prepareContext, Query::NEED_SCORES)->needsPrepare());
}

TEST_F(BooleanNormalizeTest, requiredComplementFormInlinesUnderBothOccurs) {
  TestIndex testIndex;
  const std::string_view bodies[] = {"a", "a b", "b", "c"};
  buildBodyIndex(testIndex, bodies);

  TermQuery a("body_w", "a");
  TermQuery b("body_w", "b");
  AllQuery all;
  Query* optional[] = {&all};
  Query* prohibited[] = {&b};
  BooleanQuery complement({}, optional, prohibited, {}, 1);
  BoostQuery boosted(&complement, 4.0f);

  Query* mandatory[] = {&a, &boosted};
  BooleanQuery requiredParent(mandatory, {}, {}, {});
  auto requiredView = shape(requiredParent);
  EXPECT_EQ(1, requiredView.mandatoryCount);
  EXPECT_EQ(0, requiredView.optionalCount);
  EXPECT_EQ(1, requiredView.prohibitedCount);
  EXPECT_EQ(BooleanQuery::R1_REQUIRED_INLINE,
            requiredView.ruleMask & BooleanQuery::R1_REQUIRED_INLINE);

  Query* root[] = {&a};
  Query* filter[] = {&boosted};
  BooleanQuery filterParent(root, {}, {}, filter);
  auto filterView = shape(filterParent);
  EXPECT_EQ(1, filterView.mandatoryCount);
  EXPECT_EQ(0, filterView.filterCount);
  EXPECT_EQ(1, filterView.prohibitedCount);
  EXPECT_EQ(BooleanQuery::R1_REQUIRED_INLINE,
            filterView.ruleMask & BooleanQuery::R1_REQUIRED_INLINE);

  BooleanQuery flat(root, {}, prohibited, {});
  expectScoresNear(collectScores(*testIndex.reader, flat),
                   collectScores(*testIndex.reader, requiredParent));
  expectScoresNear(collectScores(*testIndex.reader, flat),
                   collectScores(*testIndex.reader, filterParent));
}

TEST_F(BooleanNormalizeTest, optionalComplementsStayOpaque) {
  TermQuery a("body_w", "a");
  TermQuery b("body_w", "b");
  AllQuery all;
  Query* complementOptional[] = {&all};
  Query* complementProhibited[] = {&b};
  BooleanQuery complement({}, complementOptional, complementProhibited, {});
  Query* outerOptional[] = {&complement, &a};
  BooleanQuery outer({}, outerOptional, {}, {});

  auto view = shape(outer);
  EXPECT_EQ(2, view.optionalCount);
  EXPECT_EQ(std::type_index(typeid(BooleanQuery)), view.optionalTypes[0]);
  EXPECT_EQ(0u, view.ruleMask & BooleanQuery::R1_REQUIRED_INLINE);
  EXPECT_EQ(0u, view.ruleMask & BooleanQuery::R2_DISJUNCTION_FLATTEN);
}

TEST_F(BooleanNormalizeTest, doubleNegationStaysOpaqueInProhibitedList) {
  TestIndex testIndex;
  const std::string_view bodies[] = {"a", "a b", "b", "c"};
  buildBodyIndex(testIndex, bodies);

  TermQuery a("body_w", "a");
  TermQuery b("body_w", "b");
  Query* innerProhibited[] = {&b};
  BooleanQuery inner({}, {}, innerProhibited, {});
  Query* outerProhibited[] = {&inner};
  BooleanQuery outerComplement({}, {}, outerProhibited, {});
  Query* mandatory[] = {&a, &outerComplement};
  BooleanQuery nested(mandatory, {}, {}, {});

  auto view = shape(nested);
  EXPECT_EQ(1, view.mandatoryCount);
  EXPECT_EQ(1, view.prohibitedCount);
  EXPECT_EQ(std::type_index(typeid(BooleanQuery)), view.prohibitedTypes[0]);
  EXPECT_EQ(BooleanQuery::R1_REQUIRED_INLINE,
            view.ruleMask & BooleanQuery::R1_REQUIRED_INLINE);

  Query* flatMandatory[] = {&a};
  Query* flatFilter[] = {&b};
  BooleanQuery flat(flatMandatory, {}, {}, flatFilter);
  expectScoresNear(collectScores(*testIndex.reader, flat),
                   collectScores(*testIndex.reader, nested));
}

TEST_F(BooleanNormalizeTest, requiredEmptyChildStaysUnsatisfiable) {
  TestIndex testIndex;
  const std::string_view bodies[] = {"a", "b"};
  buildBodyIndex(testIndex, bodies);

  TermQuery a("body_w", "a");
  BooleanQuery empty({}, {}, {}, {});
  Query* mandatory[] = {&a, &empty};
  BooleanQuery outer(mandatory, {}, {}, {});

  auto view = shape(outer);
  EXPECT_EQ(2, view.mandatoryCount);
  EXPECT_EQ(std::type_index(typeid(BooleanQuery)), view.mandatoryTypes[1]);
  EXPECT_EQ(0u, view.ruleMask & BooleanQuery::R1_REQUIRED_INLINE);
  EXPECT_EQ(0, countHits(*testIndex.reader, outer));
}

TEST_F(BooleanNormalizeTest, requiredInlineMapsOccursAndRunsToFixpoint) {
  TermQuery root("body_w", "root");
  TermQuery a("body_w", "a");
  TermQuery f("body_w", "f");
  TermQuery p("body_w", "p");
  Query* innerMandatory[] = {&a};
  Query* innerFilter[] = {&f};
  Query* innerProhibited[] = {&p};
  BooleanQuery inner(innerMandatory, {}, innerProhibited, innerFilter);
  Query* middleMandatory[] = {&inner};
  BooleanQuery middle(middleMandatory, {}, {}, {});
  Query* outerMandatory[] = {&root, &middle};
  BooleanQuery outer(outerMandatory, {}, {}, {});

  auto view = shape(outer);
  EXPECT_EQ(2, view.mandatoryCount);
  EXPECT_EQ(0, view.optionalCount);
  EXPECT_EQ(1, view.prohibitedCount);
  EXPECT_EQ(1, view.filterCount);
  EXPECT_EQ(BooleanQuery::R1_REQUIRED_INLINE,
            view.ruleMask & BooleanQuery::R1_REQUIRED_INLINE);
  EXPECT_EQ(std::type_index(typeid(TermQuery)), view.mandatoryTypes[0]);
  EXPECT_EQ(std::type_index(typeid(TermQuery)), view.mandatoryTypes[1]);
}

TEST_F(BooleanNormalizeTest, requiredInlineGatesMandatoryOptionalsAndMinMatch) {
  TermQuery root("body_w", "root");
  TermQuery a("body_w", "a");
  TermQuery rank("body_w", "rank");
  Query* required[] = {&a};
  Query* optional[] = {&rank};
  BooleanQuery rankOnly(required, optional, {}, {}, 0);
  BooleanQuery constraining(required, optional, {}, {}, 1);

  Query* rankOuterClauses[] = {&root, &rankOnly};
  BooleanQuery rankOuter(rankOuterClauses, {}, {}, {});
  auto rankView = shape(rankOuter);
  EXPECT_EQ(2, rankView.mandatoryCount);
  EXPECT_EQ(0u, rankView.ruleMask & BooleanQuery::R1_REQUIRED_INLINE);
  EXPECT_EQ(std::type_index(typeid(BooleanQuery)), rankView.mandatoryTypes[1]);

  Query* constrainingOuterClauses[] = {&root, &constraining};
  BooleanQuery constrainingOuter(constrainingOuterClauses, {}, {}, {});
  auto constrainingView = shape(constrainingOuter);
  EXPECT_EQ(2, constrainingView.mandatoryCount);
  EXPECT_EQ(0u, constrainingView.ruleMask & BooleanQuery::R1_REQUIRED_INLINE);
}

TEST_F(BooleanNormalizeTest, filterInlineDropsRankOnlyOptionalsButKeepsMinMatchOpaque) {
  TermQuery a("body_w", "a");
  TermQuery f("body_w", "f");
  TermQuery p("body_w", "p");
  TermQuery rank("body_w", "rank");
  Query* mandatory[] = {&a};
  Query* filter[] = {&f};
  Query* prohibited[] = {&p};
  Query* optional[] = {&rank};
  BooleanQuery rankOnly(mandatory, optional, prohibited, filter, 0);
  Query* outerFilter[] = {&rankOnly};
  BooleanQuery outer({}, {}, {}, outerFilter);

  auto view = shape(outer);
  EXPECT_EQ(0, view.mandatoryCount);
  EXPECT_EQ(0, view.optionalCount);
  EXPECT_EQ(1, view.prohibitedCount);
  EXPECT_EQ(2, view.filterCount);
  EXPECT_EQ(BooleanQuery::R1_REQUIRED_INLINE,
            view.ruleMask & BooleanQuery::R1_REQUIRED_INLINE);

  BooleanQuery minMatch(mandatory, optional, prohibited, filter, 1);
  Query* opaqueFilter[] = {&minMatch};
  BooleanQuery opaque({}, {}, {}, opaqueFilter);
  auto opaqueView = shape(opaque);
  EXPECT_EQ(1, opaqueView.filterCount);
  EXPECT_EQ(std::type_index(typeid(BooleanQuery)), opaqueView.filterTypes[0]);
  EXPECT_EQ(0u, opaqueView.ruleMask & BooleanQuery::R1_REQUIRED_INLINE);
}

TEST_F(BooleanNormalizeTest, disjunctionFlattenHonorsMinMatchAndEmptyGate) {
  TermQuery a("body_w", "a");
  TermQuery b("body_w", "b");
  TermQuery c("body_w", "c");
  Query* innerOptional[] = {&a, &b};
  BooleanQuery inner({}, innerOptional, {}, {}, 1);
  Query* outerOptional[] = {&inner, &c};

  for (int outerMinMatch : {0, 1}) {
    BooleanQuery outer({}, outerOptional, {}, {}, outerMinMatch);
    auto view = shape(outer);
    EXPECT_EQ(3, view.optionalCount);
    EXPECT_EQ(BooleanQuery::R2_DISJUNCTION_FLATTEN,
              view.ruleMask & BooleanQuery::R2_DISJUNCTION_FLATTEN);
  }

  BooleanQuery constrained({}, outerOptional, {}, {}, 2);
  auto constrainedView = shape(constrained);
  EXPECT_EQ(2, constrainedView.optionalCount);
  EXPECT_EQ(0u, constrainedView.ruleMask & BooleanQuery::R2_DISJUNCTION_FLATTEN);

  BooleanQuery empty({}, {}, {}, {});
  Query* withEmpty[] = {&empty, &c};
  BooleanQuery emptyOuter({}, withEmpty, {}, {}, 1);
  auto emptyView = shape(emptyOuter);
  EXPECT_EQ(2, emptyView.optionalCount);
  EXPECT_EQ(std::type_index(typeid(BooleanQuery)), emptyView.optionalTypes[0]);
  EXPECT_EQ(0u, emptyView.ruleMask & BooleanQuery::R2_DISJUNCTION_FLATTEN);

  Query* emptyMandatory[] = {&empty};
  BooleanQuery emptyRequired(emptyMandatory, {}, {}, {});
  auto requiredView = shape(emptyRequired);
  EXPECT_EQ(1, requiredView.mandatoryCount);
  EXPECT_EQ(0, requiredView.optionalCount);
  EXPECT_EQ(0u,
            requiredView.ruleMask & BooleanQuery::R3_REQUIRED_DISJUNCTION_HOIST);
}

TEST_F(BooleanNormalizeTest, soleMandatoryDisjunctionHoistsAndRerunsFlatten) {
  TermQuery a("body_w", "a");
  TermQuery b("body_w", "b");
  TermQuery c("body_w", "c");
  TermQuery f("body_w", "f");
  TermQuery p("body_w", "p");
  Query* nestedOptional[] = {&a, &b};
  BooleanQuery nested({}, nestedOptional, {}, {});
  Query* innerOptional[] = {&nested, &c};
  Query* outerFilter[] = {&f};
  Query* outerProhibited[] = {&p};

  for (int innerMinMatch : {0, 1, 2}) {
    BooleanQuery inner({}, innerOptional, {}, {}, innerMinMatch);
    Query* outerMandatory[] = {&inner};
    BooleanQuery outer(outerMandatory, {}, outerProhibited, outerFilter);
    auto view = shape(outer);
    EXPECT_EQ(0, view.mandatoryCount);
    EXPECT_EQ(innerMinMatch > 1 ? 2 : 3, view.optionalCount);
    EXPECT_EQ(1, view.filterCount);
    EXPECT_EQ(1, view.prohibitedCount);
    EXPECT_EQ(std::max(1, innerMinMatch), view.minShouldMatch);
    EXPECT_EQ(BooleanQuery::R3_REQUIRED_DISJUNCTION_HOIST,
              view.ruleMask & BooleanQuery::R3_REQUIRED_DISJUNCTION_HOIST);
    EXPECT_EQ(innerMinMatch > 1 ? 0u : (uint32_t) BooleanQuery::R2_DISJUNCTION_FLATTEN,
              view.ruleMask & BooleanQuery::R2_DISJUNCTION_FLATTEN);
  }
}

TEST_F(BooleanNormalizeTest, r3DedupUsesNormalizedOptionalCardinality) {
  TestIndex testIndex;
  const std::string_view bodies[] = {
    "f a", "f a b", "f b c", "a b c", "f d"};
  buildBodyIndex(testIndex, bodies);

  TermQuery a1("body_w", "a");
  TermQuery a2("body_w", "a");
  TermQuery b("body_w", "b");
  TermQuery c("body_w", "c");
  TermQuery d("body_w", "d");
  TermQuery f("body_w", "f");
  Query* outerFilter[] = {&f};

  Query* missOptional[] = {&a1, &a2, &b};
  BooleanQuery missInner({}, missOptional, {}, {}, 2);
  Query* missMandatory[] = {&missInner};
  BooleanQuery missOuter(missMandatory, {}, {}, outerFilter);
  EXPECT_EQ(3, countHits(*testIndex.reader, missOuter));

  Query* absoluteOptional[] = {&a1, &a2, &b, &c, &d};
  BooleanQuery absoluteInner({}, absoluteOptional, {}, {}, 2);
  Query* absoluteMandatory[] = {&absoluteInner};
  BooleanQuery absoluteOuter(absoluteMandatory, {}, {}, outerFilter);
  EXPECT_EQ(2, countHits(*testIndex.reader, absoluteOuter));

  Query* impossibleOptional[] = {&a1, &b};
  BooleanQuery impossible({}, impossibleOptional, {}, {}, 3);
  EXPECT_EQ(0, countHits(*testIndex.reader, impossible));
}

TEST_F(BooleanNormalizeTest, singleClauseUnwrapPreservesWeightTraitsAndCount) {
  TestIndex testIndex;
  const std::string_view bodies[] = {"a", "a b", "b"};
  buildBodyIndex(testIndex, bodies);

  TermQuery a("body_w", "a");
  ConstantScoreQuery constant(&a, 4.0f);
  ForcePrepareQuery forced(&a);
  BoostQuery boosted(&a, 3.0f);

  auto check = [&](Query* child, bool optional, int32_t flags, auto verify) {
    MemPool pool;
    Query::Context context(pool, *testIndex.reader);
    Query* clause[] = {child};
    BooleanQuery query(optional ? std::span<Query*>{} : std::span<Query*>(clause),
                       optional ? std::span<Query*>(clause) : std::span<Query*>{}, {}, {});
    verify(query.createWeight(context, flags), context);
  };

  for (bool optional : {false, true}) {
    check(&a, optional, 0, [](Query::Weight* weight, Query::Context& context) {
      ASSERT_NE(dynamic_cast<TermQuery::Weight*>(weight), nullptr);
      EXPECT_TRUE(weight->isConstantScoring());
      EXPECT_EQ(2, weight->count(context.topReader.segments()[0]));
    });
  }

  check(&constant, false, Query::NEED_SCORES,
        [](Query::Weight* weight, Query::Context&) {
          EXPECT_NE(dynamic_cast<ConstantScoreQuery::Weight*>(weight), nullptr);
          EXPECT_TRUE(weight->isConstantScoring());
        });
  check(&forced, false, Query::NEED_SCORES,
        [](Query::Weight* weight, Query::Context&) {
          EXPECT_NE(dynamic_cast<ForcePrepareQuery::Weight*>(weight), nullptr);
          EXPECT_TRUE(weight->needsPrepare());
        });
  check(&boosted, true, Query::NEED_SCORES,
        [](Query::Weight* weight, Query::Context&) {
          EXPECT_NE(dynamic_cast<TermQuery::Weight*>(weight), nullptr);
        });

  MemPool pool;
  Query::Context context(pool, *testIndex.reader);
  Query* filter[] = {&a};
  BooleanQuery filterOnly({}, {}, {}, filter);
  EXPECT_NE(dynamic_cast<BooleanQuery::Weight*>(
              filterOnly.createWeight(context, Query::NEED_SCORES)), nullptr);
}

TEST_F(BooleanNormalizeTest, noScorePlanDropsRankOnlyOptionalWithFilter) {
  TestIndex testIndex;
  const std::string_view bodies[] = {"a f", "f"};
  buildBodyIndex(testIndex, bodies);

  TermQuery a("body_w", "a");
  TermQuery f("body_w", "f");
  ForcePrepareQuery optional(&a);
  Query* optionalClauses[] = {&optional};
  Query* filterClauses[] = {&f};
  BooleanQuery query({}, optionalClauses, {}, filterClauses);

  MemPool noScorePool;
  Query::Context noScoreContext(noScorePool, *testIndex.reader);
  auto* noScoreWeight = query.createWeight(noScoreContext, 0);
  EXPECT_FALSE(noScoreWeight->needsPrepare());

  MemPool scorePool;
  Query::Context scoreContext(scorePool, *testIndex.reader);
  auto* scoreWeight = query.createWeight(scoreContext, Query::NEED_SCORES);
  EXPECT_TRUE(scoreWeight->needsPrepare());
}

TEST_F(BooleanNormalizeTest, filterHoistDoesNotScoreFormerMandatoryClauses) {
  TestIndex testIndex;
  const std::string_view bodies[] = {"root a", "root", "a"};
  buildBodyIndex(testIndex, bodies);

  TermQuery root("body_w", "root");
  TermQuery a("body_w", "a");
  Query* innerMandatory[] = {&a};
  BooleanQuery inner(innerMandatory, {}, {}, {});
  Query* outerMandatory[] = {&root};
  Query* outerFilter[] = {&inner};
  BooleanQuery nested(outerMandatory, {}, {}, outerFilter);

  TermQuery flatRoot("body_w", "root");
  TermQuery flatA("body_w", "a");
  Query* flatMandatory[] = {&flatRoot};
  Query* flatFilter[] = {&flatA};
  BooleanQuery flat(flatMandatory, {}, {}, flatFilter);
  expectScoresNear(collectScores(*testIndex.reader, flat),
                   collectScores(*testIndex.reader, nested));
}

TEST_F(BooleanNormalizeTest, boostTransparencyDistributesOnlyToScoringOccurs) {
  TestIndex testIndex;
  const std::string_view bodies[] = {
    "root a b f", "root a f", "root b f", "a b f", "root a b f p"};
  buildBodyIndex(testIndex, bodies);

  TermQuery root("body_w", "root");
  TermQuery a("body_w", "a");
  TermQuery b("body_w", "b");
  TermQuery f("body_w", "f");
  TermQuery p("body_w", "p");

  Query* requiredInnerClauses[] = {&a, &b};
  Query* requiredInnerFilter[] = {&f};
  Query* requiredInnerProhibited[] = {&p};
  BooleanQuery requiredInner(requiredInnerClauses, {}, requiredInnerProhibited,
                             requiredInnerFilter);
  BoostQuery requiredBoost1(&requiredInner, 2.0f);
  BoostQuery requiredBoost2(&requiredBoost1, 3.0f);
  Query* requiredOuter[] = {&root, &requiredBoost2};
  BooleanQuery requiredNested(requiredOuter, {}, {}, {});

  BoostQuery flatA(&a, 6.0f);
  BoostQuery flatB(&b, 6.0f);
  Query* flatRequired[] = {&root, &flatA, &flatB};
  BooleanQuery requiredFlat(flatRequired, {}, requiredInnerProhibited,
                            requiredInnerFilter);
  expectScoresNear(collectScores(*testIndex.reader, requiredFlat),
                   collectScores(*testIndex.reader, requiredNested));
  auto requiredView = shape(requiredNested);
  EXPECT_EQ(std::type_index(typeid(BoostQuery)), requiredView.mandatoryTypes[1]);
  EXPECT_EQ(std::type_index(typeid(BoostQuery)), requiredView.mandatoryTypes[2]);
  EXPECT_EQ(std::type_index(typeid(TermQuery)), requiredView.filterTypes[0]);
  EXPECT_EQ(std::type_index(typeid(TermQuery)), requiredView.prohibitedTypes[0]);

  Query* disjunctionClauses[] = {&a, &b};
  BooleanQuery disjunction({}, disjunctionClauses, {}, {});
  BoostQuery disjunctionBoost1(&disjunction, 2.0f);
  BoostQuery disjunctionBoost2(&disjunctionBoost1, 3.0f);
  Query* nestedOptional[] = {&disjunctionBoost2, &root};
  BooleanQuery r2Nested({}, nestedOptional, {}, {});
  Query* flatOptional[] = {&flatA, &flatB, &root};
  BooleanQuery r2Flat({}, flatOptional, {}, {});
  expectScoresNear(collectScores(*testIndex.reader, r2Flat),
                   collectScores(*testIndex.reader, r2Nested));

  Query* r3Mandatory[] = {&disjunctionBoost2};
  Query* r3Filter[] = {&f};
  BooleanQuery r3Nested(r3Mandatory, {}, {}, r3Filter);
  Query* r3FlatOptional[] = {&flatA, &flatB};
  BooleanQuery r3Flat({}, r3FlatOptional, {}, r3Filter, 1);
  expectScoresNear(collectScores(*testIndex.reader, r3Flat),
                   collectScores(*testIndex.reader, r3Nested));
}

TEST_F(BooleanNormalizeTest, constantScoreIsOpaqueWhenScoringAndTransparentInFilter) {
  TermQuery root("body_w", "root");
  TermQuery a("body_w", "a");
  TermQuery b("body_w", "b");
  Query* pureOptional[] = {&a, &b};
  BooleanQuery disjunction({}, pureOptional, {}, {});
  ConstantScoreQuery constantDisjunction(&disjunction, 5.0f);
  Query* outerOptional[] = {&constantDisjunction, &root};
  BooleanQuery scoring({}, outerOptional, {}, {});
  auto scoringView = shape(scoring);
  EXPECT_EQ(2, scoringView.optionalCount);
  EXPECT_EQ(std::type_index(typeid(ConstantScoreQuery)), scoringView.optionalTypes[0]);
  EXPECT_EQ(0u, scoringView.ruleMask & BooleanQuery::R2_DISJUNCTION_FLATTEN);

  Query* required[] = {&a};
  Query* rankOnly[] = {&b};
  BooleanQuery requiredWithRank(required, rankOnly, {}, {});
  ConstantScoreQuery constantRequired(&requiredWithRank, 7.0f);
  Query* mandatory[] = {&root};
  Query* filter[] = {&constantRequired};
  BooleanQuery filtered(mandatory, {}, {}, filter);
  auto filteredView = shape(filtered);
  EXPECT_EQ(1, filteredView.mandatoryCount);
  EXPECT_EQ(1, filteredView.filterCount);
  EXPECT_EQ(std::type_index(typeid(TermQuery)), filteredView.filterTypes[0]);
  EXPECT_EQ(BooleanQuery::R1_REQUIRED_INLINE,
            filteredView.ruleMask & BooleanQuery::R1_REQUIRED_INLINE);
}
