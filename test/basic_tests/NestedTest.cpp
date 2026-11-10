// Copyright 2026 Luxir contributors
// SPDX-License-Identifier: Apache-2.0
// Nested block-join documents: ground truth checks on hand-built data.
#include "test/LuxirTest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/TestUtils.h"
#include "luxir/reader/NestedBlocks.h"
#include "luxir/search/IndexReader.h"

using namespace luxir;
using namespace luxir::test;

class NestedTest : public LuxirTest {
protected:
  void schema(CollectionHelper& helper) {
    std::pmr::monotonic_buffer_resource mr;
    api::SchemaDef def;
    std::string error;
    ASSERT_TRUE(api::read_json(def, R"({"fields":{
      "title_s":{"type":"string"},
      "answers":{"type":"nested","fields":{
        "user":{"type":"string"},
        "score":{"type":"int","column":true,"index":"range"},
        "body":{"type":"text","index":"match"}
      }}
    }})", mr, &error)) << error;
    helper.collection().setSchema(Schema::fromProto(def, Schema::createDefaultSchema().get()));
  }
  IndexResult update(CollectionHelper& helper, std::string_view body) {
    std::pmr::monotonic_buffer_resource mr;
    api::UpdateRequest req;
    std::string json = "{" + std::string(body) + R"(,"commit":{}})";
    if (!api::read_json(req, json, mr)) throw std::runtime_error(json);
    return helper.submit(req);
  }
  auto search(std::string_view body) {
    auto req = localReq(luxirNode->getSearchEngine());
    std::string json = R"({"collection":"main","document_format":"rows","count_limit":-1,"limit":100,)" + std::string(body) + "}";
    if (!api::read_json(req->rawRequest(), json, req->mr)) throw std::runtime_error(json);
    req->execute(1);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    return req;
  }
  int64_t count(std::string_view body = R"("query":{"all":true})") {
    return *search(body)->docList()->found;
  }
  auto segments() { return luxirNode->getCollection("main")->getReaderManager().getReader(); }
  // Physical live docs including hidden children.
  int64_t physicalLive() { return segments()->liveDocs(); }
};

TEST_F(NestedTest, blockCountsAndSourceRoundTrip) {
  CollectionHelper helper(*luxirNode);
  schema(helper);
  ASSERT_TRUE(update(helper, R"("docs":[
    {"id":"q1","title_s":"first","answers":[{"user":"alice","score":5,"body":"hello world"},{"user":"bob","score":1,"body":"foo bar"}]},
    {"id":"q2","title_s":"second","answers":[{"user":"carol","score":3,"body":"baz"}]},
    {"id":"q3","title_s":"third"}
  ])").success);
  EXPECT_EQ(3, count());
  EXPECT_EQ(6, physicalLive()); // 3 roots + 3 children
  auto req = search(R"("query":{"match":{"field":"id","val":"q1"}},"fields":["id","answers"])");
  ASSERT_EQ(1, req->docList()->docs.size());
  auto* val = req->docList()->docs[0].fields.find("answers");
  ASSERT_NE(nullptr, val);
  auto* arr = std::get_if<api::ArrVal>(&(*val)->kind);
  ASSERT_NE(nullptr, arr);
  ASSERT_EQ(2u, arr->v.size());
  EXPECT_EQ("alice", std::get<api::Map>(arr->v[0].kind).fields.at("user")->asString());
  EXPECT_EQ(1, std::get<api::Map>(arr->v[1].kind).fields.at("score")->asInt());
}

TEST_F(NestedTest, deleteAndOverwriteRemoveWholeBlocks) {
  CollectionHelper helper(*luxirNode);
  schema(helper);
  ASSERT_TRUE(update(helper, R"("docs":[{"id":"q1","answers":[{"user":"a"},{"user":"b"}]},{"id":"q2","answers":[{"user":"c"}]}])").success);
  EXPECT_EQ(5, physicalLive());
  ASSERT_TRUE(update(helper, R"("docs":[{"id":"q1","answers":[{"user":"z"}]}])").success);
  EXPECT_EQ(2, count());
  EXPECT_EQ(4, physicalLive());
  ASSERT_TRUE(update(helper, R"("delete_ids":["q1","q2"])").success);
  EXPECT_EQ(0, count());
  EXPECT_EQ(0, physicalLive());
  // in-request overwrite inside one inverter
  ASSERT_TRUE(update(helper, R"("docs":[{"id":"x","answers":[{"user":"a"},{"user":"b"}]},{"id":"x","answers":[{"user":"c"}]}])").success);
  EXPECT_EQ(1, count());
  EXPECT_EQ(2, physicalLive());
}

TEST_F(NestedTest, mergeKeepsBlocksIntact) {
  CollectionHelper helper(*luxirNode);
  schema(helper);
  for (int i = 0; i < 4; i++) {
    ASSERT_TRUE(update(helper, R"("docs":[{"id":"q)" + std::to_string(i) + R"(","answers":[{"user":"a"},{"user":"b"}]}])").success);
  }
  ASSERT_TRUE(update(helper, R"("delete_ids":["q1"])").success);
  std::pmr::monotonic_buffer_resource mr;
  api::UpdateRequest merge;
  ASSERT_TRUE(api::read_json(merge, R"({"commit":{"wait_for_merges":true,"max_segments":1}})", mr));
  ASSERT_TRUE(helper.submit(merge).success);
  auto reader = segments();
  ASSERT_EQ(1u, reader->segments().size());
  auto& seg = reader->segments()[0];
  EXPECT_EQ(9, seg.maxDoc());
  EXPECT_FALSE(seg.hasDeletes());
  auto blocks = seg.postingsReaderShared()->nestedBlocks();
  ASSERT_NE(nullptr, blocks);
  EXPECT_EQ(3, count());
  // Each root is preceded by exactly its two children.
  int roots = 0;
  for (int32_t d = 0; d < seg.maxDoc(); ) {
    EXPECT_TRUE(blocks->isChild(d)); EXPECT_TRUE(blocks->isChild(d + 1));
    EXPECT_FALSE(blocks->isChild(d + 2));
    EXPECT_EQ(d + 2, blocks->rootOf(d));
    roots++; d += 3;
  }
  EXPECT_EQ(3, roots);
}

TEST_F(NestedTest, badBlockRollsBackOnlyThatDocument) {
  CollectionHelper helper(*luxirNode);
  schema(helper);
  auto result = update(helper, R"("docs":[{"id":"good1","answers":[{"user":"a"}]},
    {"id":"bad","answers":[{"user":"b"},{"user":"c","score":"not a number"}]},
    {"id":"good2"}])");
  ASSERT_EQ(1u, result.errors.size()); // PARTIAL: the other two documents applied
  EXPECT_EQ(1, result.errors[0].index);
  EXPECT_EQ(2, count());
  EXPECT_EQ(3, physicalLive());
}

TEST_F(NestedTest, malformedAndUnknownFieldsRejected) {
  CollectionHelper helper(*luxirNode);
  schema(helper);
  auto a = update(helper, R"("docs":[{"id":"a","answers":["not-an-object"]}])");
  EXPECT_FALSE(a.success);
  auto b = update(helper, R"("docs":[{"id":"b","answers":[{"nope":1}]}])");
  EXPECT_FALSE(b.success);
  EXPECT_EQ(0, count());
  EXPECT_EQ(0, physicalLive());
}

TEST_F(NestedTest, schemaRejectsNestedInsideNested) {
  std::pmr::monotonic_buffer_resource mr;
  api::SchemaDef def;
  ASSERT_TRUE(api::read_json(def, R"({"fields":{"a":{"type":"nested","fields":{"b":{"type":"nested","fields":{"c":{"type":"string"}}}}}}})", mr));
  EXPECT_THROW(Schema::fromProto(def, Schema::createDefaultSchema().get()), SchemaError);
}

// Correlation: two child clauses must match the same child, across segments.
TEST_F(NestedTest, nestedQueryCorrelatesSameChildAcrossSegments) {
  CollectionHelper helper(*luxirNode);
  schema(helper);
  ASSERT_TRUE(update(helper, R"("docs":[
    {"id":"cross","answers":[{"user":"alice","score":1,"body":"red"},{"user":"bob","score":9,"body":"blue"}]},
    {"id":"same","answers":[{"user":"alice","score":9,"body":"red"}]}
  ])").success);
  ASSERT_TRUE(update(helper, R"("docs":[{"id":"later","answers":[{"user":"alice","score":8,"body":"red green"}]}])").success);
  // user alice AND score >= 5 must be one child: cross has alice(1), bob(9), so no match.
  auto req = search(R"("query":{"nested":{"path":"answers","query":{"boolean":{"required":[
      {"match":{"field":"answers.user","val":"alice"}},{"range":{"field":"answers.score","gte":5}}]}}}},
      "fields":["id"],"sort":["id"])");
  std::vector<std::string> got;
  for (const auto& doc : req->docList()->docs) got.emplace_back(doc.fields.at("id")->asString());
  EXPECT_EQ((std::vector<std::string>{"later", "same"}), got);
  EXPECT_EQ(2, req->docList()->found);
}

TEST_F(NestedTest, nestedScoreModesAndInnerHits) {
  CollectionHelper helper(*luxirNode);
  schema(helper);
  ASSERT_TRUE(update(helper, R"("docs":[{"id":"a","answers":[{"user":"x","body":"red red red"},{"user":"y","body":"red"}]}])").success);
  auto scoreFor = [&](std::string_view mode) {
    auto req = search("\"query\":{\"nested\":{\"path\":\"answers\",\"score_mode\":\"" + std::string(mode)
        + "\",\"query\":{\"match\":{\"field\":\"answers.body\",\"val\":\"red\"}}}},\"get_scores\":true,\"fields\":[\"id\",\"_score_\"]");
    return std::get<float>(req->docList()->docs.at(0).fields.at("_score_")->kind);
  };
  float avg = scoreFor("avg"), sum = scoreFor("sum"), max = scoreFor("max"), min = scoreFor("min"), none = scoreFor("none");
  EXPECT_NEAR(sum / 2, avg, 1e-5);
  EXPECT_GT(max, min);
  EXPECT_FLOAT_EQ(1.0f, none);
  auto req = search(R"("query":{"nested":{"path":"answers","query":{"match":{"field":"answers.user","val":"y"}},
    "inner_hits":{"name":"hit","limit":2,"fields":["user"]}}},"fields":["id"])");
  ASSERT_EQ(1u, req->docList()->matches.size());
  auto* list = req->docList()->matches[0].fields.at("hit")->docList();
  ASSERT_EQ(1u, list->docs.size());
  EXPECT_EQ("y", list->docs[0].fields.at("user")->asString());
}

// A block that fails partway leaves dead children with no root; they must not
// attach to the next root, and a trailing failed block must be harmless.
TEST_F(NestedTest, deadChildrenOfFailedBlocksStayInvisible) {
  CollectionHelper helper(*luxirNode);
  schema(helper);
  auto first = update(helper, R"("docs":[
    {"id":"bad","answers":[{"user":"b"},{"user":"c","score":"not a number"}]},
    {"id":"good","answers":[{"user":"d","score":1}]},
    {"id":"tail","answers":[{"user":"e"},{"user":"f","score":"not a number"}]}
  ])");
  ASSERT_EQ(2u, first.errors.size());
  EXPECT_EQ(1, count());
  auto matches = [&](std::string_view user) {
    return search("\"query\":{\"nested\":{\"path\":\"answers\",\"query\":{\"match\":{\"field\":\"answers.user\",\"val\":\""
        + std::string(user) + "\"}},\"inner_hits\":{\"name\":\"h\"}}},\"fields\":[\"id\"]");
  };
  EXPECT_TRUE(matches("b")->docList()->docs.empty());
  EXPECT_TRUE(matches("e")->docList()->docs.empty());
  auto good = matches("d");
  ASSERT_EQ(1u, good->docList()->docs.size());
  EXPECT_EQ("good", good->docList()->docs[0].fields.at("id")->asString());
  EXPECT_EQ("d", good->docList()->matches[0].fields.at("h")->docList()->docs[0].fields.at("user")->asString());
}
