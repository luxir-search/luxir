// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <random>

#include "bench/luxir_bench.h"
#include "luxir/search/RangeFacetCountWindow.h"

using namespace luxir;
namespace {
using Grid = RangeFacetGrid;
using Window = RangeFacetCountWindow;

std::vector<int64_t> affineValues(size_t cells, bool ordered) {
  std::vector<int64_t> values(1 << 20);
  for (size_t i = 0; i < values.size(); i++) values[i] = (int64_t)(i * cells / values.size()) * 7 + 3;
  if (!ordered) {
    std::mt19937 random(57);
    std::shuffle(values.begin(), values.end(), random);
  }
  return values;
}

void BM_RangeWindowAffine(benchmark::State& state, bool fixed, bool ordered) {
  size_t cells = (size_t)state.range(0);
  auto values = affineValues(cells, ordered);
  auto grid = Grid::fixed(FieldType::Type::INT, 7, 3);
  RequestMemTracker tracker(0);
  Window window(grid, tracker, "benchmark");
  if (!fixed) window.widen(3, (int64_t)cells * 7 + 3);
  std::vector<int64_t> counts(fixed ? cells : 0);
  for (auto _ : state) {
    if (fixed) {
      // Existing fixed-window kernel: unsigned range check then division.
      uint64_t gap = 7, base = 3;
      benchmark::DoNotOptimize(gap);
      benchmark::DoNotOptimize(base);
      uint64_t width = cells * gap;
      for (int64_t value : values) {
        uint64_t offset = (uint64_t)value - base;
        if (offset < width) counts[offset / gap]++;
      }
      benchmark::DoNotOptimize(counts.data());
    } else {
      for (int64_t value : values) window.add(value);
      benchmark::DoNotOptimize(window.counts().data());
    }
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations() * (int64_t)values.size());
  state.counters["ns/value"] = benchmark::Counter((double)values.size(), benchmark::Counter::kIsIterationInvariantRate | benchmark::Counter::kInvert);
  state.counters["peak_bytes"] = (double)(fixed ? cells * 8 : tracker.peak());
}

void BM_RangeWindowSearch(benchmark::State& state, bool ordered) {
  auto grid = Grid::fixed(FieldType::Type::DATE, 3600000, 17 * 60000,
                         resolveTimeZone("America/New_York").value());
  size_t cells = (size_t)state.range(0);
  std::vector<int64_t> keys;
  auto cell = grid.floor(parseDateToEpochMillis("2024-03-09").value());
  for (size_t i = 0; i < cells; i++) {
    keys.push_back(cell.key);
    if (i + 1 < cells) cell = grid.next(cell);
  }
  std::vector<int64_t> values(1 << 20);
  for (size_t i = 0; i < values.size(); i++) values[i] = keys[i * cells / values.size()] + 1;
  if (!ordered) {
    std::mt19937 random(57);
    std::shuffle(values.begin(), values.end(), random);
  }
  RequestMemTracker tracker(0);
  Window window(grid, tracker, "search");
  window.add(*std::min_element(values.begin(), values.end()));
  window.add(*std::max_element(values.begin(), values.end()));
  for (auto _ : state) {
    for (int64_t value : values) window.add(value);
    benchmark::DoNotOptimize(window.counts().data());
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations() * (int64_t)values.size());
  state.counters["ns/value"] = benchmark::Counter((double)values.size(), benchmark::Counter::kIsIterationInvariantRate | benchmark::Counter::kInvert);
  state.counters["peak_bytes"] = (double)tracker.peak();
}

void BM_RangeWindowGrow(benchmark::State& state, int order) {
  std::vector<int64_t> values((size_t)state.range(0));
  for (size_t i = 0; i < values.size(); i++) {
    int64_t cell = order == 2 ? (i % 2 ? -(int64_t)((i + 1) / 2) : (int64_t)(i / 2)) : (int64_t)i;
    values[i] = cell * 7 + 3;
  }
  if (order == 1) {
    std::mt19937 random(57);
    std::shuffle(values.begin(), values.end(), random);
  }
  auto grid = Grid::fixed(FieldType::Type::INT, 7, 3);
  size_t peak = 0;
  for (auto _ : state) {
    RequestMemTracker tracker(0);
    Window window(grid, tracker, "growth");
    for (int64_t value : values) window.add(value);
    peak = std::max(peak, tracker.peak());
    benchmark::DoNotOptimize(window.counts().data());
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations() * (int64_t)values.size());
  state.counters["ns/value"] = benchmark::Counter((double)values.size(), benchmark::Counter::kIsIterationInvariantRate | benchmark::Counter::kInvert);
  state.counters["peak_bytes"] = (double)peak;
}

void BM_RangeWindowMerge(benchmark::State& state, bool disjoint, bool iana) {
  size_t cells = (size_t)state.range(0), contributors = (size_t)state.range(1);
  auto grid = iana ? Grid::fixed(FieldType::Type::DATE, 3600000, 17 * 60000,
                                resolveTimeZone("America/New_York").value())
                   : Grid::fixed(FieldType::Type::INT, 7, 3);
  std::vector<int64_t> keys;
  auto cell = grid.floor(iana ? parseDateToEpochMillis("2024-03-09").value() : 3);
  for (size_t i = 0; i < cells; i++) {
    keys.push_back(cell.key);
    if (i + 1 < cells) cell = grid.next(cell);
  }
  RequestMemTracker tracker(0);
  std::vector<std::unique_ptr<Window>> windows;
  for (size_t i = 0; i < contributors; i++) {
    windows.push_back(std::make_unique<Window>(grid, tracker, "merge"));
    size_t begin = disjoint ? cells * i / contributors : 0;
    size_t end = disjoint ? cells * (i + 1) / contributors : cells;
    for (size_t j = begin; j < end; j++) windows.back()->add(keys[j]);
  }
  for (auto _ : state) {
    state.PauseTiming();
    auto result = std::make_unique<Window>(grid, tracker, "merge result");
    result->mergeFrom(*windows[0]);
    state.ResumeTiming();
    for (size_t i = 1; i < contributors; i++) result->mergeFrom(*windows[i]);
    benchmark::DoNotOptimize(result->counts().data());
    benchmark::ClobberMemory();
    state.PauseTiming();
    result.reset();
    state.ResumeTiming();
  }
  size_t merged = disjoint ? cells - cells / contributors : cells * (contributors - 1);
  state.SetItemsProcessed(state.iterations() * (int64_t)merged);
  state.counters["ns/merged_cell"] = benchmark::Counter((double)merged, benchmark::Counter::kIsIterationInvariantRate | benchmark::Counter::kInvert);
  state.counters["peak_bytes"] = (double)tracker.peak();
}

TUNING_BENCHMARK_CAPTURE(BM_RangeWindowAffine, fixed_ordered, true, true)->Arg(64)->Arg(100000);
TUNING_BENCHMARK_CAPTURE(BM_RangeWindowAffine, fixed_random, true, false)->Arg(64)->Arg(100000);
TUNING_BENCHMARK_CAPTURE(BM_RangeWindowAffine, growing_ordered, false, true)->Arg(64)->Arg(100000);
TUNING_BENCHMARK_CAPTURE(BM_RangeWindowAffine, growing_random, false, false)->Arg(64)->Arg(100000);
TUNING_BENCHMARK_CAPTURE(BM_RangeWindowSearch, ordered, true)->Arg(64)->Arg(100000);
TUNING_BENCHMARK_CAPTURE(BM_RangeWindowSearch, random, false)->Arg(64)->Arg(100000);
TUNING_BENCHMARK_CAPTURE(BM_RangeWindowGrow, ordered, 0)->Arg(64)->Arg(100000);
TUNING_BENCHMARK_CAPTURE(BM_RangeWindowGrow, random, 1)->Arg(64)->Arg(100000);
TUNING_BENCHMARK_CAPTURE(BM_RangeWindowGrow, alternating, 2)->Arg(64)->Arg(100000);
TUNING_BENCHMARK_CAPTURE(BM_RangeWindowMerge, overlapping, false, false)
    ->Args({64, 8})->Args({64, 32})->Args({100000, 8})->Args({100000, 32});
TUNING_BENCHMARK_CAPTURE(BM_RangeWindowMerge, disjoint, true, false)
    ->Args({64, 8})->Args({64, 32})->Args({100000, 8})->Args({100000, 32});
TUNING_BENCHMARK_CAPTURE(BM_RangeWindowMerge, iana_overlapping, false, true)
    ->Args({64, 8})->Args({64, 32})->Args({100000, 8})->Args({100000, 32});
TUNING_BENCHMARK_CAPTURE(BM_RangeWindowMerge, iana_disjoint, true, true)
    ->Args({64, 8})->Args({64, 32})->Args({100000, 8})->Args({100000, 32});
} // namespace
