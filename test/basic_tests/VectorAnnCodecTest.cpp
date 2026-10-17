// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// Per-field ANN codec settings (schema "ann"): IVF+RaBitQ and IVF+PQ overlays
// built from the field's settings at commit and merge, served by each
// overlay's own metadata.

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

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

struct Codec {
  api::VectorAnnDef::Codec codec;
  int32_t bits;  // rabitq.bits or pq.bits
};

void installAnn(Collection& col, api::VectorMetric metric, Codec c) {
  SchemaBuilder b;
  auto& f = b.field("emb_v");
  f.type = api::FieldDef_::FieldClass::VECTOR;
  f.dims = DIMS;
  f.metric = metric;
  auto& ann = f.ann.emplace();
  ann.codec = c.codec;
  if (c.codec == api::VectorAnnDef::Codec::PQ) {
    ann.pq.emplace().bits = c.bits;
  } else {
    ann.rabitq.emplace().bits = c.bits;
  }
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

} // namespace

TEST_F(VectorAnnCodecTest, eachCodecAndMetricBuildsFromSchemaAndRecalls) {
  IvfTestGuard guard(std::nullopt);  // the schema is the only source of settings
  using C = api::VectorAnnDef::Codec;
  for (Codec c : {Codec{C::RABITQ, 1}, Codec{C::RABITQ, 4}, Codec{C::PQ, 4}}) {
    for (auto metric : {api::VectorMetric::L2, api::VectorMetric::IP, api::VectorMetric::COSINE}) {
      SCOPED_TRACE(std::to_string((int)c.codec) + "/" + std::to_string(c.bits) + " metric "
                   + std::to_string((int)metric));
      CollectionHelper h("main");
      h.clear();
      installAnn(h.collection(), metric, c);
      indexSegment(h, 0, 2000);

      std::shared_ptr<IndexReader> reader;
      auto aux = overlays(h, reader);
      ASSERT_EQ(aux.size(), 1u);
      ASSERT_NE(aux[0], nullptr);
      ASSERT_NE(aux[0]->getFaissIndex(), nullptr);
      if (c.codec == C::PQ) {
        EXPECT_EQ(aux[0]->getEngine(), VectorAuxMeta::ENGINE_IVFPQ);
        EXPECT_EQ(aux[0]->getPqBits(), 4);
        EXPECT_EQ(aux[0]->getRotation(), nullptr);
      } else {
        EXPECT_TRUE(aux[0]->isRaBitQ());
        EXPECT_EQ(aux[0]->getRaBitQBits(), c.bits);
        ASSERT_NE(aux[0]->getRotation(), nullptr);
        EXPECT_TRUE(aux[0]->getRotation()->is_orthonormal);
      }
      EXPECT_GE(recallVsExact(h), 0.9);
    }
  }
}

TEST_F(VectorAnnCodecTest, mergeRebuildsWithFieldCodecAndSharesRotation) {
  IvfTestGuard guard(std::nullopt);
  CollectionHelper h("main");
  installAnn(h.collection(), api::VectorMetric::L2, {api::VectorAnnDef::Codec::RABITQ, 2});
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
  IvfTestGuard guard(std::nullopt);
  CollectionHelper h("main");
  installAnn(h.collection(), api::VectorMetric::IP, {api::VectorAnnDef::Codec::PQ, 4});
  indexSegment(h, 0, 1200);
  installAnn(h.collection(), api::VectorMetric::IP, {api::VectorAnnDef::Codec::RABITQ, 1});
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
