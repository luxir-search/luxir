// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <format>
#include <map>
#include <glaze/glaze.hpp>
#include "luxir/server/JsonResponse.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/SchemaBuilder.h"

namespace luxir::test {
namespace {

// Chunk retrieval over nested rows: the chunks a vector search matched for
// each document (from_match on a kNN clause), chunks capped or collapsed per
// document (owner keys) and matched chunks beside collapsed, diversified and
// fused lists. Vectors lie on one axis, so with the query at the origin an L2
// score is 1 / (1 + x^2): 1, 0.5, 0.2, 0.1 and 1/17 for x = 0..4.
class NestedChunkSearchTest : public LuxirTest {
protected:
  CollectionHelper helper;

  void SetUp() override {
    LuxirTest::SetUp();
    SchemaBuilder schema;
    using Type = api::FieldDef::FieldClass;
    schema.field("chunks").type = Type::NESTED;
    for (auto name : {"chunks.t", "chunks.lang", "group"}) schema.field(name).type = Type::STRING;
    auto& vector = schema.field("chunks.v");
    vector.type = Type::VECTOR; vector.column = true;
    vector.metric = api::VectorMetric::L2; vector.dims = 2;
    schema.set(helper.collection());
    helper.getIndexWriter()->mergePolicy->setMergeFactor(1000);
    for (auto json : {
        R"({"id":"A","group":"g1","chunks":[{"t":"a0","lang":"en","v":[0,0]},{"t":"a1","lang":"fr","v":[1,0]},{"t":"a2","lang":"en","v":[3,0]}]})",
        R"({"id":"B","group":"g1","chunks":[{"t":"b0","lang":"en","v":[2,0]},{"t":"b1","lang":"en","v":[4,0]}]})",
        R"({"id":"C","group":"g2","chunks":[{"t":"c0","lang":"fr","v":[1,0]},{"t":"c1","lang":"en"}]})",
        R"({"id":"D","group":"g2"})"}) {
      CollectionHelper::UpdateBuilder input;
      input.addRaw([&](auto& doc, auto& arena) { EXPECT_TRUE(api::read_json(doc, json, arena)); });
      input.commit(); ASSERT_TRUE(helper.submit(input).success);
    }
  }

  void merge() {
    CollectionHelper::UpdateBuilder input;
    input.commit(true, 1); ASSERT_TRUE(helper.submit(input).success);
  }

  // The "q" operation of a request, or the error message in `error`.
  glz::generic_i64 search(std::string_view op, std::string* error = nullptr) {
    auto req = localReq(helper.getSearchEngine());
    auto json = "{\"ops\":{\"q\":" + std::string(op) + "}}";
    EXPECT_TRUE(api::read_json(req->rawRequest(), json, req->mr)) << json;
    req->execute(1);
    if (error) *error = req->ok() ? "" : req->errorMsg();
    else EXPECT_OK(req);
    if (!req->ok()) return {};
    glz::generic_i64 parsed;
    auto body = renderSearchResponseBody(req->responses.back()->proto);
    EXPECT_FALSE(glz::read_json(parsed, body)) << body;
    return parsed["ops"]["q"];
  }

  // "id:t=score,t=score|id:..." for the rows of a list and one row operation.
  static std::string rows(const glz::generic_i64& list, std::string_view op, std::string_view key = "id") {
    std::string out;
    for (const auto& row : list["docs"].get_array()) {
      if (!out.empty()) out += '|';
      out += row[key].get_string() + ':';
      bool first = true;
      for (const auto& child : row[op]["docs"].get_array()) {
        if (!first) out += ',';
        first = false;
        out += child["t"].get_string();
        if (child.contains("_score_")) out += '=' + std::format("{:.4g}", (double)child["_score_"].as_number());
      }
    }
    return out;
  }

  static std::string ids(const glz::generic_i64& list, std::string_view key) {
    std::string out;
    for (const auto& row : list["docs"].get_array()) out += (out.empty() ? "" : ",") + row[key].get_string();
    return out;
  }
};

constexpr std::string_view KNN = R"("knn":{"field":"chunks.v","query":[0,0],"k":3,"exact":true})";

TEST_F(NestedChunkSearchTest, matchedChunksOfARootKnnAreScoredExactlyWithinEachDocument) {
  for (bool merged : {false, true}) {
    SCOPED_TRACE(merged);
    if (merged) merge();
    auto list = search(R"({"top_docs":{"query":{"name":"near",)" + std::string(KNN) + R"(},
        "fields":["id"],"get_scores":true,"row_ops":{
          "chunks":{"domain":{"from_match":"near"},"top_docs":{"limit":2,"count_limit":-1,"fields":["t"],"get_scores":true}},
          "english":{"domain":{"from_match":"near","filter":["lang:en"]},"top_docs":{"fields":["t"],"get_scores":true}},
          "by_text":{"domain":{"from_match":"near"},"top_docs":{"sort":[{"field":"t","dir":"desc"}],"fields":["t"]}}}}})");
    // The documents are their best chunks; each lists its own chunks by the
    // same score, and a chunk without a vector is not a match.
    EXPECT_EQ("A:a0=1,a1=0.5|C:c0=0.5|B:b0=0.2,b1=0.05882", rows(list, "chunks"));
    std::vector<int64_t> found;
    for (const auto& row : list["docs"].get_array()) found.push_back(row["chunks"]["found"].get<int64_t>());
    EXPECT_EQ((std::vector<int64_t>{3, 1, 2}), found);
    EXPECT_EQ("A:a0=1,a2=0.1|C:|B:b0=0.2,b1=0.05882", rows(list, "english"));
    EXPECT_EQ("A:a2,a1,a0|C:c0|B:b1,b0", rows(list, "by_text"));
    // A boost on the named clause scales the replayed scores like the search's.
    auto boosted = search(R"({"top_docs":{"query":{"name":"near","boost":2,)" + std::string(KNN) + R"(},
        "fields":["id"],"limit":1,"row_ops":{
          "chunks":{"domain":{"from_match":"near"},"top_docs":{"limit":2,"fields":["t"],"get_scores":true}}}}})");
    EXPECT_EQ("A:a0=2,a1=1", rows(boosted, "chunks"));
  }
}

TEST_F(NestedChunkSearchTest, aJoinedKnnReplaysItsThresholdAndAnExplicitQueryOnlyRestricts) {
  // similarity 2.5 is an L2 distance: a2 (3) and b1 (4) are out.
  auto list = search(R"({"top_docs":{"query":{"name":"near","to_parent":{"path":"chunks","query":
        {"knn":{"field":"v","query":[0,0],"k":10,"exact":true,"similarity":2.5}}}},
      "fields":["id"],"sort":[{"field":"id"}],"row_ops":{
        "all":{"domain":{"from_match":"near"},"top_docs":{"fields":["t"],"get_scores":true}},
        "english":{"domain":{"from_match":"near"},"top_docs":{"query":"lang:en","fields":["t"]}},
        "constant":{"domain":{"from_match":"near"},"top_docs":{"query":{"constant_score":{"score":3,"query":"*:*"}},"fields":["t"],"get_scores":true}}}}})");
  EXPECT_EQ("A:a0=1,a1=0.5|B:b0=0.2|C:c0=0.5", rows(list, "all"));
  EXPECT_EQ("A:a0|B:b0|C:", rows(list, "english"));
  EXPECT_EQ("A:a0=3,a1=3|B:b0=3|C:c0=3", rows(list, "constant"));
}

TEST_F(NestedChunkSearchTest, onlyAKnnLeafReplays) {
  auto request = [&](std::string_view query) {
    return R"({"top_docs":{"query":)" + std::string(query) + R"(,"fields":["id"],"sort":[{"field":"id"}],"row_ops":{
        "m":{"domain":{"from_match":"m"},"top_docs":{"fields":["t"]}}}}})";
  };
  std::string error;
  search(request(R"({"name":"m","to_parent":{"path":"chunks","query":{"boolean":{
      "required":[{"knn":{"field":"v","query":[0,0],"k":10,"exact":true}}],"filter":[{"match":{"field":"lang","val":"en"}}]}}}})"), &error);
  EXPECT_NE(std::string::npos, error.find("name the kNN clause itself")) << error;
  // Named itself, the kNN replays without its siblings: a domain filter
  // restores them.
  auto list = search(request(R"({"to_parent":{"path":"chunks","query":{"boolean":{
      "required":[{"name":"m","knn":{"field":"v","query":[0,0],"k":10,"exact":true}}],"filter":[{"match":{"field":"lang","val":"en"}}]}}}})"));
  EXPECT_EQ("A:a0,a1,a2|B:b0,b1", rows(list, "m"));
}

TEST_F(NestedChunkSearchTest, chunksAreCappedAndCollapsedByTheirDocument) {
  for (bool merged : {false, true}) {
    SCOPED_TRACE(merged);
    if (merged) merge();
    // The top chunks, at most one per document.
    auto capped = search(R"({"domain":{"to":"chunks"},"top_docs":{"query":{"knn":{"field":"v","query":[0,0],"k":10,"exact":true}},
        "limit":3,"fields":["t","/id"],"diversify":{"owner":"root","max_per_key":1}}})");
    EXPECT_EQ("a0,c0,b0", ids(capped, "t"));
    EXPECT_EQ("A,C,B", ids(capped, "/id"));
    // k nearest documents, counted inside the chunk view: one best chunk each.
    auto grouped = search(R"({"domain":{"to":"chunks"},"top_docs":{"query":{"knn":{"field":"v","query":[0,0],"k":2,"exact":true,"unit":"groups"}},
        "limit":10,"fields":["t"],"collapse":{"owner":"root"}}})");
    EXPECT_EQ("a0,c0", ids(grouped, "t"));
  }
}

TEST_F(NestedChunkSearchTest, collapsedDiversifiedAndFusedListsCarryMatchedChunks) {
  const std::string rowOps = R"("row_ops":{"chunks":{"domain":{"from_match":"near"},"top_docs":{"limit":1,"fields":["t"]}}})";
  for (std::string keys : {R"("collapse":{"field":"group"})", R"("diversify":{"field":"group","max_per_key":1})"}) {
    SCOPED_TRACE(keys);
    auto list = search(R"({"top_docs":{"query":{"name":"near",)" + std::string(KNN) + "},\"fields\":[\"id\"]," + keys + "," + rowOps + "}}");
    EXPECT_EQ("A:a0|C:c0", rows(list, "chunks"));
  }
  // Fused rows replay a clause of either source, including on rows that the
  // other source found.
  std::string fusion = R"({"fusion":{"rrf":{},"fields":["id"],"sources":{
      "vec":{"query":{"name":"near",)" + std::string(KNN) + R"(},"limit":3},
      "text":{"query":{"name":"lexical","match":{"field":"chunks.t","val":"b1"}},"limit":3}},
    "row_ops":{
      "near":{"domain":{"from_match":"near"},"top_docs":{"limit":1,"fields":["t"]}},
      "lexical":{"domain":{"from_match":"lexical"},"top_docs":{"fields":["t"]}}})";
  auto fused = search(fusion + "}}");
  std::map<std::string, std::string> byId;
  for (const auto& row : fused["docs"].get_array()) {
    std::string matched;
    for (auto op : {"near", "lexical"}) {
      matched += std::string(op) + "=";
      for (const auto& child : row[op]["docs"].get_array()) matched += child["t"].get_string();
      matched += ';';
    }
    byId[row["id"].get_string()] = matched;
  }
  EXPECT_EQ((std::map<std::string, std::string>{
      {"A", "near=a0;lexical=;"}, {"B", "near=b0;lexical=b1;"}, {"C", "near=c0;lexical=;"}}), byId);
  // The same beside fused groups.
  auto groups = search(fusion + R"(,"collapse":{"field":"group"}}})");
  EXPECT_EQ(2u, groups["docs"].get_array().size());
  for (const auto& row : groups["docs"].get_array()) EXPECT_EQ(1u, row["near"]["docs"].get_array().size());
  // A clause of another list is still out of reach.
  std::string error;
  search(R"({"fusion":{"rrf":{},"sources":{"vec":{"query":{)" + std::string(KNN) + R"(}}},
      "row_ops":{"x":{"domain":{"from_match":"elsewhere"},"top_docs":{}}}}})", &error);
  EXPECT_NE(std::string::npos, error.find("unknown clause name")) << error;
}

}  // namespace
}  // namespace luxir::test
