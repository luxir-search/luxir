// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <charconv>
#include <test/SegmentTest.h>
#include "bench/luxir_bench.h"
#include "bench/ScientificText.h"
#include "luxir/index/Inverter.h"
#include <lz4.h>
#include "test/SchemaBuilder.h"

using namespace luxir;

static void BM_Invert(benchmark::State& state, std::string field, bool writePostings) {

  std::string val = "Now is the time for all Good Men to come to the aid of their Country";
  int ntokens = 1000;
  val.reserve(ntokens * 8);
  Rng r(1);
  std::string termStr;
  termStr.reserve(22);
  for (int i=0; i<ntokens; i++) {
    val += ' ';
    std::to_chars(termStr.data(), termStr.data() + 21, r());
    size_t digits = (uint32_t)(r()) % 8 + 1;
    val += std::string_view(termStr.data(), digits);
  }


  int iter = unit_tests ? (int)LuxirTest::scaleTestWork(10) : 1000;
  int64_t inverterSz = 0;
  RAMDir dir;
  for (auto _ : state) {
    dir = RAMDir(); // clear dir
    Inverter inverter(dir, 0);
    Inverter::InputHandler& fieldHandler = inverter.getIndexHandler(field);

    for (int i=0; i<iter;i++) {
      inverter.startDoc();
      fieldHandler.index(inverter, val);
      inverter.finishDoc();
    }

    inverterSz = inverter.memSize();

    if (writePostings) {
      inverter.flush();
    }
  }

  state.counters["rate="] = benchmark::Counter(val.size()*iter, benchmark::Counter::kIsIterationInvariantRate);
  state.counters["inverterSz"] = inverterSz;
  state.counters["indexSz"] = dir.totalBytes();
}



BENCHMARK_CAPTURE(BM_Invert, ws, "text_w", false)->UseRealTime();
BENCHMARK_CAPTURE(BM_Invert, ws_lc, "text_wl", false)->UseRealTime();
BENCHMARK_CAPTURE(BM_Invert, ws_postings, "text_w", true)->UseRealTime();

// End-to-end text indexing of ~40KB PMC-like article bodies into a `_t` field
// (unicode_word + nfkc_cf + fold + kstem, positions, stored): analysis, term
// hashing, position encoding and the stored-fields copy, without the request
// parse. A handful of distinct bodies cycle so the term hash sees repeats
// across documents like a real corpus.
static void BM_InvertSci(benchmark::State& state, std::string field, bool stored = true) {
  std::shared_ptr<Schema> schema = Schema::createDefaultSchema();
  if (!stored) {
    SchemaBuilder b;
    auto& f = b.field(field);
    f.parent = "_t";
    f.stored = false;
    schema = b.build(schema.get());
  }
  std::vector<std::string> bodies;
  for (uint64_t seed = 1; seed <= 8; seed++) bodies.push_back(scientificText(40 * 1024, seed));
  size_t totalBytes = 0;
  int ndocs = unit_tests ? 4 : 256;
  for (int i = 0; i < ndocs; i++) totalBytes += bodies[(size_t) i % bodies.size()].size();
  for (auto _ : state) {
    RAMDir dir;
    Inverter inverter(dir, 0, schema);
    auto& handler = inverter.getIndexHandler(field);
    for (int i = 0; i < ndocs; i++) {
      inverter.startDoc();
      handler.index(inverter, std::string_view(bodies[(size_t) i % bodies.size()]));
      inverter.finishDoc();
    }
    benchmark::DoNotOptimize(inverter.memSize());
  }
  state.counters["docs_per_s"] = benchmark::Counter((double) ndocs * (double) state.iterations(),
      benchmark::Counter::kIsRate);
  state.counters["byte_rate"] = benchmark::Counter((double) totalBytes * (double) state.iterations(),
      benchmark::Counter::kIsRate);
}
BENCHMARK_CAPTURE(BM_InvertSci, t, "body_t")->UseRealTime();
BENCHMARK_CAPTURE(BM_InvertSci, un, "body_un")->UseRealTime();
// The same _t analysis without the stored-fields copy (index-only cost).
TUNING_BENCHMARK_CAPTURE(BM_InvertSci, t_unstored, "body", false)->UseRealTime();

// The stored-fields share of BM_InvertSci: one LZ4 chunk compression of a
// ~40KB body (a 40KB doc fills a whole 16KB-target chunk by itself).
static void BM_StoredLz4Sci(benchmark::State& state) {
  std::string body = scientificText(40 * 1024, 1);
  std::string out((size_t) LZ4_compressBound((int) body.size()), '\0');
  int compressed = 0;
  for (auto _ : state) {
    compressed = LZ4_compress_default(body.data(), out.data(), (int) body.size(), (int) out.size());
    benchmark::DoNotOptimize(compressed);
  }
  state.counters["docs_per_s"] = benchmark::Counter((double) state.iterations(), benchmark::Counter::kIsRate);
  state.counters["ratio"] = (double) body.size() / (double) compressed;
}
TUNING_BENCHMARK(BM_StoredLz4Sci)->UseRealTime();
