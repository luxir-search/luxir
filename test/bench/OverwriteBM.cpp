// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <latch>
#include <thread>

#include "bench/luxir_bench.h"
#include "test/CollectionHelper.h"

using namespace luxir;
using namespace luxir::test;


// Benchmark overwrite efficiency: N real threads concurrently indexing random documents
// with IDs in [0, maxDoc), causing overwrites/deletes when IDs collide.
// Run with overwrite=true and overwrite=false to measure the overhead of delete buffering.
// Uses CollectionHelper (local path) to isolate overwrite cost from gRPC overhead.
static void BM_Overwrite(benchmark::State& state, int nThreads, int64_t maxDoc, int batchSize, bool overwrite, bool extraInt = false) {
  int64_t totalDocs = maxDoc * 2;  // 2x overwrite ratio

  if (luxir::unit_tests) {
    totalDocs = std::min(totalDocs, LuxirTest::scaleTestWork(200));
    maxDoc = std::min(maxDoc, LuxirTest::scaleTestWork(100));
  }

  int64_t docsPerThread = totalDocs / nThreads;

  CollectionHelper helper;

  RSSWatcher watcher;

  for (auto _ : state) {
    helper.clear();

    std::latch done(nThreads);
    std::vector<std::thread> threads;
    threads.reserve(nThreads);

    for (int t = 0; t < nThreads; t++) {
      threads.emplace_back([&, t]() {
        Rng rng((uint64_t)t);
        int64_t remaining = docsPerThread;

        // Pre-allocate docs vector and reuse across batches
        std::vector<Doc> docs(batchSize);
        for (int i = 0; i < batchSize; i++) {
          if (extraInt) {
            docs[i] = {{"id", std::string()}, {"ver_i", (int64_t)0}};
          } else {
            docs[i] = {{"id", std::string()}};
          }
        }

        while (remaining > 0) {
          int thisBatch = (int)std::min(remaining, (int64_t)batchSize);

          for (int i = 0; i < thisBatch; i++) {
            int64_t docId = rng.rint(maxDoc);
            std::get<std::string>(docs[i][0].val) = std::to_string(docId);
            if (extraInt) {
              std::get<int64_t>(docs[i][1].val) = docId;
            }
          }

          helper.indexAll({docs.data(), (size_t)thisBatch}, UpdateMessage::NO_COMMIT, overwrite);
          remaining -= thisBatch;
        }

        done.count_down();
      });
    }

    done.wait();
    for (auto& t : threads) t.join();

    // Commit to force delete application
    helper.commit();
  }

  auto mem = watcher.getDeltaKB();
  state.counters["rate"] = benchmark::Counter((double)totalDocs, benchmark::Counter::kIsIterationInvariantRate);
  state.counters["threads"] = nThreads;
  state.counters["maxDoc"] = (double)maxDoc;
  state.counters["batch"] = batchSize;
  state.counters["overwrite"] = overwrite;
  state.counters["extraInt"] = extraInt;
  state.counters["RSS_delta"] = mem.first / 1024;
  state.counters["RSS_max"] = mem.second / 1024;
}


// Vary thread count with overwrite on, off, and off+extraInt.
// noOW_int measures the added cost of indexing a numeric field with overwrite disabled.
// Compare overwrite with noOW for overwrite overhead.
LUXIR_BENCHMARK_CAPTURE(BM_Overwrite, t1_100k,           1, 100'000, 100, true);
LUXIR_BENCHMARK_CAPTURE(BM_Overwrite, t1_100k_noOW,      1, 100'000, 100, false);
LUXIR_BENCHMARK_CAPTURE(BM_Overwrite, t1_100k_noOW_int,  1, 100'000, 100, false, true);
LUXIR_BENCHMARK_CAPTURE(BM_Overwrite, t2_100k,           2, 100'000, 100, true);
LUXIR_BENCHMARK_CAPTURE(BM_Overwrite, t2_100k_noOW,      2, 100'000, 100, false);
LUXIR_BENCHMARK_CAPTURE(BM_Overwrite, t2_100k_noOW_int,  2, 100'000, 100, false, true);
LUXIR_BENCHMARK_CAPTURE(BM_Overwrite, t4_100k,           4, 100'000, 100, true);
LUXIR_BENCHMARK_CAPTURE(BM_Overwrite, t4_100k_noOW,      4, 100'000, 100, false);
LUXIR_BENCHMARK_CAPTURE(BM_Overwrite, t4_100k_noOW_int,  4, 100'000, 100, false, true);
LUXIR_BENCHMARK_CAPTURE(BM_Overwrite, t8_100k,           8, 100'000, 100, true);
LUXIR_BENCHMARK_CAPTURE(BM_Overwrite, t8_100k_noOW,      8, 100'000, 100, false);
LUXIR_BENCHMARK_CAPTURE(BM_Overwrite, t8_100k_noOW_int,  8, 100'000, 100, false, true);

// Vary maxDoc to change scale (collision rate stays ~constant since totalDocs = maxDoc * 2)
LUXIR_BENCHMARK_CAPTURE(BM_Overwrite, t4_1k,          4, 1'000,     100, true);
LUXIR_BENCHMARK_CAPTURE(BM_Overwrite, t4_1k_noOW,     4, 1'000,     100, false);
LUXIR_BENCHMARK_CAPTURE(BM_Overwrite, t4_10k,         4, 10'000,    100, true);
LUXIR_BENCHMARK_CAPTURE(BM_Overwrite, t4_10k_noOW,    4, 10'000,    100, false);
LUXIR_BENCHMARK_CAPTURE(BM_Overwrite, t4_1m,          4, 1'000'000, 100, true);
LUXIR_BENCHMARK_CAPTURE(BM_Overwrite, t4_1m_noOW,     4, 1'000'000, 100, false);

LUXIR_BENCHMARK_CAPTURE(BM_Overwrite, t1_10k_batch1,       1, 10'000, 1, true);
LUXIR_BENCHMARK_CAPTURE(BM_Overwrite, t1_10k_batch1_noOW,  1, 10'000, 1, false);
LUXIR_BENCHMARK_CAPTURE(BM_Overwrite, t4_10k_batch1,       4, 10'000, 1, true);
LUXIR_BENCHMARK_CAPTURE(BM_Overwrite, t4_10k_batch1_noOW,  4, 10'000, 1, false);


// Bulk ingest of unique ids with overwrite on and no commit until the end, the shape of
// an ES-style _bulk load where the gateway generates every id. No id ever collides, but
// each flush still leaves a delete packet holding the inverter's whole id hash. Those
// packets must be retired by delete passes under a finite budget instead of piling up
// until the commit. retained_max and reserved_max should stay near the budget no matter
// how many docs are indexed; compare with noOW, which builds no packets at all.
static void BM_OverwriteUniqueNoCommit(benchmark::State& state, int64_t totalDocs, int batchSize, bool overwrite, int64_t budgetMB) {
  if (luxir::unit_tests) totalDocs = std::min(totalDocs, LuxirTest::scaleTestWork(2000));

  CollectionHelper helper;
  auto& budget = helper.getNode().getIndexRamBudget();
  auto writer = helper.getIndexWriter();
  int64_t savedTotal = budget.totalBytes();
  size_t savedInverterCap = writer->perInverterRamBytes;
  // As on a server, --indexing.max-inverter-ram-mb defaults to the whole budget.
  budget.setTotalBytes(budgetMB << 20);
  writer->perInverterRamBytes = (size_t)budgetMB << 20;

  RSSWatcher watcher;
  int64_t retainedMax = 0, reservedMax = 0;

  for (auto _ : state) {
    helper.clear();

    std::vector<Doc> docs(batchSize);
    for (auto& d : docs) d = {{"id", std::string()}};

    for (int64_t done = 0; done < totalDocs; ) {
      int thisBatch = (int)std::min(totalDocs - done, (int64_t)batchSize);
      for (int i = 0; i < thisBatch; i++) {
        std::get<std::string>(docs[i][0].val) = "doc-" + std::to_string(done + i);
      }
      helper.indexAll({docs.data(), (size_t)thisBatch}, UpdateMessage::NO_COMMIT, overwrite);
      done += thisBatch;
      retainedMax = std::max(retainedMax, budget.retainedDeleteBytesCount());
      reservedMax = std::max(reservedMax, budget.reservedBytes());
    }

    helper.commit();
  }

  budget.setTotalBytes(savedTotal);
  writer->perInverterRamBytes = savedInverterCap;

  auto mem = watcher.getDeltaKB();
  state.counters["rate"] = benchmark::Counter((double)totalDocs, benchmark::Counter::kIsIterationInvariantRate);
  state.counters["docs"] = (double)totalDocs;
  state.counters["overwrite"] = overwrite;
  state.counters["budget_MB"] = (double)budgetMB;
  state.counters["retained_max_MB"] = (double)retainedMax / (1 << 20);
  state.counters["reserved_max_MB"] = (double)reservedMax / (1 << 20);
  state.counters["RSS_delta"] = mem.first / 1024;
  state.counters["RSS_max"] = mem.second / 1024;
}

// 4x the docs at the same budget should not raise retained_max or RSS_max.
LUXIR_BENCHMARK_CAPTURE(BM_OverwriteUniqueNoCommit, 5m_64mb,       5'000'000,  1000, true,  64);
LUXIR_BENCHMARK_CAPTURE(BM_OverwriteUniqueNoCommit, 20m_64mb,      20'000'000, 1000, true,  64);
LUXIR_BENCHMARK_CAPTURE(BM_OverwriteUniqueNoCommit, 20m_64mb_noOW, 20'000'000, 1000, false, 64);
