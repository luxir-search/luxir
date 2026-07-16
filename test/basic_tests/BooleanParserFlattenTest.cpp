#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <typeindex>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "solux/query/AllQuery.h"
#include "solux/query/BooleanQuery.h"
#include "solux/query/BoostQuery.h"
#include "solux/query/ParseContext.h"
#include "solux/query/ProtobufQueryParser.h"
#include "solux/query/TermQuery.h"
#include "solux/search/ProtobufSearchParser.h"
#include "solux/search/ops/TopDocsReq.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/QueryBuild.h"
#include "test/SoluxTest.h"
#include "test/TestIndex.h"

using namespace solux;
using namespace solux::test;

class BooleanParserFlattenTest : public SoluxTest {
  using ScoreMap = std::map<std::pair<int32_t, int32_t>, float>;

  MemPool parsePool;
  std::shared_ptr<Schema> schema = Schema::createDefaultSchema();

protected:
  Query* parseExpr(std::string_view text) {
    api::Query node;
    node.kind.emplace<api::ExprQuery>().q = text;
    ParseContext context{parsePool, *schema, CoerceContext{}, ""};
    ProtobufQueryParser parser(context);
    return parser.parse(node);
  }

  Query* parseSimple(std::string_view text) {
    std::string_view fields[] = {"body_w"};
    api::Query node;
    auto& simple = node.kind.emplace<api::SimpleQuery>();
    simple.q = text;
    simple.fields = fields;
    ParseContext context{parsePool, *schema, CoerceContext{}, ""};
    ProtobufQueryParser parser(context);
    return parser.parse(node);
  }

  static BooleanQuery::NormalizeTestView shape(Query* query) {
    auto* boolean = dynamic_cast<BooleanQuery*>(query);
    EXPECT_NE(boolean, nullptr);
    if (boolean == nullptr) return {};
    MemPool pool;
    return boolean->normalizationForTest(pool);
  }

  static void buildIndex(TestIndex& index) {
    TestField field(index, "body_w");
    field.startIndexing();
    int32_t doc = 0;
    for (std::string_view body : {"a", "a b", "a c", "a b c", "b", "c", "d"}) {
      field.add(doc++, body);
    }
    index.flush();
    field.startReading();
  }

  static ScoreMap collectScores(IndexReader& reader, Query& query) {
    MemPool pool;
    Query::Context context(pool, reader);
    auto* weight = query.createWeight(context, Query::NEED_SCORES);
    ScoreMap out;
    for (auto& segment : reader.segments()) {
      auto* scorer = weight->createScorer(pool, segment);
      if (scorer == nullptr) continue;
      for (int32_t doc = scorer->next(); doc != PostingsReader::END;
           doc = scorer->next()) {
        out[{segment.ord, doc}] = scorer->score();
      }
    }
    return out;
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

  static std::map<std::string, float> responseScores(const LocalReq& req,
                                                      std::string_view name) {
    std::map<std::string, float> out;
    const auto* docs = req.docList(name);
    if (docs == nullptr) return out;
    const auto* idsColumn = docs->columns.find("id");
    const auto* scoresColumn = docs->columns.find("_score_");
    if (idsColumn == nullptr || scoresColumn == nullptr) return out;
    const auto& ids = std::get<api::ColStr>(idsColumn->kind).v;
    const auto& scores = std::get<api::ColFloat>(scoresColumn->kind).v;
    if (ids.size() != scores.size()) return out;
    for (size_t i = 0; i < ids.size(); i++) {
      out.emplace(std::string(ids[i]), scores[i]);
    }
    return out;
  }

  static void expectResponseScoresNear(const std::map<std::string, float>& expected,
                                       const std::map<std::string, float>& actual) {
    ASSERT_EQ(expected.size(), actual.size());
    for (const auto& [id, score] : expected) {
      auto found = actual.find(id);
      ASSERT_NE(found, actual.end()) << id;
      float scale = std::max({std::fabs(score), std::fabs(found->second), 1.0f});
      EXPECT_NEAR(score, found->second, 1e-6f * scale) << id;
    }
  }
};

TEST_F(BooleanParserFlattenTest, negationFormsNormalizeToComplements) {
  TestIndex index;
  buildIndex(index);

  TermQuery a("body_w", "a");
  TermQuery b("body_w", "b");
  Query* mandatory[] = {&a};
  Query* prohibited[] = {&b};
  BooleanQuery flat(mandatory, {}, prohibited, {});

  for (std::string_view text : {"body_w:a AND NOT body_w:b",
                                "body_w:a AND (NOT body_w:b)"}) {
    Query* parsed = parseExpr(text);
    auto view = shape(parsed);
    EXPECT_EQ(1, view.mandatoryCount) << text;
    EXPECT_EQ(0, view.optionalCount) << text;
    EXPECT_EQ(1, view.prohibitedCount) << text;
    EXPECT_EQ(std::type_index(typeid(TermQuery)), view.mandatoryTypes[0]);
    EXPECT_EQ(std::type_index(typeid(TermQuery)), view.prohibitedTypes[0]);
    expectScoresNear(collectScores(*index.reader, flat),
                     collectScores(*index.reader, *parsed));
  }

  AllQuery all;
  Query* allOptional[] = {&all};
  BooleanQuery complement({}, allOptional, prohibited, {});
  for (Query* parsed : {parseExpr("NOT body_w:b"), parseSimple("-b")}) {
    auto view = shape(parsed);
    EXPECT_EQ(0, view.mandatoryCount);
    EXPECT_EQ(1, view.optionalCount);
    EXPECT_EQ(1, view.prohibitedCount);
    EXPECT_EQ(std::type_index(typeid(AllQuery)), view.optionalTypes[0]);
    EXPECT_EQ(0u, view.ruleMask & BooleanQuery::R4_SINGLE_CLAUSE_UNWRAP);
    expectScoresNear(collectScores(*index.reader, complement),
                     collectScores(*index.reader, *parsed));
  }
}

TEST_F(BooleanParserFlattenTest, conjunctionAndDisjunctionFormsFlatten) {
  TestIndex index;
  buildIndex(index);

  TermQuery a("body_w", "a");
  TermQuery b("body_w", "b");
  TermQuery c("body_w", "c");

  Query* parsed = parseExpr("body_w:a AND (body_w:b OR body_w:c)");
  auto view = shape(parsed);
  EXPECT_EQ(2, view.mandatoryCount);
  EXPECT_EQ(0, view.optionalCount);
  EXPECT_EQ(0, view.minShouldMatch);
  EXPECT_EQ(0u, view.ruleMask & BooleanQuery::R3_REQUIRED_DISJUNCTION_HOIST);
  Query* bcOptional[] = {&b, &c};
  BooleanQuery bc({}, bcOptional, {}, {});
  Query* nestedMandatory[] = {&a, &bc};
  BooleanQuery nested(nestedMandatory, {}, {}, {});
  expectScoresNear(collectScores(*index.reader, nested),
                   collectScores(*index.reader, *parsed));

  parsed = parseExpr("(body_w:a OR body_w:b) AND NOT body_w:c");
  view = shape(parsed);
  EXPECT_EQ(0, view.mandatoryCount);
  EXPECT_EQ(2, view.optionalCount);
  EXPECT_EQ(1, view.prohibitedCount);
  EXPECT_EQ(1, view.minShouldMatch);
  Query* abOptional[] = {&a, &b};
  Query* cProhibited[] = {&c};
  BooleanQuery abNotC({}, abOptional, cProhibited, {}, 1);
  expectScoresNear(collectScores(*index.reader, abNotC),
                   collectScores(*index.reader, *parsed));

  parsed = parseSimple("+a b c");
  view = shape(parsed);
  EXPECT_EQ(1, view.mandatoryCount);
  EXPECT_EQ(2, view.optionalCount);
  EXPECT_EQ(0, view.minShouldMatch);
  Query* flatMandatory[] = {&a};
  Query* flatOptional[] = {&b, &c};
  BooleanQuery defaultForm(flatMandatory, flatOptional, {}, {});
  expectScoresNear(collectScores(*index.reader, defaultForm),
                   collectScores(*index.reader, *parsed));
}

TEST_F(BooleanParserFlattenTest, boostedGroupUnderAndDistributesBoost) {
  TestIndex index;
  buildIndex(index);

  Query* parsed = parseExpr("body_w:a AND (body_w:b body_w:c)^2");
  auto view = shape(parsed);
  EXPECT_EQ(2, view.mandatoryCount);
  EXPECT_EQ(0, view.optionalCount);
  EXPECT_EQ(0, view.minShouldMatch);
  EXPECT_EQ(std::type_index(typeid(BoostQuery)), view.mandatoryTypes[1]);

  TermQuery a("body_w", "a");
  TermQuery b("body_w", "b");
  TermQuery c("body_w", "c");
  Query* optional[] = {&b, &c};
  BooleanQuery group({}, optional, {}, {});
  BoostQuery boostedGroup(&group, 2.0f);
  Query* mandatory[] = {&a, &boostedGroup};
  BooleanQuery nested(mandatory, {}, {}, {});
  expectScoresNear(collectScores(*index.reader, nested),
                   collectScores(*index.reader, *parsed));
}

TEST_F(BooleanParserFlattenTest, topDocsExprAndNamedFilterCompose) {
  CollectionHelper helper;
  helper.indexAll(std::array{
    flatdoc("id", "d1", "body_w", "a b", "keep_s", "yes"),
    flatdoc("id", "d2", "body_w", "a c", "keep_s", "yes"),
    flatdoc("id", "d3", "body_w", "a b c", "keep_s", "no"),
    flatdoc("id", "d4", "body_w", "a", "keep_s", "yes"),
  }, UpdateMessage::COMMIT);

  auto shapeReq = localReq(soluxNode->getSearchEngine());
  shapeReq->collection("main");
  shapeReq->topDocs("q").exprQuery("body_w:a AND (body_w:b OR body_w:c)")
      .withStats().fields({"id"}).limit(-1).matchFilter("keep", "keep_s", "yes");
  shapeReq->schema = helper.collection().getSchema();
  shapeReq->reader = helper.getIndexWriter()->getIndexReader();
  ProtobufSearchParser parser(*shapeReq);
  SearchOp* root = parser.parse();
  auto* topDocs = dynamic_cast<TopDocsReq*>(root->subOps.at("q"));
  ASSERT_NE(topDocs, nullptr);
  auto plan = shape(topDocs->query);
  EXPECT_EQ(2, plan.mandatoryCount);
  EXPECT_EQ(0, plan.optionalCount);
  EXPECT_EQ(1, plan.filterCount);
  EXPECT_EQ(0, plan.minShouldMatch);

  auto req = localReq(soluxNode->getSearchEngine());
  req->collection("main");
  req->topDocs("folded").exprQuery("body_w:a AND (body_w:b OR body_w:c)")
      .withStats().fields({"id"}).limit(-1).matchFilter("keep", "keep_s", "yes");
  auto& twin = req->topDocs("twin").withStats().fields({"id"}).limit(-1);
  twin.rawQuery() = qb::boolean(twin.mr(),
      /*required=*/{qb::match(twin.mr(), "body_w", "a")},
      /*optional=*/{qb::match(twin.mr(), "body_w", "b"),
                    qb::match(twin.mr(), "body_w", "c")},
      /*prohibited=*/{},
      /*filter=*/{qb::match(twin.mr(), "keep_s", "yes")}, 1);
  req->execute();
  ASSERT_OK(req);
  expectResponseScoresNear(responseScores(*req, "twin"),
                           responseScores(*req, "folded"));
}
