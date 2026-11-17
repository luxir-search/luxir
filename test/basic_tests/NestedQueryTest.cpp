// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/QueryBuild.h"
#include "test/SchemaBuilder.h"
#include "luxir/api/padded_input.h"
#include "luxir/query/ParseContext.h"
#include "luxir/query/ProtobufQueryParser.h"
#include "luxir/query/ToParentQuery.h"
#include "luxir/query/ImpactsIndex.h"
#include "luxir/reader/SkipStats.h"
#include "luxir/search/OwnershipRelation.h"
#include "luxir/server/JsonResponse.h"
#include <glaze/glaze.hpp>

namespace luxir::test {
namespace {

class NestedQueryTest : public LuxirTest {
protected:
  CollectionHelper helper;

  void SetUp() override {
    LuxirTest::SetUp();
    SchemaBuilder schema;
    using Type = api::FieldDef::FieldClass;
    for (auto name : {"items", "items.parts"}) schema.field(name).type = Type::NESTED;
    for (auto name : {"root_only", "items.color", "items.size", "items.parts.material"}) schema.field(name).type = Type::STRING;
    schema.field("items.price").type = Type::INT;
    schema.field("items.text").type = Type::TEXT;
    schema.field("items.place").type = Type::GEO_POINT;
    auto& vector = schema.field("items.embedding");
    vector.type = Type::VECTOR; vector.dims = 2; vector.metric = api::VectorMetric::L2;
    schema.set(helper.collection());
    CollectionHelper::UpdateBuilder input;
    for (auto json : {
        R"({"id":"a","root_only":"root","items":[{"color":"red","size":"S","price":100,"text":"quick brown","place":[0,0],"embedding":[0,0],"parts":{"material":"steel"}},{"color":"blue","size":"L","price":600,"text":"fox","embedding":[1,0],"parts":{"material":"wood"}}]})",
        R"({"id":"b","items":{"color":"red","size":"L","price":300,"text":"quick fox","place":[1,1],"embedding":[10,0],"parts":{"material":"steel"}}})",
        R"({"id":"c","items":[{"color":"blue","size":"S","price":900,"text":"quick","embedding":[20,0],"parts":{"material":"wood"}},{"color":"blue","size":"L","text":"fox","embedding":[21,0]}]})",
        R"({"id":"d","items":[]})", R"({"id":"e","items":{}})",
        R"({"id":"f","items":{"color":"red","size":"L","price":500,"text":"quick very fox","embedding":[30,0]}})"}) {
      input.addRaw([&](auto& doc, auto& arena) { ASSERT_TRUE(api::read_json(doc, json, arena)); });
    }
    input.commit();
    ASSERT_TRUE(helper.submit(input).success);
  }

  std::shared_ptr<IndexReader> reader() { return helper.getIndexWriter()->snapshots.readers.getReader(); }

  Query* parse(LocalReq& req, const api::Query& wire) {
    req.reader = reader(); req.schema = req.reader->schema();
    ParseView view(*req.schema, nullptr, &req.reader, &req);
    ParseContext context(req.requestPool, *req.schema, req.arena, CoerceContext{}, "q", nullptr, &view);
    return ProtobufQueryParser(context).parse(wire);
  }

  static api::Query expr(std::string_view text) {
    api::Query q; q.kind.emplace<api::ExprQuery>().q = text; return q;
  }

  static FilterKey key(Query& q) {
    FilterKeyContext context;
    FilterKeyBuilder out;
    auto scope = q.appendFilterKey(out, context);
    return std::move(out).finishStructural(scope, context);
  }

  int64_t count(std::string_view text, bool filter = false) {
    auto req = localReq(helper.getSearchEngine());
    auto& op = req->topDocs().countLimit().limit(0);
    if (filter) op.allQuery().filter(expr(text));
    else op.exprQuery(text);
    req->execute(1);
    EXPECT_OK(req);
    return req->getMatchCount();
  }
};

TEST_F(NestedQueryTest, implicitExplicitAndSingletonGroupHaveOneIdentityAndCacheEntry) {
  auto cache = std::make_shared<FilterCache>(FilterCacheConfig{
      .maxBytes = 1024 * 1024, .minSegmentDocs = 0, .admissionThreshold = 2});
  helper.getIndexWriter()->snapshots.readers.filterCache = cache;
  auto req = localReq(helper.getSearchEngine());
  auto explicitQuery = qb::toParent(req->mr, "items", qb::match(req->mr, "color", "red"));
  auto* explicitTree = parse(*req, explicitQuery);
  for (auto text : {"items.color:red", "items:(color:red)"}) {
    auto wire = expr(text);
    auto* tree = parse(*req, wire);
    EXPECT_TRUE(tree->equals(*explicitTree));
    EXPECT_EQ(explicitTree->hash(), tree->hash());
    EXPECT_EQ(key(*explicitTree), key(*tree));
    EXPECT_EQ(3, count(text));
  }
  auto none = qb::toParent(req->mr, "items", qb::match(req->mr, "color", "red"), "none");
  EXPECT_EQ(key(*explicitTree), key(*parse(*req, none)));
  EXPECT_EQ(3, count("items.color:red"));
  EXPECT_EQ(3, count("items:(color:red)"));
  auto hits = cache->counters().hits;
  SkipStatsScope stats;
  auto warmed = localReq(helper.getSearchEngine());
  warmed->topDocs().countLimit().limit(0).rawQuery() = qb::toParent(warmed->mr, "items", qb::match(warmed->mr, "color", "red"), "none");
  warmed->execute(1);
  ASSERT_OK(warmed);
  EXPECT_EQ(3, warmed->getMatchCount());
  EXPECT_GT(cache->counters().hits, hits);
  EXPECT_EQ(1, SkipStats::wholeCountHits);
}

TEST_F(NestedQueryTest, uniformMaximumPreservesChildScoresAndExactBounds) {
  for (int kind = 0; kind < 6; kind++) {
    auto make = [&](auto& req) {
      auto& mr = req.mr;
      auto all = qb::all();
      if (kind == 0) return qb::range(mr, "price", qb::valI64(mr, 0), nullptr, qb::valI64(mr, 1000), nullptr);
      if (kind == 1) return qb::exists(mr, "price");
      if (kind == 2) return qb::constantScore(mr, qb::match(mr, "text", "quick"), 2.5f);
      if (kind == 3) return qb::boost(mr, qb::exists(mr, "price"), 0.3f);
      if (kind == 4) return qb::boost(mr, qb::boolean(mr, {
          qb::constantScore(mr, all, 0.1f), qb::constantScore(mr, all, 0.2f)}), 1.3f);
      return qb::all();
    };
    auto child = localReq(helper.getSearchEngine());
    auto& op = child->topDocs().getScores().fields({"/id"}).limit(-1).countLimit();
    op.rawOp().domain.emplace().to = "items";
    op.rawQuery() = make(*child);
    child->execute(1); ASSERT_OK(child);
    auto ids = std::get<api::ColStr>(child->docList()->columns.at("/id").kind).v;
    auto scores = std::get<api::ColFloat>(child->docList()->columns.at("_score_").kind).v;
    std::map<std::string_view, float> expected;
    for (size_t i = 0; i < ids.size(); i++) expected[ids[i]] = std::max(expected[ids[i]], scores[i]);
    auto joined = localReq(helper.getSearchEngine());
    joined->topDocs().getScores().fields({"id"}).limit(-1).countLimit().rawQuery() = qb::boost(joined->mr,
        qb::toParent(joined->mr, "items", make(*joined)), 0.7f);
    joined->execute(1); ASSERT_OK(joined);
    auto owners = std::get<api::ColStr>(joined->docList()->columns.at("id").kind).v;
    auto maxima = std::get<api::ColFloat>(joined->docList()->columns.at("_score_").kind).v;
    ASSERT_EQ(expected.size(), owners.size());
    for (size_t i = 0; i < owners.size(); i++) EXPECT_EQ(expected[owners[i]] * 0.7f, maxima[i]) << kind;

    auto req = localReq(helper.getSearchEngine());
    auto wire = qb::toParent(req->mr, "items", make(*req));
    auto* tree = parse(*req, wire);
    auto* planning = req->createPlanningContext(*req->reader);
    tree->validateLogical(*planning);
    auto* context = Query::Context::create(&req->arena, *planning);
    auto* weight = tree->createWeight(*context, Query::NEED_SCORES | Query::ALLOW_PRUNING);
    EXPECT_TRUE(weight->isConstantScoring());
    EXPECT_FALSE(weight->needsPrepare());
    MemPool pool;
    auto* source = weight->scorerSupplier(pool, req->reader->segments()[0]);
    ASSERT_NE(nullptr, source);
    auto planContext = Query::PlanContext::fromLeadCost(1000, Query::ExecutionUse::MATCH_WINDOWS);
    EXPECT_EQ(Query::ClauseShape::DIRECT, source->describeScorer(planContext).windowFillClause);
    auto* scorer = source->resolve(pool, planContext)->build(pool);
    ASSERT_NE(nullptr, scorer);
    ASSERT_TRUE(scorer->constantContribution());
    float constant = *scorer->constantContribution();
    EXPECT_EQ(constant, scorer->getMaxScore(PostingsReader::END));
    EXPECT_EQ(constant, scorer->getScoreBounds(PostingsReader::END).lo);
    ASSERT_NE(PostingsReader::END, scorer->next());
    EXPECT_EQ(constant, scorer->score());
    scorer->setMinCompetitiveScore(std::nextafter(constant, std::numeric_limits<float>::infinity()));
    EXPECT_EQ(constant, scorer->score());
    EXPECT_EQ(PostingsReader::END, scorer->next());
  }
}

TEST_F(NestedQueryTest, maximumBoundsCoverEveryScoreAfterTheChildMoves) {
  SearchOverridesGuard impacts(ImpactsIndex::forceEagerForTests);
  ImpactsIndex::forceEagerForTests = true;
  for (float boost : {0.0f, 0.3f, 2.5f}) {
    auto req = localReq(helper.getSearchEngine());
    auto wire = qb::boost(req->mr, qb::toParent(req->mr, "items", qb::match(req->mr, "text", "quick")), boost);
    auto* tree = parse(*req, wire);
    auto* planning = req->createPlanningContext(*req->reader);
    tree->validateLogical(*planning);
    auto* context = Query::Context::create(&req->arena, *planning);
    auto* weight = tree->createWeight(*context, Query::NEED_SCORES | Query::ALLOW_PRUNING);
    Query::Weight::PrepareContext preparation{*req->reader, {}, false, nullptr, Query::SupplierExecutionMode::ORDINARY};
    auto prepared = weight->prepare(preparation);
    MemPool pool;
    auto* source = prepared->scorerSupplier(pool, req->reader->segments()[0]);
    ASSERT_NE(nullptr, source);
    auto* scorer = source->resolve(pool, Query::PlanContext{})->build(pool);
    ASSERT_NE(nullptr, scorer);
    EXPECT_FALSE(scorer->constantContribution());
    auto bound = scorer->getScoreBounds(PostingsReader::END);
    EXPECT_TRUE(std::isfinite(bound.hi));
    EXPECT_EQ(PostingsReader::END, scorer->advanceShallowForSetup(0));
    while (scorer->next() != PostingsReader::END) {
      EXPECT_LE(bound.lo, scorer->score());
      EXPECT_GE(bound.hi, scorer->score());
      EXPECT_EQ(bound.hi, scorer->getMaxScore(scorer->docId()));
      EXPECT_EQ(bound.hi, scorer->getMaxScoreForSetup(scorer->docId()));
    }
  }
}

TEST_F(NestedQueryTest, perClauseAndSameItemNegationHaveDifferentMeanings) {
  EXPECT_EQ(3, count("items.color:red AND items.size:L"));
  EXPECT_EQ(2, count("items:(color:red AND size:L)"));
  EXPECT_EQ(1, count("items:(color:red AND NOT size:L)"));
  EXPECT_EQ(4, count("NOT items:(color:red AND size:L)"));
  EXPECT_EQ(3, count("items:(NOT color:red)"));
  EXPECT_EQ(3, count("NOT items.color:red"));
  EXPECT_EQ(2, count("items:(color:red AND size:L)", true));
  EXPECT_EQ(4, count("items.text:quick AND items.text:fox"));
  auto req = localReq(helper.getSearchEngine());
  req->topDocs().countLimit().limit(0).rawQuery() = qb::match(req->mr,
      "items.text", "quick fox", api::Match::Operator::AND);
  req->execute(1); ASSERT_OK(req); EXPECT_EQ(2, req->getMatchCount());
}

TEST_F(NestedQueryTest, twoLevelsUseRelativeNamesInEverySpelling) {
  auto req = localReq(helper.getSearchEngine());
  auto q = qb::toParent(req->mr, "items.parts", qb::match(req->mr, "material", "steel"));
  auto* explicitTree = parse(*req, q);
  for (auto text : {"items.parts.material:steel", "items.parts:(material:steel)"}) {
    auto wire = expr(text);
    auto* tree = parse(*req, wire);
    EXPECT_TRUE(explicitTree->equals(*tree));
    EXPECT_EQ(key(*explicitTree), key(*tree));
    EXPECT_EQ(2, count(text));
  }
  EXPECT_EQ(2, count("items:(parts:(material:steel) AND color:red)"));
  EXPECT_EQ(2, count("items:(parts.material:steel AND color:red)"));
}

TEST_F(NestedQueryTest, everySingleFieldLeafScopesBeforeLowering) {
  for (std::string json : {
      R"({"match":{"field":"$color","val":"red"}})",
      R"({"any_of":{"field":"$color","values":["red"]}})",
      R"({"phrase":{"field":"$text","text":"quick fox"}})",
      R"({"prefix":{"field":"$color","prefix":"re"}})",
      R"({"wildcard":{"field":"$color","pattern":"r*d"}})",
      R"({"regex":{"field":"$color","pattern":"r.*"}})",
      R"({"fuzzy":{"field":"$color","term":"red","max_edits":1}})",
      R"({"exists":{"field":"$color"}})",
      R"({"range":{"field":"$price","gte":100,"lte":500}})",
      R"({"geo_box":{"field":"$place","min_lat":-1,"max_lat":2,"min_lon":-1,"max_lon":2}})",
      R"({"geo_distance":{"field":"$place","lat":0,"lon":0,"radius_meters":200000}})",
      R"({"geo_polygon":{"field":"$place","polygons":[{"points":[{"lat":-1,"lon":-1},{"lat":-1,"lon":2},{"lat":2,"lon":2},{"lat":2,"lon":-1},{"lat":-1,"lon":-1}]}]}})",
      R"({"knn":{"field":"$embedding","query":[0,0],"k":1,"exact":true}})"}) {
    SCOPED_TRACE(json);
    auto req = localReq(helper.getSearchEngine());
    auto pos = json.find('$');
    auto innerJson = json; innerJson.erase(pos, 1);
    json.replace(pos, 1, "items.");
    api::Query inner, implicit;
    ASSERT_TRUE(api::read_json(inner, innerJson, req->mr));
    ASSERT_TRUE(api::read_json(implicit, json, req->mr));
    auto explicitQuery = qb::toParent(req->mr, "items", inner);
    auto* a = parse(*req, implicit); auto* b = parse(*req, explicitQuery);
    EXPECT_TRUE(a->equals(*b)); EXPECT_EQ(a->hash(), b->hash()); EXPECT_EQ(key(*a), key(*b));
    req->topDocs().countLimit().getScores().fields({"id"}).rawQuery() = implicit;
    req->topDocs("explicit").countLimit().getScores().fields({"id"}).rawQuery() = explicitQuery;
    req->execute(1);
    ASSERT_OK(req);
    EXPECT_EQ(req->getMatchCount(), req->getMatchCount("explicit"));
    auto aDocs = req->getDocs(); auto bDocs = req->getDocs("explicit");
    ASSERT_EQ(aDocs.size(), bDocs.size());
    for (size_t i = 0; i < aDocs.size(); i++) EXPECT_TRUE(docEquals(aDocs[i], bDocs[i]));
    auto aScores = std::get<api::ColFloat>(req->docList()->columns.find("_score_")->kind).v;
    auto bScores = std::get<api::ColFloat>(req->docList("explicit")->columns.find("_score_")->kind).v;
    EXPECT_TRUE(std::ranges::equal(aScores, bScores));
  }
}

TEST_F(NestedQueryTest, multiMatchStaysWithinAViewOrCombinesPerFieldJoins) {
  for (auto json : {
      R"({"multi_match":{"text":"quick","fields":["items.text"],"mode":"combined"}})",
      R"({"multi_match":{"text":"quick","fields":["items.te*"],"mode":"per_field"}})",
      R"({"simple_query":{"q":"quick","fields":["items.text"]}})"}) {
    auto req = localReq(helper.getSearchEngine());
    ASSERT_TRUE(api::read_json(req->topDocs().countLimit().limit(0).rawQuery(), json, req->mr));
    req->execute(1); ASSERT_OK(req); EXPECT_EQ(4, req->getMatchCount());
  }
  auto req = localReq(helper.getSearchEngine());
  req->topDocs().countLimit().limit(0).rawQuery() = qb::multiMatch(req->mr, "red",
      {{"items.color", {}}, {"root_only", {}}}, api::MultiMatchQuery::Mode::PER_FIELD);
  req->execute(1); ASSERT_OK(req); EXPECT_EQ(3, req->getMatchCount());
  auto invalid = localReq(helper.getSearchEngine());
  invalid->topDocs().rawQuery() = qb::multiMatch(invalid->mr, "red",
      {{"items.color", {}}, {"root_only", {}}});
  invalid->execute(1); EXPECT_FALSE(invalid->ok());
  EXPECT_NE(std::string::npos, invalid->errorMsg().find("mixes nested paths"));
}

TEST_F(NestedQueryTest, nestedExistsCountsFamiliesAndHonorsRootDeletes) {
  EXPECT_EQ(5, count("items:*")); EXPECT_EQ(3, count("items.parts:*"));
  EXPECT_EQ(0, count("empty_nested:*"));
  auto req = localReq(helper.getSearchEngine());
  auto explicitQuery = qb::toParent(req->mr, "items", qb::all());
  auto exists = qb::exists(req->mr, "items");
  EXPECT_TRUE(parse(*req, exists)->equals(*parse(*req, explicitQuery)));
  EXPECT_TRUE(helper.deleteById("a", UpdateMessage::COMMIT).success);
  EXPECT_EQ(4, count("items:*")); EXPECT_EQ(2, count("items.parts:*"));
}

TEST_F(NestedQueryTest, domainToDescendsAndProjectsWithOneWireKey) {
  struct Case { const char* from; const char* to; int64_t count; };
  for (auto c : {Case{"", "items", 7}, {"", "items.parts", 4}, {"items", "parts", 4},
                 {"items", "..", 5}, {"items.parts", "..", 4}, {"items.parts", "../..", 3},
                 {"items.parts", "/", 3}, {"", "/", 6}}) {
    SCOPED_TRACE(std::string(c.from) + " -> " + c.to);
    auto req = localReq(helper.getSearchEngine());
    req->topDocs().allQuery().countLimit().limit(0).rawOp().domain.emplace().to = c.to;
    req->execute(reader()->view(c.from)); ASSERT_OK(req); EXPECT_EQ(c.count, req->getMatchCount());
  }
  auto req = localReq(helper.getSearchEngine());
  ASSERT_TRUE(api::read_json(req->rawRequest(), R"({"ops":{"q":{"domain":{"to":"items"},"top_docs":{"query":"color:red","count_limit":-1,"limit":0}}}})", req->mr));
  std::vector<std::byte> bytes;
  ASSERT_TRUE(api::encode(req->rawRequest(), bytes));
  auto decoded = localReq(helper.getSearchEngine());
  ASSERT_TRUE(api::decode(decoded->rawRequest(), api::copyToPaddedInput(std::span<const std::byte>(bytes), decoded->mr), decoded->mr));
  decoded->execute(1); ASSERT_OK(decoded); EXPECT_EQ(3, decoded->getMatchCount());
  for (auto json : {R"({"to_children":"items"})", R"({"to_parent":"/"})"}) {
    api::Domain old; EXPECT_FALSE(api::read_json(old, json, req->mr));
  }
}

TEST_F(NestedQueryTest, invalidDomainAndNonDescendantNamesAreRejected) {
  for (auto [to, error] : {std::pair{"", "requires a path"}, {"..", "above the root"},
      {"../..", "above the root"}, {"../name", "reserved"}, {"../", "every upward step"},
      {"items/parts", "uses dots"}, {"/items", "uses dots"}, {"items.color", "not a nested path"},
      {"items..parts", "Invalid field"}}) {
    auto req = localReq(helper.getSearchEngine());
    req->topDocs().allQuery().limit(0).rawOp().domain.emplace().to = to;
    req->execute(1); EXPECT_FALSE(req->ok()) << to;
    EXPECT_NE(std::string::npos, req->errorMsg().find(error)) << req->errorMsg();
  }
  auto above = localReq(helper.getSearchEngine());
  above->topDocs().allQuery().limit(0).rawOp().domain.emplace().to = "../../..";
  above->execute(reader()->view("items.parts")); EXPECT_FALSE(above->ok());
  EXPECT_NE(std::string::npos, above->errorMsg().find("above the root"));
  for (auto name : {"root_only", "../root_only", "reviews.color", "items.color"}) {
    auto req = localReq(helper.getSearchEngine());
    req->topDocs().countLimit().limit(0).rawQuery() = qb::toParent(req->mr, "items", qb::match(req->mr, name, "red"));
    req->execute(1); EXPECT_FALSE(req->ok()) << name;
    EXPECT_NE(std::string::npos, req->errorMsg().find("view 'items'"));
  }
}

TEST_F(NestedQueryTest, ancestorConsumersRequireADomainAndLocalConsumersWork) {
  for (int32_t kind : {0, 1, 2, 3, 6}) {
    auto req = localReq(helper.getSearchEngine());
    auto& op = req->topDocs().allQuery().limit(0);
    if (kind == 0) qb::sort(op, "items.price", qb::ASC);
    if (kind == 1) req->facet("f", "items.color");
    if (kind == 2) req->expr("e", "sum(items.price)");
    if (kind == 3) op.rawQuery() = qb::rescore(req->mr, qb::all(), "col(\"items.price\")");
    if (kind == 6) {
      ASSERT_TRUE(api::read_json(req->rawRequest(), R"({"ops":{"q":{"top_docs":{"collapse":{"field":["items.color"]}}}}})", req->mr));
    }
    req->execute(1); EXPECT_FALSE(req->ok()) << kind;
    if (kind == 1 || kind == 6) {
      EXPECT_NE(std::string::npos, req->errorMsg().find("nested path 'items'")) << req->errorMsg();
      EXPECT_NE(std::string::npos, req->errorMsg().find("Domain 'to'")) << req->errorMsg();
    } else {
      EXPECT_NE(std::string::npos, req->errorMsg().find(kind == 3 ? "scalar numeric" : "reduce")) << req->errorMsg();
    }
  }
  auto req = localReq(helper.getSearchEngine());
  auto& rows = req->topDocs().exprQuery("color:red").countLimit().fields({"color", "price"});
  rows.rawOp().domain.emplace().to = "items";
  qb::sort(rows, "price", qb::ASC);
  req->facet("colors", "color").rawOp().domain.emplace().to = "items";
  rows.expr("sum", "sum(price)");
  req->execute(1); ASSERT_OK(req); EXPECT_EQ(3, req->getMatchCount());
  auto* sum = req->docList()->ops.find("sum");
  ASSERT_NE(nullptr, sum); EXPECT_EQ(900, std::get<int64_t>((**sum).kind));
}

TEST_F(NestedQueryTest, phraseJoinUsesTwoPhaseVerificationAndAdvance) {
  EXPECT_EQ(1, count("items.text:\"quick fox\""));
  EXPECT_EQ(1, count("items:(text:\"quick fox\")"));
  EXPECT_EQ(1, count("items:(text:\"quick fox\")", true));
  for (auto mode : {"max", "none"}) {
    auto run = localReq(helper.getSearchEngine());
    auto join = qb::toParent(run->mr, "items", qb::phraseText(run->mr, "text", "quick fox"), mode);
    run->topDocs().countLimit().getScores().fields({"id"}).rawQuery() = join;
    run->topDocs("filtered").allQuery().countLimit().limit(0).filter(join);
    run->execute(1); ASSERT_OK(run);
    EXPECT_EQ(1, run->getMatchCount()); EXPECT_EQ(1, run->getMatchCount("filtered"));
    EXPECT_TRUE(containsDoc(run->getDocs(), flatdoc("id", "b")));
    if (mode == std::string_view("none")) {
      EXPECT_FLOAT_EQ(1.0f, std::get<api::ColFloat>(run->docList()->columns.find("_score_")->kind).v[0]);
    }
  }
  auto req = localReq(helper.getSearchEngine());
  auto child = reader()->view("items");
  auto wire = qb::toParent(req->mr, "items", qb::phraseText(req->mr, "text", "quick fox"));
  auto* query = (ToParentQuery*)parse(*req, wire);
  MemPool pool;
  Query::Context context(pool, *child);
  OwnershipRelation relation(*child, *reader(), context.planningContext().memoryTracker);
  auto* weight = query->getChild()->createWeight(context, Query::NEED_SCORES);
  for (bool maximum : {false, true}) {
    for (bool seek : {false, true}) {
      auto* input = weight->scorerSupplier(pool, child->segments()[0]);
      EXPECT_EQ(Query::ReportedTwoPhase::YES, input->describeScorer({}).reportedTwoPhase);
      auto* supplier = relation.reduce(pool, 0, *input, nullptr, maximum ? RowRelation::Reduction::MAXIMUM : RowRelation::Reduction::NONE, 1.0f);
      auto* scorer = buildScorerForTests(pool, *supplier, 6);
      ASSERT_NE(nullptr, scorer); EXPECT_EQ(1, seek ? scorer->advance(1) : scorer->next());
      float score = scorer->score(); EXPECT_GT(score, 0);
      EXPECT_EQ(1, scorer->docId()); EXPECT_FLOAT_EQ(score, scorer->score());
      EXPECT_EQ(PostingsReader::END, scorer->advance(2));
    }
  }
}

TEST_F(NestedQueryTest, implicitKnnGetsOwnerEligibilityBeforeSelection) {
  ASSERT_TRUE(helper.deleteById("a", UpdateMessage::COMMIT).success);
  for (std::string_view owner : {"", "b", "f"}) {
    auto req = localReq(helper.getSearchEngine());
    auto& op = req->topDocs().countLimit().fields({"id"});
    if (!owner.empty()) op.matchFilter("id", owner);
    op.rawQuery() = qb::knn(req->mr, "items.embedding", {0,0}, 1, 0, true);
    req->execute(1); ASSERT_OK(req); EXPECT_EQ(1, req->getMatchCount());
    EXPECT_TRUE(containsDoc(req->getDocs(), flatdoc("id", std::string(owner.empty() ? std::string_view("b") : owner))));
  }
}

TEST_F(NestedQueryTest, aScoredJoinAnswersFacetsThatExcludeAListFilter) {
  auto req = localReq(helper.getSearchEngine());
  ASSERT_TRUE(api::read_json(req->rawRequest(), R"JSON({"ops":{"q":{"top_docs":{
    "query":"items.text:quick","fields":["id"],"count_limit":-1,
    "filter":[{"query":"id:a","except_ops":["ids"]}],
    "ops":{"ids":{"field_facet":{"field":"id"}}}}}}})JSON", req->mr));
  req->execute(1); ASSERT_OK(req);
  glz::generic_i64 result;
  ASSERT_FALSE(glz::read_json(result, renderSearchResponseBody(req->responses.back()->proto)));
  EXPECT_EQ(1, result["ops"]["q"]["found"].get<int64_t>());
  // The facet leaves the id filter out, so it counts every root the join matches.
  EXPECT_EQ(4u, result["ops"]["q"]["ops"]["ids"]["buckets"].get_array().size());
}

TEST_F(NestedQueryTest, rootOnlyParsingPinsNoOtherViewOrRelation) {
  auto req = localReq(helper.getSearchEngine());
  req->topDocs().matchQuery("root_only", "root").countLimit().fields({"id"});
  req->execute(1); ASSERT_OK(req); EXPECT_EQ(1, req->getMatchCount());
  EXPECT_EQ(0u, req->scopedViewCountForTests());
}


TEST_F(NestedQueryTest, aNestedPathIsNotAScalarField) {
  for (auto text : {"items:red", "items:\"a b\"", "items:[1 TO 2]", "items:re*", "items:red~",
                    "items.parts:steel", "items:(parts:steel)", "items:()"}) {
    auto req = localReq(helper.getSearchEngine());
    req->topDocs().limit(0).exprQuery(text);
    req->execute(1);
    EXPECT_FALSE(req->ok()) << text;
  }
  for (auto json : {
      R"({"match":{"field":"items","val":"red"}})", R"({"phrase":{"field":"items","text":"quick fox"}})",
      R"({"prefix":{"field":"items","prefix":"re"}})", R"({"range":{"field":"items","gte":100}})",
      R"({"any_of":{"field":"items","values":["red"]}})", R"({"knn":{"field":"items","query":[0,0],"k":1}})",
      R"({"multi_match":{"text":"quick","fields":["items"]}})", R"({"simple_query":{"q":"quick","fields":["items"]}})"}) {
    auto req = localReq(helper.getSearchEngine());
    ASSERT_TRUE(api::read_json(req->topDocs().limit(0).rawQuery(), json, req->mr));
    req->execute(1);
    EXPECT_FALSE(req->ok()) << json;
  }
  EXPECT_EQ(4, count("items.color:(red blue)"));
  EXPECT_EQ(3, count("root_only:(items:(color:red))"));
}


} // namespace
} // namespace luxir::test
