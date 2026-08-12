#include <gtest/gtest.h>

#include <algorithm>
#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

#include "solux/api/build.h"
#include "solux/query/AllQuery.h"
#include "solux/query/BoostQuery.h"
#include "solux/query/ExistsQuery.h"
#include "solux/reader/FieldReader.h"
#include "solux/reader/TermsEnum.h"
#include "solux/schema/Schema.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/QueryBuild.h"
#include "test/SchemaBuilder.h"
#include "test/SoluxTest.h"
#include "test/TestUtils.h"

using namespace solux;
using namespace solux::test;

namespace {

using FieldClass = api::FieldDef::FieldClass;
using IndexMode = api::FieldDef::IndexMode;

void installExistsSchema(CollectionHelper& helper, SchemaBuilder& b) {
  auto& multi = b.field("multi_w");
  multi.type = FieldClass::TEXT;
  multi.index = IndexMode::MATCH;
  multi.multi = true;
  multi.column = false;
  b.analyzer(multi, "whitespace");

  auto& geo = b.field("geo_p");
  geo.type = FieldClass::GEO_POINT;
  geo.index = IndexMode::RANGE;
  geo.column = true;

  auto& vec = b.field("vec_v");
  vec.type = FieldClass::VECTOR;
  vec.index = IndexMode::NONE;
  vec.column = true;
  vec.metric = api::FieldDef::Metric::COSINE;

  auto& storedOnly = b.field("stored_only");
  storedOnly.type = FieldClass::STRING;
  storedOnly.index = IndexMode::NONE;
  storedOnly.column = false;
  storedOnly.stored = true;

  b.set(helper.collection());
}

std::vector<std::string> resultIds(const LocalReq& req) {
  std::vector<std::string> ids;
  for (const Doc& doc : req.getDocs()) {
    const FieldVal* id = solux::test::find(doc, "id");
    if (id != nullptr) ids.push_back(std::get<std::string>(*id));
  }
  std::sort(ids.begin(), ids.end());
  return ids;
}

std::vector<std::string> expectedIds(
    std::initializer_list<std::string_view> values) {
  std::vector<std::string> ids;
  for (std::string_view value : values) ids.emplace_back(value);
  std::sort(ids.begin(), ids.end());
  return ids;
}

SegFieldInfo readFieldInfo(IndexReader::Segment& segment,
                           std::string_view field) {
  MemPool pool;
  FieldReader reader(segment.postingsReader());
  if (!reader.seek(field)) throw std::runtime_error("missing test field");
  SegFieldInfo info{};
  reader.readFieldInfo(info);
  return info;
}

} // namespace

class ExistsQueryTest : public SoluxTest {
public:
  CollectionHelper helper;
  SchemaBuilder schemaBuilder;  // the installed SchemaDef views its storage

  void SetUp() override {
    installExistsSchema(helper, schemaBuilder);
    std::vector<Doc> docs;
    docs.push_back(flatdoc(
        "id", "d0", "dense_w", "", "body_w", "alpha", "multi_w", vecs("x", "y"),
        "tag_s", "", "col_sc", "", "count_i", (int64_t)1,
        "when_dt", "2024-01-01", "nums_is", vec_i(1, 2),
        "tags_ssc", vecs("a"), "geo_p", std::vector<double>{10.0, 20.0},
        "vec_v", std::vector<float>{1.0f, 0.0f}));
    docs.push_back(flatdoc(
        "id", "d1", "dense_w", " ", "body_w", " ", "multi_w", vecs("")));
    docs.push_back(flatdoc("id", "d2", "dense_w", "dense"));
    docs.push_back(flatdoc(
        "id", "d3", "dense_w", "", "multi_w", std::vector<std::string>{},
        "nums_is", std::vector<int64_t>{}, "tags_ssc", std::vector<std::string>{}));
    docs.push_back(flatdoc(
        "id", "d4", "dense_w", "", "vec_v", std::vector<float>{0.0f, 0.0f}));
    ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
  }

  std::vector<std::string> exprIds(std::string_view query) {
    auto req = localReq(helper.getSearchEngine());
    req->collection("main").topDocs("q").exprQuery(query).fields({"id"}).limit(-1);
    req->execute();
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    return resultIds(*req);
  }

  std::vector<std::string> structuredIds(std::string_view field) {
    auto req = localReq(helper.getSearchEngine());
    req->collection("main").topDocs("q").existsQuery(field).fields({"id"}).limit(-1);
    req->execute();
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    return resultIds(*req);
  }

  std::string exprError(std::string_view query) {
    auto req = localReq(helper.getSearchEngine());
    req->collection("main").topDocs("q").exprQuery(query).fields({"id"}).limit(-1);
    req->execute();
    EXPECT_FALSE(req->ok());
    return req->errorMsg();
  }

  std::string structuredError(std::string_view field) {
    auto req = localReq(helper.getSearchEngine());
    req->collection("main").topDocs("q").existsQuery(field).fields({"id"}).limit(-1);
    req->execute();
    EXPECT_FALSE(req->ok());
    return req->errorMsg();
  }
};

TEST_F(ExistsQueryTest, ValueSuppliedSemanticsAcrossFieldTypes) {
  EXPECT_EQ(expectedIds({"d0", "d1", "d2", "d3", "d4"}), exprIds("dense_w:*"));
  EXPECT_EQ(expectedIds({"d0", "d1"}), exprIds("body_w:*"));
  EXPECT_EQ(expectedIds({"d0", "d1"}), exprIds("multi_w:*"));
  EXPECT_EQ(expectedIds({"d0"}), exprIds("tag_s:*"));
  EXPECT_EQ(expectedIds({"d0"}), exprIds("col_sc:*"));
  EXPECT_EQ(expectedIds({"d0"}), exprIds("count_i:*"));
  EXPECT_EQ(expectedIds({"d0"}), exprIds("when_dt:*"));
  EXPECT_EQ(expectedIds({"d0"}), exprIds("nums_is:*"));
  EXPECT_EQ(expectedIds({"d0"}), exprIds("tags_ssc:*"));
  EXPECT_EQ(expectedIds({"d0"}), exprIds("geo_p:*"));
  EXPECT_EQ(expectedIds({"d0"}), exprIds("vec_v:*"));
  EXPECT_TRUE(exprIds("schema_only_i:*").empty());
}

TEST_F(ExistsQueryTest, AllPublicSurfacesLowerToExists) {
  const auto expected = expectedIds({"d0", "d1"});
  EXPECT_EQ(expected, exprIds("body_w:*"));
  EXPECT_EQ(expected, exprIds("exists(body_w)"));
  EXPECT_EQ(expected, structuredIds("body_w"));

  auto json = localReq(helper.getSearchEngine());
  auto& jsonCursor = json->collection("main").topDocs("q");
  std::string error;
  ASSERT_TRUE(api::read_json(jsonCursor.rawQuery(),
                             R"({"exists":{"field":"body_w"}})",
                             jsonCursor.mr(), &error)) << error;
  jsonCursor.fields({"id"}).limit(-1);
  json->execute();
  ASSERT_TRUE(json->ok()) << json->errorMsg();
  EXPECT_EQ(expected, resultIds(*json));

  auto simple = localReq(helper.getSearchEngine());
  simple->collection("main").topDocs("q")
      .simpleQuery("body_w:*", {"dense_w"}).fields({"id"}).limit(-1);
  simple->execute();
  ASSERT_TRUE(simple->ok()) << simple->errorMsg();
  EXPECT_EQ(expected, resultIds(*simple));
}

TEST_F(ExistsQueryTest, BooleanCompositionAndScoring) {
  EXPECT_EQ(expectedIds({"d2", "d3", "d4"}), exprIds("NOT body_w:*"));
  EXPECT_EQ(expectedIds({"d0"}),
            exprIds("(body_w:* AND dense_w:*) AND tag_s:*"));
  EXPECT_EQ(expectedIds({"d0", "d1"}),
            exprIds("boolean(required=[all()], filter=[body_w:*])"));

  auto checkScores = [&](std::string_view query, float expected) {
    auto req = localReq(helper.getSearchEngine());
    req->collection("main").topDocs("q").exprQuery(query)
        .withStats().fields({"id"}).limit(-1);
    req->execute();
    ASSERT_TRUE(req->ok()) << req->errorMsg();
    const auto* docs = req->docList();
    ASSERT_NE(nullptr, docs);
    const auto& scores =
        std::get<api::ColFloat>(docs->columns.at("_score_").kind).v;
    ASSERT_EQ(2u, scores.size());
    for (float score : scores) EXPECT_FLOAT_EQ(expected, score);
  };
  checkScores("body_w:*", 1.0f);
  checkScores("body_w:*^=2", 2.0f);
}

TEST_F(ExistsQueryTest, PositionalBoostAndConstantScoreSemantics) {
  auto responseScores = [](const LocalReq& req) {
    const auto* docs = req.docList();
    EXPECT_NE(nullptr, docs);
    if (docs == nullptr) return std::vector<float>{};
    const auto& values =
        std::get<api::ColFloat>(docs->columns.at("_score_").kind).v;
    return std::vector<float>(values.begin(), values.end());
  };
  auto exprScores = [&](std::string_view query) {
    auto req = localReq(helper.getSearchEngine());
    req->collection("main").topDocs("q").exprQuery(query)
        .withStats().fields({"id"}).limit(-1);
    req->execute();
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    return responseScores(*req);
  };
  auto simpleScores = [&](std::string_view query) {
    auto req = localReq(helper.getSearchEngine());
    req->collection("main").topDocs("q").simpleQuery(query, {"body_w"})
        .withStats().fields({"id"}).limit(-1);
    req->execute();
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    return responseScores(*req);
  };
  auto expectUniform = [](std::vector<float> scores, size_t count, float score) {
    ASSERT_EQ(count, scores.size());
    for (float actual : scores) EXPECT_FLOAT_EQ(score, actual);
  };

  expectUniform(exprScores("body_w:*"), 2, 1.0f);
  expectUniform(exprScores("+body_w:*"), 2, 0.0f);
  expectUniform(simpleScores("+body_w:*"), 2, 0.0f);
  expectUniform(exprScores("+body_w:*^1"), 2, 0.0f);
  expectUniform(exprScores("+body_w:*^3"), 2, 0.0f);
  expectUniform(exprScores("body_w:*^3"), 2, 3.0f);
  expectUniform(exprScores("+body_w:*^=3"), 2, 3.0f);
  expectUniform(exprScores("NOT body_w:*"), 3, 0.0f);

  auto nested = exprScores("+(body_w:* dense_w:*)");
  std::sort(nested.begin(), nested.end());
  EXPECT_EQ((std::vector<float>{1.0f, 1.0f, 1.0f, 2.0f, 2.0f}), nested);

  auto baseline = exprScores("body_w:alpha^3");
  auto distributed = exprScores("(+body_w:alpha +dense_w:*)^3");
  ASSERT_EQ(1u, baseline.size());
  ASSERT_EQ(baseline.size(), distributed.size());
  EXPECT_FLOAT_EQ(baseline[0], distributed[0]);

  auto structured = localReq(helper.getSearchEngine());
  auto& structuredCursor = structured->collection("main").topDocs("q");
  structuredCursor.rawQuery() = qb::boost(
      structuredCursor.mr(), qb::exists(structuredCursor.mr(), "body_w"), 1.0f);
  structuredCursor.withStats().fields({"id"}).limit(-1);
  structured->execute();
  ASSERT_TRUE(structured->ok()) << structured->errorMsg();
  expectUniform(responseScores(*structured), 2, 1.0f);

  auto json = localReq(helper.getSearchEngine());
  auto& jsonCursor = json->collection("main").topDocs("q");
  std::string error;
  ASSERT_TRUE(api::read_json(
      jsonCursor.rawQuery(),
      R"({"boost":{"query":{"exists":{"field":"body_w"}},"boost":1}})",
      jsonCursor.mr(), &error)) << error;
  jsonCursor.withStats().fields({"id"}).limit(-1);
  json->execute();
  ASSERT_TRUE(json->ok()) << json->errorMsg();
  expectUniform(responseScores(*json), 2, 1.0f);
}

TEST_F(ExistsQueryTest, SupplierCostIterationCountAndDeletes) {
  auto reader = helper.getIndexWriter()->getIndexReader();
  ASSERT_EQ(1u, reader->segments().size());
  auto& segment = reader->segments()[0];

  MemPool pool;
  Query::Context context(pool, *reader);
  ExistsQuery sparseQuery("body_w");
  auto* sparseWeight = sparseQuery.createWeight(context, 0);
  auto* sparseSupplier = sparseWeight->scorerSupplier(pool, segment);
  ASSERT_NE(nullptr, sparseSupplier);
  EXPECT_EQ(2, sparseSupplier->cost());
  EXPECT_EQ(2, sparseWeight->count(segment));
  auto* sparseScorer = sparseSupplier->get(pool, segment.maxDoc());
  ASSERT_NE(nullptr, sparseScorer);
  EXPECT_EQ(0, sparseScorer->next());
  EXPECT_EQ(1, sparseScorer->advance(1));
  EXPECT_EQ(PostingsReader::END, sparseScorer->next());

  ExistsQuery denseQuery("dense_w");
  auto* denseWeight = denseQuery.createWeight(context, 0);
  auto* denseSupplier = denseWeight->scorerSupplier(pool, segment);
  ASSERT_NE(nullptr, denseSupplier);
  EXPECT_EQ(5, denseSupplier->cost());
  EXPECT_NE(nullptr, dynamic_cast<AllQuery::Scorer*>(
                         denseSupplier->get(pool, segment.maxDoc())));

  MemPool scorePool;
  Query::Context scoreContext(scorePool, *reader);
  BoostQuery boostedSparse(&sparseQuery, 2.0f);
  auto* scoredSparse = boostedSparse.createWeight(
      scoreContext, Query::NEED_SCORES)->createScorer(scorePool, segment);
  ASSERT_NE(nullptr, scoredSparse);
  EXPECT_FLOAT_EQ(2.0f,
                  scoredSparse->getMaxScoreForSetup(PostingsReader::END));
  scoredSparse->setMinCompetitiveScore(2.0f);
  ASSERT_EQ(0, scoredSparse->next());
  EXPECT_FLOAT_EQ(2.0f, scoredSparse->score());

  ExistsQuery scoredDenseQuery("dense_w");
  BoostQuery boostedDense(&scoredDenseQuery, 3.0f);
  auto* scoredDense = boostedDense.createWeight(
      scoreContext, Query::NEED_SCORES)->createScorer(scorePool, segment);
  ASSERT_NE(nullptr, dynamic_cast<AllQuery::Scorer*>(scoredDense));
  EXPECT_FLOAT_EQ(3.0f,
                  scoredDense->getMaxScoreForSetup(PostingsReader::END));
  ASSERT_EQ(0, scoredDense->next());
  EXPECT_FLOAT_EQ(3.0f, scoredDense->score());

  auto countReq = localReq(helper.getSearchEngine());
  countReq->collection("main").topDocs("q").existsQuery("body_w")
      .getNumber().limit(0);
  countReq->execute();
  ASSERT_TRUE(countReq->ok()) << countReq->errorMsg();
  EXPECT_EQ(2, countReq->getMatchCount());

  ASSERT_TRUE(helper.deleteById("d0", UpdateMessage::COMMIT).success);
  auto deletedReader = helper.getIndexWriter()->getIndexReader();
  ASSERT_EQ(1u, deletedReader->segments().size());
  MemPool deletedPool;
  Query::Context deletedContext(deletedPool, *deletedReader);
  ExistsQuery deletedQuery("body_w");
  auto* deletedWeight = deletedQuery.createWeight(deletedContext, 0);
  EXPECT_EQ(-1, deletedWeight->count(deletedReader->segments()[0]));

  auto deletedReq = localReq(helper.getSearchEngine());
  deletedReq->collection("main").topDocs("q").existsQuery("body_w")
      .getNumber().fields({"id"}).limit(-1);
  deletedReq->execute();
  ASSERT_TRUE(deletedReq->ok()) << deletedReq->errorMsg();
  EXPECT_EQ(1, deletedReq->getMatchCount());
  EXPECT_EQ(expectedIds({"d1"}), resultIds(*deletedReq));
}

TEST_F(ExistsQueryTest,
       FullFieldSupplierDescribesTheResolvedAllDocsArm) {
  auto reader = helper.getIndexWriter()->getIndexReader();
  ASSERT_EQ(1u, reader->segments().size());
  auto& segment = reader->segments()[0];
  MemPool pool;
  Query::Context context(pool, *reader);
  ExistsQuery query("dense_w");
  auto* supplier = query.createWeight(context, 0)->scorerSupplier(pool, segment);
  ASSERT_NE(nullptr, supplier);

  Query::ScorerShape shape = supplier->describeScorer({});
  EXPECT_EQ(Query::MatchState::NONEMPTY, shape.matchState);
  EXPECT_EQ(Query::DirectScorerKind::OTHER, shape.directKind);
  EXPECT_EQ(Query::ReportedTwoPhase::NO, shape.reportedTwoPhase);
  EXPECT_EQ(Query::ClauseShape::DIRECT, shape.windowFillClause);
  EXPECT_EQ(Query::ClauseShape::NONE, shape.termDisjunctionClause);
  EXPECT_EQ(Query::ClauseShape::NONE, shape.termConjunctionClause);
  EXPECT_EQ(Query::IndependentTermAccess::UNSUPPORTED, shape.independentTerm);
  EXPECT_EQ(Query::DocsOnlyAccess::UNSUPPORTED, shape.docsOnly);
  EXPECT_EQ(Query::DirectDocSetAccess::UNSUPPORTED, shape.directDocSet);

  bool saved = AllQuery::disableDenseClauseForTests;
  AllQuery::disableDenseClauseForTests = false;
  Query::Scorer* scorer = supplier->get(pool, segment.maxDoc());
  AllQuery::disableDenseClauseForTests = saved;
  ASSERT_NE(nullptr, dynamic_cast<AllQuery::Scorer*>(scorer));
  EXPECT_TRUE(scorer->supportsWindowFilter());
}

TEST_F(ExistsQueryTest, QueryabilityErrorsAndSimpleDegradation) {
  for (std::string_view field : {"unknown_field", "stored_only", "_stored_"}) {
    EXPECT_FALSE(exprError(std::string("exists(") + std::string(field) + ")").empty());
    EXPECT_FALSE(structuredError(field).empty());
  }

  auto unknown = localReq(helper.getSearchEngine());
  unknown->collection("main").topDocs("q")
      .simpleQuery("unknown_field:*", {"dense_w"}).fields({"id"}).limit(-1);
  unknown->execute();
  EXPECT_TRUE(unknown->ok()) << unknown->errorMsg();
  EXPECT_TRUE(unknown->hasWarning("exists_field_unknown"));

  auto stored = localReq(helper.getSearchEngine());
  stored->collection("main").topDocs("q")
      .simpleQuery("stored_only:*", {"dense_w"}).fields({"id"}).limit(-1);
  stored->execute();
  EXPECT_TRUE(stored->ok()) << stored->errorMsg();
  EXPECT_TRUE(stored->hasWarning("exists_field_unqueryable"));

  auto narrowed = localReq(helper.getSearchEngine());
  auto& narrowedCursor = narrowed->collection("main").topDocs("q");
  narrowedCursor.simpleQuery("body_w:*", {"dense_w"}).fields({"id"}).limit(-1);
  auto& sq = std::get<api::SimpleQuery>(narrowedCursor.rawQuery().kind);
  std::string_view* allowed = api::build::allocArray(sq.allowed_fields, 1,
                                                      narrowedCursor.mr());
  allowed[0] = "dense_w";
  narrowed->execute();
  EXPECT_TRUE(narrowed->ok()) << narrowed->errorMsg();
  EXPECT_TRUE(narrowed->hasWarning("field_narrowed"));
}

TEST_F(ExistsQueryTest, MissingFieldSegmentHasNoSupplier) {
  CollectionHelper split("exists_split_segments");
  split.index(flatdoc("id", "s0", "only_w", "present"), UpdateMessage::COMMIT);
  split.index(flatdoc("id", "s1"), UpdateMessage::COMMIT);
  auto reader = split.getIndexWriter()->getIndexReader();
  ASSERT_EQ(2u, reader->segments().size());

  MemPool pool;
  Query::Context context(pool, *reader);
  ExistsQuery query("only_w");
  auto* weight = query.createWeight(context, 0);
  int suppliers = 0;
  for (auto& segment : reader->segments()) {
    if (weight->scorerSupplier(pool, segment) != nullptr) suppliers++;
  }
  EXPECT_EQ(1, suppliers);

  auto req = localReq(split.getSearchEngine());
  req->collection("exists_split_segments").topDocs("q")
      .existsQuery("only_w").fields({"id"}).limit(-1);
  req->execute();
  ASSERT_TRUE(req->ok()) << req->errorMsg();
  EXPECT_EQ(expectedIds({"s0"}), resultIds(*req));
}

TEST_F(ExistsQueryTest, ZeroTermTextFlushReadsAndMergesInEitherOrder) {
  auto runOrder = [&](bool zeroFirst) {
    helper.clear();
    auto zero = flatdoc("id", "z", "zero_w", " ");
    auto term = flatdoc("id", "t", "zero_w", "term");
    std::vector<Doc> firstBatch{zeroFirst ? zero : term,
                                flatdoc("id", zeroFirst ? "m0" : "m1")};
    ASSERT_TRUE(helper.indexAll(firstBatch, UpdateMessage::COMMIT).success);

    auto firstReader = helper.getIndexWriter()->getIndexReader();
    ASSERT_EQ(1u, firstReader->segments().size());
    SegFieldInfo firstInfo = readFieldInfo(firstReader->segments()[0], "zero_w");
    EXPECT_EQ(zeroFirst ? 0 : 1, firstInfo.nTerms);
    EXPECT_EQ(1, firstInfo.docsWithField);
    if (zeroFirst) {
      MemPool pool;
      TermsEnum terms(pool, firstReader->segments()[0].postingsReader(), firstInfo);
      EXPECT_FALSE(terms.nextTerm());
      EXPECT_FALSE(terms.seek("anything"));
      EXPECT_FALSE(terms.seekForward("anything"));
      EXPECT_FALSE(terms.seekCeil("anything"));
      EXPECT_EQ(expectedIds({"z"}), exprIds("zero_w:*"));
    }

    std::vector<Doc> secondBatch{zeroFirst ? term : zero,
                                 flatdoc("id", zeroFirst ? "m1" : "m0")};
    ASSERT_TRUE(helper.indexAll(secondBatch, UpdateMessage::COMMIT).success);
    CollectionHelper::UpdateBuilder merge;
    merge.commit(true, 1);
    ASSERT_TRUE(helper.submit(merge).success);
    EXPECT_EQ(1u, helper.durableSegmentCount());
    EXPECT_EQ(expectedIds({"t", "z"}), exprIds("zero_w:*"));
  };

  runOrder(true);
  runOrder(false);

  helper.clear();
  ASSERT_TRUE(helper.index(flatdoc("id", "z0", "zero_w", " "),
                           UpdateMessage::COMMIT).success);
  ASSERT_TRUE(helper.index(flatdoc("id", "z1", "zero_w", ""),
                           UpdateMessage::COMMIT).success);
  CollectionHelper::UpdateBuilder merge;
  merge.commit(true, 1);
  ASSERT_TRUE(helper.submit(merge).success);
  auto reader = helper.getIndexWriter()->getIndexReader();
  ASSERT_EQ(1u, reader->segments().size());
  EXPECT_EQ(0, readFieldInfo(reader->segments()[0], "zero_w").nTerms);
  EXPECT_EQ(expectedIds({"z0", "z1"}), exprIds("zero_w:*"));
}
