// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <benchmark/benchmark.h>

#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "bench/luxir_bench.h"
#include "luxir/index/IndexWriter.h"
#include "luxir/index/Inverter.h"
#include "luxir/util/random.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/QueryBuild.h"
#include "test/SchemaBuilder.h"
#include "test/StorageTestUtils.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

namespace {

// How an int column's field is declared and how many values each doc holds.
enum class IntLayout { SINGLE, MULTI_ONE, MULTI_TWO };

constexpr int32_t MERGE_COLUMNS = 8;

api::FieldDef::MultiMode multiMode(IntLayout layout) {
  return layout == IntLayout::SINGLE ? api::FieldDef::MultiMode::SINGLE
                                     : api::FieldDef::MultiMode::ARRAY;
}

void addColumn(SchemaBuilder& schema, const std::string& name, IntLayout layout) {
  auto& field = schema.field(name);
  field.type = api::FieldDef::FieldClass::INT;
  field.index = api::FieldDef::IndexMode::NONE;
  field.multi = multiMode(layout);
}

void indexValues(Inverter& inverter, Inverter::InputHandler& handler, IntLayout layout,
                 SplitMix64& random) {
  int64_t values[2] = {(int64_t)random.rint(1'000'000'000), (int64_t)random.rint(1'000'000'000)};
  if (layout == IntLayout::SINGLE) handler.index(inverter, values[0]);
  else handler.index(inverter, std::span<const int64_t>(values, layout == IntLayout::MULTI_TWO ? 2 : 1));
}

// Merging two segments of column-only int fields. `sparse` leaves every
// fourth doc without values.
void BM_IntColMerge(benchmark::State& state, IntLayout layout, bool sparse) {
  int32_t docs = luxir::unit_tests ? 2000 : 4'000'000;
  int64_t valueCount = 0;
  for (auto _ : state) {
    state.PauseTiming();
    auto dir = std::make_unique<RAMDir>();
    SchemaBuilder builder;
    std::vector<std::string> names;
    for (int32_t column = 0; column < MERGE_COLUMNS; column++) {
      names.push_back(std::format("c{}", column));
      addColumn(builder, names.back(), layout);
    }
    auto base = Schema::createDefaultSchema();
    auto schema = builder.build(base.get());
    CommitSnapshotRegistry snapshots(test::reserveIncarnation(*dir));
    auto writer = std::make_unique<IndexWriter>(snapshots, schema);
    valueCount = 0;
    for (int32_t half = 0; half < 2; half++) {
      Inverter& inverter = writer->obtainInverter();
      std::vector<Inverter::InputHandler*> handlers;
      for (const auto& name : names) handlers.push_back(&inverter.getIndexHandler(name));
      SplitMix64 random(0x51a7c3e9d2b40f67ULL + (uint64_t)half);
      for (int32_t doc = 0; doc < docs / 2; doc++) {
        inverter.startDoc();
        if (!sparse || doc % 4 != 0) {
          for (auto* handler : handlers) indexValues(inverter, *handler, layout, random);
          valueCount += MERGE_COLUMNS * (layout == IntLayout::MULTI_TWO ? 2 : 1);
        }
        inverter.finishDoc();
      }
      writer->releaseInverter(inverter, true);
      writer->commit();
    }
    if (writer->snapshots.readers.getReader()->segments().size() != 2) {
      state.SkipWithError("int column merge setup did not produce two segments");
    }
    state.ResumeTiming();
    writer->mergeSegments();
    state.PauseTiming();
    if (writer->snapshots.readers.getReader()->segments().size() != 1) {
      state.SkipWithError("int column merge did not produce one segment");
    }
    writer.reset();
    dir.reset();
    state.ResumeTiming();
  }
  state.counters["values_per_second"] = benchmark::Counter(
      (double)valueCount * (double)state.iterations(), benchmark::Counter::kIsRate);
}

LUXIR_BENCHMARK_CAPTURE(BM_IntColMerge, single_dense, IntLayout::SINGLE, false)->Unit(benchmark::kMillisecond);
LUXIR_BENCHMARK_CAPTURE(BM_IntColMerge, single_sparse, IntLayout::SINGLE, true)->Unit(benchmark::kMillisecond);
LUXIR_BENCHMARK_CAPTURE(BM_IntColMerge, multi_one_dense, IntLayout::MULTI_ONE, false)->Unit(benchmark::kMillisecond);
LUXIR_BENCHMARK_CAPTURE(BM_IntColMerge, multi_two_dense, IntLayout::MULTI_TWO, false)->Unit(benchmark::kMillisecond);
LUXIR_BENCHMARK_CAPTURE(BM_IntColMerge, multi_two_sparse, IntLayout::MULTI_TWO, true)->Unit(benchmark::kMillisecond);

enum class IntRead { RANGE_COUNT, SORT_ASC, SORT_DESC };

// Reads over one segment of a column-only int field holding one value per
// doc, declared single-valued or multi-valued. The index is rebuilt on every
// run so each binary reads the layout it writes.
void BM_IntColOneValueRead(benchmark::State& state, IntLayout layout, IntRead read) {
  int64_t docs = luxir::unit_tests ? 2000 : 4'000'000;
  std::string collection = std::format("int_col_layout_bm_{}", (int)layout);
  CollectionHelper helper(collection);
  helper.clear();
  SchemaBuilder schema;
  addColumn(schema, "n", layout);
  schema.set(helper.collection());
  auto writer = helper.getIndexWriter();
  Inverter& inverter = writer->obtainInverter();
  Inverter::InputHandler& id = inverter.getIndexHandler("id");
  Inverter::InputHandler& n = inverter.getIndexHandler("n");
  SplitMix64 random(0x8d1f6a3b5c2e9047ULL);
  for (int64_t doc = 0; doc < docs; doc++) {
    inverter.startDoc();
    id.index(inverter, std::to_string(doc));
    indexValues(inverter, n, layout, random);
    inverter.finishDoc();
  }
  writer->releaseInverter(inverter, true);
  helper.commit();

  std::optional<int64_t> expected;
  int64_t iteration = 0;
  for (auto _ : state) {
    auto req = localReq(LuxirTest::luxirNode->getSearchEngine());
    req->collection(collection);
    auto& top = req->topDocs("q").fields({"id"});
    if (read == IntRead::RANGE_COUNT) {
      // Distinct bounds each time, so no request is answered from a cache.
      top.exprQuery(std::format("n:[{} TO {}]", 100'000'000 + iteration, 300'000'000 + iteration))
          .countLimit().limit(0);
      iteration++;
    } else {
      top.allQuery().limit(10);
      qb::sort(top, "n", read == IntRead::SORT_ASC ? qb::ASC : qb::DESC);
    }
    req->execute(false);
    if (!req->ok()) {
      state.SkipWithError(req->errorMsg().c_str());
      break;
    }
    int64_t result = req->getMatchCount("q");
    for (const auto& doc : req->getDocs()) {
      result = result * 31 + (int64_t)java_string_hashcode(std::get<std::string>(*find(doc, "id")));
    }
    if (read != IntRead::RANGE_COUNT && expected && *expected != result) {
      state.SkipWithError("result changed between iterations");
    }
    expected = result;
  }
  state.counters["fp"] = (double)(expected.value_or(0) & 0xffffff);
  helper.clear();
}

#define INT_READ_CAPTURES(name, layout) \
  LUXIR_BENCHMARK_CAPTURE(BM_IntColOneValueRead, name##_range_count, layout, IntRead::RANGE_COUNT); \
  LUXIR_BENCHMARK_CAPTURE(BM_IntColOneValueRead, name##_sort_asc, layout, IntRead::SORT_ASC); \
  LUXIR_BENCHMARK_CAPTURE(BM_IntColOneValueRead, name##_sort_desc, layout, IntRead::SORT_DESC);
INT_READ_CAPTURES(single, IntLayout::SINGLE)
INT_READ_CAPTURES(multi_one, IntLayout::MULTI_ONE)

} // namespace
