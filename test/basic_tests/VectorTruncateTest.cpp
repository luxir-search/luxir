// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// truncate_dims: Matryoshka prefix truncation of indexed and query vectors.

#include "gtest/gtest.h"

#include <cmath>
#include <optional>
#include <string>
#include <vector>

#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/QueryBuild.h"
#include "test/SchemaBuilder.h"
#include "test/TestUtils.h"
#include "luxir/schema/FieldType.h"
#include "luxir/schema/Schema.h"

using namespace luxir;
using namespace luxir::test;

namespace api = luxir::api;

class VectorTruncateTest : public LuxirTest {
protected:
  void SetUp() override {
    luxirNode->getCollection("main")->setSchema(Schema::createDefaultSchema());
  }

  // `_v` (and `_vs` when multi) truncated to `truncate` of `dims` input components.
  static void installSchema(Collection& col, api::VectorMetric metric, int32_t dims,
                            int32_t truncate, bool multi = false) {
    SchemaBuilder b;
    auto& f = b.templ(multi ? "_vs" : "_v");
    f.type = api::FieldDef::FieldClass::VECTOR;
    f.multi = multi;
    f.metric = metric;
    if (dims) f.dims = dims;
    f.truncate_dims = truncate;
    b.set(col);
  }

  // Schema compile error text, or "" when the def compiles.
  static std::string schemaError(void (*setup)(api::FieldDef&)) {
    SchemaBuilder b;
    auto& f = b.field("emb");
    f.type = api::FieldDef::FieldClass::VECTOR;
    setup(f);
    try {
      b.build();
    } catch (const std::exception& e) {
      return e.what();
    }
    return "";
  }

  struct Hit {
    std::string id;
    float score;
  };

  // kNN top hit, or the error message.
  static std::optional<Hit> topHit(SearchEngine& engine, std::string_view field,
                                   const std::vector<float>& query, std::string* error = nullptr) {
    auto req = localReq(engine);
    auto& cur = req->collection("main").topDocs("q").getScores().fields({"id"});
    cur.rawQuery() = qb::knn(cur.mr(), field, query, 1);
    req->execute();
    if (!req->ok()) {
      if (error) *error = req->errorMsg();
      return std::nullopt;
    }
    const auto* dl = req->docList();
    if (!dl || dl->columns.empty()) return std::nullopt;
    const auto& ids = std::get<api::ColStr>(dl->columns.find("id")->kind).v;
    const auto& scores = std::get<api::ColFloat>(dl->columns.find("_score_")->kind).v;
    if (ids.empty()) return std::nullopt;
    return Hit{std::string(ids[0]), scores[0]};
  }

  static FieldVal storedVector(SearchEngine& engine, std::string_view field) {
    auto req = localReq(engine);
    req->collection("main").topDocs("q").allQuery().fields({"id", std::string(field)}).limit(1);
    req->execute();
    auto docs = req->getDocs();
    return *find(docs.at(0), field);
  }
};

// Index and query vectors are both cut to the prefix: the tails disagree
// wildly, but the prefixes match exactly, so the L2 score is 1.
TEST_F(VectorTruncateTest, truncatesIndexAndQuery) {
  CollectionHelper h("main");
  installSchema(h.collection(), api::VectorMetric::L2, 4, 2);
  auto* ft = dynamic_cast<VectorFieldType*>(h.collection().getSchema()->getFieldTypePtr("emb_v"));
  ASSERT_NE(nullptr, ft);
  EXPECT_EQ(2, ft->dims());
  EXPECT_EQ(4, ft->inputDims());

  h.index(flatdoc("id", std::string("a"), "emb_v", std::vector<float>{1, 0, 9, 9}));
  h.index(flatdoc("id", std::string("b"), "emb_v", std::vector<float>{0, 1, -5, 7}),
          UpdateMessage::COMMIT);

  auto hit = topHit(h.getSearchEngine(), "emb_v", {1, 0, -5, 7});
  ASSERT_TRUE(hit);
  EXPECT_EQ("a", hit->id);
  EXPECT_FLOAT_EQ(1.0f, hit->score);
  EXPECT_EQ((std::vector<float>{1, 0}), std::get<std::vector<float>>(
                                            storedVector(h.getSearchEngine(), "emb_v")));
}

// A truncated unit vector is shorter than 1; cosine renormalizes the prefix.
TEST_F(VectorTruncateTest, cosineRenormalizesPrefix) {
  CollectionHelper h("main");
  installSchema(h.collection(), api::VectorMetric::COSINE, 3, 2);
  float n = std::sqrt(3.0f * 3.0f + 4.0f * 4.0f + 12.0f * 12.0f);
  h.index(flatdoc("id", std::string("a"), "emb_v", std::vector<float>{3 / n, 4 / n, 12 / n}),
          UpdateMessage::COMMIT);

  auto vec = std::get<std::vector<float>>(storedVector(h.getSearchEngine(), "emb_v"));
  ASSERT_EQ(2u, vec.size());
  EXPECT_FLOAT_EQ(0.6f, vec[0]);
  EXPECT_FLOAT_EQ(0.8f, vec[1]);
  auto hit = topHit(h.getSearchEngine(), "emb_v", {6, 8, -100});
  ASSERT_TRUE(hit);
  EXPECT_NEAR(1.0f, hit->score, 1e-6);
}

TEST_F(VectorTruncateTest, multiValued) {
  CollectionHelper h("main");
  installSchema(h.collection(), api::VectorMetric::L2, 0, 2, true);
  h.index(flatdoc("id", std::string("a"), "emb_vs",
                  std::vector<std::vector<float>>{{1, 0, 5}, {0, 2, 5, 6}}),
          UpdateMessage::COMMIT);

  auto vecs = std::get<std::vector<std::vector<float>>>(storedVector(h.getSearchEngine(), "emb_vs"));
  EXPECT_EQ((std::vector<std::vector<float>>{{1, 0}, {0, 2}}), vecs);
  auto hit = topHit(h.getSearchEngine(), "emb_vs", {0, 2, -7});
  ASSERT_TRUE(hit);
  EXPECT_FLOAT_EQ(1.0f, hit->score);
}

TEST_F(VectorTruncateTest, schemaValidation) {
  EXPECT_EQ("", schemaError([](api::FieldDef& f) { f.dims = 8; f.truncate_dims = 4; }));
  EXPECT_NE(std::string::npos, schemaError([](api::FieldDef& f) {
    f.truncate_dims = 0;
  }).find("truncate_dims must be > 0"));
  EXPECT_NE(std::string::npos, schemaError([](api::FieldDef& f) {
    f.dims = 4;
    f.truncate_dims = 4;
  }).find("truncate_dims (4) must be < dims (4)"));
  EXPECT_NE(std::string::npos, schemaError([](api::FieldDef& f) {
    f.metric = api::VectorMetric::COSINE;
    f.normalized = true;
    f.truncate_dims = 2;
  }).find("normalized=true cannot be combined with truncate_dims"));
  EXPECT_NE(std::string::npos, schemaError([](api::FieldDef& f) {
    f.type = api::FieldDef::FieldClass::FLOAT;
    f.truncate_dims = 2;
  }).find("only valid for vector fields"));
}

// Inputs shorter than truncate_dims (or not matching a declared dims) are
// rejected on index and query; dims-less fields accept any longer input.
TEST_F(VectorTruncateTest, inputDimsValidation) {
  CollectionHelper h("main");
  installSchema(h.collection(), api::VectorMetric::IP, 0, 3);
  auto r = h.index(flatdoc("id", std::string("a"), "emb_v", std::vector<float>{1, 2}));
  ASSERT_EQ(1u, r.errors.size());
  EXPECT_NE(std::string::npos,
            r.errors[0].error_message.find("expected at least truncate_dims=3 components, got 2"));
  r = h.index(flatdoc("id", std::string("b"), "emb_v", std::vector<float>{1, 2, 3, 4, 5}),
              UpdateMessage::COMMIT);
  EXPECT_TRUE(r.errors.empty());

  std::string error;
  EXPECT_FALSE(topHit(h.getSearchEngine(), "emb_v", {1, 2}, &error));
  EXPECT_NE(std::string::npos, error.find("expected at least truncate_dims=3 components, got 2"));
  auto hit = topHit(h.getSearchEngine(), "emb_v", {1, 1, 1, 100});
  ASSERT_TRUE(hit);
  EXPECT_FLOAT_EQ(6.0f, hit->score);  // dot over the 3-component prefix

  installSchema(h.collection(), api::VectorMetric::IP, 5, 3);
  r = h.index(flatdoc("id", std::string("c"), "emb_v", std::vector<float>{1, 2, 3, 4}));
  ASSERT_EQ(1u, r.errors.size());
  EXPECT_NE(std::string::npos, r.errors[0].error_message.find("expected dims=5, got 4"));
  EXPECT_FALSE(topHit(h.getSearchEngine(), "emb_v", {1, 1, 1, 1}, &error));
  EXPECT_NE(std::string::npos, error.find("expected dims=5, got 4"));
}
