#include <charconv>

#include "bench/solux_bench.h"
#include "test/SegmentTest.h"
#include "solux/search/PostingsReader.h"
#include "solux/index/PostingsWriter.h"


using namespace solux;

static void BM_TermFind(benchmark::State& state) {
  int nPosPerDoc=1;
  // int nTerms=1000000;
  int nTerms=1000;
  uint64_t maxId=4000000000;
  auto fname = "myfield";

  SegmentTest seg;
  seg.initWriter();
  MemPool& pool = seg.pool;

  std::unordered_set<int64_t> idSet(nTerms);  // used to ensure terms are unique... TODO: replace with something better like flat hash map
  std::vector<PackedTerm> terms;
  terms.reserve(nTerms);
  SplitMix64 r(1);
  std::string termStr;
  termStr.resize(21);
  while ((int)terms.size() < nTerms) {
    int64_t id = (r() & 0x7fffffffffffffff) % maxId;
    auto [iter, inserted] = idSet.insert(id);
    if (!inserted) continue; // a repeat
    auto [end, ec] = std::to_chars(termStr.data(), termStr.data() + 21, id);
    terms.emplace_back(seg.pool, termStr.data(), end-termStr.data());
  }

  // indirectly sort the terms so we can use the index as the docid
  std::vector<int> sorted(terms.size());
  std::iota( std::begin(sorted), std::end(sorted), 0 );
  std::sort( std::begin(sorted), std::end(sorted),
             [&terms] (int i, int j) { return terms[i] < terms[j]; } );

  PostingsWriter& w = *seg.writer;
  w.startField(fname);
  int64_t fp = 0;
  for (auto docid : sorted) {
    auto term = terms[docid];
    w.startTerm(term);
    fp += term.size();
    w.startDoc(docid);
    fp += docid;
    for (int j=0; j<nPosPerDoc; j++) {
      w.addPositionDelta(1);
    }
    w.endDoc(docid);
    w.endTerm(term);
  }
  w.endField(fname);

  seg.initReader();
  PostingsReader& postingsReader = *seg.reader;
  TermIndexReader& tindexReader = *seg.tindexReader;
  ASSERT_EQ(true, tindexReader.readNextField());
  ASSERT_EQ(fname, seg.tindexReader->name());
  TermsEnum tenum(pool, postingsReader, tindexReader);

  int64_t fingerprint = 0;
  for (auto _ : state) {
    fingerprint = 0;
    for (int i=0; i<nTerms; i++) {
      // go through in unsorted term order
      PackedTerm term = terms[i];
      bool found = tenum.seek((std::string_view)term);
      if (found) {
        fingerprint += tenum.term().size();
        DocsEnum docsEnum(pool, postingsReader, tindexReader, tenum);
        auto df = docsEnum.numDocs();
        for (int j=0; j<df; j++) {
          auto doc = docsEnum.nextDoc();
          fingerprint += doc;
        }
      }
    }
    benchmark::DoNotOptimize(fingerprint);
    benchmark::ClobberMemory();
    ASSERT_EQ(fingerprint, fp);
  }

  // high nTerms means a real benchmark, so print out more info.
  if (nTerms >= 1000000) {
    std::cout << "fingerprint=" << fingerprint << " nTerms=" << nTerms << std::endl;
  }
  // state.counters["terms"] = tterms;
}


BENCHMARK(BM_TermFind);
