// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "bench/luxir_bench.h"
#include "luxir/query/KnnQuery.h"
#include "luxir/reader/SkipStats.h"
#include "test/CollectionHelper.h"
#include "test/KnnTestUtils.h"
#include "test/LocalReq.h"
#include "test/QueryBuild.h"
#include "test/SchemaBuilder.h"

using namespace luxir;
using namespace luxir::test;

namespace {
void BM_OwnerKnn(benchmark::State& state) {
  ForceIvfGuard force;
  IvfTestGuard ivf(16);
  int32_t mode = (int32_t)state.range(0);
  bool sparse = state.range(1) != 0;
  int32_t count = unit_tests ? 240 : 10000;
  constexpr int32_t dims = 16;
  std::string name = "owner_knn_" + std::to_string(mode) + (sparse ? "_sparse" : "_all");
  CollectionHelper helper(name);
  helper.clear();
  helper.getIndexWriter()->snapshots.readers.filterCache = std::make_shared<FilterCache>(FilterCacheConfig{.maxBytes = 0});
  helper.getIndexWriter()->setMergeFactor(100);
  for (int segment = 0; segment < 2; segment++) {
    SchemaBuilder schema;
    auto& path = schema.field("chunks");
    path.type = api::FieldDef::FieldClass::NESTED;
    path.reverse_map = mode == 1 || (mode == 2 && segment == 1);
    auto& vector = schema.field("chunks.v");
    vector.type = api::FieldDef::FieldClass::VECTOR;
    vector.column = true; vector.dims = dims; vector.metric = api::VectorMetric::L2;
    vector.ann = rabitqAnn(4);
    schema.set(helper.collection());
    CollectionHelper::UpdateBuilder input;
    for (int32_t root = segment * count / 2; root < (segment + 1) * count / 2; root++) {
      std::string json = R"({"id":")" + std::to_string(root) + R"(","root_i":)" + std::to_string(root) + R"(,"chunks":[)";
      for (int32_t child = 0, n = root == 0 && sparse ? 4096 : 8; child < n; child++) {
        json += child ? R"(,{"v":[)" : R"({"v":[)";
        SplitMix64 random((uint64_t)root * 293 + (uint64_t)child);
        for (int32_t d = 0; d < dims; d++) json += (d ? "," : "") + std::to_string((float)(random.rint(2001) - 1000) / 1000.0f);
        json += "]}";
      }
      json += "]}";
      input.addRaw([&](auto& doc, auto& arena) { EXPECT_TRUE(api::read_json(doc, json, arena)); });
    }
    input.commit(); ASSERT_TRUE(helper.submit(input).success);
  }
  helper.commit({"vec.chunks.v"});
  if (sparse) {
    auto req = localReq(helper.getSearchEngine());
    req->reader = helper.getIndexWriter()->snapshots.readers.getReader();
    req->schema = req->reader->schema();
    auto& relation = req->ownershipRelation(*req->pinView("chunks"), *req->reader);
    ArrDocSet selected(std::vector<int32_t>{0});
    auto& segment = req->reader->segments()[0];
    size_t bytes = req->memoryTracker.bytes(), charged = 0;
    auto start = std::chrono::steady_clock::now();
    for (int n = 0; n < 100; n++) {
      auto mapped = relation.expand(0, DomainHandle::borrowed(&selected).scoped(segment));
      EXPECT_EQ(4096, mapped.get()->card());
      charged = req->memoryTracker.bytes() - bytes;
    }
    EXPECT_EQ(bytes, req->memoryTracker.bytes());
    state.counters["expand_us"] = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / 100;
    state.counters["expand_bytes"] = (double)charged;
  }
  std::vector<float> vector(dims, 0.25f);
  KnnQuery::Stats stats;
  SearchOverridesGuard counters(KnnQuery::statsForTests); KnnQuery::statsForTests = &stats;
  auto execute = [&] {
    auto req = localReq(helper.getSearchEngine());
    req->collection(name);
    auto& op = req->topDocs().limit(0).countLimit();
    op.rawQuery() = qb::knn(req->mr, "chunks.v", vector, 10, 0, false, 0, 1);
    if (sparse) {
      api::Query filter; filter.kind.emplace<api::ExprQuery>().q = "root_i:0";
      op.filter(filter);
    }
    req->execute(1);
    EXPECT_OK(req);
    EXPECT_EQ(sparse ? 1 : 10, req->getMatchCount());
    return req->memoryTracker.peak();
  };
  { SkipStatsScope mappings; execute(); state.counters["reverse_reads"] = (double)SkipStats::ownershipReverseReads; }
  int64_t rounds = stats.rounds.load(), scanned = stats.codesVisited.load(), units = stats.unitRowsVisited.load();
  size_t peak = 0;
  for (auto _ : state) peak = std::max(peak, execute());
  state.counters["rounds"] = (double)(stats.rounds.load() - rounds) / (double)state.iterations();
  state.counters["codes"] = (double)(stats.codesVisited.load() - scanned) / (double)state.iterations();
  state.counters["unit_rows"] = (double)(stats.unitRowsVisited.load() - units) / (double)state.iterations();
  state.counters["peak_bytes"] = (double)peak;
  state.counters["owners"] = count;
}
}

LUXIR_BENCHMARK(BM_OwnerKnn)->ArgNames({"reverse", "sparse"})
    ->Args({0,0})->Args({1,0})->Args({2,0})->Args({0,1})->Args({1,1})->Args({2,1});
