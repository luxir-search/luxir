#include <charconv>
#include <test/SegmentTest.h>
#include "bench/solux_bench.h"
#include "solux/index/Inverter.h"

using namespace solux;

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

  std::unique_ptr<SegmentTest> segTest;
  if (writePostings) {
    segTest = std::make_unique<SegmentTest>();
  }

  int iter = unit_tests ? 10 : 1000;
  int64_t inverterSz = 0;
  for (auto _ : state) {
    Inverter inverter;
    Inverter::SegFieldPos& segField = inverter.getSegField(field);

    for (int i=0; i<iter;i++) {
      inverter.startDoc();
      inverter.index(segField, &val[0], (int) val.size());
      inverter.finishDoc();
    }

    inverterSz = inverter.memSize();

    if (writePostings) {
      segTest->initWriter();
      inverter.writePostings(*segTest->writer);
    }
  }

  state.counters["rate="] = benchmark::Counter(val.size()*iter, benchmark::Counter::kIsIterationInvariantRate);
  state.counters["inverterSz"] = inverterSz;
  if (writePostings) {
    segTest->initReader();
    state.counters["indexSz"] = segTest->getIndexSize();
  }
}



BENCHMARK_CAPTURE(BM_Invert, ws, "text_w", false);
BENCHMARK_CAPTURE(BM_Invert, ws_lc, "text_wl", false);
BENCHMARK_CAPTURE(BM_Invert, ws_postings, "text_w", true);
