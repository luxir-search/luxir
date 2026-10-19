// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "bench/luxir_bench.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/SchemaBuilder.h"
#include "test/TestUtils.h"
#include "luxir/index/VectorIndexBuilder.h"
#include "luxir/query/KnnQuery.h"
#include "luxir/search/SearchOverrides.h"
#include <faiss/IndexIVFPQ.h>
#include <random>
#include <set>

using namespace luxir;
using namespace luxir::test;

namespace {
struct Witness { std::string group, id; };

std::vector<Witness> witnesses(const LocalReq& req) {
  std::vector<Witness> result;
  for (auto* response : req.responses) {
    auto* q = response->proto.ops.find("q");
    if (!q) continue;
    for (const auto& doc : (*q)->docList()->docs) result.push_back({
        std::string(doc.fields.at("product_s")->asString()), std::string(doc.fields.at("id")->asString())});
  }
  return result;
}

// Products have scattered variants in three indexed segments and one flat
// segment. The adversarial layout puts a large repeated group closest to the
// query; ordinary document top-k then spends its whole window on that group.
// Each full-size segment has 12K vectors: above the default 8-bit PQ training
// floor of 39*256. The small mode uses 1-bit PQ on 96 vectors per segment.
void BM_GroupKnn(benchmark::State& state, bool adversarial, bool exact, int32_t probes, int32_t key = 0,
                 bool coarse = false, bool groups = true, int32_t memberCap = 0) {
  int32_t products = (int32_t)LuxirTest::scaleTestWork(unit_tests ? 48 : 6000);
  constexpr int32_t variants = 8, dims = 16, k = 10;
  std::string name = "group_knn_structured_" + std::to_string(products) + (adversarial ? "_hog" : "_catalog") + (coarse ? "_m4b4" : "_default8");
  CollectionHelper helper(name);
  std::vector<int32_t> shape(4, products * variants / 4);
  std::mt19937 centerRng(7);
  std::vector<std::vector<float>> centers((size_t)products, std::vector<float>(dims));
  for (auto& center : centers) for (auto& value : center) value = (float)(centerRng() % 80000) / 10000.0f;
  if (!helper.indexMatchesShape(shape)) {
    helper.clear();
    SchemaBuilder b;
    auto& field = b.templ("_v");
    field.type = api::FieldDef::FieldClass::VECTOR;
    field.column = true;
    field.metric = api::VectorMetric::L2;
    field.dims = dims;
    field.ann = pqAnn(unit_tests ? 2 : 16, coarse ? 4 : 0,
                      unit_tests ? 1 : coarse ? 4 : 8);
    b.set(helper.collection());
    std::mt19937 rng(52);
    for (int segment = 0; segment < 4; segment++) {
      std::vector<Doc> docs;
      for (int i = 0; i < shape[(size_t)segment]; i++) {
        int32_t id = segment * shape[0] + i;
        int32_t product = i % products;
        bool hog = adversarial && i < shape[0] / 2;
        std::vector<float> vector(dims);
        for (int d = 0; d < dims; d++) vector[(size_t)d] = centers[(size_t)(hog ? 0 : product)][(size_t)d]
            + ((float)(rng() % 10000) / 10000.0f - 0.5f) * (hog ? 0.02f : 0.6f);
        docs.push_back(flatdoc("id", std::to_string(id), "product_s", hog ? "hog" : "p" + std::to_string(product),
            "product_i", (int64_t)(hog ? -1 : product), "catalog_i", (int64_t)(hog ? 0 : product % 3),
            "vec_v", vector));
      }
      // Active overlay names otherwise build new segments automatically once
      // effort makes the last segment exceed the default training/work floor.
      SearchOverridesGuard automaticBuild(VectorIndexBuilder::buildFaissAuxIndexes);
      if (segment == 3) VectorIndexBuilder::buildFaissAuxIndexes = false;
      helper.indexAll(docs, UpdateMessage::COMMIT);
      if (segment == 2) {
        IvfTestGuard ivf(unit_tests ? 8 : 64);
        helper.commit({"vec.vec_v"});
      }
    }
    helper.deleteByIds({"0", std::to_string(shape[0])}, UpdateMessage::COMMIT);
  }
  // Verify actual execution, not just requested builder settings: tiny PQ
  // corpora silently fall back to columns when training requirements fail.
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  ASSERT_EQ(4u, reader->segments().size());
  for (size_t segment = 0; segment < 4; segment++) {
    auto aux = reader->segments()[segment].getAuxReader("vec.vec_v");
    if (segment == 3) { ASSERT_FALSE(aux); continue; }
    auto* vectorAux = dynamic_cast<VectorAuxReader*>(aux.get());
    ASSERT_NE(nullptr, vectorAux);
    auto* index = dynamic_cast<faiss::IndexIVFPQ*>(vectorAux->getFaissIndex());
    ASSERT_NE(nullptr, index);
    ASSERT_EQ(unit_tests ? 1u : coarse ? 4u : 8u, index->pq.nbits);
    ASSERT_EQ(4u, index->pq.M);
  }
  // Product centers with independent query noise; the repeated-group queries
  // stay near the hog center. Recall is averaged over this query set.
  size_t queryCount = unit_tests ? 2 : 8;
  std::vector<std::string> vectors(queryCount);
  std::mt19937 queryRng(99);
  for (size_t q = 0; q < queryCount; q++) {
    vectors[q] = "[";
    for (int d = 0; d < dims; d++) {
      if (d) vectors[q] += ',';
      float value = centers[adversarial ? 0 : (q * 97) % (size_t)products][(size_t)d]
          + ((float)(queryRng() % 10000) / 10000.0f - 0.5f) * (adversarial ? 0.1f : 1.0f);
      vectors[q] += std::to_string(value);
    }
    vectors[q] += ']';
  }
  SearchOverridesGuard capGuard(KnnQuery::membersPerGroupForTests);
  KnnQuery::membersPerGroupForTests = memberCap;
  auto execute = [&](bool forceExact, size_t query) {
    auto req = localReq(helper.getSearchEngine());
    std::string collapse = key == 0 ? R"("product_s")" : key == 1 ? R"("product_i")"
        : R"({"field":["product_s","catalog_i"]})";
    std::string json = R"({"collection":")" + name
        + R"(","limit":10,"get_scores":true,"get_number":true,"fields":["id","product_s"],"document_format":"rows",)"
        + (groups ? R"("collapse":)" + collapse + ',' : "")
        + R"("query":{"knn":{"field":"vec_v","unit":")" + (groups ? "groups" : "docs") + R"(","query":)" + vectors[query]
        + R"(,"k":10,"refine_candidates":160,"exact":)" + (forceExact ? "true" : "false")
        + R"(,"ivf":{"nprobe":)" + std::to_string(probes) + "}}}}";
    EXPECT_TRUE(api::read_json(req->rawRequest(), json, req->mr)) << json;
    req->execute(false);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    return req;
  };
  std::vector<std::vector<Witness>> expected(queryCount);
  for (size_t q = 0; q < queryCount; q++) expected[q] = witnesses(*execute(true, q));
  KnnQuery::Stats stats;
  SearchOverridesGuard guard(KnnQuery::statsForTests);
  KnnQuery::statsForTests = &stats;
  double groupRecall = 0, representativeRecall = 0, underfill = 0;
  for (size_t q = 0; q < queryCount; q++) {
    std::set<std::string> keys, representatives;
    for (const auto& hit : expected[q]) { keys.insert(hit.group); representatives.insert(hit.id); }
    auto actual = witnesses(*execute(exact, q));
    int64_t foundGroups = 0, foundRepresentatives = 0;
    for (const auto& hit : actual) {
      foundGroups += groups ? keys.erase(hit.group) : representatives.contains(hit.id);
      foundRepresentatives += representatives.contains(hit.id);
    }
    groupRecall += (double)foundGroups / (double)expected[q].size();
    representativeRecall += (double)foundRepresentatives / (double)expected[q].size();
    underfill += (double)k - (double)actual.size();
  }
  if (!exact && stats.lists.load() == 0) {
    state.SkipWithError("ANN corpus did not build an IVF-PQ index");
    return;
  }
  state.counters["queries"] = (double)queryCount;
  state.counters[groups ? "group_recall" : "doc_recall"] = groupRecall / (double)queryCount;
  state.counters["members_per_group"] = groups ? (double)KnnQuery::membersPerGroup(160, k) : 0;
  state.counters["refine_documents"] = 160;
  state.counters["pq_bits"] = unit_tests ? 1 : coarse ? 4 : 8;
  state.counters["representative_recall"] = representativeRecall / (double)queryCount;
  state.counters["probes_requested"] = probes;
  state.counters["lists"] = (double)stats.lists.load() / (double)queryCount;
  state.counters["vectors"] = (double)stats.vectors.load() / (double)queryCount;
  state.counters["bytes_read"] = (double)stats.bytes.load() / (double)queryCount;
  state.counters["rounds"] = (double)stats.rounds.load() / (double)queryCount;
  state.counters["underfill"] = underfill / (double)queryCount;
  state.counters["documents"] = (double)products * variants;
  KnnQuery::statsForTests = nullptr;
  size_t query = 0;
  for (auto _ : state) {
    auto req = execute(exact, query++ % queryCount);
    auto count = req->docList()->row_count;
    benchmark::DoNotOptimize(count);
  }
}
}

LUXIR_BENCHMARK_CAPTURE(BM_GroupKnn, catalog_exact, false, true, 0);
LUXIR_BENCHMARK_CAPTURE(BM_GroupKnn, catalog_ann_auto, false, false, 0);
LUXIR_BENCHMARK_CAPTURE(BM_GroupKnn, catalog_ann_1, false, false, 1);
LUXIR_BENCHMARK_CAPTURE(BM_GroupKnn, catalog_ann_16, false, false, 16);
LUXIR_BENCHMARK_CAPTURE(BM_GroupKnn, repeated_exact, true, true, 0);
LUXIR_BENCHMARK_CAPTURE(BM_GroupKnn, repeated_ann_auto, true, false, 0);
LUXIR_BENCHMARK_CAPTURE(BM_GroupKnn, repeated_ann_1, true, false, 1);
LUXIR_BENCHMARK_CAPTURE(BM_GroupKnn, repeated_ann_16, true, false, 16);

LUXIR_BENCHMARK_CAPTURE(BM_GroupKnn, catalog_ann_all, false, false, 1000000);
LUXIR_BENCHMARK_CAPTURE(BM_GroupKnn, repeated_ann_all, true, false, 1000000);

LUXIR_BENCHMARK_CAPTURE(BM_GroupKnn, numeric_exact, false, true, 0, 1);
LUXIR_BENCHMARK_CAPTURE(BM_GroupKnn, numeric_ann_all, false, false, 1000000, 1);
LUXIR_BENCHMARK_CAPTURE(BM_GroupKnn, tuple_exact, false, true, 0, 2);
LUXIR_BENCHMARK_CAPTURE(BM_GroupKnn, tuple_ann_all, false, false, 1000000, 2);

LUXIR_BENCHMARK_CAPTURE(BM_GroupKnn, catalog_8bit_auto_docs, false, false, 0, 0, false, false);
LUXIR_BENCHMARK_CAPTURE(BM_GroupKnn, catalog_8bit_auto_r1, false, false, 0, 0, false, true, 1);
LUXIR_BENCHMARK_CAPTURE(BM_GroupKnn, catalog_8bit_auto_r2, false, false, 0, 0, false, true, 2);
LUXIR_BENCHMARK_CAPTURE(BM_GroupKnn, catalog_8bit_auto_r4, false, false, 0, 0, false, true, 4);
LUXIR_BENCHMARK_CAPTURE(BM_GroupKnn, catalog_8bit_auto_r8, false, false, 0, 0, false, true, 8);
LUXIR_BENCHMARK_CAPTURE(BM_GroupKnn, catalog_8bit_auto_r16, false, false, 0, 0, false, true, 16);

LUXIR_BENCHMARK_CAPTURE(BM_GroupKnn, catalog_8bit_full_docs, false, false, 1000000, 0, false, false);
LUXIR_BENCHMARK_CAPTURE(BM_GroupKnn, catalog_8bit_full_r1, false, false, 1000000, 0, false, true, 1);
LUXIR_BENCHMARK_CAPTURE(BM_GroupKnn, catalog_8bit_full_r2, false, false, 1000000, 0, false, true, 2);
LUXIR_BENCHMARK_CAPTURE(BM_GroupKnn, catalog_8bit_full_r4, false, false, 1000000, 0, false, true, 4);
LUXIR_BENCHMARK_CAPTURE(BM_GroupKnn, catalog_8bit_full_r8, false, false, 1000000, 0, false, true, 8);
LUXIR_BENCHMARK_CAPTURE(BM_GroupKnn, catalog_8bit_full_r16, false, false, 1000000, 0, false, true, 16);

LUXIR_BENCHMARK_CAPTURE(BM_GroupKnn, catalog_coarse_auto_docs, false, false, 0, 0, true, false);
LUXIR_BENCHMARK_CAPTURE(BM_GroupKnn, catalog_coarse_auto_r1, false, false, 0, 0, true, true, 1);
LUXIR_BENCHMARK_CAPTURE(BM_GroupKnn, catalog_coarse_auto_r2, false, false, 0, 0, true, true, 2);
LUXIR_BENCHMARK_CAPTURE(BM_GroupKnn, catalog_coarse_auto_r4, false, false, 0, 0, true, true, 4);
LUXIR_BENCHMARK_CAPTURE(BM_GroupKnn, catalog_coarse_auto_r8, false, false, 0, 0, true, true, 8);
LUXIR_BENCHMARK_CAPTURE(BM_GroupKnn, catalog_coarse_auto_r16, false, false, 0, 0, true, true, 16);

LUXIR_BENCHMARK_CAPTURE(BM_GroupKnn, catalog_coarse_full_docs, false, false, 1000000, 0, true, false);
LUXIR_BENCHMARK_CAPTURE(BM_GroupKnn, catalog_coarse_full_r1, false, false, 1000000, 0, true, true, 1);
LUXIR_BENCHMARK_CAPTURE(BM_GroupKnn, catalog_coarse_full_r2, false, false, 1000000, 0, true, true, 2);
LUXIR_BENCHMARK_CAPTURE(BM_GroupKnn, catalog_coarse_full_r4, false, false, 1000000, 0, true, true, 4);
LUXIR_BENCHMARK_CAPTURE(BM_GroupKnn, catalog_coarse_full_r8, false, false, 1000000, 0, true, true, 8);
LUXIR_BENCHMARK_CAPTURE(BM_GroupKnn, catalog_coarse_full_r16, false, false, 1000000, 0, true, true, 16);

LUXIR_BENCHMARK_CAPTURE(BM_GroupKnn, repeated_8bit_auto_docs, true, false, 0, 0, false, false);
LUXIR_BENCHMARK_CAPTURE(BM_GroupKnn, repeated_8bit_auto_r1, true, false, 0, 0, false, true, 1);
LUXIR_BENCHMARK_CAPTURE(BM_GroupKnn, repeated_8bit_auto_r2, true, false, 0, 0, false, true, 2);
LUXIR_BENCHMARK_CAPTURE(BM_GroupKnn, repeated_8bit_auto_r4, true, false, 0, 0, false, true, 4);
LUXIR_BENCHMARK_CAPTURE(BM_GroupKnn, repeated_8bit_auto_r8, true, false, 0, 0, false, true, 8);
LUXIR_BENCHMARK_CAPTURE(BM_GroupKnn, repeated_8bit_auto_r16, true, false, 0, 0, false, true, 16);

LUXIR_BENCHMARK_CAPTURE(BM_GroupKnn, repeated_8bit_full_docs, true, false, 1000000, 0, false, false);
LUXIR_BENCHMARK_CAPTURE(BM_GroupKnn, repeated_8bit_full_r1, true, false, 1000000, 0, false, true, 1);
LUXIR_BENCHMARK_CAPTURE(BM_GroupKnn, repeated_8bit_full_r2, true, false, 1000000, 0, false, true, 2);
LUXIR_BENCHMARK_CAPTURE(BM_GroupKnn, repeated_8bit_full_r4, true, false, 1000000, 0, false, true, 4);
LUXIR_BENCHMARK_CAPTURE(BM_GroupKnn, repeated_8bit_full_r8, true, false, 1000000, 0, false, true, 8);
LUXIR_BENCHMARK_CAPTURE(BM_GroupKnn, repeated_8bit_full_r16, true, false, 1000000, 0, false, true, 16);

LUXIR_BENCHMARK_CAPTURE(BM_GroupKnn, repeated_coarse_auto_docs, true, false, 0, 0, true, false);
LUXIR_BENCHMARK_CAPTURE(BM_GroupKnn, repeated_coarse_auto_r1, true, false, 0, 0, true, true, 1);
LUXIR_BENCHMARK_CAPTURE(BM_GroupKnn, repeated_coarse_auto_r2, true, false, 0, 0, true, true, 2);
LUXIR_BENCHMARK_CAPTURE(BM_GroupKnn, repeated_coarse_auto_r4, true, false, 0, 0, true, true, 4);
LUXIR_BENCHMARK_CAPTURE(BM_GroupKnn, repeated_coarse_auto_r8, true, false, 0, 0, true, true, 8);
LUXIR_BENCHMARK_CAPTURE(BM_GroupKnn, repeated_coarse_auto_r16, true, false, 0, 0, true, true, 16);

LUXIR_BENCHMARK_CAPTURE(BM_GroupKnn, repeated_coarse_full_docs, true, false, 1000000, 0, true, false);
LUXIR_BENCHMARK_CAPTURE(BM_GroupKnn, repeated_coarse_full_r1, true, false, 1000000, 0, true, true, 1);
LUXIR_BENCHMARK_CAPTURE(BM_GroupKnn, repeated_coarse_full_r2, true, false, 1000000, 0, true, true, 2);
LUXIR_BENCHMARK_CAPTURE(BM_GroupKnn, repeated_coarse_full_r4, true, false, 1000000, 0, true, true, 4);
LUXIR_BENCHMARK_CAPTURE(BM_GroupKnn, repeated_coarse_full_r8, true, false, 1000000, 0, true, true, 8);
LUXIR_BENCHMARK_CAPTURE(BM_GroupKnn, repeated_coarse_full_r16, true, false, 1000000, 0, true, true, 16);
