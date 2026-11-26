// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <format>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "luxir/codec/VectorCodec.h"
#include "luxir/index/VectorIndexBuilder.h"
#include "luxir/query/Sq8Kernel.h"
#include "luxir/query/VectorScorer.h"
#include "luxir/reader/VectorReader.h"
#include "luxir/schema/Schema.h"
#include "test/CollectionHelper.h"
#include "test/KnnTestUtils.h"
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
// More lists than any overlay below has: probes every list.
constexpr int32_t ALL_LISTS = 1 << 16;
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
  std::optional<bool> normalizeOnWrite = std::nullopt;
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
    def.normalize_on_write = f.normalizeOnWrite;
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

// Doc i's vectors for a multi-valued field: none for every fourth doc, else
// two or three.
Vecs docVecs(int i) {
  Vecs out;
  if (i % 4 == 0) return out;
  for (int j = 0; j < 2 + i % 2; j++) out.push_back(docVec((i + 11 * j) % 30));
  return out;
}

// Indexes docs [begin, end) into a multi-valued enc_v, recording each doc's
// vectors as a column of encoding e stores them.
void indexMulti(CollectionHelper& h, int begin, int end, api::VectorEncoding e,
                std::map<std::string, Vecs>& expected) {
  for (int i = begin; i < end; i++) {
    std::string id = "m" + std::to_string(i);
    Vecs vecs = docVecs(i);
    h.index(vecs.empty() ? flatdoc("id", id) : flatdoc("id", id, "enc_v", vecs));
    auto& out = expected[id];
    for (const auto& v : vecs) out.push_back(stored(e, v));
  }
}

std::vector<std::string> knnIds(std::string_view field, int32_t k, bool exact = true,
                                int32_t refine = 0, const Vec& query = QUERY) {
  auto* req = LocalReq::create(LuxirTest::luxirNode->getSearchEngine());
  auto& cur = req->collection("main").topDocs("q");
  cur.getScores().countLimit().fields({"id"});
  cur.rawQuery() = qb::knn(cur.mr(), field, query, k, ALL_LISTS, exact, refine);
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

  // All exactly representable in binary16 and bfloat16.
  EXPECT_EQ(v, stored(api::VectorEncoding::F16, v));
  EXPECT_EQ(v, stored(api::VectorEncoding::BF16, v));

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
  EXPECT_EQ(16, vcodec::bytesPerVector(VectorEncoding::BF16, DIMS));
  EXPECT_EQ(DIMS, vcodec::dimsFromBytes(VectorEncoding::SQ8, 16));
  EXPECT_EQ(DIMS, vcodec::dimsFromBytes(VectorEncoding::F16, 16));
  EXPECT_EQ(DIMS, vcodec::dimsFromBytes(VectorEncoding::BF16, 16));
  EXPECT_EQ(-1, vcodec::dimsFromBytes(VectorEncoding::BF16, 15));
  EXPECT_EQ(-1, vcodec::dimsFromBytes(VectorEncoding::SQ8, vcodec::SQ8_HEADER_BYTES));
  EXPECT_FALSE(vcodec::valid((int32_t)VectorEncoding::BIT + 1));
}

// bf16 keeps the high half of the float bits, rounded to nearest even.
TEST_F(VectorEncodingTest, bf16RoundsToNearestEven) {
  auto bf16 = [](uint32_t bits) {
    float x = std::bit_cast<float>(bits);
    uint16_t out;
    vcodec::encode(VectorEncoding::BF16, &x, 1, (char*)&out);
    return out;
  };
  EXPECT_EQ(0x3F80, bf16(0x3F808000));  // tie, even: down
  EXPECT_EQ(0x3F82, bf16(0x3F818000));  // tie, odd: up to even
  EXPECT_EQ(0x3F81, bf16(0x3F808001));  // above the tie: up
  EXPECT_EQ(0x3F80, bf16(0x3F807FFF));  // below the tie: down
  EXPECT_EQ(0xBF82, bf16(0xBF818000));  // the sign does not change rounding
  EXPECT_EQ(0x3F80, bf16(0x3F7FFFFF));  // rounding up carries into the exponent
  uint16_t h = 0x3FC0;
  float x;
  vcodec::decode(VectorEncoding::BF16, (const char*)&h, 1, &x);
  EXPECT_EQ(1.5f, x);
}

// f16, bf16 and sq8 rows score like f32 scoring of the decoded vector, for
// every metric, at widths that exercise SIMD blocks, partial tails and wide
// accumulation, from an unaligned row.  FAISS's 16-bit computers and the sq8
// kernels sum in another order than the f32 kernels, within a few float
// roundings of the terms.
TEST_F(VectorEncodingTest, encodedScoreMatchesDecodedScore) {
  for (int32_t dims : {8, 40, 400}) {
    Vec q((size_t)dims), v((size_t)dims);
    double terms = 0;
    for (int32_t i = 0; i < dims; i++) {
      q[(size_t)i] = QUERY[(size_t)i % DIMS] * (1.0f + 0.01f * (float)i);
      v[(size_t)i] = docVec(3)[(size_t)i % DIMS] - 0.003f * (float)i;
      terms += std::fabs((double)q[(size_t)i] * v[(size_t)i]) + (double)v[(size_t)i] * v[(size_t)i];
    }
    for (auto e : {VectorEncoding::F16, VectorEncoding::BF16, VectorEncoding::SQ8}) {
      std::vector<char> row(1 + (size_t)vcodec::bytesPerVector(e, dims));
      vcodec::encode(e, v.data(), dims, row.data() + 1);
      Vec decoded((size_t)dims);
      vcodec::decode(e, row.data() + 1, dims, decoded.data());
      std::string_view rowView(row.data() + 1, row.size() - 1);
      std::string_view decodedView((const char*)decoded.data(), decoded.size() * sizeof(float));
      for (auto metric : {api::VectorMetric::L2, api::VectorMetric::IP, api::VectorMetric::COSINE}) {
        VectorScorer encoded(q.data(), dims, (int32_t)metric, true, e);
        VectorScorer f32(q.data(), dims, (int32_t)metric, true, VectorEncoding::F32);
        float expected = f32.score(decodedView, 0), actual = encoded.score(rowView, 0);
        EXPECT_NEAR(expected, actual, 1e-6 * terms) << vcodec::name(e) << " " << dims << " " << (int)metric;
      }
    }
  }
}

// sq8 scores the decoded vector, so a query with a large dynamic range keeps
// the sign of a dot product that per-vector offset and scale terms cancel
// away.  [-254, 0, 256] stores exactly (offset -254, scale 2, codes 0, 127,
// 255); against [1, 2^30, 1] the decoded products are -254, 0 and 256 and sum
// to 2 in any order, while offset * sum(q) + scale * sum(q[i] * code[i]),
// with the code sum in float, loses the 255 and gives -508, below a zero
// vector.
TEST_F(VectorEncodingTest, sq8ExtremeQueryRanksByDecodedScore) {
  constexpr int32_t wide = 87;  // A block, single steps and a tail at both levels.
  const float values[] = {-254, 0, 256};
  const float terms[] = {1.0f, 0x1p30f, 1.0f};
  Vec x(wide), query(wide);
  for (int32_t i = 0; i < wide; i++) {
    x[(size_t)i] = values[i % 3];
    query[(size_t)i] = terms[i % 3];
  }
  EXPECT_EQ(x, stored(api::VectorEncoding::SQ8, x));
  for (int32_t dims : {3, 45, wide}) {
    SCOPED_TRACE(dims);
    std::vector<char> row((size_t)vcodec::bytesPerVector(VectorEncoding::SQ8, dims));
    vcodec::encode(VectorEncoding::SQ8, x.data(), dims, row.data());
    auto h = vcodec::readSq8Header(row.data());
    auto* codes = (const uint8_t*)(row.data() + vcodec::SQ8_HEADER_BYTES);
    float expected = 2.0f * (float)(dims / 3);
    for (const auto& kernels : sq8::supported()) {
      SCOPED_TRACE(kernels.name);
      EXPECT_EQ(expected, kernels.dot(query.data(), codes, h.offset, h.scale, dims));
      float normSq;
      EXPECT_EQ(expected, kernels.dotNorm(query.data(), codes, h.offset, h.scale, dims, normSq));
    }
    VectorScorer scorer(query.data(), dims, (int32_t)api::VectorMetric::IP, true, VectorEncoding::SQ8);
    EXPECT_EQ(expected, scorer.score(std::string_view(row.data(), row.size()), 0));
  }

  CollectionHelper h("main");
  SchemaBuilder b;
  auto& f = b.field("enc_v");
  f.type = api::FieldDef::FieldClass::VECTOR;
  f.dims = wide;
  f.metric = api::VectorMetric::IP;
  f.encoding = api::VectorEncoding::SQ8;
  b.set(h.collection());
  h.index(flatdoc("id", std::string("zero"), "enc_v", Vec(wide, 0.0f)));
  h.index(flatdoc("id", std::string("x"), "enc_v", x));
  h.commit();
  EXPECT_EQ((std::vector<std::string>{"x", "zero"}), knnIds("enc_v", 2, true, 0, query));
}

// Each SQ8 kernel level and VectorScorer agree with a double-precision
// reference. Moving the row to unaligned addresses preserves the score.
TEST_F(VectorEncodingTest, sq8KernelMatchesScalarReference) {
  std::mt19937 rng(42);
  std::uniform_real_distribution<float> uniform(-1.0f, 1.0f);
  // 1 to 40 reaches every tail length of both levels, alone and after steps.
  std::vector<int32_t> widths = {63, 64, 65, 768, 1023};
  for (int32_t dims = 1; dims <= 40; dims++) widths.push_back(dims);
  for (int32_t dims : widths) {
    Vec q((size_t)dims);
    for (auto& v : q) v = uniform(rng);
    auto bytes = (size_t)vcodec::bytesPerVector(VectorEncoding::SQ8, dims);
    Vec x((size_t)dims);
    for (auto& v : x) v = 3.0f * uniform(rng) + 0.5f;
    std::vector<char> buf(bytes + 16);
    vcodec::encode(VectorEncoding::SQ8, x.data(), dims, buf.data());
    Vec decoded((size_t)dims);
    vcodec::decode(VectorEncoding::SQ8, buf.data(), dims, decoded.data());
    double dot = 0, dist = 0, xx = 0, mag = 0;
    for (int32_t i = 0; i < dims; i++) {
      double qi = (double)q[(size_t)i];
      double xi = (double)decoded[(size_t)i];
      dot += qi * xi;
      dist += (qi - xi) * (qi - xi);
      xx += xi * xi;
      mag += std::fabs(qi * xi);
    }
    auto h = vcodec::readSq8Header(buf.data());
    auto* codes = (const uint8_t*)(buf.data() + vcodec::SQ8_HEADER_BYTES);
    for (const auto& kernels : sq8::supported()) {
      SCOPED_TRACE(std::format("dims {} level {}", dims, kernels.name));
      EXPECT_NEAR(dist, kernels.l2sq(q.data(), codes, h.offset, h.scale, dims), 1e-5 * (1.0 + dist));
      EXPECT_NEAR(dot, kernels.dot(q.data(), codes, h.offset, h.scale, dims), 1e-5 * (1.0 + mag));
      float normSq;
      EXPECT_NEAR(dot, kernels.dotNorm(q.data(), codes, h.offset, h.scale, dims, normSq), 1e-5 * (1.0 + mag));
      EXPECT_NEAR(xx, normSq, 1e-5 * (1.0 + xx));
    }
    for (auto metric : {api::VectorMetric::L2, api::VectorMetric::IP, api::VectorMetric::COSINE}) {
      SCOPED_TRACE(std::format("dims {} metric {}", dims, (int)metric));
      VectorScorer scorer(q.data(), dims, (int32_t)metric, true, VectorEncoding::SQ8);
      float got = scorer.score(std::string_view(buf.data(), bytes), 0);
      if (metric == api::VectorMetric::L2) {
        EXPECT_NEAR(dist, 1.0 / got - 1.0, 1e-5 * (1.0 + dist));
      } else if (metric == api::VectorMetric::IP) {
        EXPECT_NEAR(dot, got, 1e-5 * (1.0 + mag));
      } else {
        EXPECT_NEAR(dot / std::sqrt(xx), got, 1e-5 * (1.0 + mag / std::sqrt(xx)));
      }
      for (size_t shift = 1; shift < 16; shift += 5) {
        std::vector<char> moved(bytes + 16);
        std::memcpy(moved.data() + shift, buf.data(), bytes);
        EXPECT_EQ(got, scorer.score(std::string_view(moved.data() + shift, bytes), 0));
      }
    }
  }
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

// Merging same-encoding segments copies the stored values: multi-valued,
// sparse, with deletes, every live doc keeps its vectors in order.
TEST_F(VectorEncodingTest, mergeCopiesEncodedBytes) {
  CollectionHelper h("main");
  installFields(h.collection(), {.encoding = api::VectorEncoding::SQ8, .multi = true});
  std::map<std::string, Vecs> expected;
  for (int seg = 0; seg < 3; seg++) {
    indexMulti(h, seg * 12, seg * 12 + 12, api::VectorEncoding::SQ8, expected);
    h.commit();
  }
  h.deleteByIds({"m5", "m14", "m30"}, UpdateMessage::COMMIT);
  for (auto id : {"m5", "m14", "m30"}) expected.erase(id);
  EXPECT_EQ(expected, retrieved());
  mergeAll(h);
  EXPECT_EQ(std::vector{VectorEncoding::SQ8}, segmentEncodings(h));
  EXPECT_EQ(expected, retrieved());
}

// A schema change between segments: the merge converges to the schema's
// current encoding, copying the input already in it and re-encoding the
// others (multi-valued, sparse, with deletes).
TEST_F(VectorEncodingTest, mixedEncodingMergeUsesSchemaEncoding) {
  CollectionHelper h("main");
  std::map<std::string, Vecs> written;
  int next = 0;
  for (auto encoding : {api::VectorEncoding::F16, api::VectorEncoding::SQ8, api::VectorEncoding::F32,
                        api::VectorEncoding::BF16}) {
    installFields(h.collection(), {.encoding = encoding, .multi = true});
    indexMulti(h, next, next + 10, encoding, written);
    next += 10;
    h.commit();
  }
  h.deleteByIds({"m3", "m17", "m38"}, UpdateMessage::COMMIT);
  for (auto id : {"m3", "m17", "m38"}) written.erase(id);
  EXPECT_EQ(written, retrieved());
  mergeAll(h);
  EXPECT_EQ(std::vector{VectorEncoding::BF16}, segmentEncodings(h));
  auto expected = written;
  for (auto& [id, vecs] : expected) {
    for (auto& v : vecs) v = stored(api::VectorEncoding::BF16, v);
  }
  EXPECT_EQ(expected, retrieved());
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
  mergeAll(h);

  EXPECT_EQ(std::vector{VectorEncoding::F32}, segmentEncodings(h));
  auto docs = retrieved();
  EXPECT_EQ(30u, docs.size());
  for (const auto& [id, vecs] : docs) {
    ASSERT_EQ(1u, vecs.size()) << id;
    float norm = 0;
    for (float x : vecs[0]) norm += x * x;
    EXPECT_NEAR(1.0f, norm, 1e-5) << id;
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

// IVF+PQ overlays build from f16, bf16 and sq8 columns.  The IVF path scans
// PQ codes and rescores candidates from the encoded column; with every list
// probed and every candidate refined, it returns the exact top-k.
TEST_F(VectorEncodingTest, annOverlayBuildsFromEncodedColumn) {
  ForceIvfGuard forceIvf;
  IvfTestGuard guard(/*minTraining=*/16);
  for (auto encoding : {api::VectorEncoding::F16, api::VectorEncoding::BF16, api::VectorEncoding::SQ8}) {
    SCOPED_TRACE(vcodec::name((VectorEncoding)encoding));
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

    auto exact = knnIds("enc_v", 5, true);
    KnnQuery::Stats stats;
    SearchOverridesGuard counters(KnnQuery::statsForTests);
    KnnQuery::statsForTests = &stats;
    EXPECT_EQ(exact, knnIds("enc_v", 5, false, /*refine=*/320));
    EXPECT_GT(stats.pqScores.load(), 0);
    EXPECT_GT(stats.terminalReads.load(), 0);
    EXPECT_EQ(0, stats.vectorsScored.load());
  }
}

TEST_F(VectorEncodingTest, encodedFieldsValidateInput) {
  CollectionHelper h("main");
  installFields(h.collection(), {.encoding = api::VectorEncoding::F16});
  ExpectLog quiet({"f16 range", "non-finite"});
  Vec edge(DIMS, 1.0f);
  edge[0] = 65504.0f;
  edge[1] = -65504.0f;
  EXPECT_EQ(0, h.index(flatdoc("id", std::string("edge"), "enc_v", edge)).total_errors);
  Vec big(DIMS, 1.0f);
  big[2] = -65505.0f;
  EXPECT_EQ(1, h.index(flatdoc("id", std::string("big"), "enc_v", big)).total_errors);
  h.commit();
  EXPECT_EQ(Vecs{edge}, retrieved()["edge"]);

  installFields(h.collection(), {.encoding = api::VectorEncoding::SQ8});
  Vec nan(DIMS, 1.0f);
  nan[1] = std::nanf("");
  EXPECT_EQ(1, h.index(flatdoc("id", std::string("nan"), "enc_v", nan)).total_errors);
  EXPECT_EQ(0, h.index(flatdoc("id", std::string("flat"), "enc_v", Vec(DIMS, 2.0f))).total_errors);
  h.commit();
  EXPECT_EQ((std::vector<std::string>{"flat", "edge"}), knnIds("enc_v", 3));

  SchemaBuilder b;
  auto& f = b.field("title_s2");
  f.type = api::FieldDef::FieldClass::STRING;
  f.encoding = api::VectorEncoding::F16;
  EXPECT_THROW(Schema::fromProto(b.def()), SchemaError);
}

// A raw cosine vector whose stored value has no direction is skipped like a
// zero vector: tiny elements underflow to zero in f16, and an sq8 vector in
// the subnormal range loses its scale.
TEST_F(VectorEncodingTest, zeroDirectionAfterEncodingIsSkipped) {
  for (auto encoding : {api::VectorEncoding::F16, api::VectorEncoding::SQ8}) {
    SCOPED_TRACE(vcodec::name((VectorEncoding)encoding));
    CollectionHelper h("main");
    h.clear();
    installFields(h.collection(), {.metric = api::VectorMetric::COSINE, .encoding = encoding,
                                   .normalizeOnWrite = false});
    Vec tiny(DIMS, 0.0f);
    tiny[1] = encoding == api::VectorEncoding::F16 ? 1e-8f : FLT_TRUE_MIN;
    ExpectLog skipped("skipping zero / near-zero vector");
    EXPECT_EQ(0, h.index(flatdoc("id", std::string("tiny"), "enc_v", tiny)).total_errors);
    EXPECT_EQ(1u, skipped.suppressed());
    h.index(flatdoc("id", std::string("ok"), "enc_v", docVec(1)));
    h.commit();
    EXPECT_EQ(std::vector<std::string>{"ok"}, knnIds("enc_v", 2));
    EXPECT_EQ(Vecs{}, retrieved()["tiny"]);
  }
}
