// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/QueryBuild.h"
#include "test/NumericVerification.h"
#include "luxir/query/ParseContext.h"
#include "luxir/query/ProtobufQueryParser.h"
#include "luxir/query/ToParentQuery.h"
#include "luxir/query/NumericPredicateQuery.h"
#include "luxir/search/OwnershipRelation.h"
#include "luxir/reader/SkipStats.h"
#include "luxir/server/JsonResponse.h"
#include <glaze/glaze.hpp>

namespace luxir::test {
namespace {
class JoinModesTest : public LuxirTest {
protected:
  CollectionHelper helper;

  void SetUp() override {
    LuxirTest::SetUp();
    for (auto text : {
        R"({"id":"a","items_nested":[{"n_i":2,"parts_nested":[{"n_i":1},{"n_i":9}]},{"n_i":4,"parts_nested":{"n_i":6}},{"n_i":8}]})",
        R"({"id":"b","items_nested":[{"n_i":3},{"n_i":5}]})", R"({"id":"c","items_nested":{"n_i":7}})",
        R"({"id":"empty","items_nested":[null,{}]})"}) {
      CollectionHelper::UpdateBuilder input;
      input.addRaw([&](auto& doc, auto& mr) { ASSERT_TRUE(api::read_json(doc, text, mr)); });
      input.commit(); ASSERT_TRUE(helper.submit(input).success);
    }
  }

  static api::Query scored(LocalReq& req) {
    return qb::rescore(req.mr, qb::exists(req.mr, "n_i"), "col(\"n_i\")");
  }

  api::Query join(LocalReq& req, std::string_view mode, int min = 1, int max = INT32_MAX,
                   bool uniform = false, std::string_view path = "items_nested") {
    auto query = qb::toParent(req.mr, path, uniform ? qb::constantScore(req.mr, qb::exists(req.mr, "n_i"), 2.5f) : scored(req), mode);
    auto& spec = std::get<api::ToParentQuery>(query.kind);
    spec.min_children = min; spec.max_children = max;
    return query;
  }

  std::map<std::string, float> run(std::string_view mode, int min = 1, int max = INT32_MAX, bool uniform = false) {
    auto req = localReq(helper.getSearchEngine());
    req->topDocs().limit(-1).fields({"id"}).getScores().countLimit().rawQuery() = join(*req, mode, min, max, uniform);
    req->execute(1); EXPECT_OK(req);
    std::map<std::string, float> result;
    if (!req->ok() || req->docList()->row_count == 0) return result;
    auto ids = std::get<api::ColStr>(req->docList()->columns.at("id").kind).v;
    auto values = std::get<api::ColFloat>(req->docList()->columns.at("_score_").kind).v;
    for (size_t i = 0; i < ids.size(); i++) result.emplace(ids[i], values[i]);
    return result;
  }
};

TEST_F(JoinModesTest, reductionsAndUniformChildrenPreserveScoresAndBounds) {
  const char* modes[]{"none", "max", "min", "sum", "avg"};
  const float a[]{0,8,2,14,(float)(14.0/3)};
  const float b[]{0,5,3,8,4};
  for (int mode = 0; mode < 5; mode++) {
    SCOPED_TRACE(modes[mode]);
    auto result = run(modes[mode]);
    ASSERT_EQ(3, result.size());
    EXPECT_EQ(a[mode], result.at("a")); EXPECT_EQ(b[mode], result.at("b"));
    EXPECT_EQ(mode ? 7 : 0, result.at("c"));
    EXPECT_EQ(2, run(modes[mode], 2).size());
    auto bounded = run(modes[mode], 2, 2);
    ASSERT_EQ(1, bounded.size()); EXPECT_TRUE(bounded.contains("b"));
    auto uniform = run(modes[mode], 1, INT32_MAX, true);
    EXPECT_EQ(mode == 0 ? 0 : mode == 3 ? 7.5f : 2.5f, uniform.at("a"));
    EXPECT_EQ(mode == 0 ? 0 : mode == 3 ? 5 : 2.5f, uniform.at("b"));
    auto uniformBounded = run(modes[mode], 2, 2, true);
    ASSERT_EQ(1, uniformBounded.size());
    EXPECT_EQ(uniform.at("b"), uniformBounded.at("b"));
  }
  auto req = localReq(helper.getSearchEngine());
  req->topDocs().getScores().fields({"id"}).rawQuery() = qb::boost(req->mr, join(*req, "none"), 9);
  req->execute(1); ASSERT_OK(req);
  for (float value : std::get<api::ColFloat>(req->docList()->columns.at("_score_").kind).v) EXPECT_EQ(0, value);
}

TEST_F(JoinModesTest, sumAndAverageAccumulateInDoubleAndKeepNegativeScores) {
  CollectionHelper::UpdateBuilder input;
  input.addRaw([&](auto& doc,auto& mr) { ASSERT_TRUE(api::read_json(doc,
      R"({"id":"precise","items_nested":[{"n_i":16777216},{"n_i":1},{"n_i":-16777216}]})",mr)); });
  input.commit(); ASSERT_TRUE(helper.submit(input).success);
  EXPECT_EQ(1,run("sum").at("precise")); EXPECT_EQ((float)(1.0/3),run("avg").at("precise"));
  for (auto mode : {"sum","avg"}) {
    auto boosted = localReq(helper.getSearchEngine());
    boosted->topDocs().getScores().countLimit().limit(-1).fields({"id"}).rawQuery() = qb::boost(boosted->mr,join(*boosted,mode),0.7f);
    boosted->execute(1); ASSERT_OK(boosted);
    auto ids = std::get<api::ColStr>(boosted->docList()->columns.at("id").kind).v;
    auto values = std::get<api::ColFloat>(boosted->docList()->columns.at("_score_").kind).v;
    for (size_t i=0; i<ids.size(); i++) if (ids[i]=="a") {
      EXPECT_EQ((float)((mode==std::string_view("sum") ? 14.0 : 14.0/3)*(double)0.7f),values[i]);
    }
  }
  auto req = localReq(helper.getSearchEngine());
  auto query = qb::toParent(req->mr,"items_nested",qb::constantScore(req->mr,qb::exists(req->mr,"n_i"),-2.5f),"sum");
  std::get<api::ToParentQuery>(query.kind).max_children = 2;
  req->topDocs().getScores().limit(1).fields({"id"}).rawQuery() = query;
  req->execute(1); ASSERT_OK(req);
  EXPECT_EQ("c",std::get<api::ColStr>(req->docList()->columns.at("id").kind).v[0]);
  EXPECT_EQ(-2.5f,std::get<api::ColFloat>(req->docList()->columns.at("_score_").kind).v[0]);
}

TEST_F(JoinModesTest, composedPathAndNestedJoinsReduceAtTheRequestedLevel) {
  for (auto mode : {"min", "max", "sum", "avg", "none"}) {
    auto req = localReq(helper.getSearchEngine());
    req->topDocs("flat").getScores().countLimit().fields({"id"}).rawQuery() = join(*req, mode, 1, INT32_MAX, false, "items_nested.parts_nested");
    auto child = qb::toParent(req->mr, "parts_nested", scored(*req), mode);
    req->topDocs("levels").getScores().countLimit().fields({"id"}).rawQuery() = qb::toParent(req->mr, "items_nested", child, mode);
    req->execute(1); ASSERT_OK(req);
    auto score = [&](const char* op) { return std::get<api::ColFloat>(req->docList(op)->columns.at("_score_").kind).v[0]; };
    EXPECT_FLOAT_EQ(mode == std::string_view("avg") ? (float)(16.0/3) : mode == std::string_view("sum") ? 16 : mode == std::string_view("max") ? 9 : mode == std::string_view("min") ? 1 : 0, score("flat"));
    EXPECT_FLOAT_EQ(mode == std::string_view("avg") ? 5.5f : score("flat"), score("levels"));
  }
}

TEST_F(JoinModesTest, boundedMembershipStopsAtTheDecidingMatchInEveryArm) {
  for (int rows : {2000,20000}) {
  helper.clear();
  CollectionHelper::UpdateBuilder input;
  std::string text = R"({"id":"many","items_nested":[)";
  for (int i = 0; i < rows; i++) {
    if (i) text += ',';
    text += i==rows-1 ? "{\"n_i\":3}" : "{\"n_i\":1}";
  }
  text += "]}";
  input.addRaw([&](auto& doc, auto& mr) { ASSERT_TRUE(api::read_json(doc, text, mr)); });
  input.commit(); ASSERT_TRUE(helper.submit(input).success);
  auto root = helper.getIndexWriter()->snapshots.readers.getReader();
  auto many = root->view("items_nested");
  for (bool upper : {false,true}) for (int arm = 0; arm < 4; arm++) {
    SCOPED_TRACE(::testing::Message() << rows << '/' << upper << '/' << arm);
    MemPool pool; Query::Context context(pool, *many);
    NumericPredicateQuery query("items_nested.n_i", 0, 2);
    query.forcedArm = NumericPredicateQuery::ScorerArm::SPARSE_VERIFY;
    auto* child = query.createWeight(context, 0)->scorerSupplier(pool, many->segments()[0]);
    OwnershipRelation relation(*many, *root, context.planningContext().memoryTracker);
    RowReduction reduction{RowReduction::FIXED, upper ? 1 : 2, upper ? 2 : INT32_MAX};
    auto* source = relation.reduce(pool, 0, *child, nullptr, reduction, 0);
    auto demand = Query::PlanContext::fromLeadCost(1, Query::ExecutionUse::COUNT_WINDOWS);
    demand.membershipConsumption = arm == 0 ? Query::MembershipConsumption::WINDOWS : Query::MembershipConsumption::CANDIDATES;
    SkipStatsScope stats;
    auto* scorer = source->resolve(pool, demand)->build(pool);
    int verified = 0;
    NumericVerificationObserver observer([&](int32_t row) { verified++; EXPECT_LT(row,upper ? 3 : 2); });
    bool match;
    if (arm == 0) match = scorer->next() != PostingsReader::END;
    else if (arm == 1) match = scorer->advanceWindow(0,1) == 0;
    else {
      uint64_t bits = arm == 2 ? 1 : 0;
      if (arm == 2) scorer->intersectCandidateBits({&bits,1},0);
      else scorer->fillWindowBits({&bits,1},0,1);
      match = bits & 1;
    }
    EXPECT_EQ(!upper, match);
#ifndef NDEBUG
    EXPECT_EQ(upper ? 3 : 2, verified);
#endif
  }
  }
}

TEST_F(JoinModesTest, membershipKeysShareModesButNotBoundsInBothDirections) {
  CollectionHelper::UpdateBuilder merged; merged.commit(true,1); ASSERT_TRUE(helper.submit(merged).success);
  auto req = localReq(helper.getSearchEngine());
  req->reader = helper.getIndexWriter()->snapshots.readers.getReader(); req->schema = req->reader->schema();
  ParseView view(*req->schema, nullptr, &req->reader, req.get());
  ParseContext context(req->requestPool,*req->schema,req->arena,CoerceContext{},"q",nullptr,&view);
  ProtobufQueryParser parser(context);
  auto maxWire = join(*req,"max",2,2); auto sumWire = join(*req,"sum",2,2); auto different = join(*req,"sum",1,2);
  auto* maximum = parser.parse(maxWire); auto* sum = parser.parse(sumWire); auto* other = parser.parse(different);
  EXPECT_FALSE(maximum->equals(*sum)); EXPECT_NE(maximum->hash(),sum->hash());
  auto key = [](Query* query) {
    FilterKeyContext context; FilterKeyBuilder out;
    auto scope = query->appendFilterKey(out,context);
    return std::move(out).finishStructural(scope,context);
  };
  EXPECT_EQ(key(maximum),key(sum)); EXPECT_EQ(key(sum),key(maximum));
  EXPECT_NE(key(sum),key(other)); EXPECT_NE(sum->hash(),other->hash());
  for (bool reverse : {false,true}) {
    CollectionHelper data("mode_cache_"+std::to_string(reverse));
    auto cache = std::make_shared<FilterCache>(FilterCacheConfig{.maxBytes=1024*1024,.minSegmentDocs=0,.admissionThreshold=2});
    data.getIndexWriter()->snapshots.readers.filterCache = cache;
    CollectionHelper::UpdateBuilder input;
    for (int root=0; root<24; root++) {
      std::string doc = "{\"id\":\""+std::to_string(root)+"\",\"items_nested\":[";
      for (int i=0;i<root%3+1;i++) doc += i ? ",{\"label_s\":\"red\"}" : "{\"label_s\":\"red\"}";
      doc += "]}";
      input.addRaw([&](auto& value,auto& mr) { ASSERT_TRUE(api::read_json(value,doc,mr)); });
    }
    input.commit(); ASSERT_TRUE(data.submit(input).success);
    auto count = [&](std::string_view mode,int minimum) {
      auto request = localReq(data.getSearchEngine()); request->collection(data.collection().getId().name);
      auto query = qb::toParent(request->mr,"items_nested",qb::match(request->mr,"label_s","red"),mode);
      auto& spec = std::get<api::ToParentQuery>(query.kind); spec.min_children = minimum; spec.max_children = 2;
      request->topDocs().limit(1).countLimit().rawQuery() = qb::constantScore(request->mr, query, 1.0f);
      request->execute(1); EXPECT_OK(request); return request->getMatchCount();
    };
    EXPECT_EQ(8,count(reverse ? "sum" : "max",2)); EXPECT_EQ(8,count(reverse ? "sum" : "max",2));
    auto hits = cache->counters().hits;
    EXPECT_EQ(8,count(reverse ? "max" : "sum",2)); EXPECT_GT(cache->counters().hits,hits);
    auto admissions = cache->counters().admissions;
    EXPECT_EQ(16,count("sum",1)); EXPECT_EQ(16,count("sum",1));
    EXPECT_GT(cache->counters().admissions,admissions);
  }
}

TEST_F(JoinModesTest, denseCandidateAndWindowMasksCountShortFamiliesAndLargeTails) {
  helper.clear(); CollectionHelper::UpdateBuilder input;
  constexpr int N = 512;
  for (int owner=0; owner<N; owner++) {
    std::string doc = "{\"id\":\""+std::to_string(owner)+"\",\"items_nested\":[";
    for (int child=0; child<(owner==N-1 ? 2000 : 3); child++) {
      int value = owner==N-1 ? child%2 : owner%3==0 ? 1 : owner%3==1 ? child%2 : child==2;
      if (child) doc += ',';
      doc += "{\"n_i\":"+std::to_string(value)+"}";
    }
    doc += "]}";
    input.addRaw([&](auto& value,auto& mr) { ASSERT_TRUE(api::read_json(value,doc,mr)); });
  }
  input.commit(); ASSERT_TRUE(helper.submit(input).success);
  auto root = helper.getIndexWriter()->snapshots.readers.getReader(); auto many = root->view("items_nested");
  for (bool upper : {false,true}) for (bool fill : {false,true}) {
    MemPool pool; Query::Context context(pool,*many);
    NumericPredicateQuery query("items_nested.n_i",1,2);
    query.forcedArm = NumericPredicateQuery::ScorerArm::SPARSE_VERIFY;
    auto* child = query.createWeight(context,0)->scorerSupplier(pool,many->segments()[0]);
    OwnershipRelation relation(*many,*root,context.planningContext().memoryTracker);
    auto* source = relation.reduce(pool,0,*child,nullptr,{RowReduction::FIXED,upper ? 1 : 2,upper ? 2 : INT32_MAX},0);
    auto plan = Query::PlanContext::fromLeadCost(N,Query::ExecutionUse::COUNT_WINDOWS);
    plan.membershipConsumption = Query::MembershipConsumption::CANDIDATES;
    SkipStatsScope stats; auto* scorer = source->resolve(pool,plan)->build(pool);
    int verified = 0; NumericVerificationObserver observer([&](int32_t) { verified++; });
    std::array<uint64_t,N/64> bits; bits.fill(fill ? 1 : ~0ULL);
    if (fill) scorer->fillWindowBits(bits,0,N); else scorer->intersectCandidateBits(bits,0);
    EXPECT_GT(SkipStats::joinBoundaryRuns,0);
    for (int owner=0; owner<N; owner++) {
      bool many = owner==N-1 || owner%3==0;
      bool expected = (upper ? !many : many) || (fill && owner%64==0);
      EXPECT_EQ(expected,(bits[(size_t)owner>>6] & (1ULL<<(owner&63)))!=0);
    }
#ifndef NDEBUG
    EXPECT_LE(verified,3*N+3);
    EXPECT_GT(verified,0);
#endif
  }
  EXPECT_TRUE(run("none",INT32_MAX).empty());
}

TEST_F(JoinModesTest, fromMatchReplaysAllMatchingChildrenOfABoundedOwner) {
  auto req = localReq(helper.getSearchEngine());
  ASSERT_TRUE(api::read_json(req->rawRequest(),R"JSON({"ops":{"q":{"top_docs":{
    "query":{"name":"matches","to_parent":{"path":"items_nested","score":"sum","min_children":2,"max_children":2,
      "query":{"rescore":{"query":{"exists":{"field":"n_i"}},"expr":"col(\"n_i\")"}}}},
    "fields":["id"],"row_ops":{"children":{"domain":{"from_match":"matches"},"top_docs":{"fields":["n_i"],"count_limit":-1,"get_scores":true}}}
  }}}})JSON",req->mr));
  req->execute(1); ASSERT_OK(req);
  glz::generic_i64 json; ASSERT_FALSE(glz::read_json(json,renderSearchResponseBody(req->responses.back()->proto)));
  auto& rows = json["ops"]["q"]["docs"].get_array(); ASSERT_EQ(1,rows.size()); EXPECT_EQ("b",rows[0]["id"].get_string());
  auto& children = rows[0]["children"]["docs"].get_array(); ASSERT_EQ(2,children.size());
  EXPECT_EQ(5,children[0]["n_i"].get<int64_t>()); EXPECT_EQ(3,children[1]["n_i"].get<int64_t>());
}

TEST_F(JoinModesTest, invalidBoundsAndModesNameTheField) {
  for (auto [field,value] : {std::pair{"min_children","0"}, {"max_children","0"}, {"score","\"bogus\""}}) {
    auto req = localReq(helper.getSearchEngine());
    auto text = std::string(R"({"ops":{"q":{"top_docs":{"query":{"to_parent":{"path":"items_nested","query":{"all":true},")") + field + "\":" + value + "}}}}}}";
    ASSERT_TRUE(api::read_json(req->rawRequest(),text,req->mr)); req->execute(1);
    EXPECT_FALSE(req->ok()); EXPECT_NE(std::string::npos,req->errorMsg().find(field)) << req->errorMsg();
  }
}
} // namespace
} // namespace luxir::test
