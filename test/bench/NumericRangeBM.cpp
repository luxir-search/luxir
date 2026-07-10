#include <benchmark/benchmark.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <memory_resource>
#include <numeric>
#include <string_view>
#include <vector>

#include "bench/solux_bench.h"
#include "solux/api/build.h"
#include "solux/query/NumericRangeQuery.h"
#include "solux/schema/Schema.h"
#include "solux/util/random.h"
#include "test/TestIndex.h"

using namespace solux;
using namespace solux::test;

namespace {

enum BenchPath : int32_t {
  SELECTED,
  ZONE,
  FULL
};

enum BenchArm : int32_t {
  VERIFY,
  POINTS,
  ZONE_ARM,
  COMPLEMENT
};

class NumericRangeBenchIndex {
public:
  TestIndex index;
  std::shared_ptr<Schema> schema;
  int32_t numDocs;

  NumericRangeBenchIndex() : numDocs(solux::unit_tests ? 50'000 : 500'000) {
    std::pmr::monotonic_buffer_resource arena;
    api::SchemaDef def;
    api::FieldDef* fields = api::build::allocArray(def.fields, 2, arena);
    for (auto [slot, name] : {
           std::pair<int32_t, std::string_view>{0, "range_correlated_i"},
           {1, "range_shuffled_i"}}) {
      fields[slot].name = name;
      fields[slot].field_class = api::FieldDef::FieldClass::INT;
      fields[slot].index = api::FieldDef::IndexMode::RANGE;
    }
    auto base = Schema::createDefaultSchema();
    schema = Schema::fromProto(def, base.get());
    index.iw = std::make_unique<IndexWriter>(
        index.dir, [this]() { return schema; });

    std::vector<int32_t> shuffled((size_t)numDocs);
    std::iota(shuffled.begin(), shuffled.end(), 0);
    SplitMix64 rng(0x76a5b31d);
    for (int32_t i = numDocs - 1; i > 0; i--) {
      std::swap(shuffled[(size_t)i], shuffled[(size_t)rng.rint(i + 1)]);
    }

    Inverter& inverter = index.getInverter();
    auto& correlated = inverter.getIndexHandler("range_correlated_i");
    auto& random = inverter.getIndexHandler("range_shuffled_i");
    for (int32_t doc = 0; doc < numDocs; doc++) {
      inverter.startDoc();
      correlated.index(inverter, 7 + (int64_t)doc * 2);
      random.index(inverter, 7 + (int64_t)shuffled[(size_t)doc] * 2);
      inverter.finishDoc();
    }
    index.flush();
    index.initReader();
  }
};

NumericRangeBenchIndex& benchIndex() {
  static NumericRangeBenchIndex fixture;
  return fixture;
}

void BM_NumericRangePoints(benchmark::State& state, bool correlated,
                           int32_t path, int32_t arm, int32_t perMille) {
  auto& fixture = benchIndex();
  std::string_view field = correlated
      ? "range_correlated_i" : "range_shuffled_i";
  int32_t expected = std::max<int32_t>(1,
      (int32_t)((int64_t)fixture.numDocs * perMille / 1000));
  int64_t lo = 7;
  int64_t hi = 7 + (int64_t)(expected - 1) * 2;

  for (auto _ : state) {
    MemPool pool;
    Query::Context context(pool, *fixture.index.reader);
    NumericRangeQuery query(field, lo, hi);
    auto* weight = static_cast<NumericRangeQuery::Weight*>(
        query.createWeight(context, 0));
    auto& segment = fixture.index.reader->segments()[0];
    Query::Scorer* scorer = nullptr;
    if (path == FULL) {
      scorer = weight->createFullScanScorerForTests(pool, segment);
    } else if (path == ZONE) {
      scorer = weight->createZoneMapScorerForTests(pool, segment);
    } else {
      auto* supplier = weight->scorerSupplier(pool, segment);
      if (supplier != nullptr) {
        int64_t leadCost = arm == VERIFY
            ? 0 : std::numeric_limits<int64_t>::max();
        scorer = supplier->get(pool, leadCost);
      }
    }
    int32_t count = 0;
    if (scorer != nullptr) {
      while (scorer->next() != PostingsReader::END) count++;
    }
    benchmark::DoNotOptimize(count);
    if (count != expected) {
      state.SkipWithError("numeric range benchmark match count mismatch");
      break;
    }
  }
  state.counters["docs"] = fixture.numDocs;
  state.counters["matches"] = expected;
}

} // namespace

#define RANGE_BENCH_CASE(shape, correlated, arm, per_mille)                    \
  SOLUX_BENCHMARK_CAPTURE(BM_NumericRangePoints, shape##_##arm##_selected,     \
                          correlated, SELECTED, arm, per_mille);               \
  SOLUX_BENCHMARK_CAPTURE(BM_NumericRangePoints, shape##_##arm##_zone,         \
                          correlated, ZONE, arm, per_mille);                   \
  SOLUX_BENCHMARK_CAPTURE(BM_NumericRangePoints, shape##_##arm##_full,         \
                          correlated, FULL, arm, per_mille)

RANGE_BENCH_CASE(correlated, true, VERIFY, 1);
RANGE_BENCH_CASE(correlated, true, POINTS, 50);
RANGE_BENCH_CASE(correlated, true, ZONE_ARM, 330);
RANGE_BENCH_CASE(correlated, true, COMPLEMENT, 750);
RANGE_BENCH_CASE(shuffled, false, VERIFY, 1);
RANGE_BENCH_CASE(shuffled, false, POINTS, 50);
RANGE_BENCH_CASE(shuffled, false, ZONE_ARM, 330);
RANGE_BENCH_CASE(shuffled, false, COMPLEMENT, 750);

#undef RANGE_BENCH_CASE
