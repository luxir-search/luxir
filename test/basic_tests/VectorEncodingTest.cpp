// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>
#include <numeric>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "luxir/codec/VectorCodec.h"
#include "luxir/codec/VectorKernels.h"
#include "luxir/query/KnnQuery.h"
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
  bool raw = false;  // cosine with normalize_on_write off
  std::optional<api::VectorAnnDef> ann;
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
    if (f.raw) def.normalize_on_write = false;
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

// (id, score) of an executed request's "q" list; finishes the request.
std::vector<std::pair<std::string, float>> scoredHits(LocalReq* req) {
  EXPECT_TRUE(req->ok()) << req->errorMsg();
  std::vector<std::pair<std::string, float>> hits;
  if (req->ok()) {
    const auto* dl = req->docList("q");
    const auto* ids = std::get_if<api::ColStr>(&dl->columns.find("id")->kind);
    const auto* scores = std::get_if<api::ColFloat>(&dl->columns.find("_score_")->kind);
    for (size_t i = 0; i < ids->v.size(); i++) hits.emplace_back(ids->v[i], scores->v[i]);
  }
  req->done();
  return hits;
}

// (id, score) of a kNN request, serial or parallel.
std::vector<std::pair<std::string, float>> knnHits(std::string_view field, int32_t k, bool exact,
                                                   int32_t refine, bool parallel) {
  auto* req = LocalReq::create(LuxirTest::luxirNode->getSearchEngine());
  auto& cur = req->collection("main").topDocs("q");
  cur.getScores().getNumber().fields({"id"});
  cur.rawQuery() = qb::knn(cur.mr(), field, QUERY, k, /*nprobe=*/0, exact, refine);
  req->execute(parallel);
  return scoredHits(req);
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

// A kNN query with several late-interaction query vectors.
api::Query maxSimKnn(std::pmr::memory_resource& mr, std::string_view field,
                     const std::vector<Vec>& queries, int32_t k) {
  api::Query q = qb::knn(mr, field, queries[0], k);
  auto& knn = std::get<api::KnnQuery>(q.kind);
  knn.query.reset();
  api::Vector* vs = api::build::allocArray(knn.queries, queries.size(), mr);
  for (size_t i = 0; i < queries.size(); i++) {
    float* a = api::build::allocArray(vs[i].f32.emplace().v, queries[i].size(), mr);
    std::copy(queries[i].begin(), queries[i].end(), a);
  }
  return q;
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

  vcodec::encode(VectorEncoding::BF16, v.data(), DIMS, buf.data());
  vcodec::decode(VectorEncoding::BF16, buf.data(), DIMS, out.data());
  EXPECT_EQ(v, out);  // all exactly representable in bfloat16
  EXPECT_EQ(1.0f, vcodec::fromBf16(vcodec::toBf16(1.0f + 0x1p-8f)));  // tie to even
  EXPECT_EQ(1.0f + 0x1p-7f, vcodec::fromBf16(vcodec::toBf16(1.0f + 0x1.8p-8f)));
  EXPECT_TRUE(std::isfinite(vcodec::fromBf16(vcodec::toBf16(std::numeric_limits<float>::max()))));

  vcodec::encode(VectorEncoding::BIT, v.data(), DIMS, buf.data());
  EXPECT_EQ(0x6b, (uint8_t)buf[0]);  // 0 1 1 0 1 0 1 1, most significant bit first
  vcodec::decode(VectorEncoding::BIT, buf.data(), DIMS, out.data());
  EXPECT_EQ((Vec{0, 1, 1, 0, 1, 0, 1, 1}), out);
  EXPECT_EQ(1, vcodec::bytesPerVector(VectorEncoding::BIT, DIMS));
  EXPECT_EQ(DIMS, vcodec::dimsFromBytes(VectorEncoding::BIT, 1));

  EXPECT_EQ(VectorEncoding::F32, vcodec::mergeEncoding(VectorEncoding::F16, VectorEncoding::BF16));
  EXPECT_EQ(VectorEncoding::BF16, vcodec::mergeEncoding(VectorEncoding::INT8, VectorEncoding::BF16));
}

// The int8 decomposition (no decode buffer) agrees with scoring the decoded
// floats, for every metric.
TEST_F(VectorEncodingTest, int8ScorerMatchesDecodedScore) {
  Vec v = docVec(3);
  std::vector<char> row((size_t)vcodec::rowBytes(VectorEncoding::INT8, DIMS, true));
  vcodec::encodeRow(VectorEncoding::INT8, true, v.data(), DIMS, row.data());
  Vec decoded(DIMS);
  vcodec::decode(VectorEncoding::INT8, vcodec::rowPayload(row.data(), true), DIMS, decoded.data());
  for (auto metric : {api::VectorMetric::L2, api::VectorMetric::IP, api::VectorMetric::COSINE}) {
    VectorScorer q(QUERY.data(), DIMS, (int32_t)metric, true, VectorEncoding::INT8, true);
    VectorScorer f(QUERY.data(), DIMS, (int32_t)metric, false, VectorEncoding::F32, false);
    float expected = f.score((const char*)decoded.data());
    if (metric == api::VectorMetric::COSINE) {
      expected /= std::sqrt(std::inner_product(decoded.begin(), decoded.end(), decoded.begin(), 0.0f));
    }
    EXPECT_NEAR(expected, q.score(row.data()), 1e-5) << (int)metric;
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
    // L2: only int8 stores the norm prefix.
    bool norms = encoding == api::VectorEncoding::INT8;
    EXPECT_EQ(norms, vr.hasNorms());
    EXPECT_EQ(vcodec::rowBytes((VectorEncoding)encoding, DIMS, norms), vr.valueBytes());
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
    for (auto encoding : {api::VectorEncoding::F16, api::VectorEncoding::BF16,
                          api::VectorEncoding::INT8}) {
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

// The SIMD kernels behind VectorScorer match a double-precision reference over
// the decoded vector for every encoding x metric, at odd dims and unaligned
// row addresses, and a row scores bit-identically alone or inside a batch.
TEST_F(VectorEncodingTest, kernelsMatchScalarReference) {
  SCOPED_TRACE(vkernel::isa());
  std::mt19937 rng(42);
  std::uniform_real_distribution<float> uniform(-1.0f, 1.0f);
  constexpr int ROWS = 5;
  for (int32_t dims : {1, 3, 7, 15, 16, 17, 768, 1023}) {
    Vec q((size_t)dims);
    for (auto& x : q) x = uniform(rng);
    for (auto encoding : {VectorEncoding::F32, VectorEncoding::F16, VectorEncoding::BF16,
                          VectorEncoding::INT8}) {
      int32_t bytes = vcodec::rowBytes(encoding, dims, true);
      // Row r starts r bytes past an 8-byte boundary.
      std::vector<std::vector<char>> bufs(ROWS, std::vector<char>((size_t)bytes + 16));
      std::vector<const char*> rows;
      std::vector<Vec> decoded;
      for (int r = 0; r < ROWS; r++) {
        Vec x((size_t)dims);
        for (auto& v : x) v = uniform(rng);
        char* row = (char*)(((uintptr_t)bufs[(size_t)r].data() + 7) & ~(uintptr_t)7) + r;
        vcodec::encodeRow(encoding, true, x.data(), dims, row);
        decoded.emplace_back((size_t)dims);
        vcodec::decode(encoding, vcodec::rowPayload(row, true), dims, decoded.back().data());
        rows.push_back(row);
      }
      for (auto metric : {api::VectorMetric::L2, api::VectorMetric::IP, api::VectorMetric::COSINE}) {
        SCOPED_TRACE(std::format("dims {} encoding {} metric {}", dims, vcodec::name(encoding),
                                 (int)metric));
        VectorScorer scorer(q.data(), dims, (int32_t)metric, /*normalizeColumn=*/true, encoding,
                            /*norms=*/true);
        std::vector<float> batch(ROWS);
        scorer.scoreRows(rows.data(), ROWS, batch.data());
        for (int r = 0; r < ROWS; r++) {
          const Vec& x = decoded[(size_t)r];
          double dot = 0, dist = 0, xx = 0, mag = 0;
          for (int32_t i = 0; i < dims; i++) {
            double qi = (double)q[(size_t)i];
            double xi = (double)x[(size_t)i];
            dot += qi * xi;
            dist += (qi - xi) * (qi - xi);
            xx += xi * xi;
            mag += std::fabs(qi * xi);
          }
          float got = batch[(size_t)r];
          if (metric == api::VectorMetric::L2) {
            EXPECT_NEAR(dist, 1.0 / got - 1.0, 1e-5 * (1.0 + dist));
          } else if (metric == api::VectorMetric::IP) {
            EXPECT_NEAR(dot, got, 1e-5 * (1.0 + mag));
          } else {
            EXPECT_NEAR(dot / std::sqrt(xx), got, 1e-5 * (1.0 + mag / std::sqrt(xx)));
          }
          EXPECT_EQ(scorer.score(rows[(size_t)r]), got);
        }
      }
    }
  }
}

// The Hamming kernel matches a bytewise popcount at every length around the
// SIMD widths and at unaligned addresses.
TEST_F(VectorEncodingTest, hammingKernelMatchesPopcount) {
  std::mt19937 rng(7);
  std::vector<uint8_t> a(300), b(300);
  for (auto& x : a) x = (uint8_t)rng();
  for (auto& x : b) x = (uint8_t)rng();
  for (int32_t n = 0; n < 260; n++) {
    int32_t expected = 0;
    for (int32_t i = 0; i < n; i++) expected += std::popcount((uint8_t)(a[(size_t)i + 1] ^ b[(size_t)i + 3]));
    EXPECT_EQ(expected, vkernel::hamming(a.data() + 1, b.data() + 3, n)) << n;
  }
}

// metric hamming stores bit vectors (the default encoding) and scores
// 1 - hamming / dims by exact scan; schemas that mix bit and float scoring are
// rejected.
TEST_F(VectorEncodingTest, bitVectorsScoreByHamming) {
  CollectionHelper h("main");
  SchemaBuilder b;
  auto& def = b.field("bits_v");
  def.type = api::FieldDef::FieldClass::VECTOR;
  def.metric = api::VectorMetric::HAMMING;
  def.dims = 16;
  b.set(h.collection());
  Vec q(16, -1.0f);
  for (int i = 0; i < 4; i++) {
    Vec v = q;
    for (int j = 0; j < 3 * i; j++) v[(size_t)j] = 0.5f;  // 3 * i bits differ
    h.index(flatdoc("id", "b" + std::to_string(i), "bits_v", v));
  }
  ExpectLog quiet({"expected dims", "Ignoring vector aux selector"});
  EXPECT_EQ(1, h.index(flatdoc("id", std::string("short"), "bits_v", Vec(12, 1.0f))).total_errors);
  h.commit({"vec.bits_v"});
  EXPECT_EQ(nullptr, reader(h)->segments()[0].getAuxReader("vec.bits_v"));
  VectorEncoding encoding;
  EXPECT_EQ(2u, rows(h, "bits_v", &encoding)[0].size());
  EXPECT_EQ(VectorEncoding::BIT, encoding);

  auto* req = LocalReq::create(luxirNode->getSearchEngine());
  auto& cur = req->collection("main").topDocs("q");
  cur.getScores().fields({"id"});
  cur.rawQuery() = qb::knn(cur.mr(), "bits_v", q, 3);
  req->execute();
  EXPECT_EQ((std::vector<std::pair<std::string, float>>{
                {"b0", 1.0f}, {"b1", 1.0f - 3.0f / 16}, {"b2", 1.0f - 6.0f / 16}}),
            scoredHits(req));

  auto reject = [](api::VectorMetric metric, std::optional<api::VectorEncoding> encoding, int32_t dims) {
    SchemaBuilder sb;
    auto& f = sb.field("x_v");
    f.type = api::FieldDef::FieldClass::VECTOR;
    f.metric = metric;
    f.encoding = encoding;
    f.dims = dims;
    EXPECT_THROW(Schema::fromProto(sb.def()), SchemaError);
  };
  reject(api::VectorMetric::HAMMING, api::VectorEncoding::F32, 16);
  reject(api::VectorMetric::COSINE, api::VectorEncoding::BIT, 16);
  reject(api::VectorMetric::HAMMING, std::nullopt, 12);
}

// f16 and bf16 trade significand for range, so merging the two writes f32.
TEST_F(VectorEncodingTest, f16AndBf16MergeToF32) {
  CollectionHelper h("main");
  installFields(h.collection(), {.encoding = api::VectorEncoding::F16});
  indexDocs(h, 0, 15);
  h.commit();
  installFields(h.collection(), {.encoding = api::VectorEncoding::BF16});
  indexDocs(h, 15, 30);
  h.commit();
  auto before = knnIds("enc_v", 10);
  h.getIndexWriter()->mergeSegments();
  h.commit();
  VectorEncoding encoding;
  EXPECT_EQ(30u, rows(h, "enc_v", &encoding).size());
  EXPECT_EQ(VectorEncoding::F32, encoding);
  EXPECT_EQ(before, knnIds("enc_v", 10));
}

// Cosine over a column that is not unit length stores |x|^2 once per vector
// (the norm prefix), equal to the decoded vector's; merges carry it.  f32
// normalized on write needs none.
TEST_F(VectorEncodingTest, normPrefixStoredAndMerged) {
  auto checkNorms = [](CollectionHelper& h, bool expected) {
    auto r = reader(h);
    for (auto& seg : r->segments()) {
      const SegFieldInfo* found = seg.postingsReader().fieldInfo("enc_v");
      ASSERT_NE(nullptr, found);
      SegFieldInfo fi = *found;
      VectorReader vr(seg.postingsReader(), fi);
      ASSERT_EQ(expected, vr.hasNorms());
      for (int64_t i = 0; expected && i < vr.numVectors(); i++) {
        const char* row = vr.rowAtRank(i).data();
        EXPECT_EQ((float)vcodec::payloadNormSq(vr.encoding(), row + vcodec::NORM_BYTES, vr.dims()),
                  vcodec::rowNormSq(row));
      }
    }
  };
  for (auto encoding : {api::VectorEncoding::F32, api::VectorEncoding::F16, api::VectorEncoding::INT8}) {
    CollectionHelper h("main");
    h.clear();
    installFields(h.collection(), {.metric = api::VectorMetric::COSINE, .encoding = encoding,
                                   .raw = true});
    indexDocs(h, 0, 15);
    h.commit();
    indexDocs(h, 15, 30);
    h.commit();
    checkNorms(h, true);
    auto before = knnHits("enc_v", 10, true, 0, false);
    EXPECT_EQ("d0", before[0].first);
    h.getIndexWriter()->mergeSegments();
    h.commit();
    ASSERT_EQ(1u, reader(h)->segments().size());
    checkNorms(h, true);
    EXPECT_EQ(before, knnHits("enc_v", 10, true, 0, false));
  }

  CollectionHelper h("main");
  h.clear();
  installFields(h.collection(), {.metric = api::VectorMetric::COSINE});
  indexDocs(h, 0, 5);
  h.commit();
  checkNorms(h, false);
}

// A segment written before a metric change has no norm prefix: it scores
// exactly as one with the prefix, and the merge adds the prefix.
TEST_F(VectorEncodingTest, mergeAddsNormsAfterMetricChange) {
  CollectionHelper h("main");
  installFields(h.collection(), {.metric = api::VectorMetric::IP});
  indexDocs(h, 0, 15);
  h.commit();
  installFields(h.collection(), {.metric = api::VectorMetric::COSINE, .raw = true});
  indexDocs(h, 15, 30);
  h.commit();
  auto before = knnHits("enc_v", 10, true, 0, false);
  EXPECT_EQ("d0", before[0].first);
  h.getIndexWriter()->mergeSegments();
  h.commit();
  auto r = reader(h);
  ASSERT_EQ(1u, r->segments().size());
  const SegFieldInfo* found = r->segments()[0].postingsReader().fieldInfo("enc_v");
  ASSERT_NE(nullptr, found);
  SegFieldInfo fi = *found;
  EXPECT_TRUE(fi.vectorNorms);
  EXPECT_EQ(before, knnHits("enc_v", 10, true, 0, false));
}

// Batched scoring keeps parallel == serial bit-identical (ids and scores) for
// every encoding, over flat scans and the ANN rescore.
TEST_F(VectorEncodingTest, parallelMatchesSerialPerEncoding) {
  IvfTestGuard guard(/*minTraining=*/16);
  int64_t savedGrain = KnnQuery::rescoreTaskGrainCandidates;
  KnnQuery::rescoreTaskGrainCandidates = 1;
  for (auto encoding : {api::VectorEncoding::F32, api::VectorEncoding::F16, api::VectorEncoding::INT8}) {
    for (auto metric : {api::VectorMetric::L2, api::VectorMetric::COSINE}) {
      CollectionHelper h("main");
      h.clear();
      installFields(h.collection(), {.metric = metric, .encoding = encoding, .raw = true,
                                     .ann = smallPqAnn()});
      for (int seg = 0; seg < 3; seg++) {
        for (int i = 0; i < 160; i++) {
          Vec v = docVec(i % 30);
          v[(size_t)(i % DIMS)] += 0.001f * (float)(i + seg);
          h.index(flatdoc("id", std::format("s{}d{}", seg, i), "enc_v", v));
        }
        if (seg < 2) {
          h.commit({"vec.enc_v"});
        } else {
          h.commit();
        }
      }
      for (bool exact : {true, false}) {
        auto serial = knnHits("enc_v", 10, exact, 64, false);
        ASSERT_EQ(10u, serial.size());
        EXPECT_EQ(serial, knnHits("enc_v", 10, exact, 64, true))
            << (int)encoding << " " << (int)metric << " exact " << exact;
      }
    }
  }
  KnnQuery::rescoreTaskGrainCandidates = savedGrain;
}

// multi_vector_scoring maxsim: several query vectors score a document by
// sum over query vectors of its best vector's similarity, exactly, across
// segments, inside filters, serial or parallel; the field gets no ANN overlay.
TEST_F(VectorEncodingTest, maxSimScoresLateInteraction) {
  CollectionHelper h("main");
  SchemaBuilder b;
  auto& def = b.field("tok_vs");
  def.type = api::FieldDef::FieldClass::VECTOR;
  def.metric = api::VectorMetric::IP;
  def.multi = true;
  def.multi_vector_scoring = api::FieldDef::MultiVectorScoring::MAXSIM;
  auto schema = b.set(h.collection());
  EXPECT_FALSE(((const VectorFieldType*)schema->getFieldTypePtr("tok_vs"))->annIndexable());
  using Vecs = std::vector<Vec>;
  h.index(flatdoc("id", std::string("a"), "kind_s", std::string("drop"), "tok_vs", Vecs{{1, 0}, {0, 1}}));
  h.index(flatdoc("id", std::string("b"), "kind_s", std::string("keep"), "tok_vs", Vecs{{1, 0}, {1, 0}}));
  h.commit();
  h.index(flatdoc("id", std::string("c"), "kind_s", std::string("keep"), "tok_vs", Vecs{{0.6f, 0.6f}}));
  h.commit();

  Vecs queries = {{1, 0}, {0, 1}};
  auto search = [&](bool filtered, bool parallel) {
    auto* req = LocalReq::create(luxirNode->getSearchEngine());
    auto& cur = req->collection("main").topDocs("q");
    cur.getScores().fields({"id"});
    cur.rawQuery() = maxSimKnn(cur.mr(), "tok_vs", queries, 3);
    if (filtered) cur.matchFilter("kind_s", "keep");
    req->execute(parallel);
    return scoredHits(req);
  };
  using Hits = std::vector<std::pair<std::string, float>>;
  Hits expected = {{"a", 2.0f}, {"c", (float)(0.6 + 0.6)}, {"b", 1.0f}};
  EXPECT_EQ(expected, search(false, false));
  EXPECT_EQ(expected, search(false, true));
  EXPECT_EQ((Hits{{"c", (float)(0.6 + 0.6)}, {"b", 1.0f}}), search(true, false));

  // One query vector keeps the best-single-vector score.
  auto* req = LocalReq::create(luxirNode->getSearchEngine());
  auto& cur = req->collection("main").topDocs("q");
  cur.getScores().fields({"id"});
  cur.rawQuery() = qb::knn(cur.mr(), "tok_vs", Vec{1, 0}, 3);
  req->execute();
  EXPECT_EQ((Hits{{"a", 1.0f}, {"b", 1.0f}, {"c", 0.6f}}), scoredHits(req));
}

TEST_F(VectorEncodingTest, maxSimValidates) {
  CollectionHelper h("main");
  installFields(h.collection(), {.metric = api::VectorMetric::IP, .multi = true});
  auto* req = LocalReq::create(luxirNode->getSearchEngine());
  auto& cur = req->collection("main").topDocs("q");
  cur.rawQuery() = maxSimKnn(cur.mr(), "enc_v", {QUERY, QUERY}, 3);
  req->execute();
  EXPECT_FALSE(req->ok());
  EXPECT_NE(std::string::npos, req->errorMsg().find("maxsim")) << req->errorMsg();
  req->done();

  SchemaBuilder sb;
  auto& f = sb.field("one_v");
  f.type = api::FieldDef::FieldClass::VECTOR;
  f.metric = api::VectorMetric::IP;
  f.multi_vector_scoring = api::FieldDef::MultiVectorScoring::MAXSIM;
  EXPECT_THROW(Schema::fromProto(sb.def()), SchemaError);
}
