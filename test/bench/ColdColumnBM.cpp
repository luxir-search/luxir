// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// Measurement-only: kNN latency when the vector column is partly or wholly
// out of the page cache.  The index lives on a real filesystem (fs store under
// /var/tmp), and before every iteration a fixed subset of
// the column's pages is evicted with MADV_PAGEOUT, which needs no root.

#include <array>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>
#include <vector>

#include "bench/luxir_bench.h"
#include "luxir/server/LuxirNode.h"
#include "luxir/index/VectorIndexBuilder.h"
#include "luxir/reader/RowReadAhead.h"
#include "luxir/reader/VectorReader.h"
#include "luxir/util/random.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/QueryBuild.h"
#include "test/SchemaBuilder.h"
#include "test/TestUtils.h"

#ifdef __linux__  // MADV_PAGEOUT and MADV_POPULATE_READ

using namespace luxir;
using namespace luxir::test;

namespace api = luxir::api;

namespace {

// The index directory (about 1 GB per 500K x 384-d) goes away with the process.
struct ColdIndex {
  std::filesystem::path dir;
  std::unique_ptr<LuxirNode> node;
  std::unique_ptr<CollectionHelper> helper;

  ~ColdIndex() {
    helper.reset();
    node.reset();
    std::filesystem::remove_all(dir);
  }
};

float randomComponent(SplitMix64& rng) {
  return ((float)((int32_t)rng.rint((int64_t)2001) - 1000)) / 1000.0f;
}

std::vector<float> randomVector(uint64_t seed, int32_t dims) {
  SplitMix64 rng(seed * 0x9e3779b97f4a7c15ULL + 1);
  std::vector<float> v((size_t)dims);
  for (auto& x : v) x = randomComponent(rng);
  return v;
}

// One index per (dims, nDocs, nSegs, deletes) per process, built with an IVF
// overlay (exact=true queries bypass it).  bucket_i = id % 1000 serves clustered
// filters (runs of consecutive docs) and spread_i a scattered hash of the id;
// deletes removes every 100th doc, so the scan's domain is the live docs.
ColdIndex& coldIndex(int32_t dims, int64_t nDocs, int32_t nSegs, bool deletes) {
  static std::mutex mutex;
  static std::map<std::string, std::unique_ptr<ColdIndex>> indexes;
  std::lock_guard lock(mutex);
  std::string key = "d" + std::to_string(dims) + "-n" + std::to_string(nDocs) + "-s" +
                    std::to_string(nSegs) + (deletes ? "-del" : "");
  auto& slot = indexes[key];
  if (slot) return *slot;
  slot = std::make_unique<ColdIndex>();
  slot->dir = std::filesystem::path("/var/tmp/luxir-coldbench") / key;
  std::filesystem::remove_all(slot->dir);
  LuxirConfig config;
  config.store.backend = "fs";
  config.store.data_dir = slot->dir.string();
  slot->node = std::make_unique<LuxirNode>(config);
  slot->helper = std::make_unique<CollectionHelper>(*slot->node, "main");
  auto& h = *slot->helper;
  h.getIndexWriter()->mergePolicy->setMergeFactor(1000);

  SchemaBuilder b;
  auto& f = b.templ("_v");
  f.type = api::FieldDef_::FieldClass::VECTOR;
  f.column = true;
  f.dims = dims;
  f.metric = api::VectorMetric::IP;
  f.ann = pqAnn();
  b.set(h.collection());
  std::vector<Doc> docs;
  int64_t id = 0;
  for (int32_t s = 0; s < nSegs; s++) {
    for (int64_t i = 0; i < nDocs / nSegs; i++, id++) {
      docs.push_back(flatdoc("id", "d" + std::to_string(id), "bucket_i", (int64_t)(id % 1000),
                             "spread_i", (int64_t)((uint64_t)id * 0x9e3779b97f4a7c15ULL >> 54),
                             "bench_v", randomVector((uint64_t)id, dims)));
      if (docs.size() == 512) { h.indexAll(docs); docs.clear(); }
    }
    if (!docs.empty()) { h.indexAll(docs); docs.clear(); }
    h.commit();
  }
  {
    int64_t saved = VectorIndexBuilder::ivfBuildThresholdScanCost;
    VectorIndexBuilder::ivfBuildThresholdScanCost = 0;
    h.commit({"vec.bench_v"});
    VectorIndexBuilder::ivfBuildThresholdScanCost = saved;
  }
  if (deletes) {
    std::vector<std::string> ids;
    for (int64_t d = 0; d < nDocs; d += 100) ids.push_back("d" + std::to_string(d));
    h.deleteByIds(ids);
    h.commit();
  }
  ::sync();  // writeback of the build would compete with the first cold runs
  return *slot;
}

// The page-aligned span of each segment's vector column.
std::vector<std::pair<char*, size_t>> columnSpans(ColdIndex& index) {
  std::vector<std::pair<char*, size_t>> spans;
  auto reader = index.helper->getIndexWriter()->snapshots.readers.getReader();
  size_t page = (size_t)sysconf(_SC_PAGESIZE);
  for (const auto& seg : reader->segments()) {
    auto* info = seg.postingsReader().fieldInfo("bench_v");
    if (info == nullptr) continue;
    VectorReader vr(seg.postingsReader(), *info);
    auto begin = (uintptr_t)vr.vectorAtRank(0).data();
    auto end = begin + (uintptr_t)vr.numVectors() * (uintptr_t)vr.dims() * sizeof(float);
    begin = (begin + page - 1) & ~(uintptr_t)(page - 1);
    end &= ~(uintptr_t)(page - 1);
    if (end > begin) spans.emplace_back((char*)begin, (size_t)(end - begin));
  }
  return spans;
}

// Faults every column page in (the hot runs follow cold ones on the same index).
void warm(const std::vector<std::pair<char*, size_t>>& spans) {
  size_t page = (size_t)sysconf(_SC_PAGESIZE);
  volatile char sink = 0;
  for (auto [base, len] : spans) {
    (void)madvise(base, len, MADV_WILLNEED);
    for (size_t at = 0; at < len; at += page) sink = sink + base[at];
  }
}

// Pages mincore reports resident, as runs of (first page, count).
std::vector<std::pair<size_t, size_t>> residentRuns(char* base, size_t pages) {
  std::vector<unsigned char> vec(pages);
  std::vector<std::pair<size_t, size_t>> runs;
  if (mincore(base, pages * (size_t)sysconf(_SC_PAGESIZE), vec.data()) != 0) return {{0, pages}};
  for (size_t p = 0; p < pages;) {
    if (!(vec[p] & 1)) { p++; continue; }
    size_t end = p + 1;
    while (end < pages && (vec[end] & 1)) end++;
    runs.emplace_back(p, end - p);
    p = end;
  }
  return runs;
}

// Evicts the pages whose hash falls under coldPermille; returns the fraction
// of column pages that mincore then reports non-resident.  PAGEOUT skips
// unmapped pages and large folios it cannot split, so resident runs are
// mapped first, and for a partial eviction the column is reloaded in 128 KiB
// advice chunks (small folios) before single pages go out.
double evict(const std::vector<std::pair<char*, size_t>>& spans, int64_t coldPermille) {
  size_t page = (size_t)sysconf(_SC_PAGESIZE);
  size_t total = 0, cold = 0;
  for (auto [base, len] : spans) {
    size_t pages = len / page;
    for (auto [first, n] : residentRuns(base, pages)) {
      (void)madvise(base + first * page, n * page, MADV_POPULATE_READ);
      (void)madvise(base + first * page, n * page, MADV_PAGEOUT);
    }
    // Large folios that straddle a run's ends stay; take the whole span.
    if (!residentRuns(base, pages).empty()) {
      (void)madvise(base, len, MADV_POPULATE_READ);
      (void)madvise(base, len, MADV_PAGEOUT);
    }
    if (coldPermille < 1000) {
      for (size_t at = 0; at < len; at += 128 * 1024) {
        (void)madvise(base + at, std::min<size_t>(128 * 1024, len - at), MADV_WILLNEED);
      }
      (void)madvise(base, len, MADV_POPULATE_READ);
      for (size_t p = 0; p < pages; p++) {
        uint64_t h = (p + 1) * 0x9e3779b97f4a7c15ULL;
        h ^= h >> 29;
        if ((int64_t)(h % 1000) < coldPermille) (void)madvise(base + p * page, page, MADV_PAGEOUT);
      }
    }
    std::vector<unsigned char> vec(pages);
    if (mincore(base, len, vec.data()) == 0) {
      for (auto v : vec) cold += (v & 1) == 0;
    }
    total += pages;
  }
  return total ? (double)cold / (double)total : 0.0;
}

int64_t readBytes() {
  std::ifstream io("/proc/self/io");
  std::string key;
  int64_t value = 0;
  while (io >> key >> value) {
    if (key == "read_bytes:") return value;
  }
  return 0;
}

long processMajorFaults() {
  struct rusage usage;
  getrusage(RUSAGE_SELF, &usage);
  return usage.ru_majflt;
}

// Args: dims, nDocs, nSegs, refine (0 = exact scan), coldPermille, readAhead
// (0/1), domain (0 = none, -1 = 1% deleted docs, k in 1..999 = a clustered
// filter on k per mille of the docs, 1000 + k = a scattered one).
void BM_VectorColdColumn(benchmark::State& state) {
  if (luxir::unit_tests) {
    state.SkipWithMessage("measurement-only");
    return;
  }
  int32_t dims = (int32_t)state.range(0);
  int64_t nDocs = state.range(1);
  int32_t nSegs = (int32_t)state.range(2);
  int32_t refine = (int32_t)state.range(3);
  int64_t coldPermille = state.range(4);
  bool readAhead = state.range(5) != 0;
  int64_t domain = state.range(6);
  bool exact = refine == 0;

  ColdIndex& index = coldIndex(dims, nDocs, nSegs, domain < 0);
  if (state.thread_index() == 0) RowReadAhead::enabled = readAhead;  // threads start together
  auto spans = columnSpans(index);
  std::vector<std::vector<float>> queries;
  for (int q = 0; q < 8; q++) queries.push_back(randomVector(1'000'000'000ULL + (uint64_t)q, dims));

  if (state.thread_index() == 0 && coldPermille == 0) warm(spans);
  double coldSum = 0;
  long faults = 0;
  int64_t bytes = 0;
  int64_t iteration = 0;
  for (auto _ : state) {
    if (state.thread_index() == 0 && coldPermille > 0) {
      state.PauseTiming();
      coldSum += evict(spans, coldPermille);
      state.ResumeTiming();
    }
    long faults0 = processMajorFaults();
    int64_t bytes0 = readBytes();
    auto* req = LocalReq::create(index.helper->getSearchEngine());
    req->collection("main").requestId("cold-bench");
    auto& cur = req->topDocs();
    cur.limit(10).getNumber().fields({"id"});
    auto knn = qb::knn(cur.mr(), "bench_v", queries[(size_t)(iteration++ % 8)], 10, 0, exact, refine);
    if (domain > 0) {
      auto filter = domain < 1000
          ? qb::range(cur.mr(), "bucket_i", nullptr, nullptr, nullptr, qb::valI64(cur.mr(), domain))
          : qb::range(cur.mr(), "spread_i", nullptr, nullptr, nullptr, qb::valI64(cur.mr(), (domain - 1000) * 1024 / 1000));
      cur.rawQuery() = qb::boolean(cur.mr(), {knn}, {}, {}, {filter});
    } else {
      cur.rawQuery() = knn;
    }
    req->execute(false);
    if (req->responses.empty() || req->responses[0]->proto.error) {
      state.SkipWithError("search failed");
      break;
    }
    faults += processMajorFaults() - faults0;
    bytes += readBytes() - bytes0;
  }
  if (state.iterations() > 0) {
    double n = (double)state.iterations();
    // Fault and byte counts are process-wide, so with threads each thread's
    // share overlaps the others'; averaging keeps the hot runs' zero visible.
    state.counters["coldPct"] = benchmark::Counter(100.0 * coldSum / n, benchmark::Counter::kAvgThreads);
    state.counters["majflt"] = benchmark::Counter((double)faults / n, benchmark::Counter::kAvgThreads);
    state.counters["readMB"] = benchmark::Counter((double)bytes / n / (1024.0 * 1024.0), benchmark::Counter::kAvgThreads);
    state.counters["segs"] = benchmark::Counter((double)spans.size(), benchmark::Counter::kAvgThreads);
  }
  if (state.thread_index() == 0) RowReadAhead::enabled = true;
}

// Each point with read-ahead off and on.
void coldArgs(benchmark::Benchmark* b, std::initializer_list<std::array<int64_t, 6>> points) {
  for (auto& p : points) {
    for (int64_t readAhead : {0, 1}) b->Args({p[0], p[1], p[2], p[3], p[4], readAhead, p[5]});
  }
}

}  // namespace

// {dims, docs, segs, refine, coldPm, domain}: exact scans at hot / 0.1% / 1%
// / 10% / fully cold, the terminal rescore of 1000 and 10000 IVF candidates,
// 16 segments of ~60 candidates each, and exact scans with a domain: deletes,
// and clustered and scattered filters.
TUNING_BENCHMARK(BM_VectorColdColumn)
    ->ArgNames({"dims", "docs", "segs", "refine", "coldPm", "ra", "domain"})
    ->Apply([](benchmark::Benchmark* b) {
      coldArgs(b, {{384, 500'000, 1, 0, 0, 0}, {384, 500'000, 1, 0, 1, 0}, {384, 500'000, 1, 0, 10, 0},
                   {384, 500'000, 1, 0, 100, 0}, {384, 500'000, 1, 0, 1000, 0},
                   {128, 1'000'000, 1, 0, 0, 0},
                   {384, 500'000, 1, 1000, 0, 0}, {384, 500'000, 1, 1000, 1000, 0},
                   {384, 500'000, 1, 10000, 0, 0}, {384, 500'000, 1, 10000, 1, 0},
                   {384, 500'000, 1, 10000, 10, 0}, {384, 500'000, 1, 10000, 100, 0},
                   {384, 500'000, 1, 10000, 1000, 0},
                   {384, 480'000, 16, 1000, 0, 0}, {384, 480'000, 16, 1000, 10, 0},
                   {384, 480'000, 16, 1000, 1000, 0},
                   {384, 500'000, 1, 0, 0, -1}, {384, 500'000, 1, 0, 10, -1}, {384, 500'000, 1, 0, 1000, -1},
                   {384, 500'000, 1, 0, 0, 30}, {384, 500'000, 1, 0, 1000, 30},
                   {384, 500'000, 1, 0, 1000, 10}, {384, 500'000, 1, 0, 0, 1010},
                   {384, 500'000, 1, 0, 1000, 1010}, {384, 500'000, 1, 0, 1000, 1030},
                   {384, 500'000, 1, 0, 1000, 1002}, {384, 500'000, 1, 0, 1000, 1250},
                   {384, 500'000, 1, 0, 1000, 1500}});
    })
    ->UseRealTime()->Unit(benchmark::kMillisecond);

// Hot, concurrent: syscall contention on the shared mapping.
TUNING_BENCHMARK(BM_VectorColdColumn)->Name("Tuning/BM_VectorColdColumn/threads")
    ->ArgNames({"dims", "docs", "segs", "refine", "coldPm", "ra", "domain"})
    ->Apply([](benchmark::Benchmark* b) {
      coldArgs(b, {{384, 500'000, 1, 10000, 0, 0}, {128, 1'000'000, 1, 0, 0, 0}});
    })
    ->Threads(16)->UseRealTime()->Unit(benchmark::kMillisecond);

#endif  // __linux__
