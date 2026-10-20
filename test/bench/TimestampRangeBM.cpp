// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// Timestamp range filters and counts with and without a points index
// (index=RANGE vs column-only), over a 30-day event stream stored in three
// arrival orders: time-sorted, sorted with a one-hour jitter, and random.

#include <benchmark/benchmark.h>

#include <array>
#include <cstdint>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <vector>

#include "bench/luxir_bench.h"
#include "luxir/query/NumericPredicateQuery.h"
#include "luxir/query/QueryPrep.h"
#include "luxir/schema/Schema.h"
#include "luxir/util/random.h"
#include "test/SchemaBuilder.h"
#include "test/TestIndex.h"

using namespace luxir;
using namespace luxir::test;

namespace {

constexpr int64_t START_MS = 1'760'000'000'000LL;
constexpr int64_t HOUR_MS = 3'600'000LL;
constexpr int64_t SPAN_MS = 30 * 24 * HOUR_MS;

enum Order : int32_t { SORTED, JITTER_1H, RANDOM };
constexpr std::array<const char*, 3> ORDER_NAMES{"sorted", "jitter1h", "random"};

struct Window {
  const char* name;
  int64_t ms;
};
constexpr std::array<Window, 3> WINDOWS{{
    {"1h", HOUR_MS}, {"1d", 24 * HOUR_MS}, {"7d", 7 * 24 * HOUR_MS}}};

std::string fieldName(Order order, bool points) {
  return std::string(ORDER_NAMES[(size_t)order]) + (points ? "_pts" : "_col");
}

class TimestampIndex {
  TestIndex index;
  std::shared_ptr<Schema> schema;

public:
  int32_t numDocs;

  TimestampIndex()
      : numDocs(luxir::unit_tests
            ? (int32_t)LuxirTest::scaleTestWork(20'000) : 4'000'000) {
    SchemaBuilder b;
    for (int32_t order = 0; order < (int32_t)ORDER_NAMES.size(); order++) {
      for (bool points : {false, true}) {
        auto& f = b.field(fieldName((Order)order, points));
        f.type = api::FieldDef::FieldClass::DATE;
        f.index = points ? api::FieldDef::IndexMode::RANGE
                         : api::FieldDef::IndexMode::NONE;
      }
    }
    auto base = Schema::createDefaultSchema();
    schema = b.build(base.get());
    index.iw = std::make_unique<IndexWriter>(index.snapshots, schema);

    // Irregular arrivals: uniform gaps averaging SPAN_MS / numDocs.
    SplitMix64 rng(0x7157a3e1);
    std::vector<int64_t> sorted((size_t)numDocs);
    int64_t meanGap = SPAN_MS / numDocs;
    int64_t ts = START_MS;
    for (auto& value : sorted) {
      ts += (int64_t)rng.rint((uint32_t)(2 * meanGap + 1));
      value = ts;
    }
    std::vector<int64_t> shuffled = sorted;
    for (int32_t i = numDocs - 1; i > 0; i--) {
      std::swap(shuffled[(size_t)i], shuffled[(size_t)rng.rint(i + 1)]);
    }

    Inverter& inverter = index.getInverter();
    std::array<Inverter::InputHandler*, 6> handlers{};
    for (int32_t order = 0; order < 3; order++) {
      for (bool points : {false, true}) {
        handlers[(size_t)(order * 2 + points)] =
            &inverter.getIndexHandler(fieldName((Order)order, points));
      }
    }
    for (int32_t doc = 0; doc < numDocs; doc++) {
      int64_t jittered = sorted[(size_t)doc]
          + (int64_t)rng.rint((uint32_t)(2 * HOUR_MS + 1)) - HOUR_MS;
      std::array<int64_t, 3> values{sorted[(size_t)doc], jittered,
                                    shuffled[(size_t)doc]};
      inverter.startDoc();
      for (int32_t order = 0; order < 3; order++) {
        for (bool points : {false, true}) {
          handlers[(size_t)(order * 2 + points)]->index(
              inverter, values[(size_t)order]);
        }
      }
      inverter.finishDoc();
    }
    index.flush();
    index.initReader();
  }

  IndexReader& reader() { return *index.reader; }
};

TimestampIndex& timestampIndex() {
  static TimestampIndex fixture;
  return fixture;
}

// A window in the middle of the stream, offset off any block boundary.
std::pair<int64_t, int64_t> windowBounds(const Window& window) {
  int64_t lo = START_MS + SPAN_MS / 2 + 12'345;
  return {lo, lo + window.ms - 1};
}

void BM_TimestampStorage(benchmark::State& state, Order order) {
  auto& fixture = timestampIndex();
  auto& segment = fixture.reader().segments()[0];
  int64_t columnBytes = 0;
  int64_t pointsBytes = 0;
  for (auto _ : state) {
    MemPool pool;
    Query::Context context(pool, fixture.reader());
    auto colInfo = context.topReader.field(fieldName(order, false)).at(0);
    auto ptsInfo = context.topReader.field(fieldName(order, true)).at(0);
    IntColReader column(segment.postingsReader(), *colInfo);
    PointsReader points(segment.postingsReader(), *ptsInfo);
    columnBytes = column.sizeInBytes();
    pointsBytes = (int64_t)points.sizeInBytes();
    benchmark::DoNotOptimize(columnBytes + pointsBytes);
  }
  state.counters["column_bytes_per_value"] =
      (double)columnBytes / fixture.numDocs;
  state.counters["points_bytes_per_value"] =
      (double)pointsBytes / fixture.numDocs;
  state.counters["points_over_column"] =
      (double)pointsBytes / (double)columnBytes;
}

// count=true: the exact count a hit total resolves (count shortcut, then the
// supplier's constant count, else iteration). count=false: iterate matches as
// a range filter driving collection does.
void BM_TimestampRange(benchmark::State& state, Order order, bool points,
                       const Window& window, bool count) {
  auto& fixture = timestampIndex();
  auto& segment = fixture.reader().segments()[0];
  auto [lo, hi] = windowBounds(window);
  std::string field = fieldName(order, points);
  int64_t matches = 0;
  for (auto _ : state) {
    MemPool pool;
    Query::Context context(pool, fixture.reader());
    NumericPredicateQuery query(field, lo, hi);
    auto* weight = query.createWeight(context, 0);
    Query::ScorerSupplier* supplier = nullptr;
    std::optional<int64_t> resolved;
    if (count) {
      resolved = QueryPrep::resolveCount(
          weight->countShortcut(segment, nullptr),
          [&](Query::Weight& resolver) {
            return supplier = resolver.scorerSupplier(pool, segment);
          }, Query::COUNT_EXECUTION_LIMIT);
      if (!resolved) {
        if (supplier == nullptr) supplier = weight->scorerSupplier(pool, segment);
        resolved = QueryPrep::supplierCount(supplier);
      }
    }
    if (resolved) {
      matches = *resolved;
    } else {
      matches = 0;
      auto* scorer = weight->createScorer(pool, segment);
      if (scorer != nullptr) {
        while (scorer->next() != PostingsReader::END) matches++;
      }
    }
    benchmark::DoNotOptimize(matches);
  }
  state.counters["matches"] = (double)matches;
}

[[maybe_unused]] bool registered = [] {
  for (int32_t o = 0; o < (int32_t)ORDER_NAMES.size(); o++) {
    Order order = (Order)o;
    benchmark::RegisterBenchmark(
        std::string("BM_TimestampStorage/") + ORDER_NAMES[(size_t)o],
        [order](benchmark::State& state) { BM_TimestampStorage(state, order); })
        ->Iterations(1);
    for (bool count : {true, false}) {
      for (const Window& window : WINDOWS) {
        for (bool points : {true, false}) {
          std::string name = std::string("BM_TimestampRange/")
              + (count ? "count/" : "filter/") + ORDER_NAMES[(size_t)o] + "/"
              + window.name + "/" + (points ? "points" : "column");
          benchmark::RegisterBenchmark(
              name, [order, points, &window, count](benchmark::State& state) {
                BM_TimestampRange(state, order, points, window, count);
              })->UseRealTime()->Unit(benchmark::kMicrosecond);
        }
      }
    }
  }
  return true;
}();

} // namespace
