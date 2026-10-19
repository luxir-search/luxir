// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "bench/luxir_bench.h"
#include "test/CollectionHelper.h"

using namespace luxir;
using namespace luxir::test;

// Indexes documents of many small fields through the update message path, where
// the per-field work outside the handlers (name mapping, last-wins dedup, handler
// lookup) is not hidden behind text analysis as it is in BM_IndexBook.
static void BM_IndexSmallFields(benchmark::State& state) {
  int32_t docsPerMessage = 500;
  int32_t messages = 20;
  if (unit_tests) {
    docsPerMessage = 50;
    messages = 1;
  }

  static constexpr const char* kCategories[] = {"books", "music", "garden", "toys",
                                                "tools", "games", "sports", "food"};
  static constexpr const char* kColors[] = {"red", "green", "blue", "black", "white", "gray"};

  // Every message resubmits the same docs; allow_dups keeps them from overwriting.
  CollectionHelper::UpdateBuilder batch;
  batch.allowDups();
  for (int32_t d = 0; d < docsPerMessage; d++) {
    uint64_t h = (uint64_t)d * 0x9E3779B97F4A7C15ull;
    batch.add(flatdoc(
        "id", std::to_string(d),
        "sku_s", "sku" + std::to_string(h >> 40),
        "category_s", kCategories[h % 8],
        "brand_s", "brand" + std::to_string((h >> 8) % 500),
        "color_sc", kColors[(h >> 16) % 6],
        "price_d", (double)((h >> 20) % 100'000) / 100,
        "rating_f", (float)((h >> 28) % 50) / 10,
        "stock_i", (int64_t)((h >> 32) % 1000),
        "year_i", (int64_t)(1990 + (h >> 40) % 35),
        "views_i", (int64_t)(h >> 44),
        "updated_i", (int64_t)1'700'000'000 + d,
        "sizes_is", vec_i(h % 13, 13 + (h >> 4) % 13)));
  }
  const int64_t fieldsPerDoc = 12;

  CollectionHelper helper;
  double secs = 0;
  for (auto _ : state) {
    helper.clear();
    auto t0 = std::chrono::steady_clock::now();
    for (int32_t m = 0; m < messages; m++) {
      if (!helper.submit(batch).success) {
        state.SkipWithError("update failed");
        return;
      }
    }
    double iterSecs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    secs += iterSecs;
    state.SetIterationTime(iterSecs);
  }
  helper.clear();

  double docs = (double)docsPerMessage * messages * (double)state.iterations();
  state.counters["docs_rate"] = docs / secs;
  state.counters["ns_per_field"] = secs * 1e9 / (docs * fieldsPerDoc);
}

BENCHMARK(BM_IndexSmallFields)->UseManualTime();
