// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// The bit vector encoding and the hamming metric.

#include <gtest/gtest.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "luxir/codec/VectorCodec.h"
#include "luxir/reader/VectorReader.h"
#include "luxir/schema/Schema.h"
#include "luxir/util/Popcount.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/QueryBuild.h"
#include "test/SchemaBuilder.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

namespace api = luxir::api;

namespace {

using Vec = std::vector<float>;

// A vector whose sign bits are `bits`, dimension 0 the most significant.
Vec signs(uint32_t bits, int32_t dims = 16) {
  Vec v((size_t)dims);
  for (int32_t i = 0; i < dims; i++) v[(size_t)i] = (bits >> (dims - 1 - i)) & 1 ? 0.5f + (float)i : -1.0f;
  return v;
}

void installField(Collection& col, std::string_view name, api::VectorMetric metric, int32_t dims,
                  std::optional<api::VectorEncoding> encoding = std::nullopt) {
  SchemaBuilder b;
  auto& f = b.field(name);
  f.type = api::FieldDef::FieldClass::VECTOR;
  f.metric = metric;
  f.dims = dims;
  f.encoding = encoding;
  b.set(col);
}

struct Hit {
  std::string id;
  float score;
  bool operator==(const Hit&) const = default;
};

std::vector<Hit> knnHits(std::string_view field, const Vec& query, int k, bool exact,
                         std::optional<float> similarity = std::nullopt) {
  auto req = localReq(LuxirTest::luxirNode->getSearchEngine());
  auto& cur = req->collection("main").topDocs("q").getScores().fields({"id"});
  cur.rawQuery() = qb::knn(cur.mr(), field, query, k, 0, exact, 0, 0, similarity);
  req->execute();
  EXPECT_TRUE(req->ok()) << req->errorMsg();
  std::vector<Hit> hits;
  const auto* dl = req->docList();
  if (!dl || dl->columns.empty()) return hits;
  const auto& ids = std::get<api::ColStr>(dl->columns.find("id")->kind).v;
  const auto& scores = std::get<api::ColFloat>(dl->columns.find("_score_")->kind).v;
  for (size_t i = 0; i < ids.size(); i++) hits.push_back({std::string(ids[i]), scores[i]});
  return hits;
}

// Every doc's stored vector of field, decoded.
std::map<std::string, Vec> retrieved(std::string_view field) {
  auto req = localReq(LuxirTest::luxirNode->getSearchEngine());
  req->collection("main").topDocs("q").allQuery().fields({"id", std::string(field)}).limit(-1);
  req->execute();
  EXPECT_TRUE(req->ok()) << req->errorMsg();
  std::map<std::string, Vec> out;
  for (const auto& doc : req->getDocs()) {
    if (const auto* value = find(doc, field)) out[std::get<std::string>(*find(doc, "id"))] = std::get<Vec>(*value);
  }
  return out;
}

std::vector<VectorEncoding> segmentEncodings(CollectionHelper& h, std::string_view field) {
  std::vector<VectorEncoding> out;
  auto r = h.getIndexWriter()->snapshots.readers.getReader();
  for (auto& seg : r->segments()) {
    if (const DecodedFieldRecord* found = seg.postingsReader().fieldInfo(field)) {
      out.push_back(VectorReader::encodingOf(*found));
    }
  }
  return out;
}

IndexResult forceMerge(CollectionHelper& h) {
  CollectionHelper::UpdateBuilder request;
  request.commit(false, 1);
  return h.submit(request);
}

} // namespace

class VectorBitTest : public LuxirTest {
protected:
  void SetUp() override {
    luxirNode->getCollection("main")->setSchema(Schema::createDefaultSchema());
  }
};

// One bit per dimension, set when the element is > 0, the first dimension in
// the high bit of the first byte.
TEST_F(VectorBitTest, codec) {
  Vec v = {1, -1, 0, 0.5f, -0.0f, 2, -3, 1e-30f, -1, -1, -1, -1, -1, -1, -1, 4};
  uint8_t bytes[2];
  vcodec::encode(VectorEncoding::BIT, v.data(), 16, (char*)bytes);
  EXPECT_EQ(0x95, bytes[0]);
  EXPECT_EQ(0x01, bytes[1]);
  Vec out(16);
  vcodec::decode(VectorEncoding::BIT, (const char*)bytes, 16, out.data());
  EXPECT_EQ((Vec{1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1}), out);
  EXPECT_EQ(2, vcodec::bytesPerVector(VectorEncoding::BIT, 16));
  EXPECT_EQ(16, vcodec::dimsFromBytes(VectorEncoding::BIT, 2));
  EXPECT_TRUE(vcodec::valid((int32_t)VectorEncoding::BIT));
  EXPECT_FALSE(vcodec::valid((int32_t)VectorEncoding::BIT + 1));
  EXPECT_TRUE(vcodec::convertible(VectorEncoding::F32, VectorEncoding::SQ8));
  EXPECT_TRUE(vcodec::convertible(VectorEncoding::BIT, VectorEncoding::BIT));
  EXPECT_FALSE(vcodec::convertible(VectorEncoding::F32, VectorEncoding::BIT));
  EXPECT_FALSE(vcodec::convertible(VectorEncoding::BIT, VectorEncoding::F16));
}

// Every length through the vector chunks, 64-bit words and byte tail, from
// unaligned addresses.
TEST_F(VectorBitTest, popcountHamming) {
  std::mt19937 rng(7);
  std::vector<uint8_t> a(300), b(300);
  for (size_t i = 0; i < a.size(); i++) {
    a[i] = (uint8_t)rng();
    b[i] = (uint8_t)rng();
  }
  for (size_t offset : {0, 1, 5}) {
    for (size_t n = 0; n + offset <= 290; n++) {
      uint64_t expected = 0;
      for (size_t i = 0; i < n; i++) expected += (uint64_t)std::popcount((uint8_t)(a[offset + i] ^ b[offset + i]));
      ASSERT_EQ(expected, popcnt::hamming(a.data() + offset, b.data() + offset, n)) << n << " " << offset;
    }
  }
}

// hamming defaults to the bit encoding and scores 1 - distance / dims by an
// exact scan, with or without exact: true, across segments.  No overlay is
// built for it.
TEST_F(VectorBitTest, exactScanScoresHamming) {
  CollectionHelper h("main");
  installField(h.collection(), "bits_v", api::VectorMetric::HAMMING, 16);
  const uint32_t query = 0xA5C3;
  for (int i = 0; i <= 8; i++) {
    // Doc di differs from the query in its low i bits.
    h.index(flatdoc("id", "d" + std::to_string(i), "bits_v", signs(query ^ ((1u << i) - 1))));
    if (i == 4) h.commit();
  }
  h.commit({"vec.bits_v"});

  EXPECT_EQ(std::vector(2, VectorEncoding::BIT), segmentEncodings(h, "bits_v"));
  auto r = h.getIndexWriter()->snapshots.readers.getReader();
  for (auto& seg : r->segments()) {
    EXPECT_EQ(nullptr, seg.getAuxReader("vec.bits_v"));
    VectorReader vr(seg.postingsReader(), *seg.postingsReader().fieldInfo("bits_v"));
    EXPECT_EQ(2, vr.valueBytes());
  }

  std::vector<Hit> expected;
  for (int i = 0; i < 4; i++) expected.push_back({"d" + std::to_string(i), 1.0f - (float)i / 16.0f});
  EXPECT_EQ(expected, knnHits("bits_v", signs(query), 4, true));
  EXPECT_EQ(expected, knnHits("bits_v", signs(query), 4, false));
  // similarity is a minimum score.
  EXPECT_EQ(std::vector(expected.begin(), expected.begin() + 3), knnHits("bits_v", signs(query), 9, true, 0.85f));
  EXPECT_EQ((Vec{1, 0, 1, 0, 0, 1, 0, 1, 1, 1, 0, 0, 0, 0, 1, 1}), retrieved("bits_v")["d0"]);
}

TEST_F(VectorBitTest, schemaValidation) {
  auto error = [](void (*setup)(api::FieldDef&)) -> std::string {
    SchemaBuilder b;
    auto& f = b.field("emb");
    f.type = api::FieldDef::FieldClass::VECTOR;
    f.metric = api::VectorMetric::HAMMING;
    f.dims = 32;
    setup(f);
    try {
      auto schema = b.build();
      auto* type = (const VectorFieldType*)schema->getFieldTypePtr("emb");
      EXPECT_EQ(VectorEncoding::BIT, type->encoding());
    } catch (const std::exception& e) {
      return e.what();
    }
    return "";
  };
  auto has = [](const std::string& text, std::string_view part) { return text.find(part) != std::string::npos; };
  EXPECT_EQ("", error([](api::FieldDef&) {}));
  EXPECT_EQ("", error([](api::FieldDef& f) { f.metric = api::VectorMetric::NONE; f.encoding = api::VectorEncoding::BIT; }));
  EXPECT_TRUE(has(error([](api::FieldDef& f) { f.encoding = api::VectorEncoding::F32; }),
                  "metric hamming requires encoding bit"));
  EXPECT_TRUE(has(error([](api::FieldDef& f) { f.metric = api::VectorMetric::COSINE; f.encoding = api::VectorEncoding::BIT; }),
                  "encoding bit requires metric hamming or none"));
  EXPECT_TRUE(has(error([](api::FieldDef& f) { f.dims = 12; }), "dims must be a multiple of 8"));
  // truncate_dims is the stored dimension: it must pack, the input need not.
  EXPECT_TRUE(has(error([](api::FieldDef& f) { f.truncate_dims = 12; }), "truncate_dims must be a multiple of 8"));
  EXPECT_EQ("", error([](api::FieldDef& f) { f.dims = 30; f.truncate_dims = 16; }));
  EXPECT_TRUE(has(error([](api::FieldDef& f) { f.metric = api::VectorMetric::L2; f.encoding = api::VectorEncoding::F32;
                                               f.truncate_dims = 16; }),
                  "truncate_dims requires metric cosine or hamming"));
}

// A truncated bit field stores and compares the prefix's signs; without
// schema dims, the first vector's must pack into bytes.
TEST_F(VectorBitTest, storedDimsPackIntoBytes) {
  CollectionHelper h("main");
  SchemaBuilder b;
  auto& f = b.field("emb");
  f.type = api::FieldDef::FieldClass::VECTOR;
  f.metric = api::VectorMetric::HAMMING;
  f.dims = 30;
  f.truncate_dims = 16;
  auto& inferred = b.field("free_v");
  inferred.type = api::FieldDef::FieldClass::VECTOR;
  inferred.metric = api::VectorMetric::HAMMING;
  b.set(h.collection());

  Vec doc = signs(0x1234, 30), query = signs(0x1234, 30);
  query[20] = -query[20];  // past the prefix
  h.index(flatdoc("id", std::string("a"), "emb", doc));
  {
    ExpectLog quiet("multiple of 8");
    EXPECT_EQ(1, h.index(flatdoc("id", std::string("odd"), "free_v", Vec(12, 1.0f))).total_errors);
  }
  EXPECT_EQ(0, h.index(flatdoc("id", std::string("even"), "free_v", Vec(24, 1.0f))).total_errors);
  h.commit();
  EXPECT_EQ((std::vector<Hit>{{"a", 1.0f}}), knnHits("emb", query, 1, true));
  EXPECT_EQ(Vec(24, 1.0f), retrieved("free_v")["even"]);
}

// A declared field cannot change between bit and the other encodings, in
// either direction.
TEST_F(VectorBitTest, schemaChangeAcrossBitRejected) {
  CollectionHelper h("main");
  installField(h.collection(), "emb", api::VectorMetric::COSINE, 16);
  EXPECT_THROW(installField(h.collection(), "emb", api::VectorMetric::HAMMING, 16), SchemaError);
  installField(h.collection(), "emb", api::VectorMetric::COSINE, 16, api::VectorEncoding::SQ8);
  installField(h.collection(), "bits", api::VectorMetric::HAMMING, 16);
  EXPECT_THROW(installField(h.collection(), "bits", api::VectorMetric::NONE, 16, api::VectorEncoding::F16), SchemaError);
}

// A field removed and declared again as bit over f32 segments: a merge never
// converts across the boundary.  f32 inputs keep their encoding (the dims of
// 12 would not even pack into bytes), and each side can merge independently.
TEST_F(VectorBitTest, mergeNeverConvertsAcrossBit) {
  CollectionHelper h("main");
  installField(h.collection(), "emb", api::VectorMetric::COSINE, 12);
  std::map<std::string, Vec> expected;
  for (int seg = 0; seg < 2; seg++) {
    for (int i = 0; i < 3; i++) {
      std::string id = "f" + std::to_string(seg * 3 + i);
      Vec v(12, 0.25f);
      v[(size_t)(seg * 3 + i)] = 1.0f;
      h.index(flatdoc("id", id, "emb", v));
      float inv = (float)(1.0 / std::sqrt(11 * 0.0625 + 1.0));  // as normalize_on_write
      for (float& x : v) x *= inv;
      expected[id] = v;
    }
    h.commit();
  }
  h.collection().setSchema(Schema::createDefaultSchema());
  installField(h.collection(), "emb", api::VectorMetric::HAMMING, 16);
  {
    ExpectLog kept("keeping the inputs' f32 encoding");
    ASSERT_TRUE(forceMerge(h).success);
    EXPECT_EQ(1u, kept.suppressed());
  }
  EXPECT_EQ(std::vector{VectorEncoding::F32}, segmentEncodings(h, "emb"));
  EXPECT_EQ(expected, retrieved("emb"));

  auto req = localReq(luxirNode->getSearchEngine());
  auto& cur = req->collection("main").topDocs("q");
  cur.rawQuery() = qb::knn(cur.mr(), "emb", signs(1), 1, 0, true);
  {
    ExpectLog quiet("cannot be scored");
    req->execute();
  }
  EXPECT_FALSE(req->ok());
  EXPECT_NE(std::string::npos, req->errorMsg().find("encoding f32 cannot be scored with metric hamming")) << req->errorMsg();

  ExpectLog failed("Merge failed");
  const std::vector sides{VectorEncoding::F32, VectorEncoding::BIT};
  for (int i = 0; i < 4; i++) {
    std::string id = "b" + std::to_string(i);
    h.index(flatdoc("id", id, "emb", signs(0x00FF)));
    h.commit();
    expected[id] = Vec{0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1};
    EXPECT_EQ(expected, retrieved("emb"));
    if (i == 0) {
      ASSERT_TRUE(forceMerge(h).success);
      EXPECT_EQ(sides, segmentEncodings(h, "emb"));
      EXPECT_EQ(expected, retrieved("emb"));
    }
  }
  EXPECT_EQ((std::vector{VectorEncoding::F32, VectorEncoding::BIT, VectorEncoding::BIT,
                         VectorEncoding::BIT, VectorEncoding::BIT}), segmentEncodings(h, "emb"));
  ASSERT_TRUE(forceMerge(h).success);
  EXPECT_EQ(sides, segmentEncodings(h, "emb"));
  EXPECT_EQ(expected, retrieved("emb"));
  EXPECT_EQ(0u, failed.suppressed());
}

TEST_F(VectorBitTest, mergePrefersMajorityWhenSmallestSegmentIsOnOldSide) {
  CollectionHelper h("main");
  h.index(flatdoc("id", std::string("f"), "emb_v", Vec(16, 0.25f)));
  h.commit();
  installField(h.collection(), "emb_v", api::VectorMetric::HAMMING, 16);
  std::map<std::string, Vec> expected{{"f", Vec(16, 0.25f)}};
  for (int seg = 0; seg < 3; seg++) {
    for (int i = 0; i < 2; i++) {
      std::string id = "b" + std::to_string(2 * seg + i);
      h.index(flatdoc("id", id, "emb_v", signs(0xFFFF)));
      expected[id] = Vec(16, 1.0f);
    }
    h.commit();
  }
  EXPECT_EQ((std::vector{VectorEncoding::F32, VectorEncoding::BIT, VectorEncoding::BIT,
                         VectorEncoding::BIT}), segmentEncodings(h, "emb_v"));
  ASSERT_TRUE(forceMerge(h).success);
  EXPECT_EQ((std::vector{VectorEncoding::F32, VectorEncoding::BIT}), segmentEncodings(h, "emb_v"));
  EXPECT_EQ(expected, retrieved("emb_v"));
}
