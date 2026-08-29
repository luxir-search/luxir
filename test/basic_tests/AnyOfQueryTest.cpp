#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

#include "luxir/api/build.h"
#include "luxir/query/ConstantScoreQuery.h"
#include "luxir/query/MatchNoDocsQuery.h"
#include "luxir/query/QueryBuilder.h"
#include "luxir/reader/SkipStats.h"
#include "luxir/query/TermQuery.h"
#include "test/CollectionHelper.h"
#include "test/SchemaBuilder.h"
#include "test/LocalReq.h"
#include "test/QueryBuild.h"
#include "test/LuxirTest.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

namespace {

namespace api = luxir::api;

api::Val strVal(std::string_view value) {
  api::Val val;
  val.kind = value;
  return val;
}

api::Val intVal(int64_t value) {
  api::Val val;
  val.kind = value;
  return val;
}

api::Query anyOfQuery(std::pmr::memory_resource& mr, std::string_view field,
                      std::initializer_list<api::Val> values) {
  api::Query query;
  auto& anyOf = query.kind.emplace<api::AnyOfQuery>();
  anyOf.field = api::build::arenaStr(mr, field);
  auto* sequence = api::build::allocMessage<api::Val>(mr);
  auto& mixed = sequence->kind.emplace<api::ArrVal>();
  api::Val* stored = api::build::allocArray(mixed.v, values.size(), mr);
  std::copy(values.begin(), values.end(), stored);
  anyOf.values = sequence;
  return query;
}

std::vector<std::string> resultIds(const LocalReq& req) {
  std::vector<std::string> ids;
  for (const Doc& doc : req.getDocs()) {
    const FieldVal* id = find(doc, "id");
    if (id != nullptr) ids.push_back(std::get<std::string>(*id));
  }
  std::sort(ids.begin(), ids.end());
  return ids;
}

} // namespace

class AnyOfQueryTest : public LuxirTest {
public:
  CollectionHelper helper;

  void SetUp() override {
    std::vector<Doc> docs;
    docs.push_back(flatdoc(
        "id", "a", "brand_s", "acme", "body_un", "red MixedCase",
        "nums_is", vec_i(1, 3), "when_dt", "2024-01-01",
        "col_sc", "raw-a"));
    docs.push_back(flatdoc(
        "id", "b", "brand_s", "globex", "body_un", "red lower",
        "nums_is", vec_i(2), "when_dt", "2024-01-15",
        "col_sc", "raw-b"));
    docs.push_back(flatdoc(
        "id", "c", "brand_s", "initech", "body_un", "blue mixedcase",
        "nums_is", vec_i(3, 4), "when_dt", "2024-02-01",
        "col_sc", "raw-c"));
    ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
  }
};

TEST(AnyOfQueryBuilderTest, canonicalTermShapes) {
  auto schema = Schema::createDefaultSchema();
  MemPool pool;
  QueryBuilder builder(pool, *schema, CoerceContext{});

  std::array<std::string_view, 1> one{"MixedCase"};
  api::Val oneValue;
  oneValue.kind.emplace<api::ArrStr>().v = one;
  CanonicalValueSet oneCanonical = builder.canonicalizeFieldValues(
      "body_un", ValueSequence(oneValue, "AnyOfQuery values"));
  auto* singleton = dynamic_cast<ConstantScoreQuery*>(
      builder.createAnyOfQuery(oneCanonical));
  ASSERT_NE(nullptr, singleton);
  auto* term = dynamic_cast<TermQuery*>(singleton->getChild());
  ASSERT_NE(nullptr, term);
  EXPECT_EQ("mixedcase", term->getTerm());

  std::array<std::string_view, 2> two{"Beta", "Alpha"};
  api::Val twoValues;
  twoValues.kind.emplace<api::ArrStr>().v = two;
  CanonicalValueSet twoCanonical = builder.canonicalizeFieldValues(
      "body_un", ValueSequence(twoValues, "AnyOfQuery values"));
  ASSERT_EQ(2u, twoCanonical.size());
  for (size_t i = 0; i < twoCanonical.size(); i++) {
    auto* clause = dynamic_cast<ConstantScoreQuery*>(
        builder.createAnyOfQuery(twoCanonical.element(i)));
    ASSERT_NE(nullptr, clause);
    EXPECT_NE(nullptr, dynamic_cast<TermQuery*>(clause->getChild()));
  }

  api::Val emptyValue;
  emptyValue.kind.emplace<api::ArrStr>();
  CanonicalValueSet emptyCanonical = builder.canonicalizeFieldValues(
      "body_un", ValueSequence(emptyValue, "AnyOfQuery values"));
  EXPECT_NE(nullptr, dynamic_cast<MatchNoDocsQuery*>(
      builder.createAnyOfQuery(emptyCanonical)));
}

TEST(AnyOfQueryBuilderTest, numericIntervalsCoalesceAndValueCountIsLimited) {
  auto schema = Schema::createDefaultSchema();
  MemPool pool;
  QueryBuilder builder(pool, *schema, CoerceContext{});

  std::array<int64_t, 4> input{90, 45, 44, 46};
  api::Val value;
  value.kind.emplace<api::ArrInt>().v = input;
  CanonicalValueSet canonical = builder.canonicalizeFieldValues(
      "num_i", ValueSequence(value, "AnyOfQuery values"));
  auto* predicate = dynamic_cast<NumericPredicateQuery*>(
      builder.createAnyOfQuery(canonical));
  ASSERT_NE(nullptr, predicate);
  ASSERT_EQ(2, predicate->intervals().size());
  EXPECT_EQ(44, predicate->intervals()[0].lo);
  EXPECT_EQ(46, predicate->intervals()[0].hi);
  EXPECT_EQ(90, predicate->intervals()[1].lo);
  EXPECT_EQ(90, predicate->intervals()[1].hi);

  std::vector<int64_t> tooMany(ValueSequence::MAX_VALUES + 1);
  api::Val oversized;
  oversized.kind.emplace<api::ArrInt>().v = tooMany;
  EXPECT_THROW(builder.canonicalizeFieldValues(
      "num_i", ValueSequence(oversized, "AnyOfQuery values")),
      std::runtime_error);
}

TEST_F(AnyOfQueryTest, indexedTermsSingleParityAndUnion) {
  auto single = localReq(helper.getSearchEngine());
  auto& singleTop = single->collection("main").topDocs("q");
  singleTop.rawQuery() = anyOfQuery(singleTop.mr(), "brand_s", {strVal("acme")});
  singleTop.fields({"id"}).limit(-1);
  single->execute();
  ASSERT_TRUE(single->ok()) << single->errorMsg();

  auto match = localReq(helper.getSearchEngine());
  match->collection("main").topDocs("q").matchQuery("brand_s", "acme")
      .fields({"id"}).limit(-1);
  match->execute();
  ASSERT_TRUE(match->ok()) << match->errorMsg();
  EXPECT_EQ(resultIds(*match), resultIds(*single));

  auto multi = localReq(helper.getSearchEngine());
  auto& multiTop = multi->collection("main").topDocs("q");
  multiTop.rawQuery() = anyOfQuery(
      multiTop.mr(), "brand_s", {strVal("globex"), strVal("missing"),
                                  strVal("acme")});
  multiTop.fields({"id"}).limit(-1);
  multi->execute();
  ASSERT_TRUE(multi->ok()) << multi->errorMsg();
  EXPECT_EQ((std::vector<std::string>{"a", "b"}), resultIds(*multi));
}

TEST_F(AnyOfQueryTest, textFoldsButStringAndIdRemainVerbatim) {
  auto run = [&](std::string_view field, std::string_view value) {
    auto req = localReq(helper.getSearchEngine());
    auto& top = req->collection("main").topDocs("q");
    top.rawQuery() = anyOfQuery(top.mr(), field, {strVal(value)});
    top.fields({"id"}).limit(-1);
    req->execute();
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    return resultIds(*req);
  };

  EXPECT_EQ((std::vector<std::string>{"a", "c"}),
            run("body_un", "MixedCase"));
  EXPECT_TRUE(run("brand_s", "ACME").empty());
  EXPECT_TRUE(run("id", "A").empty());
}

TEST_F(AnyOfQueryTest, numericMultiValueMatchesOnceAndScoresConstant) {
  auto req = localReq(helper.getSearchEngine());
  auto& top = req->collection("main").topDocs("q");
  top.rawQuery() = anyOfQuery(top.mr(), "nums_is", {intVal(3), intVal(1)});
  top.withStats().fields({"id"}).limit(-1);
  req->execute();
  ASSERT_TRUE(req->ok()) << req->errorMsg();
  EXPECT_EQ((std::vector<std::string>{"a", "c"}), resultIds(*req));

  const auto& scores = std::get<api::ColFloat>(
      req->docList()->columns.at("_score_").kind).v;
  ASSERT_EQ(2u, scores.size());
  EXPECT_FLOAT_EQ(scores[0], scores[1]);
}

TEST_F(AnyOfQueryTest, dateIsOneIngestInstantNotAPartialRange) {
  auto req = localReq(helper.getSearchEngine());
  auto& top = req->collection("main").topDocs("q");
  top.rawQuery() = anyOfQuery(top.mr(), "when_dt", {strVal("2024-01")});
  top.fields({"id"}).limit(-1);
  req->execute();
  ASSERT_TRUE(req->ok()) << req->errorMsg();
  EXPECT_EQ((std::vector<std::string>{"a"}), resultIds(*req));
}

TEST_F(AnyOfQueryTest, rawStringColumnIsRejected) {
  auto req = localReq(helper.getSearchEngine());
  auto& top = req->collection("main").topDocs("q");
  top.rawQuery() = anyOfQuery(
      top.mr(), "col_sc", {strVal("raw-c"), strVal("raw-a")});
  top.fields({"id"}).limit(-1);
  req->execute();
  EXPECT_FALSE(req->ok());
  EXPECT_NE(std::string::npos,
            req->errorMsg().find("does not support exact equality"));
}

TEST_F(AnyOfQueryTest, topDocsFilterAndBooleanClauseCompose) {
  auto filterReq = localReq(helper.getSearchEngine());
  auto& filterTop = filterReq->collection("main").topDocs("q");
  api::Query filter = anyOfQuery(
      filterTop.mr(), "brand_s", {strVal("acme"), strVal("globex")});
  filterTop.allQuery().filter(filter).fields({"id"}).limit(-1);
  filterReq->execute();
  ASSERT_TRUE(filterReq->ok()) << filterReq->errorMsg();
  EXPECT_EQ((std::vector<std::string>{"a", "b"}), resultIds(*filterReq));

  auto booleanReq = localReq(helper.getSearchEngine());
  auto& booleanTop = booleanReq->collection("main").topDocs("q");
  api::Query selected = anyOfQuery(
      booleanTop.mr(), "brand_s", {strVal("acme"), strVal("initech")});
  api::Query red = qb::match(booleanTop.mr(), "body_un", "red");
  booleanTop.rawQuery() = qb::boolean(booleanTop.mr(), {red, selected});
  booleanTop.fields({"id"}).limit(-1);
  booleanReq->execute();
  ASSERT_TRUE(booleanReq->ok()) << booleanReq->errorMsg();
  EXPECT_EQ((std::vector<std::string>{"a"}), resultIds(*booleanReq));
}

TEST_F(AnyOfQueryTest, emptyAndDuplicatesCanonicalize) {
  auto empty = localReq(helper.getSearchEngine());
  auto& emptyTop = empty->collection("main").topDocs("q");
  emptyTop.rawQuery() = anyOfQuery(emptyTop.mr(), "brand_s", {});
  emptyTop.fields({"id"}).limit(-1);
  empty->execute();
  ASSERT_TRUE(empty->ok()) << empty->errorMsg();
  EXPECT_TRUE(resultIds(*empty).empty());

  auto duplicates = localReq(helper.getSearchEngine());
  auto& duplicateTop = duplicates->collection("main").topDocs("q");
  duplicateTop.rawQuery() = anyOfQuery(
      duplicateTop.mr(), "nums_is", {intVal(1), strVal("1")});
  duplicateTop.fields({"id"}).limit(-1);
  duplicates->execute();
  ASSERT_TRUE(duplicates->ok()) << duplicates->errorMsg();
  EXPECT_EQ((std::vector<std::string>{"a"}), resultIds(*duplicates));
}

TEST_F(AnyOfQueryTest, invalidValueKindsAreParseTimeErrors) {
  auto expectError = [&](auto&& build, std::string_view detail) {
    auto req = localReq(helper.getSearchEngine());
    auto& top = req->collection("main").topDocs("q");
    top.rawQuery() = build(top.mr());
    req->execute();
    EXPECT_FALSE(req->ok());
    EXPECT_NE(std::string::npos, req->errorMsg().find(detail))
        << req->errorMsg();
  };

  expectError([](std::pmr::memory_resource& mr) {
    return anyOfQuery(mr, "emb_v", {intVal(1)});
  }, "emb_v");

  expectError([](std::pmr::memory_resource& mr) {
    api::Val null;
    null.kind = google::protobuf::NullValue::NULL_VALUE;
    return anyOfQuery(mr, "brand_s", {null});
  }, "must be a scalar field value, not null");

  expectError([](std::pmr::memory_resource& mr) {
    api::Val nested;
    nested.kind.emplace<api::ArrInt>();
    return anyOfQuery(mr, "brand_s", {nested});
  }, "must be a scalar field value, not a nested array");
}

// A points leaf holds a contiguous run of values, and fenceRange() resolves an
// interval to whole leaves. For a single range every interior leaf is entirely
// inside the bounds, so leaf-granular inclusion is safe. A value SET has gaps:
// two far-apart values can fence to the same leaf, and unioning at leaf
// granularity would admit every value between them. These pin the per-value
// filtering that prevents that, on both the points and column arms.
class AnyOfLeafStraddleTest : public LuxirTest {
public:
  CollectionHelper helper;

  struct Arms {
    int64_t sparse = 0;
    int64_t complement = 0;
    int64_t points = 0;
    int64_t zone = 0;
  };
  Arms arms;

  void indexValues(bool points, const std::vector<int64_t>& values) {
    SchemaBuilder b;
    auto& f = b.field("pt");
    f.type = api::FieldDef::FieldClass::INT;
    f.index = points ? api::FieldDef::IndexMode::RANGE
                     : api::FieldDef::IndexMode::NONE;
    b.set(helper.collection());

    std::vector<Doc> docs;
    for (int64_t value : values) {
      docs.push_back(flatdoc("id", "v" + std::to_string(value), "pt", value));
    }
    ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
  }

  std::vector<std::string> runAnyOf(std::initializer_list<int64_t> selected) {
    auto req = localReq(helper.getSearchEngine());
    auto& top = req->collection("main").topDocs("q");
    std::vector<api::Val> vals;
    for (int64_t value : selected) vals.push_back(intVal(value));
    api::Query query;
    auto& anyOf = query.kind.emplace<api::AnyOfQuery>();
    anyOf.field = api::build::arenaStr(top.mr(), "pt");
    auto* sequence = api::build::allocMessage<api::Val>(top.mr());
    auto& mixed = sequence->kind.emplace<api::ArrVal>();
    api::Val* stored = api::build::allocArray(mixed.v, vals.size(), top.mr());
    std::copy(vals.begin(), vals.end(), stored);
    anyOf.values = sequence;
    top.rawQuery() = query;
    top.fields({"id"}).limit(-1);
    SkipStats::reset();
    SkipStats::enabled = true;
    req->execute();
    SkipStats::enabled = false;
    arms = {SkipStats::numericPredicateSparseVerifyArms,
            SkipStats::numericPredicateComplementArms,
            SkipStats::numericPredicatePointsArms,
            SkipStats::numericPredicateZoneArms};
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    return resultIds(*req);
  }
};

// Few enough docs that every value shares one points leaf, so both selected
// values fence to that leaf with the unselected ones sitting between them.
TEST_F(AnyOfLeafStraddleTest, pointsGapValuesWithinOneLeafDoNotMatch) {
  indexValues(true, {10, 11, 500, 999, 1000, 1001, 5000});
  EXPECT_EQ((std::vector<std::string>{"v10", "v1000"}), runAnyOf({10, 1000}));
  EXPECT_GT(arms.points, 0) << "expected the points arm";
  EXPECT_EQ((std::vector<std::string>{"v10", "v11", "v5000"}),
            runAnyOf({10, 11, 5000}));
  EXPECT_EQ((std::vector<std::string>{}), runAnyOf({12, 998}));
}

TEST_F(AnyOfLeafStraddleTest, pointsGapValuesAcrossManyLeavesDoNotMatch) {
  std::vector<int64_t> values;
  for (int64_t i = 0; i < 4000; i += 2) values.push_back(i);
  indexValues(true, values);

  EXPECT_EQ((std::vector<std::string>{"v0", "v2000", "v3998"}),
            runAnyOf({0, 2000, 3998}));
  EXPECT_GT(arms.points, 0) << "expected the points arm";
  // Odd values were never indexed: every selected value falls in a gap.
  EXPECT_EQ((std::vector<std::string>{}), runAnyOf({1, 1999, 3997}));
  // Adjacent indexed values coalesce into one interval; the absent odd value
  // between them must not widen it into a range that admits anything else.
  EXPECT_EQ((std::vector<std::string>{"v1000", "v1002"}),
            runAnyOf({1000, 1001, 1002}));
}

// The same gaps must be respected when the field has no points index and the
// query falls to the doc-order column arms.
TEST_F(AnyOfLeafStraddleTest, columnOnlyGapValuesDoNotMatch) {
  indexValues(false, {10, 11, 500, 999, 1000, 1001, 5000});
  EXPECT_EQ(0, arms.points);
  EXPECT_EQ((std::vector<std::string>{"v10", "v1000"}), runAnyOf({10, 1000}));
  EXPECT_EQ((std::vector<std::string>{"v10", "v11", "v5000"}),
            runAnyOf({10, 11, 5000}));
  EXPECT_EQ((std::vector<std::string>{}), runAnyOf({12, 998}));
}
