#include <charconv>
#include "test/SegmentTest.h"
#include "test/TestData.h"
#include "bench/luxir_bench.h"
#include "luxir/index/Inverter.h"

using namespace luxir;

// Indexes the book and reports the invert and flush phases separately. A single
// invert+flush run yields both phase numbers, so there is no need for distinct
// invert-only and invert+flush benchmarks per corpus.
static void BM_IndexBook(benchmark::State& state, std::string field, bool docPerPara) {

  Book& book = TestData::data->getBook();
  if (skipBenchIfDataMissing(state, !book.text().empty(), "book.txt")) return;

  int64_t inverterSz = 0;
  int sz = docPerPara ? book.sumParaSizes : book.text().size();

  RAMDir dir;
  double invertSecs = 0, flushSecs = 0;
  for (auto _ : state) {
    dir = RAMDir(); // clear files
    Inverter inverter(dir, 0);
    Inverter::IndexHandler& fieldHandler = inverter.getIndexHandler(field);

    auto t0 = std::chrono::steady_clock::now();
    if (!docPerPara) {
      // index whole book as a single document
      inverter.startDoc();
      fieldHandler.index(inverter, book.text());
      inverter.finishDoc();
    } else {
      // index each paragraph as its own document
      for (int i=0; i<(int)book.paraOffsets.size(); i++) {
        inverter.startDoc();
        fieldHandler.index(inverter, book.text().substr(book.paraOffsets[i], book.paraSizes[i]));
        inverter.finishDoc();
      }
    }
    auto t1 = std::chrono::steady_clock::now();
    inverterSz = inverter.memSize();
    inverter.flush();
    auto t2 = std::chrono::steady_clock::now();

    double iSecs = std::chrono::duration<double>(t1 - t0).count();
    double fSecs = std::chrono::duration<double>(t2 - t1).count();
    invertSecs += iSecs;
    flushSecs += fSecs;
    state.SetIterationTime(iSecs + fSecs);
  }

  double iters = (double)state.iterations();
  state.counters["invert_rate"] = sz * iters / invertSecs;   // bytes/sec
  state.counters["flush_rate"] = sz * iters / flushSecs;
  state.counters["total_rate"] = sz * iters / (invertSecs + flushSecs);
  state.counters["inverterSz"] = inverterSz;
  state.counters["indexSz"] = dir.totalBytes();
}



// BENCHMARK_CAPTURE(BM_IndexBook, whitespace, "text_w", false)->UseManualTime();    // index whole book as single doc
BENCHMARK_CAPTURE(BM_IndexBook, standard, "text_un", false)->UseManualTime();        // index whole book as single doc
BENCHMARK_CAPTURE(BM_IndexBook, para_whitespace, "text_w", true)->UseManualTime();   // index paragraph-per-doc
BENCHMARK_CAPTURE(BM_IndexBook, para_standard, "text_un", true)->UseManualTime();    // index paragraph-per-doc
