#include <charconv>
#include "test/SegmentTest.h"
#include "test/TestData.h"
#include "bench/solux_bench.h"
#include "solux/index/Inverter.h"

using namespace solux;

static void BM_IndexBook(benchmark::State& state, std::string field, bool writePostings, bool docPerPara) {

  Book& book = TestData::data->getBook();

  std::unique_ptr<SegmentTest> segTest;
  if (writePostings) {
    segTest = std::make_unique<SegmentTest>();
  }

  int64_t inverterSz = 0;
  char* data = const_cast<char*>(book.text().data());  // TODO: need to make a copy for any analysis that mutates? Make tokenizer do this?
  int sz = docPerPara ? book.sumParaSizes : book.text().size();

  for (auto _ : state) {
    Inverter inverter;
    Inverter::IndexHandler& fieldHandler = inverter.getIndexHandler(field);

    if (!docPerPara) {
      // index whole book as a single document
      inverter.startDoc();
      fieldHandler.index(inverter, data, sz);
      inverter.finishDoc();
    } else {
      // index each paragraph as its own document
      for (int i=0; i<(int)book.paraOffsets.size(); i++) {
        inverter.startDoc();
        fieldHandler.index(inverter, data + book.paraOffsets[i], book.paraSizes[i]);
        inverter.finishDoc();
      }
    }

    inverterSz = inverter.memSize();

    if (writePostings) {
      segTest->initWriter();
      inverter.writePostings(*segTest->writer);
    }
  }

  state.counters["rate="] = benchmark::Counter(sz, benchmark::Counter::kIsIterationInvariantRate);
  state.counters["inverterSz"] = inverterSz;
  if (writePostings) {
    segTest->initReader();
    state.counters["indexSz"] = segTest->getIndexSize();
  }
}



BENCHMARK_CAPTURE(BM_IndexBook, ws, "text_w", false, false);             // index whole book as single doc, invert only
BENCHMARK_CAPTURE(BM_IndexBook, ws_postings, "text_w", true, false);     // index whole book as single doc
BENCHMARK_CAPTURE(BM_IndexBook, para_ws, "text_w", false, true);         // index paragraph-per-doc, invert only
BENCHMARK_CAPTURE(BM_IndexBook, para_ws_postings, "text_w", true, true); // index paragraph-per-ddoc
