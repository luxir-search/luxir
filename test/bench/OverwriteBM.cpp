#include <latch>
#include <thread>

#include "bench/solux_bench.h"
#include "test/CollectionHelper.h"

using namespace solux;
using namespace solux::test;


// Benchmark overwrite efficiency: N real threads concurrently indexing random documents
// with IDs in [0, maxDoc), causing overwrites/deletes when IDs collide.
// Run with overwrite=true and overwrite=false to measure the overhead of delete buffering.
// Uses CollectionHelper (local path) to isolate overwrite cost from gRPC overhead.
static void BM_Overwrite(benchmark::State& state, int nThreads, int64_t maxDoc, int batchSize, bool overwrite, bool extraInt = false) {
  int64_t totalDocs = maxDoc * 2;  // 2x overwrite ratio

  if (solux::unit_tests) {
    totalDocs = std::min(totalDocs, (int64_t)200);
    maxDoc = std::min(maxDoc, (int64_t)100);
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
// noOW_int adds an extra int field to approximate the cost of indexing _version_ without the delete
// buffering, so the difference between overwrite and noOW_int isolates
// the delete buffering + application cost.
SOLUX_BENCHMARK_CAPTURE(BM_Overwrite, t1_100k,           1, 100'000, 100, true);
SOLUX_BENCHMARK_CAPTURE(BM_Overwrite, t1_100k_noOW,      1, 100'000, 100, false);
SOLUX_BENCHMARK_CAPTURE(BM_Overwrite, t1_100k_noOW_int,  1, 100'000, 100, false, true);
SOLUX_BENCHMARK_CAPTURE(BM_Overwrite, t2_100k,           2, 100'000, 100, true);
SOLUX_BENCHMARK_CAPTURE(BM_Overwrite, t2_100k_noOW,      2, 100'000, 100, false);
SOLUX_BENCHMARK_CAPTURE(BM_Overwrite, t2_100k_noOW_int,  2, 100'000, 100, false, true);
SOLUX_BENCHMARK_CAPTURE(BM_Overwrite, t4_100k,           4, 100'000, 100, true);
SOLUX_BENCHMARK_CAPTURE(BM_Overwrite, t4_100k_noOW,      4, 100'000, 100, false);
SOLUX_BENCHMARK_CAPTURE(BM_Overwrite, t4_100k_noOW_int,  4, 100'000, 100, false, true);
SOLUX_BENCHMARK_CAPTURE(BM_Overwrite, t8_100k,           8, 100'000, 100, true);
SOLUX_BENCHMARK_CAPTURE(BM_Overwrite, t8_100k_noOW,      8, 100'000, 100, false);
SOLUX_BENCHMARK_CAPTURE(BM_Overwrite, t8_100k_noOW_int,  8, 100'000, 100, false, true);

// Vary maxDoc to change scale (collision rate stays ~constant since totalDocs = maxDoc * 2)
SOLUX_BENCHMARK_CAPTURE(BM_Overwrite, t4_1k,          4, 1'000,     100, true);
SOLUX_BENCHMARK_CAPTURE(BM_Overwrite, t4_1k_noOW,     4, 1'000,     100, false);
SOLUX_BENCHMARK_CAPTURE(BM_Overwrite, t4_10k,         4, 10'000,    100, true);
SOLUX_BENCHMARK_CAPTURE(BM_Overwrite, t4_10k_noOW,    4, 10'000,    100, false);
SOLUX_BENCHMARK_CAPTURE(BM_Overwrite, t4_1m,          4, 1'000'000, 100, true);
SOLUX_BENCHMARK_CAPTURE(BM_Overwrite, t4_1m_noOW,     4, 1'000'000, 100, false);
