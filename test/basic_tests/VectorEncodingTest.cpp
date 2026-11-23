// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <cstring>
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <map>
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
using Vecs = std::vector<Vec>;

// Doc i lies at a monotonically worse L2 distance, inner product, and cosine
// from QUERY as i grows, with gaps far above sq8 quantization error.
const Vec QUERY = {0.9f, -0.4f, 0.3f, 0.8f, -0.7f, 0.2f, 0.5f, -0.1f};
const Vec SIDE = {0.1f, 0.6f, 0.4f, -0.2f, 0.1f, 0.7f, -0.3f, 0.2f};

Vec docVec(int i) {
  Vec v((size_t)DIMS);
  for (int32_t j = 0; j < DIMS; j++) {
    v[(size_t)j] = QUERY[(size_t)j] * (1.0f - 0.05f * (float)i) + SIDE[(size_t)j] * 0.1f * (float)i;
  }
  return v;
}

// x as a column of encoding e stores it, decoded.
Vec stored(api::VectorEncoding e, const Vec& x) {
  auto encoding = (VectorEncoding)e;
  std::vector<char> bytes((size_t)vcodec::bytesPerVector(encoding, (int32_t)x.size()));
  vcodec::encode(encoding, x.data(), (int32_t)x.size(), bytes.data());
  Vec out(x.size());
  vcodec::decode(encoding, bytes.data(), (int32_t)x.size(), out.data());
  return out;
}

struct Fields {
  api::VectorMetric metric = api::VectorMetric::L2;
  api::VectorEncoding encoding = api::VectorEncoding::F32;
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
    if (f.multi) def.multi = api::FieldDef::MultiMode::ARRAY;
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
                                int32_t refine = 0, const Vec& query = QUERY) {
  auto* req = LocalReq::create(LuxirTest::luxirNode->getSearchEngine());
  auto& cur = req->collection("main").topDocs("q");
  cur.getScores().countLimit().fields({"id"});
  cur.rawQuery() = qb::knn(cur.mr(), field, query, k, /*nprobe=*/0, exact, refine);
  req->execute();
  EXPECT_TRUE(req->ok()) << req->errorMsg();
  std::vector<std::string> ids;
  for (const auto& doc : req->getDocs()) ids.push_back(std::get<std::string>(*find(doc, "id")));
  req->done();
  return ids;
}

// Every live doc's enc_v vectors by id, as retrieval decodes them.
std::map<std::string, Vecs> retrieved() {
  auto* req = LocalReq::create(LuxirTest::luxirNode->getSearchEngine());
  req->collection("main").topDocs("q").allQuery().fields({"id", "enc_v"}).limit(-1);
  req->execute();
  EXPECT_TRUE(req->ok()) << req->errorMsg();
  std::map<std::string, Vecs> out;
  for (const auto& doc : req->getDocs()) {
    auto& vecs = out[std::get<std::string>(*find(doc, "id"))];
    if (const auto* val = find(doc, "enc_v")) {
      if (const auto* one = std::get_if<Vec>(val)) vecs.push_back(*one);
      else vecs = std::get<Vecs>(*val);
    }
  }
  req->done();
  return out;
}

std::shared_ptr<IndexReader> reader(CollectionHelper& h) {
  return h.getIndexWriter()->snapshots.readers.getReader();
}

// enc_v's encoding in each segment that has it.
std::vector<VectorEncoding> segmentEncodings(CollectionHelper& h) {
  std::vector<VectorEncoding> out;
  auto r = reader(h);
  for (auto& seg : r->segments()) {
    if (const DecodedFieldRecord* found = seg.postingsReader().fieldInfo("enc_v")) {
      out.push_back(VectorReader::encodingOf(*found));
    }
  }
  return out;
}

void mergeAll(CollectionHelper& h) {
  h.getIndexWriter()->mergeSegments();
  h.commit();
  ASSERT_EQ(1u, reader(h)->segments().size());
}

// Every stored row of a column across segments, sorted (merge-order free).
std::vector<std::string> rows(CollectionHelper& h, std::string_view field,
                              VectorEncoding* encoding = nullptr) {
  std::vector<std::string> out;
  auto r = reader(h);
  for (auto& seg : r->segments()) {
    const DecodedFieldRecord* found = seg.postingsReader().fieldInfo(field);
    if (!found) continue;
    VectorReader vr(seg.postingsReader(), *found);
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

  vcodec::encode(VectorEncoding::SQ8, v.data(), DIMS, buf.data());
  vcodec::decode(VectorEncoding::SQ8, buf.data(), DIMS, out.data());
  float halfStep = (3.0f - -1.5f) / 255.0f / 2.0f;
  for (size_t i = 0; i < v.size(); i++) EXPECT_NEAR(v[i], out[i], halfStep * 1.001f);
  EXPECT_EQ(-1.5f, out[0]);  // the minimum is the offset itself

  Vec constant(DIMS, 0.75f);
  vcodec::encode(VectorEncoding::SQ8, constant.data(), DIMS, buf.data());
  EXPECT_EQ(0.0f, vcodec::readSq8Header(buf.data()).scale);
  vcodec::decode(VectorEncoding::SQ8, buf.data(), DIMS, out.data());
  EXPECT_EQ(constant, out);

  EXPECT_EQ(16, vcodec::bytesPerVector(VectorEncoding::SQ8, DIMS));
  EXPECT_EQ(DIMS, vcodec::dimsFromBytes(VectorEncoding::SQ8, 16));
  EXPECT_EQ(DIMS, vcodec::dimsFromBytes(VectorEncoding::F16, 16));
}

// f16, bf16 and sq8 rows score exactly like f32 scoring of the decoded
// vector, for every metric.
TEST_F(VectorEncodingTest, encodedScoreMatchesDecodedScore) {
  Vec v = docVec(3);
  for (auto e : {VectorEncoding::F16, VectorEncoding::BF16, VectorEncoding::SQ8}) {
    std::vector<char> row((size_t)vcodec::bytesPerVector(e, DIMS));
    vcodec::encode(e, v.data(), DIMS, row.data());
    Vec decoded(DIMS);
    vcodec::decode(e, row.data(), DIMS, decoded.data());
    std::string_view rowView(row.data(), row.size());
    std::string_view decodedView((const char*)decoded.data(), decoded.size() * sizeof(float));
    for (auto metric : {api::VectorMetric::L2, api::VectorMetric::IP, api::VectorMetric::COSINE}) {
      VectorScorer encoded(QUERY.data(), DIMS, (int32_t)metric, true, e);
      VectorScorer f32(QUERY.data(), DIMS, (int32_t)metric, true, VectorEncoding::F32);
      EXPECT_EQ(f32.score(decodedView, 0), encoded.score(rowView, 0))
          << vcodec::name(e) << " " << (int)metric;
    }
  }
}

// sq8 scores the decoded vector, so a query with a large dynamic range keeps
// the sign of a dot product that per-vector offset and scale terms cancel away.
TEST_F(VectorEncodingTest, sq8ExtremeQueryRanksByDecodedScore) {
  const Vec x = {-255, 255, 255};
  const Vec query = {0x1p30f, 0x1p30f, 1.0f};
  std::vector<char> row((size_t)vcodec::bytesPerVector(VectorEncoding::SQ8, 3));
  vcodec::encode(VectorEncoding::SQ8, x.data(), 3, row.data());
  VectorScorer scorer(query.data(), 3, (int32_t)api::VectorMetric::IP, true, VectorEncoding::SQ8);
  EXPECT_EQ(255.0f, scorer.score(std::string_view(row.data(), row.size()), 0));

  CollectionHelper h("main");
  SchemaBuilder b;
  auto& f = b.field("enc_v");
  f.type = api::FieldDef::FieldClass::VECTOR;
  f.dims = 3;
  f.metric = api::VectorMetric::IP;
  f.encoding = api::VectorEncoding::SQ8;
  b.set(h.collection());
  h.index(flatdoc("id", std::string("zero"), "enc_v", Vec{0, 0, 0}));
  h.index(flatdoc("id", std::string("x"), "enc_v", x));
  h.commit();
  EXPECT_EQ((std::vector<std::string>{"x", "zero"}), knnIds("enc_v", 2, true, 0, query));
}

// Each encoding is recorded per segment and reads back within its precision.
TEST_F(VectorEncodingTest, storedRoundTripPerEncoding) {
  for (auto encoding : {api::VectorEncoding::F32, api::VectorEncoding::F16, api::VectorEncoding::BF16,
                        api::VectorEncoding::SQ8}) {
    CollectionHelper h("main");
    h.clear();
    installFields(h.collection(), {.encoding = encoding});
    indexDocs(h, 0, 5);
    h.commit();

    auto r = reader(h);
    auto& seg = r->segments()[0];
    const DecodedFieldRecord* found = seg.postingsReader().fieldInfo("enc_v");
    ASSERT_NE(nullptr, found);
    VectorReader vr(seg.postingsReader(), *found);
    EXPECT_EQ((VectorEncoding)encoding, vr.encoding());
    EXPECT_EQ(DIMS, vr.dims());
    EXPECT_EQ(vcodec::bytesPerVector((VectorEncoding)encoding, DIMS), vr.valueBytes());
    Vec out(DIMS);
    vr.decodeAtRank(0, out.data());
    Vec expected = docVec(0);
    for (int32_t j = 0; j < DIMS; j++) EXPECT_NEAR(expected[(size_t)j], out[(size_t)j], 0.005f);
  }
}

// Exact kNN over f16 / bf16 / sq8 ranks like f32 on well-separated data, for
// every metric.
TEST_F(VectorEncodingTest, exactKnnMatchesF32Ranking) {
  for (auto metric : {api::VectorMetric::L2, api::VectorMetric::IP, api::VectorMetric::COSINE}) {
    for (auto encoding : {api::VectorEncoding::F16, api::VectorEncoding::BF16, api::VectorEncoding::SQ8}) {
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
  installFields(h.collection(), {.encoding = api::VectorEncoding::SQ8, .multi = true});
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
  EXPECT_EQ(VectorEncoding::SQ8, encoding);
  EXPECT_EQ(idsBefore, knnIds("enc_v", 10));
}

// A schema change between segments: the merge converges to the schema's
// current encoding, transcoding the older input.
TEST_F(VectorEncodingTest, mixedEncodingMergeUsesSchemaEncoding) {
  CollectionHelper h("main");
  installFields(h.collection(), {.encoding = api::VectorEncoding::F16});
  indexDocs(h, 0, 15);
  h.commit();
  installFields(h.collection(), {.encoding = api::VectorEncoding::SQ8});
  indexDocs(h, 15, 30);
  h.commit();
  h.getIndexWriter()->mergeSegments();
  h.commit();

  VectorEncoding encoding;
  auto merged = rows(h, "enc_v", &encoding);
  EXPECT_EQ(VectorEncoding::SQ8, encoding);
  EXPECT_EQ(30u, merged.size());
  EXPECT_EQ(knnIds("ref_v", 10), knnIds("enc_v", 10));
}

// Promoting quantized cosine rows to f32 in a merge restores unit length.
TEST_F(VectorEncodingTest, promotionToF32RenormalizesCosine) {
  CollectionHelper h("main");
  installFields(h.collection(), {.metric = api::VectorMetric::COSINE, .encoding = api::VectorEncoding::SQ8});
  indexDocs(h, 0, 15);
  h.commit();
  installFields(h.collection(), {.metric = api::VectorMetric::COSINE, .encoding = api::VectorEncoding::F32});
  indexDocs(h, 15, 30);
  h.commit();
  h.getIndexWriter()->mergeSegments();
  h.commit();

  VectorEncoding encoding;
  auto merged = rows(h, "enc_v", &encoding);
  EXPECT_EQ(VectorEncoding::F32, encoding);
  for (const auto& row : merged) {
    float norm = 0;
    for (int32_t j = 0; j < DIMS; j++) {
      float x;
      std::memcpy(&x, row.data() + j * sizeof(float), sizeof(float));
      norm += x * x;
    }
    EXPECT_NEAR(1.0f, norm, 1e-5);
  }
}

// Re-encoding to f16 in a merge clamps elements beyond its range and logs the
// clamped count once for the field.  Every input is in an out-of-date
// encoding here.
TEST_F(VectorEncodingTest, mergeToF16Clamps) {
  CollectionHelper h("main");
  installFields(h.collection(), {});
  Vec big = docVec(1);
  big[2] = 100000.0f;
  h.index(flatdoc("id", std::string("big"), "enc_v", big));
  h.commit();
  h.index(flatdoc("id", std::string("small"), "enc_v", docVec(2)));
  h.commit();
  installFields(h.collection(), {.encoding = api::VectorEncoding::F16});
  ExpectLog warn("field 'enc_v': clamped 1 element(s)");
  mergeAll(h);
  EXPECT_EQ(1u, warn.suppressed());
  EXPECT_EQ(std::vector{VectorEncoding::F16}, segmentEncodings(h));
  Vec clamped = big;
  clamped[2] = 65504.0f;
  auto docs = retrieved();
  EXPECT_EQ(Vecs{stored(api::VectorEncoding::F16, clamped)}, docs["big"]);
  EXPECT_EQ(Vecs{stored(api::VectorEncoding::F16, docVec(2))}, docs["small"]);
}

// IVF+PQ overlays build from f16 / bf16 / sq8 columns; rescored ANN results match
// exact search.
TEST_F(VectorEncodingTest, annOverlayBuildsFromEncodedColumn) {
  IvfTestGuard guard(/*minTraining=*/16);
  for (auto encoding : {api::VectorEncoding::F16, api::VectorEncoding::BF16, api::VectorEncoding::SQ8}) {
    CollectionHelper h("main");
    h.clear();
    installFields(h.collection(), {.metric = api::VectorMetric::COSINE, .encoding = encoding,
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

TEST_F(VectorEncodingTest, encodedFieldsValidateInput) {
  CollectionHelper h("main");
  installFields(h.collection(), {.encoding = api::VectorEncoding::F16});
  ExpectLog quiet({"f16 range", "non-finite"});
  Vec big(DIMS, 1.0f);
  big[2] = 1.0e5f;
  EXPECT_EQ(1, h.index(flatdoc("id", std::string("big"), "enc_v", big)).total_errors);

  installFields(h.collection(), {.encoding = api::VectorEncoding::SQ8});
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
