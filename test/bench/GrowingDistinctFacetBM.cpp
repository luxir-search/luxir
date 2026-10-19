// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <random>

#include "bench/luxir_bench.h"
#include "luxir/search/ops/DistinctFacetCounts.h"

using namespace luxir;
namespace {
using Counts = DistinctFacetCounts;
using Mode = DistinctFacetMode;

void rates(benchmark::State& state, size_t pairs, size_t peak) {
  state.SetItemsProcessed(state.iterations() * (int64_t)pairs);
  state.counters["ns/pair"] = benchmark::Counter((double)pairs,
      benchmark::Counter::kIsIterationInvariantRate | benchmark::Counter::kInvert);
  state.counters["peak_bytes"] = (double)peak;
}

// Warm rows and pages outside timing. Fixed and growing use the same ordinary
// identities; the latter additionally maps raw values through the cell window.
void BM_GrowingDistinctCollect(benchmark::State& state, bool growing, Mode mode, bool dense,
                              size_t pagedLimit = 32ULL << 20) {
  size_t cells = (size_t)state.range(0);
  uint64_t groups = (uint64_t)state.range(1);
  auto grid = RangeFacetGrid::fixed(FieldType::Type::INT, 1);
  RequestMemTracker tracker(0);
  std::unique_ptr<Counts> counts;
  if (growing) counts = std::make_unique<Counts>(tracker, "collect", mode, grid, FacetDimension{true, 0, groups}, pagedLimit);
  else counts = std::make_unique<Counts>(tracker, "collect", mode, FacetDimension{true, 0, cells}, FacetDimension{true, 0, groups});
  std::vector<FacetPair> pairs(1 << 20);
  for (size_t i = 0; i < pairs.size(); i++) {
    int64_t cell = (int64_t)(i % cells);
    pairs[i] = {cell, (int64_t)(dense ? i / cells % groups : (uint64_t)cell % groups)};
  }
  // Randomize hits without changing density or the set of allocated pages.
  std::mt19937 random(57);
  std::shuffle(pairs.begin(), pairs.end(), random);
  auto collect = [&](auto add, auto) { for (auto pair : pairs) add(pair.value, pair.group); };
  if (growing) counts->collectValues(collect); else counts->collect(collect);
  for (auto _ : state) {
    if (growing) counts->collectValues(collect); else counts->collect(collect);
    benchmark::DoNotOptimize(counts.get());
    benchmark::ClobberMemory();
  }
  rates(state, pairs.size(), tracker.peak());
  state.counters["hash"] = counts->selectedMode() == Mode::HASH;
}

// Include allocations, ordered extent growth, and any one-way conversion.
// A 32MiB milestone is crossed for the wide case with one group per cell.
void BM_GrowingDistinctCrossover(benchmark::State& state, Mode mode, bool dense,
                                size_t pagedLimit = 32ULL << 20) {
  size_t cells = (size_t)state.range(0);
  auto grid = RangeFacetGrid::fixed(FieldType::Type::INT, 1);
  size_t peak = 0, conversions = 0;
  bool hash = false;
  size_t perCell = dense ? 64 : 1;
  for (auto _ : state) {
    RequestMemTracker tracker(0);
    Counts counts(tracker, "ordered growth", mode, grid, {true, 0, 4096}, pagedLimit);
    counts.collectValues([&](auto add, auto) {
      for (size_t i = 0; i < cells; i++) for (size_t g = 0; g < perCell; g++) add((int64_t)i, (int64_t)g);
    });
    peak = std::max(peak, tracker.peak());
    conversions = counts.conversionCount();
    hash = counts.selectedMode() == Mode::HASH;
    benchmark::DoNotOptimize(counts.cellWindow());
    benchmark::ClobberMemory();
  }
  rates(state, cells * perCell, peak);
  state.counters["conversions"] = (double)conversions;
  state.counters["hash"] = hash;
}

void BM_GrowingDistinctMerge(benchmark::State& state, Mode mode, bool mixed) {
  size_t cells = (size_t)state.range(0), splits = (size_t)state.range(1);
  auto grid = RangeFacetGrid::fixed(FieldType::Type::INT, 1);
  RequestMemTracker tracker(0);
  std::vector<std::unique_ptr<Counts>> sources;
  for (size_t i = 0; i < splits; i++) {
    auto requested = mixed && i % 2 ? Mode::HASH : mode;
    sources.push_back(std::make_unique<Counts>(tracker, "source", requested, grid, FacetDimension{true, 0, 64}));
    sources.back()->collectValues([&](auto add, auto) {
      for (size_t j = 0; j < cells; j++) add((int64_t)j, (int64_t)i);
    });
  }
  for (auto _ : state) {
    state.PauseTiming();
    // Same full extent prevents the reducer from taking ownership of a source.
    // HASH destination for a mixed merge makes its one-way policy explicit.
    auto result = std::make_unique<Counts>(tracker, "merge", mixed ? Mode::HASH : mode,
        grid, FacetDimension{true, 0, 64});
    result->collectValues([&](auto add, auto) { for (size_t j = 0; j < cells; j++) add((int64_t)j, (int64_t)0); });
    state.ResumeTiming();
    for (size_t i = 1; i < splits; i++) {
      auto* merged = Counts::merge(result.get(), sources[i].get());
      if (merged != result.get()) { state.SkipWithError("unexpected source ownership"); break; }
    }
    benchmark::DoNotOptimize(result->cellWindow());
    benchmark::ClobberMemory();
    state.PauseTiming();
    // Destruction is deliberately timed out, like the count-window merge cells.
    result.reset();
    state.ResumeTiming();
  }
  rates(state, cells * (splits - 1), tracker.peak());
}

#define COLLECT_CASE(name, growing, mode, dense) \
  TUNING_BENCHMARK_CAPTURE(BM_GrowingDistinctCollect, name, growing, mode, dense) \
      ->Args({64, 4096})->Args({64, 1000000})->Args({20000, 4096}) \
      ->Args({100000, 64})->Args({100000, 4096})
COLLECT_CASE(fixed_sparse, false, Mode::PAGED, false);
COLLECT_CASE(growing_sparse, true, Mode::PAGED, false);
COLLECT_CASE(auto_sparse, true, Mode::AUTO, false);
COLLECT_CASE(hash_sparse, true, Mode::HASH, false);
COLLECT_CASE(fixed_dense, false, Mode::PAGED, true);
COLLECT_CASE(growing_dense, true, Mode::PAGED, true);
COLLECT_CASE(auto_dense, true, Mode::AUTO, true);
#undef COLLECT_CASE
TUNING_BENCHMARK_CAPTURE(BM_GrowingDistinctCollect, auto_sparse_8m, true, Mode::AUTO, false, 8ULL << 20)
    ->Args({20000, 4096})->Args({100000, 4096});

TUNING_BENCHMARK_CAPTURE(BM_GrowingDistinctCrossover, auto_sparse, Mode::AUTO, false)->Arg(64)->Arg(100000);
TUNING_BENCHMARK_CAPTURE(BM_GrowingDistinctCrossover, paged_sparse, Mode::PAGED, false)->Arg(64)->Arg(100000);
TUNING_BENCHMARK_CAPTURE(BM_GrowingDistinctCrossover, hash_sparse, Mode::HASH, false)->Arg(64)->Arg(100000);
TUNING_BENCHMARK_CAPTURE(BM_GrowingDistinctCrossover, auto_sparse_8m, Mode::AUTO, false, 8ULL << 20)->Arg(100000);
TUNING_BENCHMARK_CAPTURE(BM_GrowingDistinctCrossover, auto_dense, Mode::AUTO, true)->Arg(64)->Arg(100000);
TUNING_BENCHMARK_CAPTURE(BM_GrowingDistinctCrossover, paged_dense, Mode::PAGED, true)->Arg(64)->Arg(100000);
TUNING_BENCHMARK_CAPTURE(BM_GrowingDistinctCrossover, hash_dense, Mode::HASH, true)->Arg(64)->Arg(100000);
#define MERGE_CASE(name, mode, mixed) \
  TUNING_BENCHMARK_CAPTURE(BM_GrowingDistinctMerge, name, mode, mixed) \
      ->Args({64, 8})->Args({64, 32})->Args({100000, 8})->Args({100000, 32})
MERGE_CASE(paged, Mode::PAGED, false);
MERGE_CASE(hash, Mode::HASH, false);
MERGE_CASE(mixed, Mode::PAGED, true);
#undef MERGE_CASE
} // namespace
