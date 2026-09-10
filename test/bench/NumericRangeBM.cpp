// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <benchmark/benchmark.h>

#include <array>
#include <cstdint>
#include <memory>
#include <numeric>
#include <string>
#include <string_view>
#include <vector>

#include "bench/luxir_bench.h"
#include "luxir/query/NumericPredicateQuery.h"
#include "luxir/schema/Schema.h"
#include "luxir/util/random.h"
#include "test/SchemaBuilder.h"
#include "test/TestIndex.h"

using namespace luxir;
using namespace luxir::test;

namespace {

constexpr int32_t DUPLICATE_GROUP_SIZE = 64;

enum FieldShape : int32_t {
  UNIQUE_CORRELATED,
  UNIQUE_SHUFFLED,
  DUPLICATE_CORRELATED,
  DUPLICATE_SHUFFLED
};

enum BenchArm : int32_t {
  POINTS_FORCED,
  COMPLEMENT_FORCED,
  ZONE_FORCED,
  FULLSCAN_FORCED,
  SELECTED
};

struct FieldSpec {
  std::string_view name;
  std::string_view label;
  bool duplicateHeavy;
  bool shuffled;
};

constexpr std::array<FieldSpec, 4> FIELDS{{
  {"range_unique_correlated_i", "unique_correlated", false, false},
  {"range_unique_shuffled_i", "unique_shuffled", false, true},
  {"range_duplicate_correlated_i", "duplicate_correlated", true, false},
  {"range_duplicate_shuffled_i", "duplicate_shuffled", true, true},
}};

constexpr std::array<int32_t, 12> SELECTIVITIES{{
  10, 50, 100, 150, 200, 250, 300, 400, 500, 600, 750, 900
}};

std::string_view armName(BenchArm arm) {
  switch (arm) {
    case POINTS_FORCED: return "points_forced";
    case COMPLEMENT_FORCED: return "complement_forced";
    case ZONE_FORCED: return "zone_forced";
    case FULLSCAN_FORCED: return "fullscan_forced";
    case SELECTED: return "selected";
  }
  return "unknown";
}

bool unitTestSelectivity(int32_t perMille) {
  return perMille == 50 || perMille == 500 || perMille == 900;
}

class NumericRangeBenchIndex {
  TestIndex index;
  std::shared_ptr<Schema> schema;
  int32_t numDocs;

public:
  NumericRangeBenchIndex()
      : numDocs(luxir::unit_tests
            ? (int32_t)LuxirTest::scaleTestWork(5'120)
            : 512'000) {
    SchemaBuilder b;
    for (const FieldSpec& spec : FIELDS) {
      auto& f = b.field(spec.name);
      f.type = api::FieldDef::FieldClass::INT;
      f.index = api::FieldDef::IndexMode::RANGE;
    }
    auto base = Schema::createDefaultSchema();
    schema = b.build(base.get());
    index.iw = std::make_unique<IndexWriter>(
        index.dir, schema);

    std::vector<int32_t> shuffledDocs((size_t)numDocs);
    std::iota(shuffledDocs.begin(), shuffledDocs.end(), 0);
    SplitMix64 rng(0x76a5b31d);
    for (int32_t i = numDocs - 1; i > 0; i--) {
      std::swap(shuffledDocs[(size_t)i],
                shuffledDocs[(size_t)rng.rint(i + 1)]);
    }

    Inverter& inverter = index.getInverter();
    auto& uniqueCorrelated = inverter.getIndexHandler(FIELDS[UNIQUE_CORRELATED].name);
    auto& uniqueShuffled = inverter.getIndexHandler(FIELDS[UNIQUE_SHUFFLED].name);
    auto& duplicateCorrelated =
        inverter.getIndexHandler(FIELDS[DUPLICATE_CORRELATED].name);
    auto& duplicateShuffled =
        inverter.getIndexHandler(FIELDS[DUPLICATE_SHUFFLED].name);
    for (int32_t doc = 0; doc < numDocs; doc++) {
      inverter.startDoc();
      uniqueCorrelated.index(inverter, 7 + (int64_t)doc * 2);
      uniqueShuffled.index(
          inverter, 7 + (int64_t)shuffledDocs[(size_t)doc] * 2);
      duplicateCorrelated.index(
          inverter, 7 + (int64_t)(doc / DUPLICATE_GROUP_SIZE) * 2);
      duplicateShuffled.index(
          inverter,
          7 + (int64_t)(shuffledDocs[(size_t)doc] / DUPLICATE_GROUP_SIZE) * 2);
      inverter.finishDoc();
    }
    index.flush();
    index.initReader();
  }

  TestIndex& getIndex() { return index; }
  int32_t docCount() const { return numDocs; }
};

NumericRangeBenchIndex& benchIndex() {
  static NumericRangeBenchIndex fixture;
  return fixture;
}

int32_t countMatches(Query::Scorer* scorer) {
  int32_t count = 0;
  if (scorer != nullptr) {
    while (scorer->next() != PostingsReader::END) count++;
  }
  return count;
}

Query::Scorer* createScorer(NumericPredicateQuery::Weight& weight, MemPool& pool,
                            IndexReader::Segment& segment, BenchArm arm) {
  switch (arm) {
    case POINTS_FORCED:
      return weight.createPointsScorerForTests(pool, segment);
    case COMPLEMENT_FORCED:
      return weight.createComplementScorerForTests(pool, segment);
    case ZONE_FORCED:
      return weight.createZoneMapScorerForTests(pool, segment);
    case FULLSCAN_FORCED:
      return weight.createFullScanScorerForTests(pool, segment);
    case SELECTED:
      return weight.createScorer(pool, segment);
  }
  return nullptr;
}

int32_t expectedMatches(const NumericRangeBenchIndex& fixture,
                        const FieldSpec& field, int32_t perMille) {
  int32_t expected =
      (int32_t)((int64_t)fixture.docCount() * perMille / 1000);
  if (field.duplicateHeavy) {
    if (expected % DUPLICATE_GROUP_SIZE != 0) {
      throw std::logic_error("duplicate range does not end on a value group");
    }
  }
  return expected;
}

int64_t rangeHi(const FieldSpec& field, int32_t expected) {
  int32_t selectedValues = field.duplicateHeavy
      ? expected / DUPLICATE_GROUP_SIZE : expected;
  return 7 + (int64_t)(selectedValues - 1) * 2;
}

int32_t fullScanCount(NumericRangeBenchIndex& fixture, const FieldSpec& field,
                      int64_t hi) {
  MemPool pool;
  Query::Context context(pool, *fixture.getIndex().reader);
  NumericPredicateQuery query(field.name, 7, hi);
  auto* weight = static_cast<NumericPredicateQuery::Weight*>(
      query.createWeight(context, 0));
  auto& segment = fixture.getIndex().reader->segments()[0];
  return countMatches(weight->createFullScanScorerForTests(pool, segment));
}

void BM_NumericRangePoints(benchmark::State& state, FieldShape shape,
                           BenchArm arm) {
  int32_t perMille = (int32_t)state.range(0);
  if (luxir::unit_tests && !unitTestSelectivity(perMille)) {
    state.SkipWithMessage("reduced unit-test selectivity sweep");
    return;
  }

  auto& fixture = benchIndex();
  const FieldSpec& field = FIELDS[(size_t)shape];
  int32_t expected = expectedMatches(fixture, field, perMille);
  int64_t hi = rangeHi(field, expected);
  int32_t baseline = fullScanCount(fixture, field, hi);
  if (baseline != expected) {
    state.SkipWithError("numeric range full-scan baseline mismatch");
    return;
  }

  for (auto _ : state) {
    MemPool pool;
    Query::Context context(pool, *fixture.getIndex().reader);
    NumericPredicateQuery query(field.name, 7, hi);
    auto* weight = static_cast<NumericPredicateQuery::Weight*>(
        query.createWeight(context, 0));
    auto& segment = fixture.getIndex().reader->segments()[0];
    int32_t count = countMatches(createScorer(*weight, pool, segment, arm));
    benchmark::DoNotOptimize(count);
    if (count != baseline) {
      state.SkipWithError("numeric range arm differs from full scan");
      break;
    }
  }
  state.counters["docs"] = fixture.docCount();
  state.counters["matches"] = expected;
}

void BM_NumericRangePointsSize(benchmark::State& state, FieldShape shape) {
  auto& fixture = benchIndex();
  const FieldSpec& field = FIELDS[(size_t)shape];
  uint64_t bytes = 0;
  uint64_t points = 0;
  for (auto _ : state) {
    MemPool pool;
    Query::Context context(pool, *fixture.getIndex().reader);
    auto infos = context.readSegInfos(field.name);
    auto& segment = fixture.getIndex().reader->segments()[0];
    if (infos.empty() || infos[0] == nullptr || infos[0]->pointsMetaOff == 0) {
      state.SkipWithError("points index is absent");
      return;
    }
    PointsReader reader(segment.postingsReader(), *infos[0]);
    bytes = reader.sizeInBytes();
    points = reader.pointCount();
    benchmark::DoNotOptimize(bytes);
    benchmark::DoNotOptimize(points);
  }
  state.counters["bytes_per_point"] = (double)bytes / (double)points;
  state.counters["size_bytes"] = (double)bytes;
  state.counters["points"] = (double)points;
}

benchmark::Benchmark* registerTimingBenchmark(FieldShape shape, BenchArm arm) {
  const FieldSpec& field = FIELDS[(size_t)shape];
  std::string name = "BM_NumericRangePoints/" + std::string(field.label)
      + "/" + std::string(armName(arm));
  auto* registration = benchmark::RegisterBenchmark(
      name, [shape, arm](benchmark::State& state) {
        BM_NumericRangePoints(state, shape, arm);
      });
  registration->ArgName("permille");
  for (int32_t perMille : SELECTIVITIES) registration->Arg(perMille);
  return registration->UseRealTime();
}

benchmark::Benchmark* registerSizeBenchmark(FieldShape shape) {
  const FieldSpec& field = FIELDS[(size_t)shape];
  std::string name =
      "BM_NumericRangePointsSize/" + std::string(field.label);
  return benchmark::RegisterBenchmark(
      name, [shape](benchmark::State& state) {
        BM_NumericRangePointsSize(state, shape);
      })->Iterations(1);
}

[[maybe_unused]] bool benchmarksRegistered = [] {
  for (int32_t field = 0; field < (int32_t)FIELDS.size(); field++) {
    FieldShape shape = (FieldShape)field;
    registerTimingBenchmark(shape, POINTS_FORCED);
    registerTimingBenchmark(shape, COMPLEMENT_FORCED);
    registerTimingBenchmark(shape, ZONE_FORCED);
    registerTimingBenchmark(shape, FULLSCAN_FORCED);
    registerTimingBenchmark(shape, SELECTED);
    registerSizeBenchmark(shape);
  }
  return true;
}();

} // namespace
