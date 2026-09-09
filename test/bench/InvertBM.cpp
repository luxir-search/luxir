// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <charconv>
#include <test/SegmentTest.h>
#include "bench/luxir_bench.h"
#include "luxir/index/Inverter.h"

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
