#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <typeindex>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "luxir/LuxirConfig.h"
#include "luxir/query/AllQuery.h"
#include "luxir/query/BooleanQuery.h"
#include "luxir/query/BoostQuery.h"
#include "luxir/query/ParseContext.h"
#include "luxir/query/ProtobufQueryParser.h"
#include "luxir/query/TermQuery.h"
#include "luxir/search/FieldSortCollector.h"
#include "luxir/search/ProtobufSearchParser.h"
#include "luxir/search/ops/RootOp.h"
#include "luxir/search/ops/TopDocsReq.h"
#include "luxir/server/LuxirNode.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/QueryBuild.h"
#include "test/LuxirTest.h"
#include "test/TestIndex.h"

using namespace luxir;
using namespace luxir::test;

class SearchParserTest : public LuxirTest {
  using ScoreMap = std::map<std::pair<int32_t, int32_t>, float>;

  MemPool parsePool;
  std::shared_ptr<Schema> schema = Schema::createDefaultSchema();
  google::protobuf::Arena arena;

protected:
  Query* parseExpr(std::string_view text) {
    api::Query node;
    node.kind.emplace<api::ExprQuery>().q = text;
    ParseContext context{parsePool, *schema, arena, CoerceContext{}, ""};
    ProtobufQueryParser parser(context);
    return parser.parse(node);
  }

  Query* parseSimple(std::string_view text) {
    std::string_view fields[] = {"body_w"};
    api::Query node;
    auto& simple = node.kind.emplace<api::SimpleQuery>();
    simple.q = text;
    simple.fields = fields;
    ParseContext context{parsePool, *schema, arena, CoerceContext{}, ""};
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

TEST_F(SearchParserTest, negationFormsNormalizeToComplements) {
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
  Query* allRequired[] = {&all};
  BooleanQuery complement(allRequired, {}, prohibited, {});
  for (Query* parsed : {parseExpr("NOT body_w:b"), parseSimple("-b")}) {
    auto view = shape(parsed);
    EXPECT_EQ(1, view.mandatoryCount);
    EXPECT_EQ(0, view.optionalCount);
    EXPECT_EQ(1, view.prohibitedCount);
    EXPECT_EQ(std::type_index(typeid(AllQuery)), view.mandatoryTypes[0]);
    EXPECT_EQ(0u, view.ruleMask & BooleanQuery::R4_SINGLE_CLAUSE_UNWRAP);
    expectScoresNear(collectScores(*index.reader, complement),
                     collectScores(*index.reader, *parsed));
  }
}

TEST_F(SearchParserTest, requiredDisjunctionWithExclusionHoists) {
  auto view = shape(parseSimple("+(a b) -c"));
  EXPECT_EQ(0, view.mandatoryCount);
  EXPECT_EQ(2, view.optionalCount);
  EXPECT_EQ(1, view.prohibitedCount);
  EXPECT_EQ(1, view.minShouldMatch);
  EXPECT_EQ(BooleanQuery::R3_REQUIRED_DISJUNCTION_HOIST,
            view.ruleMask & BooleanQuery::R3_REQUIRED_DISJUNCTION_HOIST);
  EXPECT_EQ(std::type_index(typeid(TermQuery)), view.optionalTypes[0]);
  EXPECT_EQ(std::type_index(typeid(TermQuery)), view.optionalTypes[1]);
  EXPECT_EQ(std::type_index(typeid(TermQuery)), view.prohibitedTypes[0]);
}

TEST_F(SearchParserTest, conjunctionAndDisjunctionFormsFlatten) {
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

TEST_F(SearchParserTest, boostedGroupUnderAndDistributesBoost) {
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

TEST_F(SearchParserTest, topDocsExprAndNamedFilterCompose) {
  CollectionHelper helper;
  helper.indexAll(std::array{
    flatdoc("id", "d1", "body_w", "a b", "keep_s", "yes"),
    flatdoc("id", "d2", "body_w", "a c", "keep_s", "yes"),
    flatdoc("id", "d3", "body_w", "a b c", "keep_s", "no"),
    flatdoc("id", "d4", "body_w", "a", "keep_s", "yes"),
  }, UpdateMessage::COMMIT);

  auto shapeReq = localReq(luxirNode->getSearchEngine());
  shapeReq->collection("main");
  shapeReq->topDocs("q").exprQuery("body_w:a AND (body_w:b OR body_w:c)")
      .withStats().fields({"id"}).limit(-1).matchFilter("keep_s", "yes");
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

  auto req = localReq(luxirNode->getSearchEngine());
  req->collection("main");
  req->topDocs("folded").exprQuery("body_w:a AND (body_w:b OR body_w:c)")
      .withStats().fields({"id"}).limit(-1).matchFilter("keep_s", "yes");
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

TEST_F(SearchParserTest, absentQueryIsMatchAllAndNormalizesAway) {
  CollectionHelper helper;
  helper.indexAll(std::array{
    flatdoc("id", "d1", "body_w", "a", "keep_s", "yes"),
    flatdoc("id", "d2", "body_w", "b", "keep_s", "yes"),
    flatdoc("id", "d3", "body_w", "c", "keep_s", "no"),
  }, UpdateMessage::COMMIT);

  // Filter-only search: no query at all. The fold's match-all placeholder
  // must be eliminated, leaving the filter as the sole (required) clause.
  auto shapeReq = localReq(luxirNode->getSearchEngine());
  shapeReq->collection("main");
  shapeReq->topDocs("q").withStats().fields({"id"}).limit(-1)
      .matchFilter("keep_s", "yes");
  shapeReq->schema = helper.collection().getSchema();
  shapeReq->reader = helper.getIndexWriter()->getIndexReader();
  ProtobufSearchParser parser(*shapeReq);
  SearchOp* root = parser.parse();
  auto* topDocs = dynamic_cast<TopDocsReq*>(root->subOps.at("q"));
  ASSERT_NE(topDocs, nullptr);
  auto plan = shape(topDocs->query);
  EXPECT_EQ(0, plan.mandatoryCount);
  EXPECT_EQ(1, plan.filterCount);
  EXPECT_EQ(BooleanQuery::R5_MATCH_ALL_ELIMINATE,
            plan.ruleMask & BooleanQuery::R5_MATCH_ALL_ELIMINATE);

  auto req = localReq(luxirNode->getSearchEngine());
  req->collection("main");
  req->topDocs("q").withStats().fields({"id"}).limit(-1)
      .matchFilter("keep_s", "yes");
  req->execute();
  ASSERT_OK(req);
  EXPECT_EQ(2, req->getMatchCount("q"));
  EXPECT_EQ(2, req->getDocs("q").size());

  // No query and no filters selects everything.
  auto browse = localReq(luxirNode->getSearchEngine());
  browse->collection("main");
  browse->topDocs("q").withStats().fields({"id"}).limit(-1);
  browse->execute();
  ASSERT_OK(browse);
  EXPECT_EQ(3, browse->getMatchCount("q"));

  // An explicitly empty Query (unset oneof) behaves like `all`.
  auto empty = localReq(luxirNode->getSearchEngine());
  empty->collection("main");
  auto& td = empty->topDocs("q").withStats().fields({"id"}).limit(-1);
  td.rawQuery();
  empty->execute();
  ASSERT_OK(empty);
  EXPECT_EQ(3, empty->getMatchCount("q"));
}

namespace {

struct StringSortModeGuard {
  StringSortMode saved;

  explicit StringSortModeGuard(StringSortMode mode)
      : saved(SortField::setStringSortModeForTests(mode)) {}
  ~StringSortModeGuard() { SortField::setStringSortModeForTests(saved); }
};

} // namespace

// Sort-plan shape: what the parser derives for each sort expression (clause
// kind/order, useFieldSort, rankNeedsScores) and the score/pruning flags it
// puts on the weight. Moved here from ValueExprSortTest so ProtobufSearchParser
// stays confined to this TU.
TEST_F(SearchParserTest, normalizationScoreModesAndStringComparator) {
  CollectionHelper helper;
  helper.index(flatdoc("id_s", "a", "name_s", "z", "price_i", 2,
                       "popularity_i", 10, "body_w", "term term"),
               UpdateMessage::COMMIT);
  helper.index(flatdoc("id_s", "b", "name_s", "a", "price_i", 1,
                       "popularity_i", 20, "body_w", "term"),
               UpdateMessage::COMMIT);
  auto reader = helper.getIndexWriter()->getIndexReader();
  StringSortModeGuard stringMode(StringSortMode::SEGMENT);

  auto inspect = [&](std::string_view expression, qb::SortDir direction,
                     bool getScores, auto&& verify) {
    auto request = localReq(luxirNode->getSearchEngine());
    request->collection("main");
    auto& top = request->topDocs("q").matchQuery("body_w", "term").limit(10);
    top.getScores(getScores);
    if (!expression.empty()) qb::sort(top, expression, direction);
    request->reader = reader;
    request->schema = helper.collection().getSchema();
    ProtobufSearchParser parser(*request);
    auto* root = static_cast<RootOp*>(parser.parse());
    auto* parsed = dynamic_cast<TopDocsReq*>(root->subOps.at("q"));
    ASSERT_NE(parsed, nullptr);
    verify(*parsed);
  };

  inspect("price_i", qb::UNKNOWN, false, [&](TopDocsReq& parsed) {
    ASSERT_EQ(1, parsed.sortPlan.clauses.size());
    EXPECT_EQ(SortClause::COLUMN, parsed.sortPlan.clauses[0].getKind());
    EXPECT_EQ(SortField::ASC, parsed.sortPlan.clauses[0].getOrder());
    EXPECT_FALSE(parsed.weight->needsScores());
  });
  inspect("name_s", qb::ASC, false, [&](TopDocsReq& parsed) {
    ASSERT_EQ(SortClause::COLUMN, parsed.sortPlan.clauses[0].getKind());
    FieldSortCollector collector(2, parsed.sortPlan.clauses, reader.get(), false);
    EXPECT_NE(nullptr, dynamic_cast<SegmentOrdComparator*>(collector.soleColumn));
  });
  inspect("_docid_", qb::UNKNOWN, false, [&](TopDocsReq& parsed) {
    EXPECT_EQ(SortClause::DOC, parsed.sortPlan.clauses[0].getKind());
    EXPECT_EQ(SortField::ASC, parsed.sortPlan.clauses[0].getOrder());
    EXPECT_FALSE(parsed.weight->needsScores());
  });
  inspect("_score_", qb::UNKNOWN, false, [&](TopDocsReq& parsed) {
    EXPECT_EQ(SortClause::SCORE, parsed.sortPlan.clauses[0].getKind());
    EXPECT_EQ(SortField::DESC, parsed.sortPlan.clauses[0].getOrder());
    EXPECT_FALSE(parsed.sortPlan.useFieldSort);
    EXPECT_TRUE(parsed.weight->needsScores());
    EXPECT_TRUE(parsed.weight->allowsPruning());
  });
  inspect("add(price_i,1)", qb::UNKNOWN, false, [&](TopDocsReq& parsed) {
    EXPECT_EQ(SortClause::EXPR, parsed.sortPlan.clauses[0].getKind());
    EXPECT_EQ(SortField::ASC, parsed.sortPlan.clauses[0].getOrder());
    EXPECT_FALSE(parsed.weight->needsScores());
    EXPECT_FALSE(parsed.weight->allowsPruning());
  });
  inspect("add(price_i,1)", qb::ASC, true, [&](TopDocsReq& parsed) {
    EXPECT_TRUE(parsed.weight->needsScores());
  });
  inspect("add(score,popularity_i)", qb::DESC, false, [&](TopDocsReq& parsed) {
    EXPECT_TRUE(parsed.sortPlan.rankNeedsScores);
    EXPECT_TRUE(parsed.weight->needsScores());
    EXPECT_FALSE(parsed.weight->allowsPruning());
  });
  inspect({}, qb::UNKNOWN, false, [&](TopDocsReq& parsed) {
    EXPECT_TRUE(parsed.sortPlan.rankNeedsScores);
    EXPECT_TRUE(parsed.weight->needsScores());
    EXPECT_FALSE(parsed.sortPlan.useFieldSort);
  });

  auto canonical = localReq(luxirNode->getSearchEngine());
  canonical->collection("main");
  auto& top = canonical->topDocs("q").matchQuery("body_w", "term").limit(10);
  qb::sort(top, "score", qb::DESC);
  qb::sort(top, "_docid_", qb::ASC);
  canonical->reader = reader;
  canonical->schema = helper.collection().getSchema();
  ProtobufSearchParser parser(*canonical);
  auto* root = static_cast<RootOp*>(parser.parse());
  auto* parsed = dynamic_cast<TopDocsReq*>(root->subOps.at("q"));
  ASSERT_NE(parsed, nullptr);
  EXPECT_FALSE(parsed->sortPlan.useFieldSort);
  EXPECT_TRUE(parsed->weight->allowsPruning());
}

// Pruning is withdrawn when the request asks for an exact match count.
TEST_F(SearchParserTest, getNumberControlsPruningWeightFlag) {
  CollectionHelper helper;
  helper.indexAll(std::array{
    flatdoc("id", "d1", "body_w", "apple apricot"),
    flatdoc("id", "d2", "body_w", "banana"),
    flatdoc("id", "d3", "body_w", "apricot avocado"),
  }, UpdateMessage::COMMIT);

  auto parseAllowsPruning = [&](bool getNumber) -> std::optional<bool> {
    auto request = localReq(helper.getSearchEngine());
    auto& topDocs = request->collection("main").topDocs("q")
        .prefixQuery("body_w", "ap").limit(10);
    topDocs.getNumber(getNumber);
    request->reader = helper.getIndexWriter()->getIndexReader();
    request->schema = helper.collection().getSchema();
    ProtobufSearchParser parser(*request);
    auto* root = static_cast<RootOp*>(parser.parse());
    auto it = root->subOps.find("q");
    if (it == root->subOps.end()) return std::nullopt;
    auto* parsed = dynamic_cast<TopDocsReq*>(it->second);
    if (parsed == nullptr) return std::nullopt;
    return parsed->weight->allowsPruning();
  };

  EXPECT_EQ(std::optional<bool>(true), parseAllowsPruning(false));
  EXPECT_EQ(std::optional<bool>(false), parseAllowsPruning(true));
}

TEST_F(SearchParserTest, opNestingDepthCapped) {
  CollectionHelper helper;
  helper.indexAll(std::array{flatdoc("id", "d1", "body_w", "a")}, UpdateMessage::COMMIT);

  // Builds a chain of `depth` nested TopDocs ops and parses it.
  auto parseNested = [&](SearchEngine& engine, int depth) {
    auto request = localReq(engine);
    OpCursor* cursor = &request->collection("main").topDocs("op0");
    for (int i = 1; i < depth; i++) {
      cursor = &cursor->topDocs("op" + std::to_string(i));
    }
    request->reader = helper.getIndexWriter()->getIndexReader();
    request->schema = helper.collection().getSchema();
    ProtobufSearchParser parser(*request);
    parser.parse();
  };

  auto defaultRequest = localReq(helper.getSearchEngine());
  int cap = defaultRequest->searchConfig.max_op_depth;
  parseNested(helper.getSearchEngine(), cap);  // at the cap: accepted
  try {
    parseNested(helper.getSearchEngine(), cap + 1);
    FAIL() << "expected an op nesting error";
  } catch (const std::exception& e) {
    EXPECT_NE(std::string(e.what()).find("nesting exceeds"), std::string::npos) << e.what();
  }

  // The cap is node configuration (--search.max-op-depth), not a constant.
  LuxirConfig config;
  config.search.max_op_depth = 2;
  LuxirNode node{config};
  parseNested(node.getSearchEngine(), 2);
  EXPECT_THROW(parseNested(node.getSearchEngine(), 3), std::runtime_error);
}
