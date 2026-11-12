// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// Selected and forced arms share the planner and COUNT_WINDOWS execution.
// Ingest rows rebuild one field/mode at a time.
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <numeric>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "bench/luxir_bench.h"
#include "luxir/query/BooleanWeight.h"
#include "luxir/query/NumericPredicateQuery.h"
#include "luxir/schema/ValCoerce.h"
#include "luxir/server/LuxirNode.h"
#include "luxir/util/random.h"
#include "test/CollectionHelper.h"
#include "test/DurableIndexInfo.h"
#include "test/SchemaBuilder.h"

using namespace luxir;
using namespace luxir::test;

namespace {

constexpr std::array<const char*, 3> MODES{"none", "range", "match"};
constexpr std::array<const char*, 3> LEADS{"alone", "l1", "l10"};
constexpr int64_t EPOCH_MS = 1'700'000'000'000;

struct FieldSpec {
  bool date;
  bool shuffled;
  int32_t density;

  std::string name() const {
    return fmt::format("{}/{}{}", date ? "date" : "bool",
        shuffled ? "shuffled" : date ? "near" : "clustered",
        date ? "" : fmt::format("/d{}", density));
  }
};

template <class T>
void shuffle(std::span<T> values, SplitMix64& rng) {
  for (size_t i = values.size(); i > 1; i--) {
    std::swap(values[i - 1], values[(size_t)rng.rint((int64_t)i)]);
  }
}

const std::vector<uint8_t>& leadFlags() {
  static const auto flags = [] {
    int32_t size = unit_tests ? 8192 : 1 << 20;
    std::vector<uint8_t> result((size_t)size);
    std::vector<int32_t> order((size_t)size);
    std::iota(order.begin(), order.end(), 0);
    SplitMix64 rng(0x1ead);
    for (int lead = 1; lead <= 2; lead++) {
      shuffle<int32_t>(order, rng);
      int32_t count = size / (lead == 1 ? 100 : 10);
      for (int32_t i = 0; i < count; i++) result[(size_t)order[(size_t)i]] |= lead;
    }
    return result;
  }();
  return flags;
}

struct Corpus {
  std::vector<int64_t> values;
  int64_t equality;

  explicit Corpus(FieldSpec spec) : values(leadFlags().size()) {
    int32_t size = (int32_t)values.size();
    if (spec.date) {
      std::iota(values.begin(), values.end(), EPOCH_MS);
    } else {
      std::fill_n(values.begin(), (size_t)((int64_t)size * spec.density / 100), 1);
    }
    SplitMix64 rng(0x5eed + (uint64_t)spec.density);
    if (spec.shuffled) {
      shuffle<int64_t>(values, rng);
    } else if (spec.date) {
      // Unique millisecond timestamps, displaced by at most 255 ingest slots.
      for (size_t begin = 0; begin < values.size(); begin += 256) {
        shuffle<int64_t>(std::span(values).subspan(
            begin, std::min((size_t)256, values.size() - begin)), rng);
      }
    } else {
      // Shuffle whole runs, retaining exact density and long constant zones.
      // Small runs still span a column leaf, so zone forcing is available.
      int32_t run = unit_tests ? 512 : 4096;
      std::vector<int32_t> order((size_t)(size / run));
      std::iota(order.begin(), order.end(), 0);
      shuffle<int32_t>(order, rng);
      auto original = values;
      for (size_t i = 0; i < order.size(); i++) {
        std::copy_n(original.begin() + order[i] * run, run, values.begin() + i * run);
      }
    }
    // Exercise a nonempty equality under both leads even on the small corpus.
    auto both = std::find(leadFlags().begin(), leadFlags().end(), 3);
    if (both == leadFlags().end()) throw std::logic_error("numeric bench leads do not overlap");
    equality = spec.date ? values[(size_t)(both - leadFlags().begin())] : 1;
  }
};

const Corpus& corpus(FieldSpec spec) {
  static std::map<std::string, std::unique_ptr<Corpus>> corpora;
  auto& slot = corpora[spec.name()];
  if (!slot) slot = std::make_unique<Corpus>(spec);
  return *slot;
}

struct NumericIndex {
  std::unique_ptr<LuxirNode> node;
  std::unique_ptr<CollectionHelper> helper;
  std::shared_ptr<IndexReader> reader;
  const Corpus& data;
  bool date;

  NumericIndex(FieldSpec spec, int mode) : data(corpus(spec)), date(spec.date) {
    LuxirConfig config;
    config.store.backend = "ram";
    node = std::make_unique<LuxirNode>(config);
    helper = std::make_unique<CollectionHelper>(*node, "main");
    SchemaBuilder schema;
    auto& field = schema.field("value");
    field.type = date ? api::FieldDef::FieldClass::DATE : api::FieldDef::FieldClass::BOOLEAN;
    field.index = mode == 0 ? api::FieldDef::IndexMode::NONE
        : mode == 1 ? api::FieldDef::IndexMode::RANGE : api::FieldDef::IndexMode::MATCH;
    field.column = true;
    field.multi = api::FieldDef::MultiMode::SINGLE;
    if (date) field.unit = api::FieldDef::DateUnit::MS;
    schema.set(helper->collection());
    auto writer = helper->getIndexWriter();
    writer->mergePolicy->setMergeFactor(1000);
    writer->snapshots.readers.filterCache = std::make_shared<FilterCache>(
        FilterCacheConfig{.maxBytes = 0});
  }

  ~NumericIndex() {
    // The reader and helper must die before their own node, including at exit.
    reader.reset();
    helper.reset();
    node.reset();
  }

  double ingest() {
    static constexpr std::array<const char*, 4> TEXT{"all", "all l1", "all l10", "all l1 l10"};
    auto writer = helper->getIndexWriter();
    auto start = std::chrono::steady_clock::now();
    auto& inverter = writer->obtainInverter();
    auto& value = inverter.getIndexHandler("value");
    auto& body = inverter.getIndexHandler("body_w");
    for (size_t doc = 0; doc < data.values.size(); doc++) {
      inverter.startDoc();
      if (date) value.index(inverter, data.values[doc]);
      else value.index(inverter, coerce::scalarVal(data.values[doc] != 0));
      body.index(inverter, TEXT[leadFlags()[doc]]);
      inverter.finishDoc();
    }
    writer->releaseInverter(inverter, true);
    helper->commit();
    double seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();
    reader = std::make_shared<IndexReader>(writer->dir);
    if (reader->segments().size() != 1
        || reader->segments()[0].maxDoc() != (int32_t)data.values.size()) {
      throw std::logic_error("numeric bench requires one complete segment");
    }
    return seconds;
  }

  uint64_t bytes() const {
    // Count the committed inventory, independent of obsolete-file cleanup.
    auto root = Manifest::load(helper->getIndexWriter()->dir);
    std::pmr::monotonic_buffer_resource arena;
    auto info = Manifest::decode(root.bytes, arena);
    uint64_t total = root.bytes->size();
    for (const auto& file : manifestFiles(info)) total += file.size;
    return total;
  }
};

NumericIndex& indexFor(FieldSpec spec, int mode) {
  static std::map<std::string, std::unique_ptr<NumericIndex>> indexes;
  auto& slot = indexes[spec.name() + "/" + MODES[(size_t)mode]];
  if (!slot) {
    slot = std::make_unique<NumericIndex>(spec, mode);
    slot->ingest();
  }
  return *slot;
}

struct Predicate {
  int64_t lo;
  int64_t hi;
};

Predicate predicateFor(const Corpus& data, bool date, int perMille) {
  if (!date || perMille == 0) return {data.equality, data.equality};
  int64_t count = std::max<int64_t>(1, (int64_t)data.values.size() * perMille / 1000);
  int64_t lo = EPOCH_MS + ((int64_t)data.values.size() - count) / 2;
  return {lo, lo + count - 1};
}

int64_t expectedCount(const Corpus& data, Predicate predicate, int lead) {
  int64_t count = 0;
  for (size_t doc = 0; doc < data.values.size(); doc++) {
    if ((lead == 0 || (leadFlags()[doc] & lead) != 0)
        && predicate.lo <= data.values[doc] && data.values[doc] <= predicate.hi) count++;
  }
  return count;
}

using Arm = NumericPredicateQuery::ScorerArm;

int64_t run(NumericIndex& index, Predicate bounds, int lead, std::optional<Arm> arm) {
  MemPool pool;
  Query::Context context(pool, *index.reader);
  NumericPredicateQuery predicate("value", bounds.lo, bounds.hi);
  predicate.forcedArm = arm;
  TermQuery term("body_w", LEADS[(size_t)lead]);
  std::array<Query*, 1> required{&term}, filters{&predicate};
  BooleanQuery filtered(required, {}, {}, filters);
  Query& query = lead == 0 ? (Query&)predicate : (Query&)filtered;
  auto& segment = index.reader->segments()[0];
  int64_t count = 0;
  auto* weight = query.createWeight(context, 0);
  auto* supplier = weight->scorerSupplier(pool, segment);
  if (!supplier) return 0;
  auto plan = supplier->planBulk(Query::ExecutionUse::COUNT_WINDOWS, {});
  if (plan.available == Query::ScorerSupplier::BulkAnswer::YES) {
    auto* bulk = supplier->buildBulk(pool, plan);
    for (int32_t cursor = 0; cursor < segment.maxDoc();) {
      cursor = bulk->countNextWindow(count, nullptr, nullptr, cursor, segment.maxDoc());
    }
    return count;
  }
  auto* scorer = supplier->resolve(pool, supplier->makePlanContext(
      Query::Demand::fromLeadCost(INT64_MAX)))->build(pool);
  while (scorer && scorer->next() != PostingsReader::END) count++;
  return count;
}

void queryBench(benchmark::State& state, FieldSpec spec, int perMille,
                int lead, int mode, std::optional<Arm> arm) {
  auto& index = indexFor(spec, mode);
  auto bounds = predicateFor(index.data, spec.date, perMille);
  int64_t expected = expectedCount(index.data, bounds, lead);
  if (run(index, bounds, lead, arm) != expected) {
    state.SkipWithError("numeric predicate differs from generated data");
    return;
  }
  for (auto _ : state) benchmark::DoNotOptimize(run(index, bounds, lead, arm));
  SkipStatsScope stats;
  if (run(index, bounds, lead, arm) != expected) {
    state.SkipWithError("numeric predicate differs after timed iterations");
    return;
  }
  state.counters["result"] = (double)expected;
  state.counters["docs"] = (double)index.data.values.size();
  struct Counter { Arm arm; const char* name; int64_t count; };
  const Counter counters[] = {
    {Arm::POINTS, "pts", SkipStats::numericPredicatePointsArms},
    {Arm::COMPLEMENT, "cmp", SkipStats::numericPredicateComplementArms},
    {Arm::POSTINGS, "post", SkipStats::numericPredicatePostingsArms},
    {Arm::SPARSE_VERIFY, "sv", SkipStats::numericPredicateSparseVerifyArms},
    {Arm::ZONE_MAP, "zone", SkipStats::numericPredicateZoneArms},
    {Arm::SCAN, "scan", SkipStats::numericPredicateScanArms},
  };
  for (const auto& counter : counters) {
    state.counters[counter.name] = (double)counter.count;
    if (arm && (counter.arm == *arm ? counter.count == 0 : counter.count != 0)) {
      state.SkipWithError("planner did not build the requested numeric arm");
    }
  }
}

void ingestBench(benchmark::State& state, FieldSpec spec, int mode) {
  const auto& data = corpus(spec); // Generation and node/schema setup are untimed.
  double seconds = 0;
  uint64_t bytes = 0;
  for (auto _ : state) {
    NumericIndex index(spec, mode);
    double elapsed = index.ingest();
    state.SetIterationTime(elapsed);
    seconds += elapsed;
    bytes = index.bytes();
  }
  state.counters["ns_doc"] = seconds * 1e9 / ((double)state.iterations() * data.values.size());
  state.counters["bytes_doc"] = (double)bytes / data.values.size();
  state.counters["docs"] = (double)data.values.size();
}

void registerQuery(FieldSpec spec, int perMille, int lead, int mode, std::optional<Arm> arm) {
  const char* armName = arm == Arm::POINTS ? "points" : arm == Arm::COMPLEMENT ? "complement"
      : arm == Arm::SPARSE_VERIFY ? (mode == 2 ? "verify_match" : "verify")
      : arm == Arm::ZONE_MAP ? "zone" : arm == Arm::SCAN ? "scan" : "postings";
  std::string shape = spec.name();
  if (spec.date) shape += perMille == 0 ? "/eq" : fmt::format("/r{}", perMille == 1 ? "01"
      : perMille == 10 ? "1" : perMille == 100 ? "10" : "50");
  auto name = fmt::format("BM_Numeric{}/{}/{}/{}", arm == std::nullopt ? "Mode" : "Forced",
      shape, LEADS[(size_t)lead], arm == std::nullopt ? MODES[(size_t)mode] : armName);
  benchmark::RegisterBenchmark(name.c_str(), [=](benchmark::State& state) {
    queryBench(state, spec, perMille, lead, mode, arm);
  })->UseRealTime()->Unit(benchmark::kMicrosecond);
}

[[maybe_unused]] bool registered = [] {
  // Register the format-work gate first, independently runnable by its name.
  for (Arm arm : {Arm::POINTS, Arm::SPARSE_VERIFY}) {
    registerQuery({false, true, 50}, 0, 1, 1, arm);
  }
  for (bool date : {false, true}) {
    for (bool shuffled : {true, false}) {
      for (int density : {1, 10, 50, 90}) {
        if (date && density != 1) continue;
        FieldSpec spec{date, shuffled, date ? 0 : density};
        for (int perMille : {0, 1, 10, 100, 500}) {
          if (!date && perMille != 0) continue;
          for (int lead = 0; lead < 3; lead++) {
            for (int mode = 0; mode < 3; mode++) {
              registerQuery(spec, perMille, lead, mode, std::nullopt);
            }
            for (Arm arm : {Arm::POINTS, Arm::SPARSE_VERIFY, Arm::POSTINGS, Arm::COMPLEMENT,
                            Arm::ZONE_MAP, Arm::SCAN}) {
              if (arm == Arm::POSTINGS && perMille != 0) continue;
              if (arm == Arm::ZONE_MAP && shuffled) continue;
              if (arm == Arm::COMPLEMENT && (date || density != 90)) continue;
              if (!date && shuffled && density == 50 && lead == 1
                  && (arm == Arm::POINTS || arm == Arm::SPARSE_VERIFY)) continue;
              registerQuery(spec, perMille, lead, arm == Arm::POSTINGS ? 2 : 1, arm);
            }
            if (perMille == 0) registerQuery(spec, perMille, lead, 2, Arm::SPARSE_VERIFY);
          }
        }
        for (int mode = 0; mode < 3; mode++) {
          auto name = fmt::format("BM_NumericIngest/{}/{}", spec.name(), MODES[(size_t)mode]);
          benchmark::RegisterBenchmark(name.c_str(), [=](benchmark::State& state) {
            ingestBench(state, spec, mode);
          })->UseManualTime()->Unit(benchmark::kMillisecond);
        }
      }
    }
  }
  return true;
}();

} // namespace
