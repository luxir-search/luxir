// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "luxir/api/build.h"
#include "luxir/schema/FieldType.h"
#include "luxir/search/Similarity.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/SchemaBuilder.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

namespace {

namespace api = luxir::api;
using FieldClass = api::FieldDef_::FieldClass;

using Hits = std::vector<std::pair<std::string, float>>;

float stored(float weight) {
  return FeatureFieldType::decodeWeight(FeatureFieldType::encodeWeight(weight));
}

} // namespace

class FeatureFieldTest : public LuxirTest {
public:
  CollectionHelper helper;
  SchemaBuilder schema;

  void SetUp() override {
    helper.clear();
    schema.field("terms").type = FieldClass::SPARSE_VECTOR;
    schema.field("topics").type = FieldClass::RANK_FEATURES;
    schema.field("pagerank").type = FieldClass::RANK_FEATURE;
    schema.set(helper.collection());
  }

  void TearDown() override { helper.clear(); }

  // Index JSON documents; returns the per-doc error count.
  int64_t index(std::initializer_list<std::string_view> docs, uint32_t maxSegments = 0) {
    CollectionHelper::UpdateBuilder update;
    for (std::string_view doc : docs) {
      update.addRaw([&](api::Map& map, std::pmr::memory_resource& mr) {
        std::string error;
        EXPECT_TRUE(api::read_json(map, doc, mr, &error)) << error;
      });
    }
    update.commit(false, maxSegments);
    auto result = helper.submit(update);
    EXPECT_TRUE(result.error_message.empty()) << result.error_message;
    return result.total_errors;
  }

  // Run a JSON query; returns (id, score) in rank order.
  Hits search(std::string_view query, int64_t limit = -1) {
    auto req = localReq(helper.getSearchEngine());
    auto& top = req->collection("main").topDocs("q");
    std::string error;
    EXPECT_TRUE(api::read_json(top.rawQuery(), query, top.mr(), &error)) << error;
    top.fields({"id"}).getScores().limit(limit);
    req->execute();
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    Hits hits;
    if (!req->ok()) return hits;
    const auto& scores = std::get<api::ColFloat>(req->docList()->columns.at("_score_").kind).v;
    auto docs = req->getDocs();
    for (size_t i = 0; i < docs.size(); i++) {
      hits.emplace_back(std::get<std::string>(*find(docs[i], "id")), scores[i]);
    }
    return hits;
  }

  std::string searchError(std::string_view query) {
    auto req = localReq(helper.getSearchEngine());
    auto& top = req->collection("main").topDocs("q");
    EXPECT_TRUE(api::read_json(top.rawQuery(), query, top.mr()));
    req->execute();
    return req->ok() ? std::string() : req->errorMsg();
  }
};

TEST_F(FeatureFieldTest, weightEncoding) {
  EXPECT_EQ(1.0f, stored(1.0f));
  EXPECT_EQ(0.5f, stored(0.5f));
  for (float w : {0.001f, 0.3f, 1.7f, 123.456f, 1e30f}) {
    EXPECT_LE(stored(w), w);
    EXPECT_GT(stored(w), w * (1.0f - 1.0f / 256));
  }
  EXPECT_EQ(65279, FeatureFieldType::encodeWeight(std::numeric_limits<float>::max()));
  EXPECT_FALSE(FeatureFieldType::validWeight(0.0));
  EXPECT_FALSE(FeatureFieldType::validWeight(-1.0));
  EXPECT_FALSE(FeatureFieldType::validWeight(INFINITY));
}

TEST_F(FeatureFieldTest, sparseVectorDotProduct) {
  ASSERT_EQ(0, index({
      R"({"id":"a","terms":{"dune":2.0,"spice":0.3}})",
      R"({"id":"b","terms":{"spice":1.5,"worm":4}})",
      R"({"id":"c","terms":{"worm":1}})"}));
  auto hits = search(R"({"sparse_vector":{"field":"terms","query_vector":{"dune":1.5,"spice":2}}})");
  ASSERT_EQ(2u, hits.size());
  EXPECT_EQ("a", hits[0].first);
  EXPECT_FLOAT_EQ(1.5f * stored(2.0f) + 2 * stored(0.3f), hits[0].second);
  EXPECT_EQ("b", hits[1].first);
  EXPECT_FLOAT_EQ(2 * stored(1.5f), hits[1].second);

  // A term query on a feature name matches exactly; exists sees every doc.
  EXPECT_EQ(2u, search(R"({"match":{"field":"terms","val":"worm"}})").size());
  EXPECT_EQ(3u, search(R"({"exists":{"field":"terms"}})").size());
}

TEST_F(FeatureFieldTest, rankFeatureFunctions) {
  ASSERT_EQ(0, index({
      R"({"id":"a","pagerank":4,"topics":{"sports":0.5}})",
      R"({"id":"b","pagerank":1,"topics":{"sports":2,"art":1}})",
      R"({"id":"c","pagerank":16})"}));
  auto score = [&](std::string_view query, std::string_view id) {
    for (const auto& [hit, value] : search(query)) {
      if (hit == id) return value;
    }
    return -1.0f;
  };
  EXPECT_FLOAT_EQ(4.0f / (4 + 2),
      score(R"({"rank_feature":{"field":"pagerank","saturation":{"pivot":2}}})", "a"));
  EXPECT_FLOAT_EQ((float) std::log(3.0 + 16),
      score(R"({"rank_feature":{"field":"pagerank","log":{"scaling_factor":3}}})", "c"));
  EXPECT_FLOAT_EQ(16.0f / (16 + 4),
      score(R"({"rank_feature":{"field":"pagerank","sigmoid":{"pivot":2,"exponent":2}}})", "a"));
  EXPECT_FLOAT_EQ(16.0f,
      score(R"({"rank_feature":{"field":"pagerank","linear":{}}})", "c"));
  // The default pivot is the geometric mean of 1, 4 and 16.
  EXPECT_FLOAT_EQ(0.5f, score(R"({"rank_feature":{"field":"pagerank"}})", "a"));

  // A rank_features feature, named either way.
  EXPECT_FLOAT_EQ(2.0f, score(R"({"rank_feature":{"field":"topics.sports","linear":{}}})", "b"));
  EXPECT_FLOAT_EQ(0.5f,
      score(R"({"rank_feature":{"field":"topics","feature":"sports","linear":{}}})", "a"));

  // As a should clause, the feature boosts matches without restricting them.
  auto hits = search(R"({"boolean":{"required":[{"exists":{"field":"pagerank"}}],
      "optional":[{"rank_feature":{"field":"topics.sports","linear":{}}}]}})");
  ASSERT_EQ(3u, hits.size());
  EXPECT_EQ("b", hits[0].first);
  EXPECT_EQ("c", hits[2].first);
}

TEST_F(FeatureFieldTest, prunedTopKMatchesExhaustive) {
  // Enough docs for several postings blocks per token, over two segments
  // that are then merged, so block-max pruning and merged freq-only
  // postings both run.
  std::vector<std::string> docs;
  std::map<std::string, std::map<std::string, float>> weights;
  const char* tokens[] = {"t0", "t1", "t2", "t3"};
  for (int d = 0; d < 1200; d++) {
    std::string id = std::to_string(d);
    std::string doc = R"({"id":")" + id + R"(","terms":{)";
    bool first = true;
    for (int t = 0; t < 4; t++) {
      if ((d + t) % 3 == 0) continue;
      float w = (float) (1 + rng.rint(1000000)) / 1000.0f;
      weights[id][tokens[t]] = stored(w);
      doc += std::string(first ? "" : ",") + "\"" + tokens[t] + "\":" + std::to_string(w);
      first = false;
    }
    docs.push_back(doc + "}}");
  }
  CollectionHelper::UpdateBuilder update;
  for (const auto& doc : docs) {
    update.addRaw([&](api::Map& map, std::pmr::memory_resource& mr) {
      ASSERT_TRUE(api::read_json(map, doc, mr));
    });
  }
  update.commit();
  ASSERT_TRUE(helper.submit(update).success);
  ASSERT_EQ(0, index({R"({"id":"x","terms":{"t0":0.25}})"}, 1));

  std::string query = R"({"sparse_vector":{"field":"terms","query_vector":{"t0":3,"t1":0.5,"t2":1,"t3":2}}})";
  std::vector<float> expected;
  for (const auto& [id, features] : weights) {
    float sum = 0;
    for (const auto& [token, w] : features) {
      sum += w * (token == "t0" ? 3 : token == "t1" ? 0.5f : token == "t2" ? 1 : 2);
    }
    expected.push_back(sum);
  }
  std::sort(expected.rbegin(), expected.rend());
  auto top = search(query, 10);
  ASSERT_EQ(10u, top.size());
  for (size_t i = 0; i < top.size(); i++) {
    EXPECT_NEAR(expected[i], top[i].second, expected[i] * 1e-5) << i;
  }
  auto all = search(query);
  ASSERT_EQ(weights.size() + 1, all.size());
  for (size_t i = 0; i < top.size(); i++) EXPECT_EQ(all[i].second, top[i].second) << i;
}

TEST_F(FeatureFieldTest, errors) {
  EXPECT_EQ(1, index({R"({"id":"a","terms":{"dune":-1}})"}));
  EXPECT_EQ(1, index({R"({"id":"b","terms":"dune"})"}));
  EXPECT_EQ(1, index({R"({"id":"c","pagerank":{"x":1}})"}));
  EXPECT_EQ(1, index({R"({"id":"d","pagerank":0})"}));

  EXPECT_NE("", searchError(R"({"sparse_vector":{"field":"terms","query_vector":{}}})"));
  EXPECT_NE("", searchError(R"({"sparse_vector":{"field":"pagerank","query_vector":{"_":1}}})"));
  EXPECT_NE("", searchError(R"({"sparse_vector":{"field":"id","query_vector":{"a":1}}})"));
  EXPECT_NE("", searchError(R"({"rank_feature":{"field":"topics"}})"));
  EXPECT_NE("", searchError(R"({"rank_feature":{"field":"pagerank","log":{"scaling_factor":0.5}}})"));

  SchemaBuilder bad;
  auto& field = bad.field("vec");
  field.type = FieldClass::SPARSE_VECTOR;
  field.stored = true;
  EXPECT_THROW(bad.build(), SchemaError);
}
