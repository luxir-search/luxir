// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// truncate_dims: Matryoshka prefix truncation of indexed and query vectors.

#include "gtest/gtest.h"

#include <cmath>
#include <limits>
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

  // Cosine `_v` (and `_vs` when multi) truncated to `truncate` of `dims` input components.
  static void installSchema(Collection& col, int32_t dims, int32_t truncate, bool multi = false) {
    SchemaBuilder b;
    auto& f = b.templ(multi ? "_vs" : "_v");
    f.type = api::FieldDef::FieldClass::VECTOR;
    f.multi = multi;
    f.metric = api::VectorMetric::COSINE;
    f.dims = dims;
    f.truncate_dims = truncate;
    b.set(col);
  }

  // Schema compile error text for a cosine dims=8 field adjusted by setup, or
  // "" when the def compiles.
  static std::string schemaError(void (*setup)(api::FieldDef&)) {
    SchemaBuilder b;
    auto& f = b.field("emb");
    f.type = api::FieldDef::FieldClass::VECTOR;
    f.metric = api::VectorMetric::COSINE;
    f.dims = 8;
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
// wildly, but the prefixes match exactly, so the cosine score is 1.
TEST_F(VectorTruncateTest, truncatesIndexAndQuery) {
  CollectionHelper h("main");
  installSchema(h.collection(), 4, 2);
  auto* ft = dynamic_cast<VectorFieldType*>(h.collection().getSchema()->getFieldTypePtr("emb_v"));
  ASSERT_NE(nullptr, ft);
  EXPECT_EQ(2, ft->storedDims());
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
  installSchema(h.collection(), 3, 2);
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

// normalize_on_write=false stores the raw prefix; cosine still compares the
// prefix's direction at query time.
TEST_F(VectorTruncateTest, rawPrefixWithoutNormalizeOnWrite) {
  CollectionHelper h("main");
  SchemaBuilder b;
  auto& f = b.templ("_v");
  f.type = api::FieldDef::FieldClass::VECTOR;
  f.metric = api::VectorMetric::COSINE;
  f.dims = 3;
  f.truncate_dims = 2;
  f.normalize_on_write = false;
  b.set(h.collection());
  h.index(flatdoc("id", std::string("a"), "emb_v", std::vector<float>{3, 4, 12}));
  h.index(flatdoc("id", std::string("b"), "emb_v", std::vector<float>{4, -3, 1}),
          UpdateMessage::COMMIT);

  auto req = localReq(h.getSearchEngine());
  req->collection("main").topDocs("q").matchQuery("id", "a").fields({"emb_v"});
  req->execute();
  EXPECT_EQ((std::vector<float>{3, 4}), std::get<std::vector<float>>(*find(req->getDocs().at(0), "emb_v")));
  auto hit = topHit(h.getSearchEngine(), "emb_v", {6, 8, -100});
  ASSERT_TRUE(hit);
  EXPECT_EQ("a", hit->id);
  EXPECT_NEAR(1.0f, hit->score, 1e-6);
}

// Full and already truncated inputs mix on index and query, and a stored
// vector read back can be indexed again.
TEST_F(VectorTruncateTest, acceptsFullOrTruncatedInput) {
  CollectionHelper h("main");
  installSchema(h.collection(), 4, 2, true);
  h.index(flatdoc("id", std::string("a"), "emb_vs",
                  std::vector<std::vector<float>>{{1, 0, 5, 5}, {0, 2}}),
          UpdateMessage::COMMIT);

  auto vecs = std::get<std::vector<std::vector<float>>>(storedVector(h.getSearchEngine(), "emb_vs"));
  EXPECT_EQ((std::vector<std::vector<float>>{{1, 0}, {0, 1}}), vecs);
  for (const auto& query : {std::vector<float>{0, 2, -7, 1}, std::vector<float>{0, 2}}) {
    auto hit = topHit(h.getSearchEngine(), "emb_vs", query);
    ASSERT_TRUE(hit);
    EXPECT_FLOAT_EQ(1.0f, hit->score);
  }
  auto r = h.index(flatdoc("id", std::string("a"), "emb_vs", vecs), UpdateMessage::COMMIT);
  EXPECT_TRUE(r.errors.empty());
}

TEST_F(VectorTruncateTest, schemaValidation) {
  EXPECT_EQ("", schemaError([](api::FieldDef& f) { f.truncate_dims = 4; }));
  EXPECT_NE(std::string::npos, schemaError([](api::FieldDef& f) {
    f.truncate_dims = -1;
  }).find("truncate_dims must be >= 0"));
  EXPECT_NE(std::string::npos, schemaError([](api::FieldDef& f) {
    f.dims = std::nullopt;
    f.truncate_dims = 4;
  }).find("truncate_dims requires dims"));
  EXPECT_NE(std::string::npos, schemaError([](api::FieldDef& f) {
    f.truncate_dims = 8;
  }).find("truncate_dims (8) must be < dims (8)"));
  EXPECT_NE(std::string::npos, schemaError([](api::FieldDef& f) {
    f.metric = api::VectorMetric::IP;
    f.truncate_dims = 4;
  }).find("truncate_dims requires metric cosine"));
  EXPECT_NE(std::string::npos, schemaError([](api::FieldDef& f) {
    f.normalized = true;
    f.truncate_dims = 4;
  }).find("normalized=true cannot be combined with truncate_dims"));
  // pq.m must divide the stored (truncated) dimension, not dims.
  EXPECT_EQ("", schemaError([](api::FieldDef& f) {
    f.truncate_dims = 6;
    f.ann = pqAnn(0, 3);
  }));
  EXPECT_NE(std::string::npos, schemaError([](api::FieldDef& f) {
    f.truncate_dims = 6;
    f.ann = pqAnn(0, 4);
  }).find("pq.m=4 does not divide truncate_dims=6"));
  EXPECT_NE(std::string::npos, schemaError([](api::FieldDef& f) {
    f.type = api::FieldDef::FieldClass::FLOAT;
    f.metric = std::nullopt;
    f.dims = std::nullopt;
    f.truncate_dims = 4;
  }).find("only valid for vector fields"));
}

// truncate_dims: 0 turns off truncation inherited from a parent.
TEST_F(VectorTruncateTest, zeroClearsInheritedTruncation) {
  SchemaBuilder b;
  auto& t = b.templ("_m4");
  t.type = api::FieldDef::FieldClass::VECTOR;
  t.metric = api::VectorMetric::COSINE;
  t.dims = 8;
  t.truncate_dims = 4;
  auto& full = b.field("full");
  full.parent = "_m4";
  full.truncate_dims = 0;
  b.field("short").parent = "_m4";
  auto schema = b.build();
  EXPECT_EQ(8, ((VectorFieldType*)schema->getFieldTypePtr("full"))->storedDims());
  EXPECT_EQ(4, ((VectorFieldType*)schema->getFieldTypePtr("short"))->storedDims());
  // The signature carries truncate_dims only when it truncates.
  auto signatures = schema->signatures();
  EXPECT_FALSE(signatures.at("full").properties.contains("truncate_dims"));
  EXPECT_EQ("4", signatures.at("short").properties.at("truncate_dims"));
}

// Inputs must carry dims or truncate_dims components, and the whole input
// must be finite, including the tail truncation drops.
TEST_F(VectorTruncateTest, inputValidation) {
  CollectionHelper h("main");
  installSchema(h.collection(), 5, 3);
  float nan = std::numeric_limits<float>::quiet_NaN();
  auto r = h.index(flatdoc("id", std::string("a"), "emb_v", std::vector<float>{1, 2, 3, 4}));
  ASSERT_EQ(1u, r.errors.size());
  EXPECT_NE(std::string::npos,
            r.errors[0].error_message.find("expected dims=5 or truncate_dims=3, got 4"));
  r = h.index(flatdoc("id", std::string("a"), "emb_v", std::vector<float>{1, 2, 3, 4, nan}));
  ASSERT_EQ(1u, r.errors.size());
  EXPECT_NE(std::string::npos, r.errors[0].error_message.find("element 4 is not a finite float32"));
  h.index(flatdoc("id", std::string("b"), "emb_v", std::vector<float>{1, 2, 3, 4, 5}));
  r = h.index(flatdoc("id", std::string("c"), "emb_v", std::vector<float>{3, 2, 1}),
              UpdateMessage::COMMIT);
  EXPECT_TRUE(r.errors.empty());

  std::string error;
  EXPECT_FALSE(topHit(h.getSearchEngine(), "emb_v", {1, 1, 1, 1}, &error));
  EXPECT_NE(std::string::npos, error.find("expected dims=5 or truncate_dims=3, got 4"));
  EXPECT_FALSE(topHit(h.getSearchEngine(), "emb_v", {1, 2, 3, 0, nan}, &error));
  EXPECT_NE(std::string::npos, error.find("element 4 is not a finite float32"));
  // A non-zero full vector whose prefix is zero: skipped on index (cosine has
  // no direction), rejected as a query.
  r = h.index(flatdoc("id", std::string("z"), "emb_v", std::vector<float>{0, 0, 0, 1, 1}),
              UpdateMessage::COMMIT);
  EXPECT_TRUE(r.errors.empty());
  EXPECT_FALSE(topHit(h.getSearchEngine(), "emb_v", {0, 0, 0, 1, 1}, &error));
  EXPECT_NE(std::string::npos, error.find("has zero norm"));
  auto hit = topHit(h.getSearchEngine(), "emb_v", {1, 2, 3, 100, -100});
  ASSERT_TRUE(hit);
  EXPECT_EQ("b", hit->id);
  hit = topHit(h.getSearchEngine(), "emb_v", {3, 2, 1});
  ASSERT_TRUE(hit);
  EXPECT_EQ("c", hit->id);
}

// A truncated variant of a full field indexes the same input; each
// representation is searched with the full query vector.
TEST_F(VectorTruncateTest, truncatedVariant) {
  CollectionHelper h("main");
  SchemaBuilder b;
  auto& f = b.field("emb");
  f.type = api::FieldDef::FieldClass::VECTOR;
  f.metric = api::VectorMetric::COSINE;
  f.dims = 4;
  auto& v = b.variant(f, "d2");
  v.type = api::FieldDef::FieldClass::VECTOR;
  v.metric = api::VectorMetric::COSINE;
  v.dims = 4;
  v.truncate_dims = 2;
  b.set(h.collection());

  h.index(flatdoc("id", std::string("a"), "emb", std::vector<float>{1, 0, 9, 9}));
  h.index(flatdoc("id", std::string("b"), "emb", std::vector<float>{0, 1, -5, 7}),
          UpdateMessage::COMMIT);
  EXPECT_EQ(2, ((VectorFieldType*)h.collection().getSchema()->getFieldTypePtr("emb__d2"))->storedDims());
  EXPECT_EQ("b", topHit(h.getSearchEngine(), "emb", {1, 0, -5, 7})->id);
  EXPECT_EQ("a", topHit(h.getSearchEngine(), "emb__d2", {1, 0, -5, 7})->id);
}
