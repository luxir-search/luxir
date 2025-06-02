#include <charconv>
#include <latch>

#include "bench/solux_bench.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"

using namespace solux;
using namespace solux::test;

//
// Facet code currently hooks in at the SearchEngine level, which currently uses protobuf input and output.
// For this reason, we need to interface at a higher level (CollectionHelper) than test::TestIndex for benchmarking.
// TODO: optionally go through grpc for searching to see how much overhead that adds.
//
static void BM_FacetIntCol(benchmark::State& state, int64_t nDocs, std::string_view shape, int64_t maxVal, bool para) {
  int mergeFactor = 10;  // TODO: actually get from IW?

  if (solux::unit_tests) {
    nDocs = 1000;
  }

  //
  // Figure out how many docs in each segment we want.
  //
  std::vector<int32_t> docsPerSeg;
  CollectionHelper::calcSegSizes(nDocs, mergeFactor, shape, docsPerSeg);

  // std::println(std::cout, "TARGET index shape: {}", docsPerSeg);

  Rng rng(0);
  test::CollectionHelper helper;
  helper.clear();

#ifdef REMOVED
  // real    0m3.207s   #1'000'000 docs, 9 segments, 1000 maxVal
  // user    0m59.540s
  // sys     0m36.047s
  for (int segnum=0; segnum<nSegs; segnum++) {
    int segDocs = docsPerSeg[segnum];
    for (int i=0; i<segDocs; i++) {
      // add some random values to the index
      auto foo_i = rng.rint(maxVal);
      helper.index(flatdoc("foo_i", foo_i));
    }
    helper.commit();
  }
#endif


  // drop down to inverter / IndexHandler level to build the index faster.
  // real    0m0.032s  #1'000'000 docs, 9 segments, 1000 maxVal
  // user    0m0.050s
  // sys     0m0.060s
  auto iw = helper.getIndexWriter();
  for (size_t segnum=0; segnum<docsPerSeg.size(); segnum++) {
    int segDocs = docsPerSeg[segnum];
    Inverter& inverter = iw->obtainInverter();
    Inverter::IndexHandler& handler = inverter.getIndexHandler("foo_i");
    for (int i=0; i<segDocs; i++) {
      // add some random values to the index
      auto foo_i = rng.rint(maxVal);
      inverter.startDoc();
      handler.index(inverter, foo_i);
      inverter.finishDoc();
    }
    iw->releaseInverter(inverter);
    helper.commit();
  }


  // get the IndexReader
  auto reader = iw->getIndexReader();
  int readerSegs = reader->segments().size();
  ASSERT_EQ(readerSegs, docsPerSeg.size());
  // check each segment size
  for (size_t i=0; i<docsPerSeg.size(); i++) {
    auto& seg = reader->segments()[i];
    ASSERT_EQ(seg.postingsReader().numDocs(), docsPerSeg[i]);
  }


  int64_t ret;
  for (auto _ : state) {
    ret = 0;

    auto* lreq = LocalReq::create(SoluxTest::soluxNode->getSearchEngine());
    lreq->proto.mutable_collection()->add_name("main");
    lreq->proto.set_request_id("myrequestid");
    // match all docs query
    auto& ops = *lreq->proto.mutable_ops();
    auto& topDocs = *ops["q"].mutable_top_docs();
    topDocs.mutable_query()->set_all(true);
    topDocs.set_get_number(true);
    topDocs.set_get_scores(false);
    // add the field we want to facet
    auto& facet = *ops["f"].mutable_field_facet();
    facet.set_field("foo_i");
    facet.set_limit(5);

    lreq->engine.submit(*lreq, para);
    // LOG_DEBUG("ENGINE REQ: {}", lreq->toString());
    // add all the facet counts into the fingerprint "ret"
    auto& facetResult = lreq->responses[0]->proto.ops().at("f").facet();
    auto& bucketIds = facetResult.bucket_ids().col_i();
    auto& counts = facetResult.counts();
    for (int i = 0; i < facetResult.counts().size(); i++) {
      ret += bucketIds.v(i) * counts[i];
    }

    lreq->done();
    benchmark::DoNotOptimize(ret);
  }

  state.counters["fp"] = ret;  // sanity check.
  state.counters["rate"] = benchmark::Counter(state.iterations(),benchmark::Counter::kIsRate);
}



// When we test sparse sets for performance, the most interesting case is when it's still a bitset in the block.
// Search code will spend much less time in very sparse sets.
constexpr int32_t nDocs = 10'000'000;
// constexpr int32_t nDocs = 10;

// static void BM_FacetIntCol(benchmark::State& state, int64_t nDocs, std::string_view shape, int64_t maxVal) {
BENCHMARK_CAPTURE(BM_FacetIntCol, basic, nDocs, "9555", 10000, false); // docs are dense, iterate over all values
BENCHMARK_CAPTURE(BM_FacetIntCol, basic_para, nDocs, "9555", 10000, true); // docs are dense, iterate over all values
// TODO: how to share the same index across multiple benchmarks?