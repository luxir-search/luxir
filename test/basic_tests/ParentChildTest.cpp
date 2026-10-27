// Copyright 2026 Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#include "test/LuxirTest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/TestUtils.h"
#include "luxir/search/SearchOverrides.h"

using namespace luxir;
using namespace luxir::test;

class ParentChildTest : public LuxirTest {
protected:
  void schema(CollectionHelper& helper) {
    std::pmr::monotonic_buffer_resource mr;
    api::SchemaDef def;
    ASSERT_TRUE(api::read_json(def, R"({"relations":{"variants":{"fields":{
      "color":{"type":"string","index":"match","column":true},
      "price":{"type":"int","index":"range","column":true},
      "text":{"type":"text","index":"match"},
      "vec":{"type":"vector","column":true,"dims":2,"metric":"l2"}
    }}}})", mr));
    helper.collection().setSchema(Schema::fromProto(def, Schema::createDefaultSchema().get()));
  }
  IndexResult update(CollectionHelper& helper, std::string_view body) {
    std::pmr::monotonic_buffer_resource mr;
    api::UpdateRequest req;
    std::string json = "{" + std::string(body) + R"(,"commit":{}})";
    std::string error;
    if (!api::read_json(req, json, mr, &error)) throw std::runtime_error(error + json);
    return helper.submit(req);
  }
  auto search(std::string_view body, int parallel = 1, bool ok = true) {
    auto req = localReq(luxirNode->getSearchEngine());
    std::string prefix = body.starts_with("\"ops\"") ? R"({"collection":"main",)"
        : R"({"collection":"main","document_format":"rows","get_scores":true,"count_limit":-1,"limit":100,)";
    std::string json = prefix + std::string(body) + "}";
    if (!api::read_json(req->rawRequest(), json, req->mr)) throw std::runtime_error(json);
    req->execute(parallel);
    EXPECT_EQ(ok, req->ok()) << req->errorMsg();
    return req;
  }
  std::vector<std::string> ids(const LocalReq& req, std::string_view name = "q") {
    std::vector<std::string> result;
    for (auto* response : req.responses) {
      auto* q = response->proto.ops.find(name);
      if (!q) continue;
      for (const auto& doc : (*q)->docList()->docs) result.emplace_back(doc.fields.at("id")->asString());
    }
    return result;
  }
  static std::string join(std::string_view query, std::string_view extra = {}) {
    return R"("query":{"has_child":{"relation":"variants","query":)" + std::string(query)
        + R"(,"inner_hits":{"name":"matched","limit":3})" + std::string(extra) + "}}";
  }
};

TEST_F(ParentChildTest, correlatedFiltersAndActualParentAndWitness) {
  CollectionHelper helper;
  schema(helper);
  auto indexed = update(helper, R"("families":[
    {"parent":{"id":"p1","title_s":"shirt"},"children":{"variants":[
      {"id":"red-expensive","color":"red","price":100,"text":"cotton shirt"},
      {"id":"blue-cheap","color":"blue","price":10,"text":"cotton shirt"}]}},
    {"parent":{"id":"p2","title_s":"hat"},"children":{"variants":[
      {"id":"red-cheap","color":"red","price":20,"text":"cotton hat"}]}}
  ])");
  ASSERT_TRUE(indexed.success) << indexed.error_message;
  EXPECT_EQ(2u, indexed.families.size());
  auto all = search(R"("query":{"all":true})");
  ASSERT_EQ(2u, ids(*all).size());
  for (int parallel : {1, -1}) {
    auto req = search(join(R"({"boolean":{"required":[{"match":{"field":"color","val":"red"}},{"range":{"field":"price","lt":50}}]}})"), parallel);
    ASSERT_EQ(std::vector<std::string>{"p2"}, ids(*req));
    EXPECT_EQ("hat", req->docList()->docs[0].fields.at("title_s")->asString());
    ASSERT_EQ(1u, req->docList()->matches.size());
    auto* matches = req->docList()->matches[0].fields.at("matched")->docList();
    ASSERT_NE(nullptr, matches);
    ASSERT_EQ(1u, matches->docs.size());
    EXPECT_EQ("red-cheap", matches->docs[0].fields.at("id")->asString());
  }
}

TEST_F(ParentChildTest, independentChildrenDeleteReplaceAndMerge) {
  CollectionHelper helper;
  schema(helper);
  auto first = update(helper, R"("families":[{"parent":{"id":"p"},"children":{"variants":[{"id":"a","color":"red"}]}}])");
  ASSERT_TRUE(first.success) << first.error_message;
  auto token = first.families.at("p");
  auto mutate = [&](std::string_view body) {
    return update(helper, R"("child_updates":[{"parent_id":"p","relation":"variants","family":")" + token + R"(",)" + std::string(body) + "}]");
  };
  ASSERT_TRUE(mutate(R"("docs":[{"id":"b","color":"blue"}])").success);
  auto joined = search(join(R"({"all":true})"));
  ASSERT_EQ(1u, ids(*joined).size());
  EXPECT_EQ(2, joined->docList()->matches[0].fields.at("matched")->docList()->found);
  ASSERT_TRUE(mutate(R"("delete_ids":["a"])").success);
  EXPECT_EQ(0u, ids(*search(join(R"({"match":{"field":"color","val":"red"}})"))).size());
  ASSERT_TRUE(helper.deleteByIds({"p"}, UpdateMessage::COMMIT).success);
  EXPECT_TRUE(ids(*search(join(R"({"all":true})"))).empty());
  ASSERT_TRUE(update(helper, R"("families":[{"parent":{"id":"p"},"children":{"variants":[{"id":"c","color":"green"}]}}])").success);
  ASSERT_TRUE(mutate(R"("docs":[{"id":"late","color":"blue"}])").success);
  EXPECT_TRUE(ids(*search(join(R"({"match":{"field":"color","val":"blue"}})"))).empty());
  CollectionHelper::UpdateBuilder merge;
  merge.commit(true, 1);
  ASSERT_TRUE(helper.submit(merge).success);
  auto after = search(join(R"({"all":true})"));
  ASSERT_EQ(1u, ids(*after).size());
  EXPECT_EQ("c", after->docList()->matches[0].fields.at("matched")->docList()->docs[0].fields.at("id")->asString());
}

TEST_F(ParentChildTest, familyFailureRollsBackDeletesAndParentOverwrite) {
  CollectionHelper helper;
  schema(helper);
  ASSERT_TRUE(update(helper, R"("families":[{"parent":{"id":"p","title_s":"old"},"children":{"variants":[{"id":"a","price":20}]}}])").success);
  auto failed = update(helper, R"("delete_ids":["p"],"families":[{"parent":{"id":"p","title_s":"new"},"children":{"variants":[{"id":"b","price":{} }]}}])");
  EXPECT_FALSE(failed.success);
  EXPECT_TRUE(failed.families.empty());
  auto req = search(join(R"({"all":true})"));
  ASSERT_EQ(std::vector<std::string>{"p"}, ids(*req));
  EXPECT_EQ("old", req->docList()->docs[0].fields.at("title_s")->asString());
}

TEST_F(ParentChildTest, vectorKCountsParentsAndPrefiltersBothScopes) {
  CollectionHelper helper;
  schema(helper);
  std::string children;
  for (int i = 0; i < 40; ++i) {
    if (i) children += ',';
    children += R"({"id":")" + std::to_string(i) + R"(","color":"red","vec":[0,0]})";
  }
  ASSERT_TRUE(update(helper, R"("families":[{"parent":{"id":"dense","eligible_s":"yes"},"children":{"variants":[)" + children + R"(]}},
    {"parent":{"id":"second","eligible_s":"yes"},"children":{"variants":[{"id":"good","color":"red","vec":[1,0]},{"id":"wrong","color":"blue","vec":[0,0]}]}},
    {"parent":{"id":"blocked","eligible_s":"no"},"children":{"variants":[{"id":"x","color":"red","vec":[0,0]}]}}
  ])").success);
  for (bool exact : {true, false}) for (int parallel : {1, -1}) {
    auto req = search(join(R"({"knn":{"field":"vec","query":[0,0],"k":2,"exact":)" + std::string(exact ? "true" : "false") + "}}",
      R"(,"filter":[{"match":{"field":"color","val":"red"}}])") + R"(,"filter":["eligible_s:yes"])", parallel);
    ASSERT_EQ((std::vector<std::string>{"dense", "second"}), ids(*req));
    ASSERT_EQ(2u, req->docList()->matches.size());
    EXPECT_EQ("good", req->docList()->matches[1].fields.at("matched")->docList()->docs[0].fields.at("id")->asString());
  }
}

TEST_F(ParentChildTest, exactReducersCountsPhraseAndWrappers) {
  CollectionHelper helper;
  schema(helper);
  ASSERT_TRUE(update(helper, R"("families":[
    {"parent":{"id":"two"},"children":{"variants":[{"id":"a","text":"red cotton shirt","price":2},{"id":"b","text":"red cotton hat","price":6}]}},
    {"parent":{"id":"one"},"children":{"variants":[{"id":"c","text":"cotton red shirt","price":4}]}}
  ])").success);
  for (auto [mode, expected] : std::vector<std::pair<std::string, float>>{{"max", 6}, {"min", 2}, {"sum", 8}, {"avg", 4}, {"none", 1}}) {
    auto req = search(join(R"({"rescore":{"query":{"all":true},"expr":"price"}})",
        R"(,"score_mode":")" + mode + R"(","min_children":2,"max_children":2)"));
    ASSERT_EQ(std::vector<std::string>{"two"}, ids(*req));
    EXPECT_FLOAT_EQ(expected, std::get<float>(req->docList()->docs[0].fields.at("_score_")->kind));
    auto* hits = req->docList()->matches[0].fields.at("matched")->docList();
    EXPECT_EQ(2, hits->found);
    EXPECT_EQ("b", hits->docs[0].fields.at("id")->asString());
  }
  auto phrase = search(join(R"({"phrase":{"field":"text","words":["red","cotton"]}})"));
  EXPECT_EQ(std::vector<std::string>{"two"}, ids(*phrase));
  for (std::string wrapper : {"constant_score", "boost", "rescore"}) {
    std::string extra = wrapper == "boost" ? R"(,"boost":2)" : wrapper == "rescore" ? R"(,"expr":"score")" : "";
    auto req = search(R"("query":{")" + wrapper + R"(":{"query":{"has_child":{"relation":"variants","query":{"all":true},"inner_hits":{"name":"w"}}})" + extra + "}}");
    ASSERT_EQ(2u, ids(*req).size());
    EXPECT_EQ(2u, req->docList()->matches.size());
  }
}

TEST_F(ParentChildTest, fusionCombinesParentsWithDifferentMatchingChildren) {
  CollectionHelper helper;
  schema(helper);
  ASSERT_TRUE(update(helper, R"("families":[
    {"parent":{"id":"both"},"children":{"variants":[{"id":"lex","text":"cotton","vec":[100,0]},{"id":"vec","text":"silk","vec":[0,0]}]}},
    {"parent":{"id":"lexonly"},"children":{"variants":[{"id":"lex","text":"cotton","vec":[200,0]}]}}
  ])").success);
  auto req = search(R"("ops":{"f":{"fusion":{"rrf":{},"document_format":"rows","limit":10,"sources":{
    "text":{"limit":10,"query":{"has_child":{"relation":"variants","query":{"match":{"field":"text","val":"cotton"}},"inner_hits":{"name":"text"}}}},
    "vector":{"limit":10,"query":{"has_child":{"relation":"variants","query":{"knn":{"field":"vec","query":[0,0],"k":1,"exact":true}},"inner_hits":{"name":"vector"}}}}
  }}}})", -1);
  ASSERT_EQ(2u, ids(*req, "f").size());
  EXPECT_EQ("both", ids(*req, "f")[0]);
  ASSERT_EQ(2u, req->docList("f")->matches.size());
  auto& witnesses = req->docList("f")->matches[0].fields;
  EXPECT_EQ("lex", witnesses.at("text")->docList()->docs[0].fields.at("id")->asString());
  EXPECT_EQ("vec", witnesses.at("vector")->docList()->docs[0].fields.at("id")->asString());
}

TEST_F(ParentChildTest, streamsWitnessesAndRejectsInvalidScopes) {
  CollectionHelper helper;
  schema(helper);
  std::string families;
  for (int i = 0; i < 12; ++i) {
    if (i) families += ',';
    families += R"({"parent":{"id":")" + std::to_string(i) + R"("},"children":{"variants":[{"id":"v","color":"red"}]}})";
  }
  ASSERT_TRUE(update(helper, R"("families":[)" + families + "]").success);
  auto req = search(join(R"({"all":true})") + R"(,"batch_size":3)", -1);
  ASSERT_EQ(12u, ids(*req).size());
  size_t count = 0;
  for (auto* response : req->responses) {
    auto* q = response->proto.ops.find("q");
    if (!q) continue;
    auto* list = (*q)->docList();
    EXPECT_EQ(list->docs.size(), list->matches.size());
    count += list->matches.size();
    std::string json;
    EXPECT_TRUE(api::write_json(response->proto, json));
    EXPECT_NE(std::string::npos, json.find("matched"));
  }
  EXPECT_EQ(12u, count);
  EXPECT_FALSE(search(R"("query":{"has_child":{"relation":"missing"}})", 1, false)->ok());
  EXPECT_FALSE(search(join(R"({"has_child":{"relation":"variants"}})"), 1, false)->ok());
  EXPECT_FALSE(search(join(R"({"all":true})", R"(,"min_children":0)"), 1, false)->ok());
  EXPECT_FALSE(update(helper, R"("docs":[{"id":"x","luxir_relation_s":"variants"}])").success);
}

TEST_F(ParentChildTest, optionalBranchEvidenceMustMatchItsParentPredicate) {
  CollectionHelper helper;
  schema(helper);
  ASSERT_TRUE(update(helper, R"("families":[{"parent":{"id":"p","eligible_s":"no"},"children":{"variants":[{"id":"v","color":"red"}]}}])").success);
  auto req = search(R"("query":{"boolean":{"optional":[{"all":true},{"boolean":{"required":[
    {"match":{"field":"eligible_s","val":"yes"}},
    {"has_child":{"relation":"variants","query":{"all":true},"inner_hits":{"name":"rejected"}}}
  ]}}]}})");
  ASSERT_EQ(std::vector<std::string>{"p"}, ids(*req));
  EXPECT_TRUE(req->docList()->matches.empty());
}

TEST_F(ParentChildTest, vectorMatchesIndependentExhaustiveParentOracle) {
  CollectionHelper helper;
  schema(helper);
  struct Child { std::string parent, id; float x, y; bool eligible; };
  std::vector<Child> corpus;
  uint32_t seed = 9841;
  auto random = [&]() { seed = seed * 1664525u + 1013904223u; return seed; };
  for (int segment = 0; segment < 3; ++segment) {
    std::string families;
    for (int p = segment * 13; p < (segment + 1) * 13; ++p) {
      if (!families.empty()) families += ',';
      families += R"({"parent":{"id":")" + std::to_string(p) + R"(","eligible_s":")" + (p % 4 ? "yes" : "no") + R"("},"children":{"variants":[)";
      for (int c = 0; c < 9; ++c) {
        if (c) families += ',';
        float x = (float)(random() % 10001) / 1000, y = (float)(random() % 13001) / 1000;
        bool eligible = p % 4 && c % 3;
        corpus.push_back({std::to_string(p), std::to_string(c), x, y, eligible});
        families += R"({"id":")" + std::to_string(c) + R"(","color":")" + (c % 3 ? "red" : "blue")
            + R"(","vec":[)" + std::to_string(x) + ',' + std::to_string(y) + "]}";
      }
      families += "]}}";
    }
    ASSERT_TRUE(update(helper, R"("families":[)" + families + "]").success);
  }
  for (float query : {0.1f, 3.4f, 8.7f}) {
    std::map<std::string, std::pair<float, std::string>> best;
    for (const auto& child : corpus) if (child.eligible) {
      float d = (child.x - query) * (child.x - query) + child.y * child.y;
      auto at = best.find(child.parent);
      if (at == best.end() || d < at->second.first) best[child.parent] = {d, child.id};
    }
    std::vector<std::pair<float, std::string>> expected;
    for (const auto& [parent, child] : best) expected.emplace_back(child.first, parent);
    std::sort(expected.begin(), expected.end());
    for (int k : {1, 7, 100}) for (int parallel : {1, -1}) {
      auto req = search(join(R"({"knn":{"field":"vec","query":[)" + std::to_string(query)
        + R"(,0],"k":)" + std::to_string(k) + R"(,"exact":true}})",
        R"(,"filter":[{"match":{"field":"color","val":"red"}}])") + R"(,"filter":["eligible_s:yes"])", parallel);
      auto actual = ids(*req);
      ASSERT_EQ(std::min((size_t)k, expected.size()), actual.size());
      for (size_t i = 0; i < actual.size(); ++i) {
        EXPECT_EQ(expected[i].second, actual[i]);
        EXPECT_EQ(best.at(actual[i]).second, req->docList()->matches[i].fields.at("matched")->docList()->docs[0].fields.at("id")->asString());
      }
    }
  }
}

TEST_F(ParentChildTest, protobufRoundTripAndSchemaPersistence) {
  CollectionHelper helper;
  schema(helper);
  std::pmr::monotonic_buffer_resource mr;
  api::SchemaDef saved;
  helper.collection().getSchema()->toProto(&saved, mr);
  std::vector<std::byte> bytes;
  ASSERT_TRUE(api::encode(saved, bytes));
  api::SchemaDef decodedSchema;
  ASSERT_TRUE(api::decode(decodedSchema, api::copyToPaddedInput(bytes, mr), mr));
  ASSERT_EQ(1u, decodedSchema.relations.size());
  helper.collection().setSchema(Schema::fromProto(decodedSchema));
  api::UpdateRequest request;
  ASSERT_TRUE(api::read_json(request, R"({"families":[{"parent":{"id":"p"},"children":{"variants":[{"id":"v","color":"red"}]}}],"commit":{}})", mr));
  bytes.clear();
  ASSERT_TRUE(api::encode(request, bytes));
  api::UpdateRequest decoded;
  ASSERT_TRUE(api::decode(decoded, api::copyToPaddedInput(bytes, mr), mr));
  ASSERT_TRUE(helper.submit(decoded).success);
  auto query = localReq(luxirNode->getSearchEngine());
  api::SearchRequest searchRequest;
  ASSERT_TRUE(api::read_json(searchRequest, R"({"collection":"main","query":{"has_child":{"relation":"variants","query":{"simple_query":{"q":"red","fields":["color"]}},"inner_hits":{"name":"w"}}},"document_format":"rows","fields":["id","_family_"]})", mr));
  bytes.clear();
  ASSERT_TRUE(api::encode(searchRequest, bytes));
  ASSERT_TRUE(api::decode(query->rawRequest(), api::copyToPaddedInput(bytes, query->mr), query->mr));
  query->execute(1);
  ASSERT_TRUE(query->ok()) << query->errorMsg();
  ASSERT_EQ(std::vector<std::string>{"p"}, ids(*query));
  EXPECT_FALSE(query->docList()->docs[0].fields.at("_family_")->asString().empty());
  bytes.clear();
  ASSERT_TRUE(api::encode(query->responses[0]->proto, bytes));
  api::SearchResponse response;
  ASSERT_TRUE(api::decode(response, api::copyToPaddedInput(bytes, mr), mr));
  EXPECT_EQ("v", response.ops.at("q")->docList()->matches[0].fields.at("w")->docList()->docs[0].fields.at("id")->asString());
}

TEST_F(ParentChildTest, resetFacetDomainsRemainParentScoped) {
  CollectionHelper helper;
  schema(helper);
  ASSERT_TRUE(update(helper, R"("families":[
    {"parent":{"id":"p1"},"children":{"variants":[{"id":"a","color":"red"},{"id":"b","color":"red"}]}},
    {"parent":{"id":"p2"},"children":{"variants":[{"id":"c","color":"blue"}]}}
  ])").success);
  auto req = search(join(R"({"match":{"field":"color","val":"red"}})") + R"(,"ops":{
    "all":{"query_facet":{"buckets":{"all":{"all":true}},"ops":{
      "page":{"top_docs":{"query":{"all":true},"count_limit":-1,"document_format":"rows","fields":["id"]},
              "domain":{"query":{"all":true},"apply_parent_filters":false}}
    }},"domain":{"query":{"all":true},"apply_parent_filters":false}}
  })", -1);
  ASSERT_TRUE(req->ok());
  ASSERT_EQ(std::vector<std::string>{"p1"}, ids(*req));
  auto* facet = req->docList()->ops.at("all")->facetResult();
  ASSERT_NE(nullptr, facet);
  ASSERT_EQ(1u, facet->counts.size());
  EXPECT_EQ(2, facet->counts[0]);
  const auto& pages = std::get<api::ArrVal>(facet->ops.at("page")->kind);
  ASSERT_EQ(1u, pages.v.size());
  EXPECT_EQ(2, pages.v[0].docList()->found);
}

TEST_F(ParentChildTest, requestMemoryLimitsDomainsAndWitnessGrowth) {
  CollectionHelper helper;
  schema(helper);
  std::string children;
  for (int i = 0; i < 300; ++i) {
    if (i) children += ',';
    children += R"({"id":")" + std::to_string(i) + R"(","color":"red"})";
  }
  ASSERT_TRUE(update(helper, R"("families":[{"parent":{"id":"p"},"children":{"variants":[)" + children + "]}}]").success);
  for (size_t budget : {128u, 4096u}) {
    SearchOverridesGuard guard(forcedRequestMemoryMaxBytes);
    forcedRequestMemoryMaxBytes = budget;
    auto req = search(R"("query":{"has_child":{"relation":"variants","inner_hits":{"name":"all","limit":1000}}})", 1, false);
    EXPECT_NE(std::string::npos, req->errorMsg().find("parent-child state"));
    EXPECT_EQ(0u, req->memoryTracker.bytes());
  }
  auto req = search(join(R"({"all":true})"));
  ASSERT_EQ(std::vector<std::string>{"p"}, ids(*req));
  EXPECT_EQ(300, req->docList()->matches[0].fields.at("matched")->docList()->found);
}
