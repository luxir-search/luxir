// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// Per-field ANN codec settings (schema "ann"): IVF+RaBitQ and IVF+PQ overlays
// built from the field's settings at commit and merge, served by each
// overlay's own metadata.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <string>
#include <typeinfo>
#include <vector>

#include <faiss/IndexFlat.h>
#include <faiss/IndexIVFPQ.h>
#include <faiss/utils/distances.h>

#include "luxir/index/VectorIndexBuilder.h"
#include "luxir/query/KnnQuery.h"
#include "luxir/reader/VectorAuxReader.h"
#include "luxir/schema/Schema.h"
#include "luxir/search/SearchOverrides.h"
#include "luxir/server/LuxirNode.h"
#include "luxir/util/random.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/QueryBuild.h"
#include "test/SchemaBuilder.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;
namespace api = luxir::api;

class VectorAnnCodecTest : public LuxirTest {
protected:
  void SetUp() override {
    luxirNode->getCollection("main")->setSchema(Schema::createDefaultSchema());
  }
};

namespace {

constexpr int32_t DIMS = 32;
constexpr int32_t CLUSTERS = 16;
// More than sqrt(live vectors) for every corpus below: probes every list.
constexpr int32_t ALL_LISTS = 1 << 16;

void installAnn(Collection& col, api::VectorMetric metric, const api::VectorAnnDef& ann) {
  SchemaBuilder b;
  auto& f = b.field("emb_v");
  f.type = api::FieldDef_::FieldClass::VECTOR;
  f.dims = DIMS;
  f.metric = metric;
  f.ann = ann;
  b.set(col);
}

float noise(SplitMix64& rng) { return (float)((int32_t)rng.rint((int64_t)2001) - 1000) / 1000.0f; }

// Clustered vectors: real embeddings cluster, and IVF on uniform noise
// measures nothing.  Cluster centers in [-1, 1], points within +-0.2.
std::vector<float> clustered(int64_t seed, int32_t cluster) {
  SplitMix64 centerRng((uint64_t)(cluster + 1) * 0x9e3779b97f4a7c15ULL);
  SplitMix64 rng((uint64_t)seed * 0xbf58476d1ce4e5b9ULL + 7);
  std::vector<float> v(DIMS);
  for (auto& x : v) x = noise(centerRng) + 0.2f * noise(rng);
  return v;
}

void indexSegment(CollectionHelper& h, int64_t first, int64_t count) {
  std::vector<Doc> docs;
  for (int64_t i = first; i < first + count; i++) {
    docs.push_back(flatdoc("id", "d" + std::to_string(i), "emb_v",
                           clustered(i, (int32_t)(i % CLUSTERS))));
  }
  h.indexAll(docs);
  h.commit({"*"});
}

std::vector<std::string> knnIds(CollectionHelper& h, const std::vector<float>& q, bool exact,
                                int32_t refine, bool groups = false, int32_t parallel = 1, int32_t k = 10) {
  auto req = localReq(h.getSearchEngine());
  auto& cur = req->collection("main").topDocs("q").limit(k).fields({"id"});
  cur.rawQuery() = qb::knn(cur.mr(), "emb_v", q, k, ALL_LISTS, exact, refine);
  api::Collapse collapse;
  std::array<std::string_view, 1> fields{"id"};
  if (groups) {
    std::get<api::KnnQuery>(cur.rawQuery().kind).unit = "groups";
    collapse.field = fields;
    std::get<api::TopDocs>(cur.rawOp().kind).collapse = &collapse;
  }
  req->execute(parallel);
  EXPECT_OK(req);
  std::vector<std::string> ids;
  if (const auto* dl = req->docList("q")) {
    if (const auto* p = dl->columns.find("id")) {
      if (const auto* col = std::get_if<api::ColStr>(&p->kind)) {
        for (auto id : col->v) ids.emplace_back(id);
      }
    }
  }
  return ids;
}

// Mean recall@10 of the ANN path against the exact path over in-distribution
// queries; nprobe probes every list, so this measures the codec's ranking.
double recallVsExact(CollectionHelper& h, int32_t refine = 100) {
  double sum = 0;
  constexpr int32_t QUERIES = 8;
  for (int32_t q = 0; q < QUERIES; q++) {
    auto query = clustered(1'000'000 + q, q % CLUSTERS);
    auto exact = knnIds(h, query, true, refine);
    auto approx = knnIds(h, query, false, refine);
    EXPECT_EQ(exact.size(), 10u);
    int32_t hits = 0;
    for (const auto& id : approx) hits += std::ranges::count(exact, id) > 0 ? 1 : 0;
    sum += (double)hits / (double)exact.size();
  }
  return sum / QUERIES;
}

std::vector<const VectorAuxReader*> overlays(CollectionHelper& h,
                                             std::shared_ptr<IndexReader>& holder) {
  holder = h.getIndexWriter()->snapshots.readers.getReader();
  std::vector<const VectorAuxReader*> out;
  for (const auto& seg : holder->segments()) {
    auto aux = seg.getAuxReader("vec.emb_v");
    out.push_back(dynamic_cast<const VectorAuxReader*>(aux.get()));
  }
  return out;
}

// Sparse rotated coordinates give negative estimates with a wide margin.
// Symmetric pairs keep the single IVF centroid at zero after real training.
std::vector<float> unrotate(float x, float y) {
  VectorRotation rotation(VectorRotation::FHT_KAC, DIMS, VectorIndexBuilder::RABITQ_ROTATION_SEED);
  std::vector<float> basis(DIMS), column(DIMS), canonical(DIMS);
  // Transpose by rotating basis vectors; inverse is only needed by this fixture.
  for (int i = 0; i < DIMS; ++i) {
    basis[i] = 1;
    rotation.apply(basis.data(), column.data());
    canonical[i] = column[0] * x + column[1] * y;
    basis[i] = 0;
  }
  return canonical;
}

void symmetricSegment(CollectionHelper& h, int32_t first, std::span<const float> scales) {
  std::vector<Doc> docs;
  for (int32_t i = 0; i < 40; i++) {
    float scale = scales[(size_t)(i / 2) % scales.size()] * (i % 2 ? -1.0f : 1.0f);
    docs.push_back(flatdoc("id", "d" + std::to_string(first + i), "emb_v", unrotate(scale, 2 * scale)));
  }
  h.indexAll(docs);
  h.commit({"*"});
}

std::vector<float> rabitqDistances(const VectorAuxReader& aux, const std::vector<float>& query,
                                  bool base = false) {
  auto* index = dynamic_cast<const faiss::IndexIVFRaBitQ*>(aux.getFaissIndex());
  EXPECT_NE(index, nullptr);
  EXPECT_EQ(index->nlist, 1u);
  std::vector<float> rotated(DIMS), centroid(DIMS), result((size_t)index->ntotal);
  aux.getRotation()->apply(query.data(), rotated.data());
  faiss::IVFRaBitQSearchParameters params;
  params.qb = 0;
  std::unique_ptr<faiss::InvertedListScanner> scanner(index->get_InvertedListScanner(false, nullptr, &params));
  scanner->set_query(rotated.data());
  scanner->set_list(0, 0);
  index->quantizer->reconstruct(0, centroid.data());
  std::unique_ptr<faiss::FlatCodesDistanceComputer> dc(index->rabitq.get_distance_computer(0, centroid.data()));
  dc->set_query(rotated.data());
  faiss::InvertedLists::ScopedCodes codes(index->invlists, 0);
  faiss::InvertedLists::ScopedIds ids(index->invlists, 0);
  for (size_t i = 0; i < index->invlists->list_size(0); i++) {
    const auto* code = codes.get() + i * index->code_size;
    result[(size_t)ids.get()[i]] = base ? dynamic_cast<faiss::RaBitQDistanceComputer&>(*dc).distance_to_code_1bit(code)
                                     : scanner->distance_to_code(code);
  }
  return result;
}

using ScoredId = std::pair<std::string, float>;
std::vector<ScoredId> boundedKnn(CollectionHelper& h, const std::vector<float>& q, bool groups,
                                bool exact = false, std::optional<float> similarity = {}, int32_t parallel = 1) {
  auto req = localReq(h.getSearchEngine());
  auto& cur = req->collection("main").topDocs("q").limit(1).fields({"id"})
      .getScores().documentFormat(api::DocFormat::ROWS);
  cur.rawQuery() = qb::knn(cur.mr(), "emb_v", q, 1, ALL_LISTS, exact, 1, 0, similarity);
  api::Collapse collapse;
  std::array<std::string_view, 1> fields{"id"};
  if (groups) {
    std::get<api::KnnQuery>(cur.rawQuery().kind).unit = "groups";
    collapse.field = fields;
    std::get<api::TopDocs>(cur.rawOp().kind).collapse = &collapse;
  }
  req->execute(parallel);
  EXPECT_OK(req);
  std::vector<ScoredId> result;
  for (const auto& doc : req->docList("q")->docs) {
    result.emplace_back(std::string(doc.fields.at("id")->asString()),
        std::get<float>(doc.fields.at("_score_")->kind));
  }
  return result;
}

} // namespace

TEST_F(VectorAnnCodecTest, eachCodecAndMetricBuildsFromSchemaAndRecalls) {
  IvfTestGuard guard;
  using C = api::VectorAnnDef::Codec;
  for (const auto& ann : {rabitqAnn(), rabitqAnn(0, 4), pqAnn(0, 0, 4)}) {
    int32_t bits = ann.codec == C::PQ ? *ann.pq->bits : *ann.rabitq->bits;
    for (auto metric : {api::VectorMetric::L2, api::VectorMetric::IP, api::VectorMetric::COSINE}) {
      SCOPED_TRACE(std::to_string((int)*ann.codec) + "/" + std::to_string(bits) + " metric "
                   + std::to_string((int)metric));
      CollectionHelper h("main");
      h.clear();
      installAnn(h.collection(), metric, ann);
      indexSegment(h, 0, 2000);

      std::shared_ptr<IndexReader> reader;
      auto aux = overlays(h, reader);
      ASSERT_EQ(aux.size(), 1u);
      ASSERT_NE(aux[0], nullptr);
      ASSERT_NE(aux[0]->getFaissIndex(), nullptr);
      if (ann.codec == C::PQ) {
        EXPECT_EQ(aux[0]->getEngine(), VectorAuxMeta::ENGINE_IVFPQ);
        EXPECT_EQ(aux[0]->getPqBits(), bits);
        EXPECT_EQ(aux[0]->getRotation(), nullptr);
      } else {
        EXPECT_TRUE(aux[0]->isRaBitQ());
        EXPECT_EQ(aux[0]->getRaBitQBits(), bits);
        ASSERT_NE(aux[0]->getRotation(), nullptr);
        EXPECT_EQ(aux[0]->getRotation()->kind, VectorRotation::FHT_KAC);
      }
      EXPECT_GE(recallVsExact(h), 0.9);
    }
  }
}

TEST_F(VectorAnnCodecTest, amplitudesFollowListOffsetsAcrossListsAndMerge) {
  IvfTestGuard guard;
  for (int32_t dims : {1, 7, 32}) for (int32_t bits : {1, 4, 9}) {
    for (auto metric : {api::VectorMetric::IP, api::VectorMetric::L2, api::VectorMetric::COSINE}) {
      SCOPED_TRACE(std::format("dims={} bits={} metric={}", dims, bits, (int)metric));
      CollectionHelper h("main");
      h.clear();
      SchemaBuilder schema;
      auto& f = schema.field("emb_v");
      f.type = api::FieldDef_::FieldClass::VECTOR;
      f.dims = dims;
      f.metric = metric;
      f.ann = rabitqAnn(2, bits);
      schema.set(h.collection());
      for (int32_t part = 0; part < 2; part++) {
        std::vector<Doc> docs;
        for (int32_t i = 0; i < 16; i++) {
          auto v = clustered(i + part * 16, i % 4);
          v.resize((size_t)dims);
          if (i == 0) std::fill(v.begin(), v.end(), 0);
          docs.push_back(flatdoc("id", std::to_string(i + part * 16), "emb_v", v));
        }
        h.indexAll(docs);
        h.commit({"*"});
      }
      for (bool merge : {false, true}) {
        if (merge) h.getIndexWriter()->mergeSegments();
        std::shared_ptr<IndexReader> reader;
        auto aux = overlays(h, reader);
        for (size_t seg = 0; seg < aux.size(); seg++) {
          ASSERT_NE(aux[seg], nullptr);
          auto* ivf = dynamic_cast<faiss::IndexIVFRaBitQ*>(aux[seg]->getFaissIndex());
          ASSERT_NE(ivf, nullptr);
          const auto& amplitudes = aux[seg]->getAmplitudes();
          EXPECT_EQ(amplitudes.offsets.back(), (size_t)ivf->ntotal);
          auto& segment = reader->segments()[seg];
          VectorReader column(segment.postingsReader(), *segment.postingsReader().fieldInfo("emb_v"));
          std::vector<float> x((size_t)dims), rotated((size_t)dims), centroid((size_t)dims);
          for (size_t list = 0; list < ivf->nlist; list++) {
            ivf->quantizer->reconstruct((faiss::idx_t)list, centroid.data());
            auto span = amplitudes.list(list);
            ASSERT_EQ(span.size(), ivf->invlists->list_size(list));
            std::unique_ptr<faiss::FlatCodesDistanceComputer> computer(
                ivf->rabitq.get_distance_computer(0, centroid.data()));
            auto* dc = dynamic_cast<faiss::RaBitQDistanceComputer*>(computer.get());
            ASSERT_NE(dc, nullptr);
            faiss::InvertedLists::ScopedIds ids(ivf->invlists, list);
            faiss::InvertedLists::ScopedCodes codes(ivf->invlists, list);
            for (size_t row = 0; row < ivf->invlists->list_size(list); row++) {
              int64_t rank = ids.get()[row];
              std::memcpy(x.data(), column.vectorAtRank(rank).data(), x.size() * sizeof(float));
              if (metric == api::VectorMetric::COSINE) faiss::fvec_renorm_L2(x.size(), 1, x.data());
              aux[seg]->getRotation()->apply(x.data(), rotated.data());
              EXPECT_TRUE(std::isfinite(span[row]));
              EXPECT_GE(span[row], 0);
              if (bits == 1) {
                auto factors = faiss::rabitq_utils::compute_vector_factors(
                    rotated.data(), (size_t)dims, centroid.data(), ivf->metric_type, true);
                EXPECT_NEAR(span[row], factors.f_error / 1.9f, 0.0001f);
              } else {
                // decode() only decodes sign bits. Recover the full reconstruction
                // from FAISS's distance kernel, without unpacking any code bits.
                const uint8_t* code = codes.get() + row * ivf->code_size;
                float step = std::max(1.0f, 2 * std::sqrt(faiss::fvec_L2sqr(
                    rotated.data(), centroid.data(), (size_t)dims)));
                double error2 = 0;
                for (int32_t j = 0; j < dims; j++) {
                  x = centroid;
                  x[j] += step;
                  dc->set_query(x.data());
                  float plus = dc->distance_to_code_full(code);
                  x[j] = centroid[j] - step;
                  dc->set_query(x.data());
                  float minus = dc->distance_to_code_full(code);
                  double slope = ((double)plus - minus) / (2 * step);
                  double residual = metric == api::VectorMetric::L2 ? slope / -2 : slope - centroid[j];
                  double error = residual - ((double)rotated[j] - centroid[j]);
                  error2 += error * error;
                }
                double expected = std::sqrt(error2 / std::max(1, dims - 1))
                    * (metric == api::VectorMetric::L2 ? 2 : 1);
                EXPECT_NEAR(span[row], expected, 0.00001);
              }
            }
          }
        }
      }
    }
  }
}

TEST_F(VectorAnnCodecTest, rejectsOldAmplitudeMetadata) {
  VectorAuxMeta meta;
  meta.engine = VectorAuxMeta::ENGINE_IVFRABITQ;
  meta.version--;
  EXPECT_THROW(VectorAuxMeta::fromBytes(meta.toBytes(), "test"), std::runtime_error);
  auto bytes = meta.toBytes();
  bytes.resize(bytes.size() - sizeof(int32_t));
  EXPECT_THROW(VectorAuxMeta::fromBytes(bytes, "test"), std::runtime_error);
}

TEST_F(VectorAnnCodecTest, mergeRebuildsWithFieldCodecAndSharesRotation) {
  IvfTestGuard guard;
  CollectionHelper h("main");
  installAnn(h.collection(), api::VectorMetric::L2, rabitqAnn(0, 2));
  indexSegment(h, 0, 1000);
  indexSegment(h, 1000, 1000);
  {
    std::shared_ptr<IndexReader> reader;
    auto aux = overlays(h, reader);
    ASSERT_EQ(aux.size(), 2u);
    aux[0]->getFaissIndex();
    aux[1]->getFaissIndex();
    // Same dims and seed: one interned rotation serves both overlays.
    EXPECT_EQ(aux[0]->getRotation(), aux[1]->getRotation());
  }

  VectorIndexBuilder::ivfMergeBuildCountForTests.store(0);
  h.getIndexWriter()->mergeSegments();
  EXPECT_EQ(VectorIndexBuilder::ivfMergeBuildCountForTests.load(), 1);

  std::shared_ptr<IndexReader> reader;
  auto aux = overlays(h, reader);
  ASSERT_EQ(aux.size(), 1u);
  ASSERT_NE(aux[0], nullptr);
  EXPECT_TRUE(aux[0]->isRaBitQ());
  EXPECT_EQ(aux[0]->getRaBitQBits(), 2);
  EXPECT_EQ(aux[0]->getFaissIndex()->ntotal, 2000);
  EXPECT_GE(recallVsExact(h), 0.9);
}

// A codec change applies to overlays built afterwards; existing segments keep
// theirs, and one query serves each segment by its own overlay metadata.
TEST_F(VectorAnnCodecTest, codecChangeLeavesMixedSegmentsQueryable) {
  IvfTestGuard guard;
  CollectionHelper h("main");
  installAnn(h.collection(), api::VectorMetric::IP, pqAnn(0, 0, 4));
  indexSegment(h, 0, 1200);
  installAnn(h.collection(), api::VectorMetric::IP, rabitqAnn());
  indexSegment(h, 1200, 1200);

  std::shared_ptr<IndexReader> reader;
  auto aux = overlays(h, reader);
  ASSERT_EQ(aux.size(), 2u);
  ASSERT_TRUE(aux[0] != nullptr && aux[1] != nullptr);
  EXPECT_EQ(aux[0]->getEngine(), VectorAuxMeta::ENGINE_IVFPQ);
  EXPECT_EQ(aux[1]->getEngine(), VectorAuxMeta::ENGINE_IVFRABITQ);
  // 4-bit PQ on 4-dim subspaces is coarse: give it a deeper rescore pool.
  EXPECT_GE(recallVsExact(h, /*refine=*/300), 0.9);
}

TEST_F(VectorAnnCodecTest, boundedFloorSpansChunksAndCodecs) {
  IvfTestGuard guard;
  SearchOverridesGuard overrides(KnnQuery::statsForTests, KnnQuery::scanTaskGrainVectors);
  CollectionHelper h("main");
  std::vector<float> query(DIMS, 0);
  for (const auto& first : {pqAnn(4, 8, 2), rabitqAnn(4), rabitqAnn(4, 4)}) {
    h.clear();
    installAnn(h.collection(), api::VectorMetric::L2, first);
    indexSegment(h, 0, 256);
    installAnn(h.collection(), api::VectorMetric::L2, rabitqAnn(4, 4));
    std::vector<Doc> far;
    for (int i = 0; i < 256; i++) {
      auto vector = clustered(i, i % CLUSTERS);
      for (auto& x : vector) x += 100;
      far.push_back(flatdoc("id", "far" + std::to_string(i), "emb_v", vector));
    }
    h.indexAll(far);
    h.commit({"*"});
    auto expected = knnIds(h, query, true, 256);
    int64_t firstExtra = first.rabitq && first.rabitq->bits == 4 ? 256 : 0;
    for (bool groups : {false, true}) for (int64_t grain : {1, 1000000}) {
      KnnQuery::scanTaskGrainVectors = grain;
      for (int32_t parallel : {1, -1}) {
        KnnQuery::Stats stats;
        KnnQuery::statsForTests = &stats;
        EXPECT_EQ(knnIds(h, query, false, 256, groups, parallel), expected);
        EXPECT_EQ(stats.rounds.load(), 1);
        EXPECT_EQ(stats.scanned.load(), 512);
        // Serial scans fill R from the first segment. The far segment must
        // inherit that floor even when its chunks or codec differ.
        if (parallel == 1) EXPECT_EQ(stats.extraBitEvaluations.load(), firstExtra);
        else EXPECT_LE(stats.extraBitEvaluations.load(), firstExtra + 256);
        KnnQuery::statsForTests = nullptr;
      }
    }
  }
}

TEST_F(VectorAnnCodecTest, negativeEstimatesRemainOrderedAcrossSegments) {
  IvfTestGuard guard;
  CollectionHelper h("main");
  installAnn(h.collection(), api::VectorMetric::L2, rabitqAnn(1, 1, 0));
  symmetricSegment(h, 0, std::vector<float>{1.1f});
  symmetricSegment(h, 40, std::vector<float>{1});
  auto q = unrotate(1.5f, 1.5f);
  std::shared_ptr<IndexReader> reader;
  auto aux = overlays(h, reader);
  ASSERT_EQ(aux.size(), 2u);
  float worse = rabitqDistances(*aux[0], q)[0], better = rabitqDistances(*aux[1], q)[0];
  EXPECT_NEAR(worse, -0.45f, 0.001f);
  EXPECT_NEAR(better, -0.5f, 0.001f);
  for (bool groups : {false, true}) for (int32_t parallel : {1, -1}) {
    auto exact = boundedKnn(h, q, groups, true);
    ASSERT_EQ(exact.size(), 1u);
    EXPECT_EQ(exact[0].first, "d40");
    EXPECT_NEAR(exact[0].second, 1.0f / 1.5f, 0.00001f);
    EXPECT_EQ(boundedKnn(h, q, groups, false, {}, parallel), exact);
    EXPECT_EQ(boundedKnn(h, q, groups, false, 0.75f, parallel), exact);
    EXPECT_TRUE(boundedKnn(h, q, groups, false, 0.65f, parallel).empty());
  }
}

TEST_F(VectorAnnCodecTest, groupedNegativeDistanceFloor) {
  IvfTestGuard guard;
  for (int32_t bits : {1, 2}) {
    SCOPED_TRACE(bits);
    CollectionHelper h("main");
    h.clear();
    installAnn(h.collection(), api::VectorMetric::L2, rabitqAnn(1, bits, 0));
    symmetricSegment(h, 0, std::vector<float>{1.1f, 1});
    auto q = unrotate(1.5f, 1.5f);
    std::shared_ptr<IndexReader> reader;
    auto aux = overlays(h, reader);
    ASSERT_EQ(aux.size(), 1u);
    auto base = rabitqDistances(*aux[0], q, true);
    EXPECT_LT(base[2], base[0]);
    EXPECT_LT(base[0], -0.4f);
    auto distances = rabitqDistances(*aux[0], q);
    auto best = std::min_element(distances.begin(), distances.end()) - distances.begin();
    // Multi-bit FAISS clamps its full estimate; its first stage is still negative.
    if (bits == 1) { EXPECT_EQ(best, 2); EXPECT_LT(distances[2], -0.49f); }
    else EXPECT_GE(distances[(size_t)best], 0);
    for (int32_t parallel : {1, -1}) {
      auto selected = boundedKnn(h, q, true, false, {}, parallel);
      ASSERT_EQ(selected.size(), 1u);
      EXPECT_EQ(selected[0].first, "d" + std::to_string(best));
      float scale = (best / 2) % 2 == 0 ? 1.1f : 1.0f;
      auto v = unrotate(scale, 2 * scale);
      EXPECT_NEAR(selected[0].second, 1.0f / (1.0f + faiss::fvec_L2sqr(q.data(), v.data(), DIMS)), 0.00001f);
    }
  }
}

TEST_F(VectorAnnCodecTest, structuredRotationPreservesGeometry) {
  for (int d : {1, 2, 3, 5, 64, 100, 127, 128, 384, 768, 960, 1024, 1536, 3072}) {
    SCOPED_TRACE(d);
    VectorRotation r(VectorRotation::FHT_KAC, d, 42), same(VectorRotation::FHT_KAC, d, 42);
    std::vector<float> x(d), y(d), rx(d), ry(d), batch(2 * d);
    for (int i = 0; i < d; ++i) { x[i] = std::sin((float)i + 1); y[i] = std::cos((float)i * 3); }
    r.apply(x.data(), rx.data());
    r.apply(y.data(), ry.data());
    std::copy(x.begin(), x.end(), batch.begin());
    std::copy(y.begin(), y.end(), batch.begin() + d);
    same.applyBatch(2, batch.data(), batch.data());
    double xx = 0, rr = 0, xy = 0, rxy = 0;
    for (int i = 0; i < d; ++i) {
      EXPECT_EQ(rx[i], batch[i]); EXPECT_EQ(ry[i], batch[d + i]);
      xx += (double)x[i] * x[i]; rr += (double)rx[i] * rx[i];
      xy += (double)x[i] * y[i]; rxy += (double)rx[i] * ry[i];
    }
    EXPECT_NEAR(xx, rr, 2e-6 * d);
    EXPECT_NEAR(xy, rxy, 2e-6 * d);
  }
  auto shared = internVectorRotation(VectorRotation::FHT_KAC, 17, 42);
  EXPECT_EQ(shared, internVectorRotation(VectorRotation::FHT_KAC, 17, 42));
  EXPECT_NE(shared, internVectorRotation(VectorRotation::FHT_KAC, 17, 43));
  EXPECT_NE(shared, internVectorRotation(VectorRotation::FHT_KAC, 18, 42));
  // Seed 0, low bits of the first SplitMix64 word, four rounds.
  VectorRotation golden(VectorRotation::FHT_KAC, 2, 0);
  float value[] = {1, 2};
  golden.apply(value, value);
  EXPECT_NEAR(value[0], -2, 1e-6);
  EXPECT_NEAR(value[1], 1, 1e-6);
  // Cross a sign-word boundary and exercise the odd-dimension Kac layout.
  VectorRotation odd(VectorRotation::FHT_KAC, 17, 0);
  float sequence[17];
  for (int i = 0; i < 17; ++i) sequence[i] = (float)(i + 1);
  odd.apply(sequence, sequence);
  EXPECT_NEAR(sequence[0], -3.53748822f, 1e-5);
  EXPECT_NEAR(sequence[8], -18.0413551f, 1e-5);
  EXPECT_NEAR(sequence[16], -0.602243066f, 1e-5);
}

// Compare list payloads rather than files: partitioned builds
// can differ in the last bits, so only PQ codes must match exactly.
TEST_F(VectorAnnCodecTest, partitionedBuildMatchesSingleTask) {
  IvfTestGuard guard;
  SearchOverridesGuard rows(VectorIndexBuilder::ivfBuildMinRowsPerTask);
  using List = std::pair<std::vector<faiss::idx_t>, std::vector<uint8_t>>;
  using Snapshot = std::vector<std::vector<List>>;
  for (const auto& ann : {pqAnn(0, 2, 4), rabitqAnn()}) {
    SCOPED_TRACE(std::to_string((int)*ann.codec));
    auto build = [&](int64_t minRows) {
      VectorIndexBuilder::ivfBuildMinRowsPerTask = minRows;
      CollectionHelper h("main");
      h.clear();
      installAnn(h.collection(), api::VectorMetric::L2, ann);
      indexSegment(h, 0, 1500);
      indexSegment(h, 1500, 1500);
      Snapshot result;
      for (bool merge : {false, true}) {
        if (merge) h.getIndexWriter()->mergeSegments();
        std::shared_ptr<IndexReader> reader;
        auto aux = overlays(h, reader);
        EXPECT_EQ(aux.size(), merge ? 1u : 2u);
        for (auto* overlay : aux) {
          std::vector<List> lists;
          if (!overlay) { ADD_FAILURE() << "Missing overlay"; continue; }
          auto* ivf = dynamic_cast<const faiss::IndexIVF*>(overlay->getFaissIndex());
          if (!ivf) { ADD_FAILURE() << "Missing IVF index"; continue; }
          for (size_t i = 0; i < ivf->nlist; i++) {
            auto* inv = ivf->invlists;
            size_t n = inv->list_size(i);
            List list;
            if (n) {
              faiss::InvertedLists::ScopedIds ids(inv, i);
              list.first.assign(ids.get(), ids.get() + n);
              if (ann.codec == api::VectorAnnDef::Codec::PQ) {
                faiss::InvertedLists::ScopedCodes codes(inv, i);
                list.second.assign(codes.get(), codes.get() + n * ivf->code_size);
              }
            }
            lists.push_back(std::move(list));
          }
          result.push_back(std::move(lists));
        }
      }
      return result;
    };
    auto single = build(1000000000);
    EXPECT_EQ(build(64), single);
  }
}

TEST_F(VectorAnnCodecTest, partitionedBlasBuildKeepsFormatAndRecall) {
  IvfTestGuard guard;
  SearchOverridesGuard saved(VectorIndexBuilder::ivfBuildMinRowsPerTask,
                              faiss::distance_compute_blas_threshold);
  VectorIndexBuilder::ivfBuildMinRowsPerTask = 64;
  faiss::distance_compute_blas_threshold = 64 * DIMS;
  for (const auto& ann : {pqAnn(0, 2, 4), rabitqAnn()}) {
    SCOPED_TRACE(std::to_string((int)*ann.codec));
    CollectionHelper h("main");
    h.clear();
    installAnn(h.collection(), api::VectorMetric::L2, ann);
    indexSegment(h, 0, 1500);
    indexSegment(h, 1500, 1500);
    int32_t refine = ann.codec == api::VectorAnnDef::Codec::PQ ? 300 : 100;
    for (bool merge : {false, true}) {
      if (merge) h.getIndexWriter()->mergeSegments();
      std::shared_ptr<IndexReader> reader;
      auto aux = overlays(h, reader);
      ASSERT_EQ(aux.size(), merge ? 1u : 2u);
      for (auto* overlay : aux) {
        ASSERT_NE(overlay, nullptr);
        auto* ivf = dynamic_cast<const faiss::IndexIVF*>(overlay->getFaissIndex());
        ASSERT_NE(ivf, nullptr);
        EXPECT_EQ(ivf->ntotal, merge ? 3000 : 1500);
        EXPECT_EQ(typeid(*ivf->quantizer), typeid(faiss::IndexFlatL2));
        if (ann.codec == api::VectorAnnDef::Codec::PQ) {
          EXPECT_EQ(typeid(*ivf), typeid(faiss::IndexIVFPQ));
        }
      }
      EXPECT_GE(recallVsExact(h, refine), 0.9);
    }
  }
}

TEST_F(VectorAnnCodecTest, negativeEstimateCannotEvictExactColumnWitness) {
  IvfTestGuard guard;
  CollectionHelper h("main");
  installAnn(h.collection(), api::VectorMetric::L2, rabitqAnn(1, 1, 0));
  symmetricSegment(h, 0, std::vector<float>{1});
  auto q = unrotate(1.5f, 1.5f);
  {
    SearchOverridesGuard builds(VectorIndexBuilder::buildFaissAuxIndexes);
    VectorIndexBuilder::buildFaissAuxIndexes = false;
    h.index(flatdoc("id", "exact", "emb_v", q));
    h.commit({"*"});
  }
  std::shared_ptr<IndexReader> reader;
  auto aux = overlays(h, reader);
  ASSERT_EQ(aux.size(), 2u);
  ASSERT_NE(aux[0], nullptr);
  EXPECT_EQ(aux[1], nullptr);
  EXPECT_LT(rabitqDistances(*aux[0], q)[0], -0.49f);
  for (bool groups : {false, true}) for (int32_t parallel : {1, -1}) {
    EXPECT_EQ(boundedKnn(h, q, groups, false, {}, parallel), (std::vector<ScoredId>{{"exact", 1}}));
    EXPECT_EQ(boundedKnn(h, q, groups, false, 0.01f, parallel), boundedKnn(h, q, groups, true, 0.01f));
  }
}

TEST_F(VectorAnnCodecTest, intervalSeedsReuseScoresAndIgnoreBoundedCap) {
  IvfTestGuard ivf;
  SearchOverridesGuard saved(KnnQuery::maxKnnCandidates, KnnQuery::statsForTests, KnnQuery::scanTaskGrainVectors);
  KnnQuery::maxKnnCandidates = 1;
  KnnQuery::scanTaskGrainVectors = 1;
  for (int bits : {1, 2, 4, 8}) for (auto metric : {api::VectorMetric::L2, api::VectorMetric::IP, api::VectorMetric::COSINE}) {
    SCOPED_TRACE(std::format("bits={} metric={}", bits, (int)metric));
    CollectionHelper h("main");
    h.clear();
    installAnn(h.collection(), metric, rabitqAnn(4, bits));
    indexSegment(h, 0, 128);
    for (int k : {1, 10}) for (int parallel : {1, -1}) {
      auto exact = knnIds(h, clustered(1000, 2), true, 0, false, parallel, k);
      KnnQuery::Stats stats;
      KnnQuery::statsForTests = &stats;
      auto ids = knnIds(h, clustered(1000, 2), false, 0, false, parallel, k);
      EXPECT_EQ(ids.size(), (size_t)k);
      EXPECT_EQ(ids, exact);
      EXPECT_EQ(stats.scanned.load(), 128);
      EXPECT_EQ(stats.seedReads.load(), k);
      EXPECT_LE(stats.seedReads.load() + stats.remainingReads.load(), stats.retainedTerminal.load());
      EXPECT_GE(stats.retainedPeak.load(), stats.retainedTerminal.load());
      EXPECT_EQ(stats.rounds.load(), 1);
      if (bits == 1) {
        EXPECT_EQ(stats.extraBitEvaluations.load(), 0);
        EXPECT_EQ(stats.gateRejections.load(), 0);
      } else EXPECT_EQ(stats.extraBitEvaluations.load() + stats.gateRejections.load(), stats.scanned.load());
      KnnQuery::statsForTests = nullptr;
    }
    EXPECT_EQ(knnIds(h, clustered(1000, 2), false, 10).size(), 1u);
  }
}

TEST_F(VectorAnnCodecTest, intervalMemoryCeilingReleasesOnFailureAndSuccess) {
  IvfTestGuard ivf;
  CollectionHelper h("main");
  installAnn(h.collection(), api::VectorMetric::IP, rabitqAnn(4));
  indexSegment(h, 0, 128);
  SearchOverridesGuard ceiling(forcedRequestMemoryMaxBytes);
  for (size_t bytes : {1u, 4096u, 0u}) for (int parallel : {1, -1}) {
    forcedRequestMemoryMaxBytes = bytes;
    auto req = localReq(h.getSearchEngine());
    auto& cur = req->collection("main").topDocs("q").limit(10).fields({"id"});
    cur.rawQuery() = qb::knn(cur.mr(), "emb_v", clustered(1000, 2), 10, ALL_LISTS, false, 0);
    req->execute(parallel);
    EXPECT_EQ(req->ok(), bytes == 0) << req->errorMsg();
    if (bytes) { EXPECT_NE(req->errorMsg().find("knn interval state"), std::string::npos); }
    EXPECT_EQ(req->memoryTracker.bytes(), 0u);
    if (!bytes) { EXPECT_GT(req->memoryTracker.peak(), 4096u); }
  }
}

TEST_F(VectorAnnCodecTest, mixedCollectorsKeepTheirOwnObligations) {
  IvfTestGuard ivf;
  CollectionHelper h("main");
  installAnn(h.collection(), api::VectorMetric::IP, pqAnn(4, 8, 2));
  indexSegment(h, 0, 160);
  for (int bits : {1, 4, 8}) {
    installAnn(h.collection(), api::VectorMetric::IP, rabitqAnn(4, bits));
    indexSegment(h, 160 + bits * 128, 128);
  }
  auto query = clustered(1000, 2);
  {
    SearchOverridesGuard builds(VectorIndexBuilder::buildFaissAuxIndexes);
    VectorIndexBuilder::buildFaissAuxIndexes = false;
    h.index(flatdoc("id", "exact", "emb_v", query));
    h.commit({"*"});
  }
  auto expected = knnIds(h, query, true, 0);
  for (bool groups : {false, true}) for (int parallel : {1, -1}) {
    KnnQuery::Stats stats;
    SearchOverridesGuard counters(KnnQuery::statsForTests);
    KnnQuery::statsForTests = &stats;
    EXPECT_EQ(knnIds(h, query, false, 0, groups, parallel), expected);
    EXPECT_GE(stats.seedReads.load(), KnnQuery::boundedDefaultCandidates(10, 160));
    EXPECT_GT(stats.retainedTerminal.load(), 0);
    EXPECT_GE(stats.scanned.load(), 160 + 3 * 128);
  }
}

TEST_F(VectorAnnCodecTest, intervalSeedCanLoseToAnotherGroupMember) {
  IvfTestGuard ivf;
  CollectionHelper h("main");
  installAnn(h.collection(), api::VectorMetric::IP, rabitqAnn(1, 1, 0));
  for (int i = 0; i < 128; i++) h.index(flatdoc("id", std::to_string(i), "key_s", "one",
      "emb_v", clustered(i, i % CLUSTERS)));
  h.commit({"*"});
  std::shared_ptr<IndexReader> reader;
  auto aux = overlays(h, reader);
  auto& seg = reader->segments()[0];
  VectorReader column(seg.postingsReader(), *seg.postingsReader().fieldInfo("emb_v"));
  std::vector<float> query;
  for (int i = 0; i < 32; i++) {
    auto q = clustered(1000 + i, i % CLUSTERS);
    auto estimated = rabitqDistances(*aux[0], q);
    size_t seed = (size_t)(std::max_element(estimated.begin(), estimated.end()) - estimated.begin());
    std::vector<float> exact(estimated.size());
    for (size_t rank = 0; rank < exact.size(); rank++) {
      exact[rank] = faiss::fvec_inner_product(q.data(), column.vectorAtRank((int64_t)rank).data(), DIMS);
    }
    size_t best = (size_t)(std::max_element(exact.begin(), exact.end()) - exact.begin());
    if (exact[best] > exact[seed]) { query = std::move(q); break; }
  }
  ASSERT_FALSE(query.empty());
  auto expected = knnIds(h, query, true, 0, false, 1, 1);
  for (int parallel : {1, -1}) {
    KnnQuery::Stats stats;
    SearchOverridesGuard counters(KnnQuery::statsForTests);
    KnnQuery::statsForTests = &stats;
    auto req = localReq(h.getSearchEngine());
    auto& cur = req->collection("main").topDocs("q").limit(1).fields({"id"});
    cur.rawQuery() = qb::knn(cur.mr(), "emb_v", query, 1, ALL_LISTS, false, 0);
    std::get<api::KnnQuery>(cur.rawQuery().kind).unit = "groups";
    api::Collapse collapse;
    std::array<std::string_view, 1> fields{"key_s"};
    collapse.field = fields;
    std::get<api::TopDocs>(cur.rawOp().kind).collapse = &collapse;
    req->execute(parallel);
    EXPECT_OK(req);
    const auto* list = req->docList("q");
    ASSERT_NE(list, nullptr);
    const auto& ids = std::get<api::ColStr>(list->columns.at("id").kind).v;
    ASSERT_EQ(ids.size(), 1u);
    EXPECT_EQ(ids[0], expected[0]);
    EXPECT_EQ(stats.seedReads.load(), 1);
    EXPECT_GT(stats.remainingReads.load(), 0);
  }
}

TEST_F(VectorAnnCodecTest, intervalEndpointsSupportPaddedDimensionsAndQueryBits) {
  IvfTestGuard ivf;
  for (int dims : {1, 7}) for (int bits : {1, 9}) for (int queryBits : {0, 8}) {
    CollectionHelper h("main");
    h.clear();
    SchemaBuilder b;
    auto& f = b.field("emb_v");
    f.type = api::FieldDef_::FieldClass::VECTOR;
    f.dims = dims;
    f.metric = api::VectorMetric::L2;
    f.ann = rabitqAnn(2, bits, queryBits);
    b.set(h.collection());
    for (int i = 0; i < 16; i++) {
      h.index(flatdoc("id", std::to_string(i), "emb_v", std::vector<float>((size_t)dims, (float)i)));
    }
    h.commit({"*"});
    std::vector<float> query((size_t)dims, 0);
    EXPECT_EQ(knnIds(h, query, false, 0, false, 1, 1), knnIds(h, query, true, 0, false, 1, 1));
  }
}
