#include <charconv>
#include "test/SegmentTest.h"
#include "test/TestData.h"
#include "bench/solux_bench.h"
#include "solux/index/Inverter.h"

using namespace solux;

static void BM_IndexBook(benchmark::State& state, std::string field, bool writePostings) {

  Book& book = TestData::data->getBook();

  std::unique_ptr<SegmentTest> segTest;
  if (writePostings) {
    segTest = std::make_unique<SegmentTest>();
  }

  int iter = unit_tests ? 1 : 1;
  int64_t inverterSz = 0;
  char* data = const_cast<char*>(book.text().data());  // TODO: need to make a copy for any analysis that mutates? Make tokenizer do this?
  int sz = book.text().size();

  for (auto _ : state) {
    Inverter inverter;
    Inverter::SegFieldPos& segField = inverter.getSegField(field);

    for (int i=0; i<iter;i++) {
      inverter.startDoc();
      inverter.index(segField, data, sz);
      inverter.finishDoc();
    }

    inverterSz = inverter.memSize();

    if (writePostings) {
      segTest->initWriter();
      inverter.writePostings(*segTest->writer);
    }
  }

  state.counters["rate="] = benchmark::Counter(sz*iter, benchmark::Counter::kIsIterationInvariantRate);
  state.counters["inverterSz"] = inverterSz;
  if (writePostings) {
    segTest->initReader();
    state.counters["indexSz"] = segTest->getIndexSize();
  }
}



BENCHMARK_CAPTURE(BM_IndexBook, ws, "text_w", false);
BENCHMARK_CAPTURE(BM_IndexBook, ws_postings, "text_w", true);
// BENCHMARK_CAPTURE(BM_IndexBook, ws_lc, "text_wl", false);
// BENCHMARK_CAPTURE(BM_IndexBook, ws_postings, "text_w", true);
