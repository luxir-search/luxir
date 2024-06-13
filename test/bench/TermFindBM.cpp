#include <charconv>

#include "bench/solux_bench.h"
#include "test/SegmentTest.h"
#include "solux/reader/PostingsReader.h"
#include "solux/index/PostingsWriter.h"


using namespace solux;

// about 1.2% slower when not ommitting frame pointer
// adding term hashes (without using them) resulted in a slowdown of ~1%
// med is about 5% slower than small (before any optimizations like using hashes or pulling out prefixes from block starts)
//

static void BM_TermFind(benchmark::State& state, uint64_t maxId, int hitPercent) {
  int nPosPerDoc=1;
  // int nTerms=1000000;
  int nTerms=1000000;
  uint64_t seed = 1;
  if (solux::unit_tests) {
    nTerms = SoluxTest::rng.rint(1, Postings::TERMS_BLOCK_SIZE*10);
    seed = SoluxTest::rng_seed;
  }
  auto fname = "myfield";

  SegmentTest seg;
  seg.initWriter();
  MemPool& pool = seg.pool;

  std::unordered_set<int64_t> idSet(nTerms*2);  // used to ensure terms are unique... TODO: replace with something better like flat hash map
  std::vector<PackedTerm> terms;
  terms.reserve(nTerms*2);
  SplitMix64 r(seed);
  std::string termStr;
  termStr.resize(21);
  while ((int)terms.size() < nTerms*2) {   // generate twice as many terms, but only index the first half
    int64_t id = (r() & 0x7fffffffffffffff) % maxId;
    auto [iter, inserted] = idSet.insert(id);
    if (!inserted) continue; // a repeat
    auto [end, ec] = std::to_chars(termStr.data(), termStr.data() + 21, id);
    terms.emplace_back(seg.pool, termStr.data(), end-termStr.data());
  }

  // indirectly sort the terms so we can use the index as the docid
  std::vector<int> sorted(nTerms);
  std::iota( std::begin(sorted), std::end(sorted), 0 );
  std::sort( std::begin(sorted), std::end(sorted),
             [&terms] (int i, int j) { return terms[i] < terms[j]; } );
  uint32_t fp = 1;

  {
    TextWriter w(*seg.postingsWriter.get());
    w.startField(fname);
    for (auto docid: sorted) {
      auto term = terms[docid];
      w.startTerm(term);
      fp += term.size();
      int df = 1;
      fp += df;
      w.startDoc(docid);
      fp += docid;
      for (int j = 0; j < nPosPerDoc; j++) {
        w.addPositionDelta(1);
      }
      w.endDoc(docid);
      w.endTerm(term);
    }
    w.endField();
  }

  seg.initReader();
  PostingsReader& postingsReader = *seg.reader;
  FieldReader& fieldReader = *seg.fieldReader;
  ASSERT_EQ(true, fieldReader.readNextField());
  ASSERT_EQ((std::string_view)fname, seg.fieldReader->name());
  SegFieldInfo fieldInfo;
  fieldReader.readFieldInfo(fieldInfo);
  TermsEnum tenum(pool, postingsReader, fieldInfo);


  int hitFrac = hitPercent==0 ? -1 : hitPercent * 0x00ffff / 100; // convert to a fraction of 0x00ffff
  uint32_t fingerprint = 1;
  // int32_t hits = 0; int64_t sumdf = 0; int64_t sumdoc = 0;  // some counters to debug where something is off (when matching lucene)
  for (auto _ : state) {
    // hits = 0; sumdf = 0; sumdoc = 0;

    SplitMix64 r2(seed);
    fingerprint = 1;
    for (int i=0; i<nTerms; i++) {
      // go through in unsorted term order
      PackedTerm term;
      bool shouldHit = (int)(r2() & 0x00ffff) <= hitFrac;
      if (shouldHit) {
        term = terms[i];
      } else {
        term = terms[i+nTerms]; // we never indexed second half of array, so it should not be found.
      }
      bool found = tenum.seek((std::string_view)term);
      ASSERT_EQ(found, shouldHit);
      if (found) {
        // hits++;
        fingerprint += tenum.term().size();
        DocsEnum docsEnum(pool, postingsReader, tenum);
        auto df = docsEnum.numDocs();
        // sumdf += df;
        fingerprint += df;
        for (int j=0; j<df; j++) {
          auto doc = docsEnum.nextDoc();
          // sumdoc += doc;
          fingerprint += doc;
        }
      } else {
        fingerprint *= 3;
      }
    }
    benchmark::DoNotOptimize(fingerprint);
    benchmark::ClobberMemory();
    if (hitPercent >= 100) {
      ASSERT_EQ(fingerprint, fp);
    }
  }

  // std::cout << "fp=" << fp << " fingerprint=" << fingerprint << " hits=" << hits << " sumdf=" << sumdf << " sumdoc=" << sumdoc << std::endl;
  state.counters["fp"] = int32_t(fingerprint) % 100000;  // hmmm, how to get the full resolution on this?  Take mod to try and see least significant digits.
  state.counters["terms"] = nTerms;
  state.counters["isize"] = seg.getIndexSize();
  // state.counters["hits"] = hits; // just a check to see if things are working correctly
}


BENCHMARK_CAPTURE(BM_TermFind, smallHit, 4000000000, 100);
BENCHMARK_CAPTURE(BM_TermFind, smallMiss, 4000000000, 0);
BENCHMARK_CAPTURE(BM_TermFind, small50, 4000000000, 50);
BENCHMARK_CAPTURE(BM_TermFind, medHit, 2000000000000000000, 100);
BENCHMARK_CAPTURE(BM_TermFind, medMiss, 2000000000000000000, 0);
BENCHMARK_CAPTURE(BM_TermFind, med50, 2000000000000000000, 50);