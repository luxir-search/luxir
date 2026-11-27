// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "luxir/search/SearchOverrides.h"
#include "luxir/server/JsonRequest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

namespace {

using Buckets = std::vector<std::pair<std::string, int64_t>>;

std::string label(const api::Column& ids, size_t i) {
  if (auto* strs = std::get_if<api::ColStr>(&ids.kind)) return std::string(strs->v[i]);
  if (auto* ints = std::get_if<api::ColInt>(&ids.kind)) return std::to_string(ints->v[i]);
  return "?";
}

}  // namespace

// FieldFacet.sort by `_count` and `_key`, `after` paging, other_count, and
// numeric term_filter lists, over string, integer, and text fields.
class FacetOrderTest : public LuxirTest {
protected:
  CollectionHelper helper;

  // cat_s: a=5 b=3 c=3 d=1 e=2; n_i mirrors it as 1..5; body_t one word each.
  // Two segments, and one document per row has no value.
  void SetUp() override {
    LuxirTest::SetUp();
    struct Row { const char* cat; int64_t n; int count; };
    const std::vector<Row> rows{{"a", 1, 5}, {"b", 2, 3}, {"c", 3, 3}, {"d", 4, 1}, {"e", 5, 2}};
    int id = 0;
    for (int segment = 0; segment < 2; segment++) {
      std::vector<Doc> docs;
      for (const auto& row : rows) {
        for (int i = 0; i < row.count; i++) {
          if (i % 2 != segment) continue;
          docs.push_back(flatdoc("id", "d" + std::to_string(id++), "cat_s", row.cat,
                                 "n_i", row.n, "body_t", row.cat, "v_i", (int64_t)i));
        }
      }
      docs.push_back(flatdoc("id", "m" + std::to_string(segment), "v_i", (int64_t)7));
      ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
    }
  }

  struct Result {
    Buckets buckets;
    std::optional<int64_t> other;
    std::string error;
  };

  Result run(const std::string& facet, const std::string& query = "") {
    auto req = localReq(helper.getSearchEngine());
    std::string json = R"({"ops":{"f":{"field_facet":)" + facet
        + (query.empty() ? "" : R"(,"domain":{"query":")" + query + "\"}") + "}}}";
    parseQueryRequest(json, req->rawRequest(), req->mr);
    req->collection("main");
    req->execute();
    Result result;
    if (!req->ok()) {
      result.error = req->errorMsg();
      return result;
    }
    const auto& facetResult = *req->responses[0]->proto.ops.at("f")->facetResult();
    for (size_t i = 0; i < facetResult.counts.size(); i++) {
      result.buckets.emplace_back(label(*facetResult.bucket_ids, i), facetResult.counts[i]);
    }
    result.other = facetResult.other_count;
    return result;
  }

  Buckets ok(const std::string& facet, const std::string& query = "") {
    auto result = run(facet, query);
    EXPECT_TRUE(result.error.empty()) << result.error;
    return result.buckets;
  }

  // The same body on the string, integer, and text fields, as string labels.
  void expectAll(const std::string& body, const Buckets& strings) {
    Buckets ints;
    for (const auto& [key, count] : strings) ints.emplace_back(std::to_string(key[0] - 'a' + 1), count);
    SCOPED_TRACE(body);
    EXPECT_EQ(strings, ok(R"({"field":"cat_s",)" + body + "}"));
    EXPECT_EQ(ints, ok(R"({"field":"n_i",)" + body + "}"));
    EXPECT_EQ(strings, ok(R"({"field":"body_t",)" + body + "}"));
  }
};

TEST_F(FacetOrderTest, countAndKeyOrders) {
  SearchOverridesGuard guard(forcedFacetCounterMode);
  for (auto mode : {FacetCounterMode::AUTO, FacetCounterMode::FORCE_VECTOR,
                    FacetCounterMode::FORCE_SKINNY, FacetCounterMode::FORCE_HASH,
                    FacetCounterMode::SPAN_GLOBAL}) {
    forcedFacetCounterMode = mode;
    SCOPED_TRACE((int)mode);
    expectAll(R"("limit":3,"sort":["_count asc"])", {{"d", 1}, {"e", 2}, {"b", 3}});
    expectAll(R"("limit":2,"sort":["_count asc","_key desc"])", {{"d", 1}, {"e", 2}});
    expectAll(R"("limit":3,"sort":["_count desc","_key desc"])", {{"a", 5}, {"c", 3}, {"b", 3}});
    expectAll(R"("limit":2,"sort":["_key asc"])", {{"a", 5}, {"b", 3}});
    expectAll(R"("limit":2,"sort":["_key desc","_count asc"])", {{"e", 2}, {"d", 1}});
    expectAll(R"("limit":2,"sort":["_count desc"])", {{"a", 5}, {"b", 3}});
  }
}

TEST_F(FacetOrderTest, afterPagesInKeyOrder) {
  for (std::string field : {"cat_s", "body_t"}) {
    auto base = R"({"field":")" + field + R"(",)";
    EXPECT_EQ((Buckets{{"c", 3}, {"d", 1}}), ok(base + R"("limit":2,"sort":["_key asc"],"after":"b"})"));
    EXPECT_EQ((Buckets{{"c", 3}, {"b", 3}}), ok(base + R"("limit":2,"sort":["_key desc"],"after":"d"})"));
    // A start that is not a value: the page resumes after it.
    EXPECT_EQ((Buckets{{"c", 3}}), ok(base + R"("limit":1,"sort":["_key"],"after":"bb"})"));
    EXPECT_TRUE(ok(base + R"("sort":["_key"],"after":"e"})").empty());
  }
  EXPECT_EQ((Buckets{{"4", 1}}), ok(R"({"field":"n_i","limit":1,"sort":["_key"],"after":3})"));
  EXPECT_EQ((Buckets{{"2", 3}}), ok(R"({"field":"n_i","limit":1,"sort":["_key desc"],"after":3})"));
  EXPECT_NE(std::string::npos, run(R"({"field":"cat_s","after":"b"})").error.find("requires sort to begin with _key"));
}

TEST_F(FacetOrderTest, zeroCountsInOtherOrders) {
  // v_i:2 matches one document each of a, b, and c.
  auto page = [&](const std::string& body) { return ok(R"({"field":"cat_s","mincount":0,)" + body + "}", "v_i:2"); };
  EXPECT_EQ((Buckets{{"d", 0}, {"e", 0}}), page(R"("limit":2,"sort":["_count asc"])"));
  EXPECT_EQ((Buckets{{"e", 0}, {"d", 0}}), page(R"("limit":2,"sort":["_count asc","_key desc"])"));
  EXPECT_EQ((Buckets{{"e", 0}, {"d", 0}}), page(R"("limit":2,"sort":["_key desc"])"));
  EXPECT_EQ((Buckets{{"a", 1}, {"b", 1}}), page(R"("limit":2,"sort":["_key"])"));
  EXPECT_EQ((Buckets{{"d", 0}, {"c", 1}}),
            page(R"("limit":2,"sort":["_key desc"],"term_filter":{"exclude":["e"]})"));
  EXPECT_EQ((Buckets{{"d", 0}, {"e", 0}}), page(R"("limit":2,"sort":["_key"],"after":"c")"));
}

TEST_F(FacetOrderTest, otherCount) {
  // 14 values in all, minus the page.
  for (std::string field : {"cat_s", "n_i", "body_t"}) {
    SCOPED_TRACE(field);
    auto base = R"({"field":")" + field + R"(","other_count":true)";
    EXPECT_EQ(std::optional<int64_t>(6), run(base + R"(,"limit":2})").other);
    EXPECT_EQ(std::optional<int64_t>(13), run(base + R"(,"limit":1,"sort":["_count asc"]})").other);
    EXPECT_EQ(std::optional<int64_t>(0), run(base + R"(,"limit":-1})").other);
    // Values below mincount are not on the page, so they count as other.
    EXPECT_EQ(std::optional<int64_t>(3), run(base + R"(,"limit":5,"mincount":3})").other);
    // after: only d=1 and e=2 can be buckets.
    std::string after = field == "n_i" ? "3" : "\"c\"";
    EXPECT_EQ(std::optional<int64_t>(2),
              run(base + R"(,"limit":1,"sort":["_key"],"after":)" + after + "}").other);
  }
  EXPECT_FALSE(run(R"({"field":"cat_s","limit":2})").other.has_value());
  // v_i:0 matches one document of each value.
  EXPECT_EQ(std::optional<int64_t>(4), run(R"({"field":"cat_s","limit":1,"other_count":true})", "v_i:0").other);
  // term_filter: only kept terms count.
  EXPECT_EQ(std::optional<int64_t>(3),
            run(R"({"field":"cat_s","limit":1,"other_count":true,"term_filter":{"exclude":["a","b"]}})").other);
  EXPECT_EQ(std::optional<int64_t>(5),
            run(R"({"field":"cat_s","limit":0,"other_count":true,"term_filter":{"prefix":"a"}})").other);
  EXPECT_EQ(std::optional<int64_t>(3),
            run(R"({"field":"n_i","limit":1,"other_count":true,"term_filter":{"include":["2","3"]}})").other);
}

TEST_F(FacetOrderTest, otherCountAcrossStrategies) {
  SearchOverridesGuard guard(forcedFieldFacetStrategy);
  for (auto strategy : {FieldFacetStrategy::AUTO, FieldFacetStrategy::TOP_TERMS,
                        FieldFacetStrategy::COLUMN_DOMAIN}) {
    forcedFieldFacetStrategy = strategy;
    SCOPED_TRACE((int)strategy);
    auto result = run(R"({"field":"cat_s","limit":2,"other_count":true})");
    EXPECT_EQ((Buckets{{"a", 5}, {"b", 3}}), result.buckets);
    EXPECT_EQ(std::optional<int64_t>(6), result.other);
  }
}

TEST_F(FacetOrderTest, metricSortWithCountTieBreak) {
  // max(v_i): a=4, b=2, c=2, d=0, e=1. b and c tie on it and on count.
  auto body = R"j("limit":3,"sort":["top desc","_count asc"],"other_count":true,
                 "ops":{"top":"max(v_i)"})j";
  EXPECT_EQ((Buckets{{"a", 5}, {"b", 3}, {"c", 3}}), ok(R"({"field":"cat_s",)" + std::string(body) + "}"));
  EXPECT_EQ(std::optional<int64_t>(3), run(R"({"field":"n_i",)" + std::string(body) + "}").other);
  EXPECT_NE(std::string::npos,
            run(R"({"field":"cat_s","sort":["_count","_count desc"]})").error.find("duplicate sort key"));
}

TEST_F(FacetOrderTest, numericTermFilter) {
  EXPECT_EQ((Buckets{{"3", 3}, {"5", 2}}),
            ok(R"({"field":"n_i","term_filter":{"include":["3","5","9"]}})"));
  EXPECT_EQ((Buckets{{"2", 3}, {"3", 3}}),
            ok(R"({"field":"n_i","limit":2,"term_filter":{"exclude":["1"]}})"));
  // With an inline metric sort the filter still picks the buckets.
  EXPECT_EQ((Buckets{{"2", 3}}),
            ok(R"j({"field":"n_i","limit":1,"sort":["top desc"],"ops":{"top":"max(v_i)"},
                   "term_filter":{"include":["2","4"]}})j"));
  EXPECT_NE(std::string::npos,
            run(R"({"field":"n_i","term_filter":{"prefix":"1"}})").error.find("only include and exclude"));
  EXPECT_FALSE(run(R"({"field":"n_i","term_filter":{"include":["x"]}})").error.empty());
}
