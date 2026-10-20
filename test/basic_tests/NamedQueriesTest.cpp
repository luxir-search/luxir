// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "luxir/server/JsonRequest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

// Named queries: Query.name ("_name" in JSON) on any clause, reported per hit
// as _matched_queries_ when get_matched_queries is set.
class NamedQueriesTest : public LuxirTest {
protected:
  using Names = std::map<std::string, std::vector<std::string>>;
  using Scores = std::map<std::string, std::map<std::string, float>>;

  // d1..d4 over two segments.
  static void indexDocs(CollectionHelper& h) {
    h.index(flatdoc("id", "d1", "body_w", "red apple"), UpdateMessage::NO_COMMIT);
    h.index(flatdoc("id", "d2", "body_w", "green apple"), UpdateMessage::COMMIT);
    h.index(flatdoc("id", "d3", "body_w", "red car"), UpdateMessage::NO_COMMIT);
    h.index(flatdoc("id", "d4", "body_w", "blue apple pie"), UpdateMessage::COMMIT);
  }

  static LocalReqHandle run(CollectionHelper& h, const std::string& json, int parallel = 1) {
    auto req = localReq(h.getSearchEngine());
    parseQueryRequest(json, req->rawRequest(), req->mr);
    req->collection("main");
    req->execute(parallel);
    EXPECT_TRUE(req->ok()) << req->errorMsg() << json;
    return req;
  }

  // Every batch of op `op`: id -> matched names, from rows or columns.
  static Names names(const LocalReq& req, std::string_view op = "q") {
    Names out;
    for (auto* response : req.responses) {
      auto* val = response->proto.ops.find(op);
      if (val == nullptr) continue;
      const auto& list = *(**val).docList();
      if (const auto* idCol = list.columns.find("id")) {
        auto idv = std::get<api::ColStr>(idCol->kind).v;
        const auto* matched = list.columns.find("_matched_queries_");
        for (size_t i = 0; i < idv.size(); i++) {
          auto& entry = out[std::string(idv[i])];
          if (matched == nullptr) continue;
          for (auto n : std::get<api::ArrArrStr>(matched->kind).v[i].v) entry.emplace_back(n);
        }
        continue;
      }
      for (const auto& row : list.docs) {
        auto& entry = out[std::string(std::get<std::string_view>((*row.fields.find("id"))->kind))];
        const auto* matched = row.fields.find("_matched_queries_");
        if (matched == nullptr) continue;
        for (auto n : std::get<api::ArrStr>((*matched)->kind).v) entry.emplace_back(n);
      }
    }
    return out;
  }

  static Scores scores(const LocalReq& req) {
    Scores out;
    for (auto* response : req.responses) {
      const auto& list = *(**response->proto.ops.find("q")).docList();
      for (const auto& row : list.docs) {
        auto& entry = out[std::string(std::get<std::string_view>((*row.fields.find("id"))->kind))];
        const auto* matched = row.fields.find("_matched_queries_");
        if (matched == nullptr) continue;
        for (const auto& [name, v] : std::get<api::Map>((*matched)->kind).fields) {
          entry[std::string(name)] = std::get<float>(v->kind);
        }
      }
    }
    return out;
  }

  static std::string topDocs(const std::string& query, const std::string& extra = "") {
    return R"({"query":)" + query + R"(,"fields":["id"],"limit":-1,"document_format":"rows",)"
        R"("get_matched_queries":true)" + extra + "}";
  }
};

TEST_F(NamedQueriesTest, boolClausesAndFilters) {
  CollectionHelper h;
  indexDocs(h);
  for (int parallel : {1, -1}) {
    auto req = run(h, topDocs(R"({"boolean":{
        "required":[{"match":{"body_w":"apple"},"_name":"must_apple"}],
        "optional":[{"match":{"body_w":"red"},"_name":"should_red"},
                    {"match":{"body_w":"green"},"_name":"should_green"}],
        "prohibited":[{"match":{"body_w":"pie"},"_name":"not_pie"}],
        "filter":[{"exists":{"field":"body_w"},"_name":"has_body"}]}})",
        R"(,"filter":[{"match":{"body_w":"apple"},"_name":"top_filter"}])"), parallel);
    // Unmatched named clauses (should_green on d1, not_pie anywhere) are absent.
    EXPECT_EQ((Names{{"d1", {"must_apple", "should_red", "has_body", "top_filter"}},
                     {"d2", {"must_apple", "should_green", "has_body", "top_filter"}}}),
              names(*req));
  }
}

TEST_F(NamedQueriesTest, nestedNamesAndUnnamedHits) {
  CollectionHelper h;
  indexDocs(h);
  auto req = run(h, topDocs(R"({"_name":"outer","boolean":{"optional":[
      {"boolean":{"optional":[{"match":{"body_w":"car"},"_name":"inner_car"}]}},
      {"match":{"body_w":"pie"}}]}})"));
  EXPECT_EQ((Names{{"d3", {"outer", "inner_car"}}, {"d4", {"outer"}}}), names(*req));
}

// The same clause twice is deduplicated by Boolean normalization; both names
// still report. A reused name merges its clauses.
TEST_F(NamedQueriesTest, dedupedClausesAndMergedNames) {
  CollectionHelper h;
  indexDocs(h);
  auto req = run(h, topDocs(R"({"boolean":{"optional":[
      {"match":{"body_w":"red"},"_name":"a"},{"match":{"body_w":"red"},"_name":"b"},
      {"match":{"body_w":"green"},"_name":"color"},{"match":{"body_w":"blue"},"_name":"color"}]}})"));
  EXPECT_EQ((Names{{"d1", {"a", "b"}}, {"d2", {"color"}}, {"d3", {"a", "b"}}, {"d4", {"color"}}}),
            names(*req));
}

// Each name maps to its own clause's score, the score that clause gets as a
// query by itself.
TEST_F(NamedQueriesTest, scores) {
  CollectionHelper h;
  indexDocs(h);
  auto req = run(h, topDocs(R"({"boolean":{"optional":[
      {"match":{"body_w":"red"},"_name":"red"},{"match":{"body_w":"apple"},"_name":"apple"}]}})",
      R"(,"include_named_queries_score":true)"));
  auto got = scores(*req);
  for (std::string term : {"red", "apple"}) {
    auto alone = run(h, R"({"query":{"match":{"body_w":")" + term
        + R"("}},"fields":["id","_score_"],"limit":-1,"document_format":"rows"})");
    const auto& list = *(**alone->responses.back()->proto.ops.find("q")).docList();
    ASSERT_FALSE(list.docs.empty());
    for (const auto& row : list.docs) {
      std::string id(std::get<std::string_view>((*row.fields.find("id"))->kind));
      EXPECT_FLOAT_EQ(std::get<float>((*row.fields.find("_score_"))->kind), got[id][term]) << id;
    }
  }
  EXPECT_EQ(2u, got["d1"].size());
  EXPECT_EQ(1u, got["d3"].size());
}

// Batches of one row each, over two segments; names only are a column under
// columns and adaptive, like _score_.
TEST_F(NamedQueriesTest, batchesAcrossSegmentsInColumns) {
  CollectionHelper h;
  indexDocs(h);
  for (std::string format : {"columns", "adaptive"}) {
    auto req = run(h, R"({"query":{"boolean":{"optional":[
        {"match":{"body_w":"apple"},"_name":"apple"},{"match":{"body_w":"red"},"_name":"red"}]}},
        "sort":"id asc","fields":["id"],"limit":-1,"batch_size":1,"document_format":")"
        + format + R"(","get_matched_queries":true})", -1);
    EXPECT_EQ(4u, req->responses.size()) << format;
    EXPECT_EQ((Names{{"d1", {"apple", "red"}}, {"d2", {"apple"}}, {"d3", {"red"}},
                     {"d4", {"apple"}}}),
              names(*req)) << format;
  }
}

// With scores, _matched_queries_ is a row map even when every field is a
// column; a row matching no named clause leaves it out.
TEST_F(NamedQueriesTest, scoresBesideColumns) {
  CollectionHelper h;
  indexDocs(h);
  auto req = run(h, R"({"query":{"boolean":{"optional":[
      {"match":{"body_w":"red"},"_name":"red"},{"match":{"body_w":"apple"}}]}},
      "sort":"id asc","fields":["id"],"limit":-1,"document_format":"columns",
      "get_matched_queries":true,"include_named_queries_score":true})");
  const auto& list = *(**req->responses.back()->proto.ops.find("q")).docList();
  ASSERT_NE(nullptr, list.columns.find("id"));
  EXPECT_EQ(nullptr, list.columns.find("_matched_queries_"));
  ASSERT_EQ(4u, list.docs.size());
  for (size_t i = 0; i < list.docs.size(); i++) {
    const auto* matched = list.docs[i].fields.find("_matched_queries_");
    bool red = i == 0 || i == 2;  // d1, d3
    ASSERT_EQ(red, matched != nullptr) << i;
    if (red) EXPECT_EQ(1u, std::get<api::Map>((*matched)->kind).fields.size());
  }
}

// A fused document lists the names it matches in any source.
TEST_F(NamedQueriesTest, fusion) {
  CollectionHelper h;
  indexDocs(h);
  auto req = run(h, R"({"ops":{"f":{"fusion":{"rrf":{},"sources":{
      "text":{"query":{"match":{"body_w":"red"},"_name":"red"}},
      "other":{"query":{"match":{"body_w":"apple"},"_name":"apple"}}},
      "fields":["id"],"document_format":"rows","get_matched_queries":true}}}})");
  auto got = names(*req, "f");
  std::ranges::sort(got["d1"]);  // source order is not part of the contract
  EXPECT_EQ((Names{{"d1", {"apple", "red"}}, {"d2", {"apple"}}, {"d3", {"red"}},
                   {"d4", {"apple"}}}),
            got);
}

TEST_F(NamedQueriesTest, notRequestedOrUnnamed) {
  CollectionHelper h;
  indexDocs(h);
  auto off = run(h, R"({"query":{"match":{"body_w":"red"},"_name":"red"},"fields":["id"],)"
      R"("document_format":"rows"})");
  EXPECT_EQ((Names{{"d1", {}}, {"d3", {}}}), names(*off));
  auto unnamed = run(h, topDocs(R"({"match":{"body_w":"red"}})"));
  EXPECT_EQ((Names{{"d1", {}}, {"d3", {}}}), names(*unnamed));
}

// Collapsed and diversified lists report on the documents they return.
TEST_F(NamedQueriesTest, collapseAndDiversify) {
  CollectionHelper h;
  h.index(flatdoc("id", "g1", "body_w", "red apple", "grp_s", "a"), UpdateMessage::NO_COMMIT);
  h.index(flatdoc("id", "g2", "body_w", "green apple", "grp_s", "a"), UpdateMessage::COMMIT);
  h.index(flatdoc("id", "g3", "body_w", "blue apple", "grp_s", "b"), UpdateMessage::COMMIT);
  for (std::string shape : {R"("collapse":"grp_s")",
                            R"("diversify":{"field":"grp_s","max_per_key":1})"}) {
    auto req = run(h, topDocs(R"({"boolean":{
        "required":[{"match":{"body_w":"apple"},"_name":"apple"}],
        "optional":[{"match":{"body_w":"red"},"_name":"red"}]}})", "," + shape));
    EXPECT_EQ((Names{{"g1", {"apple", "red"}}, {"g3", {"apple"}}}), names(*req)) << shape;
  }
}
