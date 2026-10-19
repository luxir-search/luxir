// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// Per-field ANN codec settings (schema "ann"): IVF+RaBitQ and IVF+PQ overlays
// built from the field's settings at commit and merge, served by each
// overlay's own metadata.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <string>
#include <vector>

#include <faiss/utils/distances.h>

#include "luxir/index/VectorIndexBuilder.h"
#include "luxir/reader/VectorAuxReader.h"
#include "luxir/schema/Schema.h"
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
                                int32_t refine) {
  auto req = localReq(h.getSearchEngine());
  auto& cur = req->collection("main").topDocs("q").limit(10).fields({"id"});
  cur.rawQuery() = qb::knn(cur.mr(), "emb_v", q, 10, ALL_LISTS, exact, refine);
  req->execute();
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
  faiss::RandomRotationMatrix rotation(DIMS, DIMS);
  rotation.init(VectorIndexBuilder::RABITQ_ROTATION_SEED);
  std::vector<float> rotated(DIMS), canonical(DIMS);
  rotated[0] = x;
  rotated[1] = y;
  rotation.reverse_transform(1, rotated.data(), canonical.data());
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
  aux.getRotation()->apply_noalloc(1, query.data(), rotated.data());
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
        EXPECT_TRUE(aux[0]->getRotation()->is_orthonormal);
      }
      EXPECT_GE(recallVsExact(h), 0.9);
    }
  }
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
