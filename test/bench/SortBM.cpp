// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "bench/luxir_bench.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/QueryBuild.h"
#include "luxir/reader/SkipStats.h"
#include "luxir/search/SortField.h"

using namespace luxir;
using namespace luxir::test;

static void BM_StringSort(benchmark::State& state, int64_t nDocs,
                          std::string_view shape, qb::SortDir direction,
                          StringSortMode mode) {
  class ModeGuard {
    StringSortMode saved;
  public:
    explicit ModeGuard(StringSortMode mode)
        : saved(SortField::setStringSortModeForTests(mode)) {}
    ~ModeGuard() { SortField::setStringSortModeForTests(saved); }
  } guard(mode);

  if (luxir::unit_tests) nDocs = LuxirTest::scaleTestWork(200);
  std::vector<int32_t> docsPerSeg;
  CollectionHelper::calcSegSizes(nDocs, 10, shape, docsPerSeg);

  CollectionHelper helper("string_sort_bm");
  bool reused = helper.indexMatchesShape(docsPerSeg);
  if (!reused) buildBenchIndex(helper, nDocs, docsPerSeg);

  int64_t fingerprint = -1;
  for (auto _ : state) {
    auto req = localReq(LuxirTest::luxirNode->getSearchEngine());
    req->collection("string_sort_bm");
    auto& top = req->topDocs("q").getNumber(true).allQuery().limit(100)
        .fields({"id", "short_u10k_s"});
    qb::sort(top, "short_u10k_s", direction);
    req->execute(false);
    const auto* docs = req->docList("q");
    int64_t current = docs->found.value_or(0);
    for (const auto& batch : req->responses) {
      const auto* list = batch->proto.ops.at("q")->docList();
      const auto& ids = std::get<api::ColStr>(list->columns.at("id").kind).v;
      const auto& values =
          std::get<api::ColStr>(list->columns.at("short_u10k_s").kind).v;
      for (size_t i = 0; i < ids.size(); i++) {
        current = current * 31 + java_string_hashcode(ids[i]);
        current = current * 31 + java_string_hashcode(values[i]);
      }
    }
    benchmark::DoNotOptimize(current);
    if (fingerprint != -1) {
      ASSERT_EQ(fingerprint, current);
    }
    fingerprint = current;
  }
  state.counters["fp"] = fingerprint;
  state.counters["reused"] = reused;
  state.counters["rate"] = benchmark::Counter(
      state.iterations(), benchmark::Counter::kIsRate);
}

static constexpr int64_t STRING_SORT_DOCS = 10'000'000;
static constexpr const char* STRING_SORT_SHAPE = "9555";

// One instrumented pass's pruning-attribution SkipStats, exposed as counters:
// competitiveRanges vs irreducibleBlocks is the reordering headroom,
// lastAdmission/docsGathered is threshold maturity.
static void recordFieldSortCounters(benchmark::State& state) {
  state.counters["warmupRanges"] = (double)SkipStats::fieldSortWarmupRanges;
  state.counters["competitiveRanges"] =
      (double)SkipStats::fieldSortCompetitiveRanges;
  state.counters["blocksSkipped"] = (double)SkipStats::fieldSortBlocksSkipped;
  state.counters["leavesSkipped"] = (double)SkipStats::fieldSortLeavesSkipped;
  state.counters["docsGathered"] = (double)SkipStats::fieldSortDocsGathered;
  state.counters["lastAdmission"] =
      (double)SkipStats::fieldSortGatherAtLastAdmission;
  state.counters["irreducibleBlocks"] =
      (double)SkipStats::fieldSortIrreducibleBlocks;
  state.counters["bulkCollections"] =
      (double)SkipStats::fieldSortBulkCollections;
  state.counters["requiredBlocks"] =
      (double)SkipStats::fieldSortRequiredBlocks;
  state.counters["irreducibleLeaves"] =
      (double)SkipStats::fieldSortIrreducibleLeaves;
  state.counters["requiredLeaves"] =
      (double)SkipStats::fieldSortRequiredLeaves;
  state.counters["bfActivations"] =
      (double)SkipStats::fieldSortBestFirstActivations;
  state.counters["bfExpansions"] =
      (double)SkipStats::fieldSortBestFirstExpansions;
  state.counters["bfLeaves"] = (double)SkipStats::fieldSortBestFirstLeaves;
  state.counters["bfTerminations"] =
      (double)SkipStats::fieldSortBestFirstTerminations;
  state.counters["bfFallbacks"] =
      (double)SkipStats::fieldSortBestFirstFallbacks;
  state.counters["seedActivations"] =
      (double)SkipStats::fieldSortSeededActivations;
  state.counters["seedLeaves"] = (double)SkipStats::fieldSortSeedLeaves;
  state.counters["seedClassifiedOut"] =
      (double)SkipStats::fieldSortSeedClassifiedOut;
  state.counters["seedFillAborts"] =
      (double)SkipStats::fieldSortSeedFillAborts;
  state.counters["seedPass2Skips"] =
      (double)SkipStats::fieldSortSeedPass2Skips;
}

// Near-unique int sort under a cached filter: the serverbench sort10
// red-cell shape. filterBelow selects u10k_i < filterBelow (0 = unfiltered;
// 1000 ~= 10% density, 100 ~= 1%, 10 ~= 0.1%).
static void BM_IntSortFiltered(benchmark::State& state, int64_t nDocs,
                               int64_t filterBelow, bool singleSeg = false) {
  if (luxir::unit_tests) nDocs = LuxirTest::scaleTestWork(200);
  std::vector<int32_t> docsPerSeg;
  if (singleSeg) {
    // The serverbench sort-grid posture: one segment, so the irreducible
    // floor is measured against the true final bottom rather than summed
    // per segment against a maturing one.
    docsPerSeg.push_back((int32_t)nDocs);
  } else {
    CollectionHelper::calcSegSizes(nDocs, 10, STRING_SORT_SHAPE, docsPerSeg);
  }

  const char* collection = singleSeg ? "int_sort_bm" : "string_sort_bm";
  CollectionHelper helper(collection);
  bool reused = helper.indexMatchesShape(docsPerSeg);
  if (!reused) buildBenchIndex(helper, nDocs, docsPerSeg);

  auto execute = [&]() -> int64_t {
    auto req = localReq(LuxirTest::luxirNode->getSearchEngine());
    req->collection(collection);
    auto& top = req->topDocs("q").allQuery().limit(10).fields({"id"});
    qb::sort(top, "u10m_i", qb::ASC);
    if (filterBelow > 0) {
      top.filter(qb::range(top.mr(), "u10k_i", nullptr, nullptr, nullptr,
                           qb::valI64(top.mr(), filterBelow)));
    }
    req->execute(false);
    int64_t current = 0;
    for (const auto& batch : req->responses) {
      const auto* list = batch->proto.ops.at("q")->docList();
      const auto& ids = std::get<api::ColStr>(list->columns.at("id").kind).v;
      for (const auto& id : ids) {
        current = current * 31 + java_string_hashcode(id);
      }
    }
    return current;
  };

  int64_t fingerprint = execute();  // warms the filter cache too

  {
    SkipStatsScope stats;
    ASSERT_EQ(fingerprint, execute());
    recordFieldSortCounters(state);
  }

  for (auto _ : state) {
    int64_t current = execute();
    benchmark::DoNotOptimize(current);
    ASSERT_EQ(fingerprint, current);
  }
  state.counters["fp"] = fingerprint;
  state.counters["reused"] = reused;
  state.counters["rate"] = benchmark::Counter(
      state.iterations(), benchmark::Counter::kIsRate);
}

LUXIR_BENCHMARK_CAPTURE(BM_StringSort, global_asc, STRING_SORT_DOCS,
                        STRING_SORT_SHAPE, qb::ASC, StringSortMode::GLOBAL);
LUXIR_BENCHMARK_CAPTURE(BM_StringSort, global_desc, STRING_SORT_DOCS,
                        STRING_SORT_SHAPE, qb::DESC, StringSortMode::GLOBAL);
LUXIR_BENCHMARK_CAPTURE(BM_StringSort, segment_asc, STRING_SORT_DOCS,
                        STRING_SORT_SHAPE, qb::ASC, StringSortMode::SEGMENT);
LUXIR_BENCHMARK_CAPTURE(BM_StringSort, segment_desc, STRING_SORT_DOCS,
                        STRING_SORT_SHAPE, qb::DESC, StringSortMode::SEGMENT);
// Near-unique int sort where the selectivity predicate IS the main query
// (a term match, uncached): the GRID_FILTER_MODE=query sort10 posture. The
// term postings are the domain - no materialized set exists, so the exact-
// domain best-first route cannot fire and collection rides the pruned
// doc-order window walk. queryField "short_u10_s" ~= 10% density per term,
// "short_u100_s" ~= 1%, "short_u10k_s" ~= 0.01% (sparse control).
static void BM_IntSortQuery(benchmark::State& state, int64_t nDocs,
                            const char* queryField, bool singleSeg = false) {
  if (luxir::unit_tests) nDocs = LuxirTest::scaleTestWork(200);
  std::vector<int32_t> docsPerSeg;
  if (singleSeg) {
    docsPerSeg.push_back((int32_t)nDocs);
  } else {
    CollectionHelper::calcSegSizes(nDocs, 10, STRING_SORT_SHAPE, docsPerSeg);
  }

  const char* collection = singleSeg ? "int_sort_bm" : "string_sort_bm";
  CollectionHelper helper(collection);
  bool reused = helper.indexMatchesShape(docsPerSeg);
  if (!reused) buildBenchIndex(helper, nDocs, docsPerSeg);

  auto execute = [&]() -> int64_t {
    auto req = localReq(LuxirTest::luxirNode->getSearchEngine());
    req->collection(collection);
    auto& top = req->topDocs("q").limit(10).fields({"id"})
        .matchQuery(queryField, "3");
    qb::sort(top, "u10m_i", qb::ASC);
    req->execute(false);
    int64_t current = 0;
    for (const auto& batch : req->responses) {
      const auto* list = batch->proto.ops.at("q")->docList();
      // The unit-test corpus is small enough for a sparse term to match
      // nothing at all.
      if (list == nullptr) continue;
      const auto* column = list->columns.find("id");
      if (column == nullptr) continue;
      for (const auto& id : std::get<api::ColStr>(column->kind).v) {
        current = current * 31 + java_string_hashcode(id);
      }
    }
    return current;
  };

  int64_t fingerprint = execute();

  {
    SkipStatsScope stats;
    ASSERT_EQ(fingerprint, execute());
    recordFieldSortCounters(state);
  }

  for (auto _ : state) {
    int64_t current = execute();
    benchmark::DoNotOptimize(current);
    ASSERT_EQ(fingerprint, current);
  }
  state.counters["fp"] = fingerprint;
  state.counters["reused"] = reused;
  state.counters["rate"] = benchmark::Counter(
      state.iterations(), benchmark::Counter::kIsRate);
}

LUXIR_BENCHMARK_CAPTURE(BM_IntSortFiltered, all, STRING_SORT_DOCS, 0);
LUXIR_BENCHMARK_CAPTURE(BM_IntSortFiltered, f10pct, STRING_SORT_DOCS, 1000);
LUXIR_BENCHMARK_CAPTURE(BM_IntSortFiltered, f1pct, STRING_SORT_DOCS, 100);
LUXIR_BENCHMARK_CAPTURE(BM_IntSortFiltered, f0p1pct, STRING_SORT_DOCS, 10);
static constexpr int64_t INT_SORT_SS_DOCS = 5'000'000;
LUXIR_BENCHMARK_CAPTURE(BM_IntSortFiltered, ss_all, INT_SORT_SS_DOCS, 0, true);
LUXIR_BENCHMARK_CAPTURE(BM_IntSortFiltered, ss_f10pct, INT_SORT_SS_DOCS, 1000, true);
LUXIR_BENCHMARK_CAPTURE(BM_IntSortFiltered, ss_f1pct, INT_SORT_SS_DOCS, 100, true);
LUXIR_BENCHMARK_CAPTURE(BM_IntSortFiltered, ss_f0p1pct, INT_SORT_SS_DOCS, 10, true);
LUXIR_BENCHMARK_CAPTURE(BM_IntSortQuery, q10pct, STRING_SORT_DOCS, "short_u10_s");
LUXIR_BENCHMARK_CAPTURE(BM_IntSortQuery, ss_q10pct, INT_SORT_SS_DOCS,
                        "short_u10_s", true);
LUXIR_BENCHMARK_CAPTURE(BM_IntSortQuery, ss_q1pct, INT_SORT_SS_DOCS,
                        "short_u100_s", true);
LUXIR_BENCHMARK_CAPTURE(BM_IntSortQuery, ss_q0p1pct, INT_SORT_SS_DOCS,
                        "short_u1000_s", true);
LUXIR_BENCHMARK_CAPTURE(BM_IntSortQuery, ss_q0p01pct, INT_SORT_SS_DOCS,
                        "short_u10k_s", true);
