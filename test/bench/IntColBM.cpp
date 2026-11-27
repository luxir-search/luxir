// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <charconv>
#include "bench/luxir_bench.h"
#include "test/TestIndex.h"
#include "luxir/index/Inverter.h"
#include "luxir/index/IntColWriter.h"
#include "test/NumericCorpora.h"

using namespace luxir;

namespace {

// One corpus of test/NumericCorpora.h written as a standalone column.
struct NumericCorpusColumn {
  RAMDir dir;
  std::shared_ptr<InputFile> in;
  std::optional<HeldRange> hold;
  std::optional<InputStream> input;
  std::optional<IntColReader> reader;
  double bitsPerValue = 0;

  NumericCorpusColumn(std::string_view name, size_t n, bool codecs) {
    for (auto& corpus : test::numericCorpora(n)) {
      if (corpus.name != name) continue;
      auto file = dir.createFile("numeric");
      OutputStream out(file.get());
      NumColumnFormat::BlockCodecs blockCodecs{codecs, corpus.domain};
      IntColWriter writer(out, NumColumnFormat::SketchMode::NONE, blockCodecs);
      for (int64_t value : corpus.values) writer.addInt64(value);
      auto data = writer.finish();
      out.close();
      dir.finishFile(*file);
      in = dir.openFile("numeric");
      hold.emplace(in->acquire({0, in->size()}));
      input.emplace(hold->stream());
      reader.emplace(input->ptr(data.columnLoc),
                   std::span(input->ptr(data.columnLoc + data.columnMetaOff), data.indexBytes),
                   data.numValues);
      bitsPerValue = 8.0 * (double)(data.columnMetaOff
          + reader->numBlocks() * (int64_t)sizeof(NumBlockInfo)) / (double)n;
    }
  }
};

} // namespace

template <class IterType>
static void BM_IntCol(benchmark::State& state, int32_t nDocs, int32_t docDelta, int64_t maxVal, int skip) {
  Rng rng(0);
  test::TestIndex testIndex;
  test::TestField testField(testIndex, "foo_i");
  testField.startIndexing();
  int32_t docid = -1;
  for (;;) {
    if (docDelta != 1) {
      docid += rng.rint(1,docDelta);
    } else {
      docid++;
    }
    if (docid >= nDocs) {
      break;
    }
    testField.add(docid, rng.rint(maxVal));
  }
  testIndex.flush();
  testField.startReading();
  testField.nextSegment();
  IntColReader& intColReader = *testField.colReader;

  int64_t ret = 0;
  int64_t count = 0;
  for (auto _ : state) {
    ret = 0;
    count = 0;

    IterType it(intColReader);
    if (skip==1) {
      while (it.next() != IntColReader::ENDDOC) {
        ret += it.value();
        count++;
      }
    } else {
      // skipping
      int32_t doc = -1;
      for(;;) {
        doc = it.advance(doc+skip);
        if (doc == IntColReader::ENDDOC) {
          break;
        }
        ret += it.value();
        count++;
      }
    }

    benchmark::DoNotOptimize(ret);
  }

  state.counters["fp"] = ret;  // sanity check.
  state.counters["count"] = count;  // sanity check.
  state.counters["rate"] = benchmark::Counter(count, benchmark::Counter::kIsIterationInvariantRate);
}

void BM_IntColSparse(benchmark::State& state, int32_t nDocs, int32_t docDelta, int64_t maxVal, int skip) {
  BM_IntCol<IntColReader::SparseIterator>(state, nDocs, docDelta, maxVal, skip);
}
void BM_IntColBulk(benchmark::State& state, int32_t nDocs, int32_t docDelta, int64_t maxVal, int skip) {
  BM_IntCol<IntColReader::BulkIterator>(state, nDocs, docDelta, maxVal, skip);
}

// When we test sparse sets for performance, the most interesting case is when it's still a bitset in the block.
// Search code will spend much less time in very sparse sets.
constexpr int32_t nDocs = 65536;

// IntColSparse uses iterators that decode a value at a time.
TUNING_BENCHMARK_CAPTURE(BM_IntColSparse, denseIter,   nDocs, 1, 1100, 1);  // docs are dense, iterate over all values
TUNING_BENCHMARK_CAPTURE(BM_IntColSparse, sparseIter,  nDocs, 4, 1100, 1);  // docs are sparse, iterate over all values
TUNING_BENCHMARK_CAPTURE(BM_IntColSparse, denseSkip3,  nDocs, 1, 1100, 3);  // docs are dense, use skipping of size 3
TUNING_BENCHMARK_CAPTURE(BM_IntColSparse, denseSkip27, nDocs, 1, 1100, 27); // docs are dense, use skipping of size 27
TUNING_BENCHMARK_CAPTURE(BM_IntColSparse, sparseSkip3, nDocs, 4, 1100, 3);  // docs are sparse, use skipping of size 3

// IntColBulk uses iterators that decode a block at a time.
TUNING_BENCHMARK_CAPTURE(BM_IntColBulk, denseIter,   nDocs, 1, 1100, 1);
TUNING_BENCHMARK_CAPTURE(BM_IntColBulk, sparseIter,  nDocs, 4, 1100, 1);
TUNING_BENCHMARK_CAPTURE(BM_IntColBulk, denseSkip3,  nDocs, 1, 1100, 3);
TUNING_BENCHMARK_CAPTURE(BM_IntColBulk, denseSkip27, nDocs, 1, 1100, 27);
TUNING_BENCHMARK_CAPTURE(BM_IntColBulk, sparseSkip3, nDocs, 4, 1100, 3);

// Decode cost per block kind (NumBlockKind): one 1M-value column per corpus
// of test/NumericCorpora.h, written with the field codecs on (or off, the
// FOR baseline), then read whole through the bulk path or at random ranks
// through the point path. Reports bits/value next to the decode rate.
static void BM_NumColumnKind(benchmark::State& state, std::string_view corpusName,
                             bool codecs, bool bulk) {
  constexpr size_t N = 1 << 20;
  NumericCorpusColumn column(corpusName, N, codecs);
  IntColReader& reader = *column.reader;
  Rng rng(1);
  std::vector<int64_t> ranks(4096);
  for (auto& rank : ranks) rank = rng.rint((int64_t)N);
  int64_t sum = 0;
  int64_t reads = 0;
  for (auto _ : state) {
    if (bulk) {
      IntColReader::BulkValues values(reader);
      for (int64_t rank = 0; rank < (int64_t)N; rank++) sum += values.valueAt(rank);
      reads += (int64_t)N;
    } else {
      IntColReader::SparseValues values(reader);
      for (int64_t rank : ranks) sum += values.valueAt(rank);
      reads += (int64_t)ranks.size();
    }
    benchmark::DoNotOptimize(sum);
  }
  state.counters["bits_per_value"] = column.bitsPerValue;
  state.counters["values"] = benchmark::Counter((double)reads, benchmark::Counter::kIsRate);
}

#define NUM_COLUMN_KIND_BM(corpus)                                                  \
  TUNING_BENCHMARK_CAPTURE(BM_NumColumnKind, corpus##_bulk, #corpus, true, true);    \
  TUNING_BENCHMARK_CAPTURE(BM_NumColumnKind, corpus##_bulk_for, #corpus, false, true); \
  TUNING_BENCHMARK_CAPTURE(BM_NumColumnKind, corpus##_point, #corpus, true, false);  \
  TUNING_BENCHMARK_CAPTURE(BM_NumColumnKind, corpus##_point_for, #corpus, false, false);
NUM_COLUMN_KIND_BM(prices)
NUM_COLUMN_KIND_BM(signed_doubles)
NUM_COLUMN_KIND_BM(float_readings)
NUM_COLUMN_KIND_BM(regular_times)
NUM_COLUMN_KIND_BM(bursty_times)
NUM_COLUMN_KIND_BM(status_codes)
NUM_COLUMN_KIND_BM(outliers)
