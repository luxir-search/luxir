// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "luxir/api/luxir_types.hpp"
#include "luxir/server/JsonRequest.h"
#include "luxir/util/log.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/TestUtils.h"

namespace {

namespace api = luxir::api;
using namespace luxir;
using namespace luxir::test;

class SketchOpsTest : public LuxirTest {
protected:
  CollectionHelper helper;

  LocalReqHandle run(std::string_view json) {
    auto request = localReq(helper.getSearchEngine());
    parseQueryRequest(json, request->rawRequest(), request->mr);
    request->execute();
    return request;
  }

  // Ops of the top_docs op "q".
  const api::Val& op(const LocalReqHandle& request, std::string_view name) {
    return *request->docList("q")->ops.at(name);
  }

  static std::string json(const api::Val& value) {
    std::string out;
    EXPECT_TRUE(api::write_json(value, out));
    return out;
  }

  static double keyedValue(const api::Val& value, std::string_view key) {
    return std::get<api::Map>(value.kind).fields.at(key)->asDouble();
  }

  void expectError(std::string_view request, std::string_view text) {
    ExpectLog quiet("Search request rejected");
    auto result = run(request);
    ASSERT_FALSE(result->ok());
    EXPECT_NE(std::string::npos, result->errorMsg().find(text))
        << result->errorMsg();
  }
};

TEST_F(SketchOpsTest, CardinalityIsExactBelowThresholdAcrossSegments) {
  // Three segments with overlapping values, multi-valued tags, and gaps.
  helper.indexAll({
    flatdoc("id", "1", "ip_s", "10.0.0.1", "size_i", 100, "tags_ss", vecs("a", "b"), "lat_d", 1.5),
    flatdoc("id", "2", "ip_s", "10.0.0.2", "size_i", 200),
  }, UpdateMessage::COMMIT);
  helper.indexAll({
    flatdoc("id", "3", "ip_s", "10.0.0.1", "size_i", 100, "tags_ss", vecs("b", "c")),
    flatdoc("id", "4", "size_i", 300, "lat_d", 2.5),
  }, UpdateMessage::COMMIT);
  helper.indexAll({
    flatdoc("id", "5", "ip_s", "10.0.0.3", "tags_ss", vecs("c", "d"), "lat_d", 1.5),
  }, UpdateMessage::COMMIT);

  auto request = run(R"json({"ops":{"q":{"top_docs":{"limit":0,"ops":{
    "ips":{"cardinality":{"field":"ip_s"}},
    "sizes":{"cardinality":{"field":"size_i"}},
    "tags":{"cardinality":{"field":"tags_ss"}},
    "lats":{"cardinality":{"field":"lat_d","precision_threshold":10}},
    "ids":{"cardinality":{"field":"id"}}
  }}}}})json");
  ASSERT_TRUE(request->ok()) << request->errorMsg();
  EXPECT_EQ(3, op(request, "ips").asInt());
  EXPECT_EQ(3, op(request, "sizes").asInt());
  EXPECT_EQ(4, op(request, "tags").asInt());
  EXPECT_EQ(2, op(request, "lats").asInt());
  EXPECT_EQ(5, op(request, "ids").asInt());
  EXPECT_EQ("3", json(op(request, "ips")));

  // The incoming domain restricts the values counted.
  auto again1 = run(R"json({"ops":{"q":{"top_docs":{"query":"size_i:[150 TO *]",
    "limit":0,"ops":{"ips":{"cardinality":{"field":"ip_s"}}}}}}})json");
  ASSERT_TRUE(again1->ok()) << again1->errorMsg();
  EXPECT_EQ(1, op(again1, "ips").asInt());
}

TEST_F(SketchOpsTest, CardinalityEstimateOnRandomData) {
  std::mt19937_64 rng(5);
  std::uniform_int_distribution<int> user(0, 19999);
  std::set<int> distinctUsers;
  std::set<int64_t> distinctValues;
  for (int segment = 0; segment < 4; segment++) {
    std::vector<Doc> docs;
    for (int i = 0; i < 6000; i++) {
      int u = user(rng);
      int64_t value = (int64_t)(rng() % 50000);
      distinctUsers.insert(u);
      distinctValues.insert(value);
      docs.push_back(flatdoc("id", std::to_string(segment * 6000 + i),
                             "user_s", "user-" + std::to_string(u),
                             "value_i", value));
    }
    helper.indexAll(docs, UpdateMessage::COMMIT);
  }

  auto request = run(R"json({"ops":{"q":{"top_docs":{"limit":0,"ops":{
    "users":{"cardinality":{"field":"user_s"}},
    "values":{"cardinality":{"field":"value_i"}},
    "users_exact":{"cardinality":{"field":"user_s","precision_threshold":40000}}
  }}}}})json");
  ASSERT_TRUE(request->ok()) << request->errorMsg();
  // Default threshold 3000 -> 4096 registers, 1.6% standard error.
  auto withinError = [](int64_t estimate, size_t exact) {
    return std::fabs((double)estimate - (double)exact) / (double)exact < 0.065;
  };
  EXPECT_PRED2(withinError, op(request, "users").asInt(), distinctUsers.size());
  EXPECT_PRED2(withinError, op(request, "values").asInt(), distinctValues.size());
  EXPECT_EQ((int64_t)distinctUsers.size(), op(request, "users_exact").asInt());
}

// Fraction of sorted values the answer x misses q by, in rank space.
double rankError(const std::vector<double>& sorted, double x, double q) {
  auto lo = std::lower_bound(sorted.begin(), sorted.end(), x);
  auto hi = std::upper_bound(sorted.begin(), sorted.end(), x);
  double loRank = (double)(lo - sorted.begin()) / (double)sorted.size();
  double hiRank = (double)(hi - sorted.begin()) / (double)sorted.size();
  if (q < loRank) return loRank - q;
  if (q > hiRank) return q - hiRank;
  return 0.0;
}

TEST_F(SketchOpsTest, PercentilesMatchExactWithinRankError) {
  std::mt19937_64 rng(9);
  std::lognormal_distribution<double> latency(3.0, 1.0);
  std::vector<double> latencies;
  std::vector<double> sizes;
  for (int segment = 0; segment < 3; segment++) {
    std::vector<Doc> docs;
    for (int i = 0; i < 4000; i++) {
      double lat = latency(rng);
      int64_t size = (int64_t)(rng() % 100000);
      latencies.push_back(lat);
      sizes.push_back((double)size);
      sizes.push_back((double)(size + 1));
      docs.push_back(flatdoc("id", std::to_string(segment * 4000 + i),
                             "lat_d", lat, "sizes_is", vec_i(size, size + 1)));
    }
    // Documents without values contribute nothing.
    docs.push_back(flatdoc("id", "none" + std::to_string(segment)));
    helper.indexAll(docs, UpdateMessage::COMMIT);
  }
  std::sort(latencies.begin(), latencies.end());
  std::sort(sizes.begin(), sizes.end());

  auto request = run(R"json({"ops":{"q":{"top_docs":{"limit":0,"ops":{
    "lat":{"percentiles":{"field":"lat_d","percents":[1,50,95,99,99.9]}},
    "sizes":{"percentiles":{"field":"sizes_is"}}
  }}}}})json");
  ASSERT_TRUE(request->ok()) << request->errorMsg();

  const auto& lat = op(request, "lat");
  for (auto [key, q] : {std::pair{"1.0", 0.01}, {"50.0", 0.5}, {"95.0", 0.95},
                        {"99.0", 0.99}, {"99.9", 0.999}}) {
    EXPECT_LE(rankError(latencies, keyedValue(lat, key), q), 0.01) << key;
  }
  const auto& sizesResult = op(request, "sizes");
  const auto& fields = std::get<api::Map>(sizesResult.kind).fields;
  ASSERT_EQ(7u, fields.size());
  std::vector<std::string_view> keys;
  for (const auto& [key, value] : fields) keys.push_back(key);
  EXPECT_EQ((std::vector<std::string_view>{
                "1.0", "5.0", "25.0", "50.0", "75.0", "95.0", "99.0"}), keys);
  for (auto [key, q] : {std::pair{"1.0", 0.01}, {"25.0", 0.25}, {"50.0", 0.5},
                        {"99.0", 0.99}}) {
    EXPECT_LE(rankError(sizes, keyedValue(sizesResult, key), q), 0.01) << key;
  }
}

TEST_F(SketchOpsTest, SmallInputsAreExactAndShapesFollowKeyed) {
  std::vector<Doc> docs;
  for (int i = 1; i <= 100; i++) {
    docs.push_back(flatdoc("id", std::to_string(i), "n_i", i, "f_f", (float)i / 2));
    if (i == 50) {
      helper.indexAll(docs, UpdateMessage::COMMIT);
      docs.clear();
    }
  }
  helper.indexAll(docs, UpdateMessage::COMMIT);

  auto request = run(R"json({"ops":{"q":{"top_docs":{"limit":0,"ops":{
    "keyed":{"percentiles":{"field":"n_i","percents":[50,95,0,100]}},
    "list":{"percentiles":{"field":"f_f","percents":[50],"keyed":false}},
    "ranks":{"percentile_ranks":{"field":"n_i","values":[50,0.5,100,25.5]}},
    "rank_list":{"percentile_ranks":{"field":"n_i","values":[10],"keyed":false}}
  }}}}})json");
  ASSERT_TRUE(request->ok()) << request->errorMsg();
  EXPECT_EQ(R"({"50.0":50.5,"95.0":95.05,"0.0":1,"100.0":100})",
            json(op(request, "keyed")));
  EXPECT_EQ(R"([{"key":50,"value":25.25}])", json(op(request, "list")));
  EXPECT_EQ(R"({"50.0":50,"0.5":0,"100.0":100,"25.5":25})",
            json(op(request, "ranks")));
  EXPECT_EQ(R"([{"key":10,"value":10}])", json(op(request, "rank_list")));
}

TEST_F(SketchOpsTest, EmptyDomainsAndAbsentFields) {
  helper.indexAll({
    flatdoc("id", "1", "n_i", 1, "ip_s", "a"),
    flatdoc("id", "2", "n_i", 2, "ip_s", "b"),
  }, UpdateMessage::COMMIT);

  // No matching documents.
  auto request = run(R"json({"ops":{"q":{"top_docs":{"query":"n_i:[100 TO *]",
    "limit":0,"ops":{
      "ips":{"cardinality":{"field":"ip_s"}},
      "p":{"percentiles":{"field":"n_i","percents":[50,99]}},
      "plist":{"percentiles":{"field":"n_i","percents":[50],"keyed":false}},
      "r":{"percentile_ranks":{"field":"n_i","values":[1]}}
  }}}}})json");
  ASSERT_TRUE(request->ok()) << request->errorMsg();
  EXPECT_EQ(0, op(request, "ips").asInt());
  EXPECT_EQ(R"({"50.0":null,"99.0":null})", json(op(request, "p")));
  EXPECT_EQ(R"([{"key":50,"value":null}])", json(op(request, "plist")));
  EXPECT_EQ(R"({"1.0":null})", json(op(request, "r")));

  // Matching documents, but none has the (declared by template) field.
  auto again2 = run(R"json({"ops":{"q":{"top_docs":{"limit":0,"ops":{
    "c":{"cardinality":{"field":"other_s"}},
    "p":{"percentiles":{"field":"other_d","percents":[50]}}
  }}}}})json");
  ASSERT_TRUE(again2->ok()) << again2->errorMsg();
  EXPECT_EQ(0, op(again2, "c").asInt());
  EXPECT_EQ(R"({"50.0":null})", json(op(again2, "p")));
}

TEST_F(SketchOpsTest, EmptyIndex) {
  auto request = run(R"json({"ops":{"q":{"top_docs":{"limit":0,"ops":{
    "c":{"cardinality":{"field":"ip_s"}},
    "p":{"percentiles":{"field":"n_i","percents":[50]}}
  }}}}})json");
  ASSERT_TRUE(request->ok()) << request->errorMsg();
  EXPECT_EQ(0, op(request, "c").asInt());
  EXPECT_EQ(R"({"50.0":null})", json(op(request, "p")));
}

TEST_F(SketchOpsTest, PerBucketUnderFieldAndRangeFacets) {
  helper.indexAll({
    flatdoc("id", "1", "cat_s", "x", "user_s", "u1", "ms_i", 10),
    flatdoc("id", "2", "cat_s", "x", "user_s", "u2", "ms_i", 20),
    flatdoc("id", "3", "cat_s", "y", "user_s", "u1", "ms_i", 30),
  }, UpdateMessage::COMMIT);
  helper.indexAll({
    flatdoc("id", "4", "cat_s", "x", "user_s", "u1", "ms_i", 40),
    flatdoc("id", "5", "cat_s", "y", "user_s", "u3", "ms_i", 50),
    flatdoc("id", "6", "cat_s", "z", "ms_i", 60),
  }, UpdateMessage::COMMIT);

  for (int limit : {10, -1}) {
    auto request = run(R"json({"ops":{"q":{"top_docs":{"limit":0,"ops":{
      "cats":{"field_facet":{"field":"cat_s","limit":)json" + std::to_string(limit)
        + R"json(,"ops":{
        "users":{"cardinality":{"field":"user_s"}},
        "p":{"percentiles":{"field":"ms_i","percents":[50]}}
      }}},
      "ranges":{"range_facet":{"field":"ms_i","start":0,"end":90,"gap":30,"ops":{
        "users":{"cardinality":{"field":"user_s"}},
        "p":{"percentiles":{"field":"ms_i","percents":[100],"keyed":false}}
      }}}
    }}}}})json");
    ASSERT_TRUE(request->ok()) << request->errorMsg();
    const auto& cats = *op(request, "cats").facetResult();
    const auto& ids = std::get<api::ColStr>(cats.bucket_ids->kind).v;
    ASSERT_EQ(3u, ids.size());
    EXPECT_EQ("x", ids[0]);
    EXPECT_EQ("y", ids[1]);
    EXPECT_EQ("z", ids[2]);
    EXPECT_EQ(R"([2,2,0])", json(*cats.ops.at("users"))) << limit;
    EXPECT_EQ(R"([{"50.0":20},{"50.0":40},{"50.0":60}])",
              json(*cats.ops.at("p"))) << limit;

    const auto& ranges = *op(request, "ranges").facetResult();
    EXPECT_EQ(R"([2,2,0])", json(*ranges.ops.at("users")));
    EXPECT_EQ(R"([[{"key":100,"value":20}],[{"key":100,"value":50}],)"
              R"([{"key":100,"value":60}]])",
              json(*ranges.ops.at("p")));
  }
}

TEST_F(SketchOpsTest, RootOpsAndJsonEcho) {
  helper.indexAll({flatdoc("id", "1", "n_i", 5)}, UpdateMessage::COMMIT);
  auto request = run(R"json({"ops":{
    "c":{"cardinality":{"field":"n_i"}},
    "p":{"percentiles":{"field":"n_i","percents":[50],"compression":200}}
  }})json");
  ASSERT_TRUE(request->ok()) << request->errorMsg();
  EXPECT_EQ(1, request->scalar<int64_t>("c"));

  std::pmr::monotonic_buffer_resource mr;
  api::SearchOp parsed;
  ASSERT_TRUE(api::read_json(parsed, R"({"percentiles":{"field":"n_i","percents":[50,99.5],"keyed":false}})", mr));
  std::string encoded;
  ASSERT_TRUE(api::write_json(parsed, encoded));
  EXPECT_EQ(R"({"percentiles":{"field":"n_i","percents":[50,99.5],"keyed":false}})", encoded);
}

TEST_F(SketchOpsTest, ValidationErrors) {
  helper.indexAll({flatdoc("id", "1", "n_i", 5, "body_t", "some words",
                           "ip_s", "a")}, UpdateMessage::COMMIT);
  auto request = [](std::string_view op) {
    return R"json({"ops":{"q":{"top_docs":{"limit":0,"ops":{"m":)json"
        + std::string(op) + "}}}}}";
  };
  expectError(request(R"({"cardinality":{"field":"body_t"}})"),
              "must be a STRING, ID, INT, FLOAT, DOUBLE, or DATE field");
  expectError(request(R"({"cardinality":{}})"), "field is required");
  expectError(request(R"({"cardinality":{"field":"n_i","precision_threshold":40001}})"),
              "precision_threshold must be in [0, 40000]");
  expectError(request(R"({"percentiles":{"field":"ip_s"}})"),
              "must be an INT, FLOAT, DOUBLE, or DATE field");
  expectError(request(R"({"percentiles":{"field":"n_i","percents":[101]}})"),
              "percents must be in [0, 100]");
  expectError(request(R"({"percentiles":{"field":"n_i","percents":[50,50]}})"),
              "percents must be distinct");
  expectError(request(R"({"percentiles":{"field":"n_i","compression":5}})"),
              "compression must be in [20, 1000]");
  expectError(request(R"({"percentile_ranks":{"field":"n_i"}})"),
              "values are required");
}

} // namespace
