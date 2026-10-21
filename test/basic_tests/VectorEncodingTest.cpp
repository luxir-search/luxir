// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "luxir/codec/VectorCodec.h"
#include "luxir/index/VectorIndexBuilder.h"
#include "luxir/query/VectorScorer.h"
#include "luxir/reader/VectorReader.h"
#include "luxir/schema/Schema.h"
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

constexpr int32_t DIMS = 8;
using Vec = std::vector<float>;

// Doc i lies at a monotonically worse L2 distance, inner product, and cosine
// from QUERY as i grows, with gaps far above int8 quantization error.
const Vec QUERY = {0.9f, -0.4f, 0.3f, 0.8f, -0.7f, 0.2f, 0.5f, -0.1f};
const Vec SIDE = {0.1f, 0.6f, 0.4f, -0.2f, 0.1f, 0.7f, -0.3f, 0.2f};

Vec docVec(int i) {
  Vec v((size_t)DIMS);
  for (int32_t j = 0; j < DIMS; j++) {
    v[(size_t)j] = QUERY[(size_t)j] * (1.0f - 0.05f * (float)i) + SIDE[(size_t)j] * 0.1f * (float)i;
  }
  return v;
}

struct Fields {
  api::VectorMetric metric = api::VectorMetric::L2;
  api::VectorEncoding encoding = api::VectorEncoding::F32;
  bool keepOriginal = false;
  bool multi = false;
  std::optional<api::VectorAnnDef> ann = std::nullopt;
};

// ref_v is the f32 reference; enc_v stores the same values in the encoding.
void installFields(Collection& col, const Fields& f) {
  SchemaBuilder b;
  for (auto [name, encoding] : {std::pair{"ref_v", api::VectorEncoding::F32},
                                std::pair{"enc_v", f.encoding}}) {
    auto& def = b.field(name);
    def.type = api::FieldDef::FieldClass::VECTOR;
    def.metric = f.metric;
    def.encoding = encoding;
    def.multi = f.multi;
    if (encoding == f.encoding) def.keep_original = f.keepOriginal;
    if (f.ann) {
      def.dims = DIMS;
      def.ann = f.ann;
    }
  }
  b.set(col);
}

void indexDocs(CollectionHelper& h, int begin, int end) {
  for (int k = begin; k < end; k++) {
    int i = (k * 7) % 30;  // scattered, so doc order is not score order
    Vec v = docVec(i);
    h.index(flatdoc("id", "d" + std::to_string(i), "ref_v", v, "enc_v", v));
  }
}

std::vector<std::string> knnIds(std::string_view field, int32_t k, bool exact = true,
                                int32_t refine = 0) {
  auto* req = LocalReq::create(LuxirTest::luxirNode->getSearchEngine());
  auto& cur = req->collection("main").topDocs("q");
  cur.getScores().getNumber().fields({"id"});
  cur.rawQuery() = qb::knn(cur.mr(), field, QUERY, k, /*nprobe=*/0, exact, refine);
  req->execute();
  EXPECT_TRUE(req->ok()) << req->errorMsg();
  std::vector<std::string> ids;
  for (const auto& doc : req->getDocs()) ids.push_back(std::get<std::string>(*find(doc, "id")));
  req->done();
  return ids;
}

std::shared_ptr<IndexReader> reader(CollectionHelper& h) {
  return h.getIndexWriter()->snapshots.readers.getReader();
}

// Every stored row of a column across segments, sorted (merge-order free).
std::vector<std::string> rows(CollectionHelper& h, std::string_view field,
                              VectorEncoding* encoding = nullptr) {
  std::vector<std::string> out;
  auto r = reader(h);
  for (auto& seg : r->segments()) {
    const SegFieldInfo* found = seg.postingsReader().fieldInfo(field);
    if (!found) continue;
    SegFieldInfo fi = *found;
    VectorReader vr(seg.postingsReader(), fi);
    if (encoding) *encoding = vr.encoding();
    for (int64_t i = 0; i < vr.numVectors(); i++) out.emplace_back(vr.rowAtRank(i));
  }
  std::sort(out.begin(), out.end());
  return out;
}

// Small IVF+PQ overlay settings for the tiny test corpus.
api::VectorAnnDef smallPqAnn() {
  api::VectorAnnDef ann;
  ann.codec = api::VectorAnnDef::Codec::PQ;
  ann.nlist = 4;
  ann.pq.emplace();
  ann.pq->m = 2;
  ann.pq->bits = 2;
  return ann;
}

} // namespace

class VectorEncodingTest : public LuxirTest {
protected:
  void SetUp() override {
    luxirNode->getCollection("main")->setSchema(Schema::createDefaultSchema());
  }
};

TEST_F(VectorEncodingTest, codecRoundTrip) {
  Vec v = {-1.5f, 0.25f, 3.0f, 0.0f, 2.75f, -0.125f, 1.0f, 0.5f};
  std::vector<char> buf(64);
  Vec out(v.size());

  vcodec::encode(VectorEncoding::F16, v.data(), DIMS, buf.data());
  vcodec::decode(VectorEncoding::F16, buf.data(), DIMS, out.data());
  EXPECT_EQ(v, out);  // all exactly representable in binary16

  vcodec::encode(VectorEncoding::INT8, v.data(), DIMS, buf.data());
  vcodec::decode(VectorEncoding::INT8, buf.data(), DIMS, out.data());
  float halfStep = (3.0f - -1.5f) / 255.0f / 2.0f;
  for (size_t i = 0; i < v.size(); i++) EXPECT_NEAR(v[i], out[i], halfStep * 1.001f);
  EXPECT_EQ(-1.5f, out[0]);  // the minimum is the offset itself

  Vec constant(DIMS, 0.75f);
  vcodec::encode(VectorEncoding::INT8, constant.data(), DIMS, buf.data());
  EXPECT_EQ(0.0f, vcodec::readInt8Header(buf.data()).scale);
  vcodec::decode(VectorEncoding::INT8, buf.data(), DIMS, out.data());
  EXPECT_EQ(constant, out);

  EXPECT_EQ(16, vcodec::bytesPerVector(VectorEncoding::INT8, DIMS));
  EXPECT_EQ(DIMS, vcodec::dimsFromBytes(VectorEncoding::INT8, 16));
  EXPECT_EQ(DIMS, vcodec::dimsFromBytes(VectorEncoding::F16, 16));
}

// The int8 decomposition (no decode buffer) agrees with scoring the decoded
// floats, for every metric.
TEST_F(VectorEncodingTest, int8ScorerMatchesDecodedScore) {
  Vec v = docVec(3);
  std::vector<char> row((size_t)vcodec::bytesPerVector(VectorEncoding::INT8, DIMS));
  vcodec::encode(VectorEncoding::INT8, v.data(), DIMS, row.data());
  Vec decoded(DIMS);
  vcodec::decode(VectorEncoding::INT8, row.data(), DIMS, decoded.data());
  std::string_view rowView(row.data(), row.size());
  std::string_view decodedView((const char*)decoded.data(), decoded.size() * sizeof(float));
  for (auto metric : {api::VectorMetric::L2, api::VectorMetric::IP, api::VectorMetric::COSINE}) {
    VectorScorer q(QUERY.data(), DIMS, (int32_t)metric, true, VectorEncoding::INT8);
    VectorScorer f(QUERY.data(), DIMS, (int32_t)metric, true, VectorEncoding::F32);
    EXPECT_NEAR(f.score(decodedView, 0), q.score(rowView, 0), 1e-5) << (int)metric;
  }
}

// Each encoding is recorded per segment and reads back within its precision.
TEST_F(VectorEncodingTest, storedRoundTripPerEncoding) {
  for (auto encoding : {api::VectorEncoding::F32, api::VectorEncoding::F16, api::VectorEncoding::INT8}) {
    CollectionHelper h("main");
    h.clear();
    installFields(h.collection(), {.encoding = encoding});
    indexDocs(h, 0, 5);
    h.commit();

    auto r = reader(h);
    auto& seg = r->segments()[0];
    const SegFieldInfo* found = seg.postingsReader().fieldInfo("enc_v");
    ASSERT_NE(nullptr, found);
    SegFieldInfo fi = *found;
    VectorReader vr(seg.postingsReader(), fi);
    EXPECT_EQ((VectorEncoding)encoding, vr.encoding());
    EXPECT_EQ(DIMS, vr.dims());
    EXPECT_EQ(vcodec::bytesPerVector((VectorEncoding)encoding, DIMS), vr.valueBytes());
    Vec out(DIMS);
    vr.decodeAtRank(0, out.data());
    Vec expected = docVec(0);
    for (int32_t j = 0; j < DIMS; j++) EXPECT_NEAR(expected[(size_t)j], out[(size_t)j], 0.005f);
  }
}

// Exact kNN over f16 / int8 ranks like f32 on well-separated data, for every
// metric.
TEST_F(VectorEncodingTest, exactKnnMatchesF32Ranking) {
  for (auto metric : {api::VectorMetric::L2, api::VectorMetric::IP, api::VectorMetric::COSINE}) {
    for (auto encoding : {api::VectorEncoding::F16, api::VectorEncoding::INT8}) {
      CollectionHelper h("main");
      h.clear();
      installFields(h.collection(), {.metric = metric, .encoding = encoding});
      indexDocs(h, 0, 30);
      h.commit();
      auto expected = knnIds("ref_v", 10);
      ASSERT_EQ(10u, expected.size());
      EXPECT_EQ("d0", expected[0]);
      EXPECT_EQ(expected, knnIds("enc_v", 10)) << (int)metric << " " << (int)encoding;
    }
  }
}

// Merging same-encoding segments copies the encoded bytes verbatim.
TEST_F(VectorEncodingTest, mergeCopiesEncodedBytes) {
  CollectionHelper h("main");
  installFields(h.collection(), {.encoding = api::VectorEncoding::INT8, .multi = true});
  for (int k = 0; k < 3; k++) {
    indexDocs(h, k * 10, k * 10 + 10);
    h.commit();
  }
  auto before = rows(h, "enc_v");
  auto idsBefore = knnIds("enc_v", 10);
  h.getIndexWriter()->mergeSegments();
  h.commit();
  ASSERT_EQ(1u, reader(h)->segments().size());
  VectorEncoding encoding;
  EXPECT_EQ(before, rows(h, "enc_v", &encoding));
  EXPECT_EQ(VectorEncoding::INT8, encoding);
  EXPECT_EQ(idsBefore, knnIds("enc_v", 10));
}

// A schema change between segments: the merge writes the more precise
// encoding and transcodes only the other input.
TEST_F(VectorEncodingTest, mixedEncodingMergeKeepsPrecision) {
  CollectionHelper h("main");
  installFields(h.collection(), {.encoding = api::VectorEncoding::F16});
  indexDocs(h, 0, 15);
  h.commit();
  auto f16Rows = rows(h, "enc_v");
  installFields(h.collection(), {.encoding = api::VectorEncoding::INT8});
  indexDocs(h, 15, 30);
  h.commit();
  h.getIndexWriter()->mergeSegments();
  h.commit();

  VectorEncoding encoding;
  auto merged = rows(h, "enc_v", &encoding);
  EXPECT_EQ(VectorEncoding::F16, encoding);
  EXPECT_EQ(30u, merged.size());
  for (const auto& row : f16Rows) EXPECT_TRUE(std::ranges::binary_search(merged, row));
  EXPECT_EQ(knnIds("ref_v", 10), knnIds("enc_v", 10));
}

// keep_original stores the submitted floats (before cosine normalization) in
// a cold column, carried by merges; retrieval returns them.
TEST_F(VectorEncodingTest, keepOriginalSurvivesMerge) {
  CollectionHelper h("main");
  installFields(h.collection(), {.metric = api::VectorMetric::COSINE,
                                 .encoding = api::VectorEncoding::INT8, .keepOriginal = true});
  Vec raw = {3, 4, 0, 0, 0, 0, 0, 0};
  h.index(flatdoc("id", std::string("raw"), "ref_v", raw, "enc_v", raw));
  h.commit();
  indexDocs(h, 0, 10);
  h.commit();
  h.getIndexWriter()->mergeSegments();
  h.commit();

  std::string original = VectorFieldType::originalColumnName("enc_v");
  auto originals = rows(h, original);
  ASSERT_EQ(11u, originals.size());
  EXPECT_TRUE(std::ranges::binary_search(
      originals, std::string((const char*)raw.data(), raw.size() * sizeof(float))));
  EXPECT_EQ(rows(h, "ref_v").size(), originals.size());

  auto* req = LocalReq::create(luxirNode->getSearchEngine());
  req->collection("main").topDocs("q").allQuery().fields({"id", "enc_v"}).limit(-1);
  req->execute();
  ASSERT_TRUE(req->ok()) << req->errorMsg();
  bool found = false;
  for (const auto& doc : req->getDocs()) {
    if (std::get<std::string>(*find(doc, "id")) != "raw") continue;
    found = true;
    EXPECT_EQ(raw, std::get<Vec>(*find(doc, "enc_v")));
  }
  EXPECT_TRUE(found);
  req->done();
  EXPECT_EQ(knnIds("ref_v", 5), knnIds("enc_v", 5));
}

// Enabling keep_original later leaves a partial companion after a merge; it
// is not aligned, so readers ignore it.
TEST_F(VectorEncodingTest, partialOriginalColumnIsIgnored) {
  CollectionHelper h("main");
  installFields(h.collection(), {.encoding = api::VectorEncoding::INT8});
  indexDocs(h, 0, 10);
  h.commit();
  installFields(h.collection(), {.encoding = api::VectorEncoding::INT8, .keepOriginal = true});
  indexDocs(h, 10, 20);
  h.commit();
  h.getIndexWriter()->mergeSegments();
  h.commit();

  auto r = reader(h);
  ASSERT_EQ(1u, r->segments().size());
  auto& pr = r->segments()[0].postingsReader();
  const SegFieldInfo* found = pr.fieldInfo("enc_v");
  ASSERT_NE(nullptr, found);
  SegFieldInfo fi = *found, original;
  EXPECT_FALSE(findOriginalColumn(pr, fi, original));
  EXPECT_EQ(10, original.numValues);
}

// IVF+PQ overlays build from f16 / int8 columns (and from the original
// column when kept); rescored ANN results match exact search.
TEST_F(VectorEncodingTest, annOverlayBuildsFromEncodedColumn) {
  IvfTestGuard guard(/*minTraining=*/16);
  for (auto encoding : {api::VectorEncoding::F16, api::VectorEncoding::INT8}) {
    for (bool keep : {false, true}) {
      CollectionHelper h("main");
      h.clear();
      installFields(h.collection(), {.metric = api::VectorMetric::COSINE,
                                     .encoding = encoding, .keepOriginal = keep,
                                     .ann = smallPqAnn()});
      for (int i = 0; i < 160; i++) {
        Vec v = docVec(i % 30);
        v[(size_t)(i % DIMS)] += 0.001f * (float)i;
        h.index(flatdoc("id", "d" + std::to_string(i), "enc_v", v));
      }
      h.commit({"vec.enc_v"});
      auto r = reader(h);
      ASSERT_EQ(1u, r->segments().size());
      EXPECT_NE(nullptr, r->segments()[0].getAuxReader("vec.enc_v"));
      EXPECT_EQ(knnIds("enc_v", 5, true), knnIds("enc_v", 5, false, /*refine=*/320));
    }
  }
}

TEST_F(VectorEncodingTest, encodedFieldsValidateInput) {
  CollectionHelper h("main");
  installFields(h.collection(), {.encoding = api::VectorEncoding::F16});
  ExpectLog quiet({"f16 range", "non-finite"});
  Vec big(DIMS, 1.0f);
  big[2] = 1.0e5f;
  EXPECT_EQ(1, h.index(flatdoc("id", std::string("big"), "enc_v", big)).total_errors);

  installFields(h.collection(), {.encoding = api::VectorEncoding::INT8});
  Vec nan(DIMS, 1.0f);
  nan[1] = std::nanf("");
  EXPECT_EQ(1, h.index(flatdoc("id", std::string("nan"), "enc_v", nan)).total_errors);
  EXPECT_EQ(0, h.index(flatdoc("id", std::string("flat"), "enc_v", Vec(DIMS, 2.0f))).total_errors);
  h.commit();
  EXPECT_EQ(std::vector<std::string>{"flat"}, knnIds("enc_v", 3));

  SchemaBuilder b;
  auto& f = b.field("title_s2");
  f.type = api::FieldDef::FieldClass::STRING;
  f.encoding = api::VectorEncoding::F16;
  EXPECT_THROW(Schema::fromProto(b.def()), SchemaError);
}
