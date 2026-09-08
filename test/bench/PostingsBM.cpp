// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "bench/luxir_bench.h"
#include "test/SegmentTest.h"
#include "luxir/reader/PostingsReader.h"

/* Results of postings reading before position blocks are supported (just reading vints directly,
 * without decoding into intermediate array:
Load Average: 0.33, 0.20, 0.13
----------------------------------------------------------------------------
Benchmark                                  Time             CPU   Iterations
----------------------------------------------------------------------------
BM_blockDecode/SimpleCodec              7.84 ns         7.84 ns     89575561
BM_blockDecode/FastPFor                  697 ns          697 ns      1009853
BM_blockDecode/SIMDFastPFor              659 ns          659 ns      1064500
BM_blockDecode/SIMDFastPForDelta1        680 ns          680 ns      1016907
BM_readAllPositions                      403 ns          403 ns      1775818 ndoc=10 npos=120
(a few more runs yielded CPU time of 415,406,412,412)
 This is faster than a block decode?! Is some of the calculation being moved outside of the loop or eliminated???
 No, even creating a new random index each time, it came out to 663ns

 For clang 11 (instead of gcc 10.2), it looks more like this:
 Benchmark                                  Time             CPU   Iterations
----------------------------------------------------------------------------
BM_blockDecode/SimpleCodec              7.60 ns         7.60 ns     91480782
BM_blockDecode/FastPFor                  467 ns          467 ns      1506020
BM_blockDecode/SIMDFastPFor              446 ns          446 ns      1575341
BM_blockDecode/SIMDFastPForDelta1        494 ns          494 ns      1421284
BM_readAllPositions                      479 ns          479 ns      1498213 ndoc=10 npos=120

 TODO: is there a compilation issue with gcc here?  Normally it's faster than clang, so being so much slower
 is something to look into!

===================================== performance progression ===========================================
1: simple code that didn't handle blocks and decoded vints directly (no intermediate decode buffer): ~410ns
2: moving to always using a pos buffer (but still no block decoding): ~620ns
3: implementing block position decoding: ~1050ns (for 128 positions as opposed to previous 120)
BM_readAllPositions                     1041 ns         1041 ns       680326 ndoc=8 npos=128
****************/

/**** Block Doc Decoding:
 As a baseline, the simple vint decoding w/o a buffer shown below is fast:
   int32_t nextDoc() {
    if (docsSize != 0) {
      uint32_t doccode = docIs.readVint();
      if ((doccode & 0x01)==1) {tfreq = 1;}
      else {tfreq = docIs.readVint();}
      posOrdStart = cumulativeTermFreq;
      cumulativeTermFreq += tfreq;
      auto docDelta = doccode >> 1;
      docid += docDelta;
    }
    return docid;
  }
Anywhere from 425ns to 450ns for decoding 127 docs while skipping positions:
BM_Postings/readDocsTail                 446 ns          438 ns      1628727 docs=127 pos=0 terms=1

 */

// percentReadPos is the percentage of documents for which we decide to read it's positions.
// pass 0 to just read documents or 100 to always read positions.
static void BM_Postings(benchmark::State& state, int nTerms, int nDocs, int nPosPerDoc, int percentReadPos=100) {

  // std::cout << "BM_Postings START" << std::endl;

  auto seg = std::make_unique<luxir::SegmentTest>();

  if (!luxir::unit_tests) {
    seg->r.init(1);  // keep seed the same for performance benchmark
  }

  seg->initWriter();
  seg->addFields(false, 1, nTerms, nDocs, nPosPerDoc);
  seg->initReader();
  seg->addFields(true, 1, nTerms, nDocs, nPosPerDoc);  // verify reading

  uint64_t tterms=0, tdocs=0, tpos=0;
  // std::cout << "\tBM_Postings starting for loop" << std::endl;
  uint64_t iter = 0;
  for (auto _ : state) {
    iter++;
    if (luxir::unit_tests) {
      // Under clang debug, if we pause timing, it causes things to get very slow.

      // std::cout << "\tBM_Postings PauseTiming" << std::endl;
      // state.PauseTiming();  // PauseTiming and ResumeTiming are very slow (~200ns)! Don't use in conjunction with anything fast!
      // add a new different index each time if we are running unit tests
      // seg = std::make_unique<luxir::SegmentTest>();
      // seg->r.init(luxir::LuxirTest::rng());
      seg->initWriter();
      seg->addFields(false, 1, nTerms, nDocs, nPosPerDoc);
      seg->initReader();
      //      state.ResumeTiming();
      // std::cout << "\tBM_Postings ResumeTiming" << std::endl;
    }

    uint64_t fingerprint = 1;
    benchmark::DoNotOptimize( std::tie(fingerprint, tterms, tdocs, tpos) = seg->readFingerprint(percentReadPos) );
    // benchmark::DoNotOptimize(fingerprint); // this causes fingerprint to be 0???? (when I tie'd directly to seg.readFingerprint()) compiler bug?
    benchmark::ClobberMemory();
    if (percentReadPos >= 100) {
      ASSERT_EQ(seg->fingerprint, fingerprint);
      ASSERT_EQ(nTerms * nDocs * nPosPerDoc, tpos);
    }
    ASSERT_EQ(tdocs, nDocs * nTerms);
    ASSERT_EQ(tterms, nTerms);
    // std::cout << "\tterms=" << tterms << " docs=" << tdocs << " pos=" << tpos << std::endl;
  }

  state.counters["terms"] = tterms;
  state.counters["docs"] = tdocs;
  state.counters["pos"] = tpos;

  // std::cout << "\tBM_Postings END iter=" << iter << std::endl;
  luxir::unused(iter);
}



BENCHMARK_CAPTURE(BM_Postings, readTailPos, 1, 8, luxir::Postings::POSITIONS_BLOCK_SIZE/8-1);      // read non-block encoded positions (tail)
BENCHMARK_CAPTURE(BM_Postings, readBlockPos, 1, 8, luxir::Postings::POSITIONS_BLOCK_SIZE/8);      // read positions when they are block encoded
BENCHMARK_CAPTURE(BM_Postings, readDocsTail, 1, luxir::Postings::DOCS_BLOCK_SIZE-1, 2, 0);        // read non-block encoded documents (tail)
BENCHMARK_CAPTURE(BM_Postings, readDocsBlock, 1, luxir::Postings::DOCS_BLOCK_SIZE, 2, 0);
BENCHMARK_CAPTURE(BM_Postings, readDocsBlockPos, 1, luxir::Postings::DOCS_BLOCK_SIZE, 2, 100);
BENCHMARK_CAPTURE(BM_Postings, readPulsedDoc, luxir::Postings::TERMS_BLOCK_SIZE-1, 1, 1, 0);
BENCHMARK_CAPTURE(BM_Postings, readPulsedPos, luxir::Postings::TERMS_BLOCK_SIZE-1, 1, 1, 100);
