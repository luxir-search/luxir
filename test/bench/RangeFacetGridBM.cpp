// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <random>

#include "bench/luxir_bench.h"
#include "luxir/search/RangeFacetGrid.h"
#include "test/RangeFacetTestUtils.h"

using namespace luxir;
namespace {
using Grid = RangeFacetGrid;
constexpr int64_t hour = 3600000;

Grid benchGrid(bool iana, bool shifted) {
  return Grid::fixed(iana ? FieldType::Type::DATE : FieldType::Type::INT, hour,
      shifted ? 17 * 60000 : 0,
      iana ? *resolveTimeZone("America/New_York") : TimeZone::utc());
}

void BM_RangeGridCertify(benchmark::State& state, bool shifted) {
  auto grid = benchGrid(true, shifted);
  int64_t start = grid.floor(*parseDateToEpochMillis("2024-01-01")).key;
  for (auto _ : state) {
    auto certified = grid.affineGap(start, start + 100000 * hour);
    benchmark::DoNotOptimize(certified);
  }
}

void BM_RangeGridBuild(benchmark::State& state, bool iana, bool shifted) {
  auto grid = benchGrid(iana, shifted);
  int64_t start = grid.floor(*parseDateToEpochMillis("2024-01-01")).key;
  auto cell = grid.floor(start);
  // Exactly 100k canonical cells, prepared outside timing.
  for (int i = 0; i < 100000; i++) cell = grid.next(cell);
  for (auto _ : state) {
    auto fences = grid.fences(start, cell.key, true);
    benchmark::DoNotOptimize(fences.values.data());
    benchmark::ClobberMemory();
  }
  state.SetItemsProcessed(state.iterations() * 100000);
}

void BM_RangeGridLookup(benchmark::State& state, bool ordered) {
  auto grid = benchGrid(true, true);
  int64_t start = grid.floor(*parseDateToEpochMillis("2024-01-01")).key;
  auto fences = grid.fences(start, start + 99000 * hour, true).values;
  std::vector<int64_t> values;
  for (size_t i = 0; i + 1 < fences.size(); i++) {
    values.push_back(fences[i] + (fences[i + 1] - fences[i]) / 2);
  }
  if (!ordered) {
    std::mt19937 random(57);
    std::shuffle(values.begin(), values.end(), random);
  }
  for (auto _ : state) {
    for (int64_t value : values) {
      auto cell = std::upper_bound(fences.begin(), fences.end(), value) - fences.begin() - 1;
      benchmark::DoNotOptimize(cell);
    }
  }
  state.SetItemsProcessed(state.iterations() * (int64_t)values.size());
}

void BM_RangeGridCalendarBuild(benchmark::State& state, CalendarUnit unit, bool old) {
  auto zone = *resolveTimeZone("America/New_York");
  int64_t start = *parseDateToEpochMillis("2024-01-01T05:00Z");
  auto civil = civilFromInstant(start, zone).value();
  int64_t end = instantFromCivil(
      *civilCalendarAt(civil.millis, state.range(0), unit), zone, civil.offset).value();
  auto grid = Grid::calendar(unit, 1, zone, start);
  // Compare the exact encoded vectors outside timing on every benchmark lane.
  auto expected = test::oldCalendarFences(start, end, unit, zone);
  if (expected != grid.fences(start, end, true).values) {
    state.SkipWithError("calendar fences differ from the old builder");
    return;
  }
  for (auto _ : state) {
    if (old) {
      auto fences = test::oldCalendarFences(start, end, unit, zone);
      benchmark::DoNotOptimize(fences.data());
      benchmark::ClobberMemory();
    } else {
      auto fences = grid.fences(start, end, true);
      benchmark::DoNotOptimize(fences.values.data());
      benchmark::ClobberMemory();
    }
  }
  state.SetItemsProcessed(state.iterations() * state.range(0));
}

TUNING_BENCHMARK_CAPTURE(BM_RangeGridCertify, affine, false);
TUNING_BENCHMARK_CAPTURE(BM_RangeGridCertify, non_affine, true);
TUNING_BENCHMARK_CAPTURE(BM_RangeGridBuild, integer_100k, false, false);
TUNING_BENCHMARK_CAPTURE(BM_RangeGridBuild, iana_affine_100k, true, false);
TUNING_BENCHMARK_CAPTURE(BM_RangeGridBuild, iana_non_affine_100k, true, true);
TUNING_BENCHMARK_CAPTURE(BM_RangeGridLookup, ordered, true);
TUNING_BENCHMARK_CAPTURE(BM_RangeGridLookup, random, false);
TUNING_BENCHMARK_CAPTURE(BM_RangeGridCalendarBuild, day_old, CalendarUnit::DAY, true)
    ->Arg(365)->Arg(100000);
TUNING_BENCHMARK_CAPTURE(BM_RangeGridCalendarBuild, day_grid, CalendarUnit::DAY, false)
    ->Arg(365)->Arg(100000);
TUNING_BENCHMARK_CAPTURE(BM_RangeGridCalendarBuild, month_old, CalendarUnit::MONTH, true)
    ->Arg(365)->Arg(100000);
TUNING_BENCHMARK_CAPTURE(BM_RangeGridCalendarBuild, month_grid, CalendarUnit::MONTH, false)
    ->Arg(365)->Arg(100000);
} // namespace
