// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#include "bench/luxir_bench.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/TestUtils.h"
#include "luxir/reader/SkipStats.h"
#include "luxir/search/FilterCache.h"
#include "luxir/search/SearchOverrides.h"

using namespace luxir;
using namespace luxir::test;

// Multi-term-heavy root ranked lists against the estimated-count pass that
// an unset count_limit adds. Needs a real corpus: LUXIR_ESTIMATE_BENCH_DATA
// names a TSV of passage, url path, client ip, short title (one doc per
// line; the first 1M lines are indexed as five segments).
//
// Args: query, count mode (0 unset, 1 count_limit -1, 2 count_limit 1000),
// option (0 estimate, 1 no estimate). Each timed request starts with an
// empty filter cache, as a query's first sighting does: a cached membership
// answers an exact count without touching the dictionary. Counters: terms is
// what the request's MultiTermBudget was charged, fills the multi-term
// dictionary scans run (one per segment and query when they are shared).
// Fuzzy scans are not fills; terms covers them.

namespace {

constexpr int32_t kSegments = 5;
constexpr int32_t kDocsPerSegment = 200'000;

const char* kQueries[] = {
  "body_t:ca*",                                        // short prefix
  "body_t:pre*",                                       // prefix
  "wildcard(*ology, field=body_t)",                    // leading wildcard
  "wildcard(*mbo*, field=body_t)",                     // infix, some hits
  "wildcard(*qzx*, field=body_t)",                     // infix, zero hits
  "regex(c[aeiou]t.*, field=body_t)",                  // regex
  "regex(.*phon.*, field=body_t)",                     // open regex
  "body_t:pyramd~2",                                   // fuzzy
  "body_t:[ca TO cb]",                                 // term range
  "title_s:th*",                                       // high-card string
  "wildcard(*water*, field=title_s)",                  // string infix
  "+body_t:water +body_t:ca*",                         // mix: AND
  "body_t:water body_t:wat* body_t:pyramd~2",          // mix: OR
  "+body_t:energy wildcard(*ology, field=body_t)",     // mix: term + scan
  "ip_s:85.1* url_s:*",                                // low-card string
};
constexpr int32_t kNumQueries = (int32_t) std::size(kQueries);

bool loadCorpus(CollectionHelper& helper) {
  std::vector<int32_t> shape(kSegments, kDocsPerSegment);
  if (helper.indexMatchesShape(shape)) return true;
  const char* path = std::getenv("LUXIR_ESTIMATE_BENCH_DATA");
  if (path == nullptr) return false;
  std::ifstream in(path);
  if (!in) return false;
  helper.clear();
  std::string line;
  for (int32_t seg = 0; seg < kSegments; seg++) {
    std::vector<Doc> docs;
    docs.reserve(kDocsPerSegment);
    for (int32_t i = 0; i < kDocsPerSegment && std::getline(in, line); i++) {
      std::vector<std::string> cols;
      size_t start = 0;
      for (int c = 0; c < 4; c++) {
        size_t tab = line.find('\t', start);
        cols.push_back(line.substr(start, tab == std::string::npos ? tab : tab - start));
        start = tab == std::string::npos ? line.size() : tab + 1;
      }
      docs.push_back(flatdoc("id", std::to_string(seg * kDocsPerSegment + i),
                             "body_t", cols[0], "url_s", cols[1], "ip_s", cols[2],
                             "title_s", cols[3]));
    }
    if (!helper.indexAll(docs, UpdateMessage::COMMIT).success) return false;
  }
  return helper.indexMatchesShape(shape);
}

void applyOption(int64_t option) {
  disableEstimatedCount = option == 1;
}

struct RunResult {
  bool ok = false;
  std::string error;
  int64_t found = 0;
  bool estimated = false;
  int64_t termsExamined = 0;
};

void clearFilterCache(CollectionHelper& helper) {
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  if (auto* cache = reader->filterCache()) {
    cache->clear();
    cache->resetAdmission();
  }
}

RunResult runOnce(CollectionHelper& helper, int64_t query, int64_t countMode) {
  auto req = localReq(helper.getSearchEngine());
  auto& op = req->collection("estimate_bm").topDocs("q")
      .exprQuery(kQueries[query]).limit(10);
  if (countMode == 1) op.countLimit(-1);
  if (countMode == 2) op.countLimit(1000);
  req->execute(false);
  RunResult result;
  result.ok = req->ok();
  if (!result.ok) {
    result.error = req->errorCode();
    return result;
  }
  const auto* dl = req->docList("q");
  result.found = dl && dl->found ? *dl->found : -1;
  result.estimated = dl && dl->found_estimated;
  result.termsExamined = req->multiTermBudget.termsExamined();
  return result;
}

void BM_EstimateBudget(benchmark::State& state) {
  if (unit_tests) {
    state.SkipWithMessage("needs a real corpus");
    return;
  }
  CollectionHelper helper("estimate_bm");
  if (skipBenchIfDataMissing(state, loadCorpus(helper), "LUXIR_ESTIMATE_BENCH_DATA")) return;
  int64_t query = state.range(0);
  int64_t countMode = state.range(1);
  int64_t option = state.range(2);
  bool savedDisable = disableEstimatedCount;
  auto restore = scope_guard([&]() { disableEstimatedCount = savedDisable; });
  applyOption(option);

  // Untimed: dictionary fills, the exact count, and a warm-up.
  RunResult exact;
  {
    bool saved = disableEstimatedCount;
    disableEstimatedCount = true;
    clearFilterCache(helper);
    exact = runOnce(helper, query, 1);
    disableEstimatedCount = saved;
  }
  int64_t fills = 0;
  {
    clearFilterCache(helper);
    SkipStatsScope scope;
    RunResult probe = runOnce(helper, query, countMode);
    fills = SkipStats::multitermExpansions;
    if (!probe.ok) {
      state.SkipWithError("request failed: " + probe.error);
      return;
    }
  }
  RunResult last;
  for (auto _ : state) {
    state.PauseTiming();
    clearFilterCache(helper);
    state.ResumeTiming();
    last = runOnce(helper, query, countMode);
    benchmark::DoNotOptimize(last.found);
  }
  state.SetLabel(kQueries[query]);
  state.counters["found"] = (double) last.found;
  state.counters["exact"] = (double) exact.found;
  state.counters["est"] = last.estimated ? 1 : 0;
  state.counters["terms"] = (double) last.termsExamined;
  state.counters["fills"] = (double) fills;
}

} // namespace

TUNING_BENCHMARK(BM_EstimateBudget)
    ->ArgsProduct({benchmark::CreateDenseRange(0, kNumQueries - 1, 1),
                   {0, 1, 2}, {0, 1}})
    ->Iterations(3)
    ->Unit(benchmark::kMillisecond)
    ->UseRealTime();
