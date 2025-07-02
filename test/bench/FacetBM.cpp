#include <charconv>
#include <latch>

#include "bench/solux_bench.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"

using namespace solux;
using namespace solux::test;


static void buildIndex(CollectionHelper& helper, int64_t nDocs, std::span<const int32_t> docsPerSeg) {
  unused(nDocs);
  helper.clear();

#ifdef REMOVED
  // Single Int Field:
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
  // Single Int Field:
  // real    0m0.032s  #1'000'000 docs, 9 segments, 1000 maxVal
  // user    0m0.050s
  // sys     0m0.060s
  auto iw = helper.getIndexWriter();
  for (size_t segnum=0; segnum<docsPerSeg.size(); segnum++) {
    Rng r(segnum);  // make each segment predictable so segment build order doesn't affect the results.
    int segDocs = docsPerSeg[segnum];
    Inverter& inverter = iw->obtainInverter();
    Inverter::IndexHandler& f1 = inverter.getIndexHandler("short_u10_s");
    Inverter::IndexHandler& f2 = inverter.getIndexHandler("short_u10k_s");
    Inverter::IndexHandler& f3 = inverter.getIndexHandler("u10k_i");

    for (int i=0; i<segDocs; i++) {
      // add some random values to the index
      inverter.startDoc();
      f1.index(inverter, std::to_string(r.rint(10)));
      f2.index(inverter, std::to_string(r.rint(10000)));
      f3.index(inverter, r.rint(10000));
      inverter.finishDoc();
    }
    iw->releaseInverter(inverter);
    helper.commit();
  }

}

static void BM_FacetBuildIndex(benchmark::State& state, int64_t nDocs, std::string_view shape) {
  int mergeFactor = 10;  // TODO: actually get from IW?

  if (solux::unit_tests) {
    nDocs = 200;
  }

  test::CollectionHelper helper;
  std::vector<int32_t> docsPerSeg;
  CollectionHelper::calcSegSizes(nDocs, mergeFactor, shape, docsPerSeg);

  for (auto _ : state) {
    buildIndex(helper, nDocs, docsPerSeg);
    benchmark::ClobberMemory();
  }

  // test that index was built correctly
  auto reader = helper.getIndexWriter()->getIndexReader();
  int readerSegs = reader->segments().size();
  ASSERT_EQ(readerSegs, docsPerSeg.size());
  // check each segment size
  for (size_t i=0; i<docsPerSeg.size(); i++) {
    auto& seg = reader->segments()[i];
    ASSERT_EQ(seg.postingsReader().maxDoc(), docsPerSeg[i]);
  }


  auto indexWriter = helper.getIndexWriter();
  auto& dir = indexWriter->dir;
  std::vector<std::string> files;
  dir.listFiles(files);
  // std::println(std::cout, "Index files: {}", files);
  // calculate the total size of the index
  int64_t totalSize = 0;
  for (const auto& file : files) {
    auto f = dir.openFile(file);
    totalSize += f->size();
  }
  // std::println(std::cout, "Total index size: {} bytes", totalSize);

  state.counters["rate"] = benchmark::Counter(state.iterations(),benchmark::Counter::kIsRate);
  state.counters["nSegs"] = readerSegs; // number of segments in the index
  state.counters["indexSz"] = totalSize; // size of the index in bytes
}

//
// TODO: optionally go through grpc for searching to see how much overhead that adds.
//
static void BM_Facet(benchmark::State& state, int64_t nDocs, std::string_view shape, std::string_view qfield, std::string_view ffield, bool para) {
  int mergeFactor = 10;  // TODO: actually get from IW?

  if (solux::unit_tests) {
    nDocs = 200;
  }

  //
  // Figure out how many docs in each segment we want.
  //
  std::vector<int32_t> docsPerSeg;
  CollectionHelper::calcSegSizes(nDocs, mergeFactor, shape, docsPerSeg);

  CollectionHelper helper;
  bool reuseIndex = helper.indexMatchesShape(docsPerSeg);
  if (!reuseIndex) {
    // build the index if it doesn't exist or is not correct.
    buildIndex(helper, nDocs, docsPerSeg);
  }


  int64_t fp = -1;
  for (auto _ : state) {
    int64_t ret = 0;

    auto* lreq = LocalReq::create(SoluxTest::soluxNode->getSearchEngine());
    lreq->proto.mutable_collection()->add_name("main");
    lreq->proto.set_request_id("myrequestid");
    // match all docs query
    auto& ops = *lreq->proto.mutable_ops();
    auto& topDocs = *ops["q"].mutable_top_docs();

    if (qfield == "all") {
      topDocs.mutable_query()->set_all(true);
    } else {
      auto& matchQuery = *topDocs.mutable_query()->mutable_match();
      matchQuery.set_field(qfield);
      matchQuery.mutable_val()->set_s("0");
    }
    topDocs.set_get_number(true);
    topDocs.set_get_scores(false);
    // add the field we want to facet
    auto& facet = *ops["f"].mutable_field_facet();
    facet.set_field(ffield);
    facet.set_limit(5);

    lreq->engine.submit(*lreq, para);
    // LOG_DEBUG("ENGINE REQ: {}", lreq->toString());
    // add all the facet counts into the fingerprint "ret"
    auto& facetResult = lreq->responses[0]->proto.ops().at("f").facet();
    auto& counts = facetResult.counts();
    if (facetResult.bucket_ids().has_col_i()) {
      auto& bucketIds = facetResult.bucket_ids().col_i();
      for (int i = 0; i < facetResult.counts().size(); i++) {
        ret += bucketIds.v(i) * counts[i];
      }
    } else if (facetResult.bucket_ids().has_col_s()) {
      auto& bucketIds = facetResult.bucket_ids().col_s();
      for (int i = 0; i < facetResult.counts().size(); i++) {
        ret += std::hash<std::string_view>()(bucketIds.v(i)) * counts[i];
      }
    } else if (facetResult.bucket_ids().has_multi_i()) {
      auto& bucketIds = facetResult.bucket_ids().multi_i();
      for (int i = 0; i < facetResult.counts().size(); i++) {
        ret += bucketIds.v(i).v(0) * counts[i];
      }
    } else {
      LOG_ERROR("Unknown bucket ids type in facet result: {}", facetResult.bucket_ids().DebugString());
    }


    lreq->done();
    benchmark::DoNotOptimize(ret);

    if (fp != -1) {
      ASSERT_EQ(fp, ret); // sanity check that we get the same result every time.
    }
    fp = ret;  // save the fingerprint for the next iteration
  }

  state.counters["fp"] = fp;  // sanity check.
  state.counters["reused"] = reuseIndex;; // did we reuse the index?
  state.counters["rate"] = benchmark::Counter(state.iterations(),benchmark::Counter::kIsRate);
}



// When we test sparse sets for performance, the most interesting case is when it's still a bitset in the block.
// Search code will spend much less time in very sparse sets.
constexpr int32_t nDocs = 10'000'000;
constexpr const char* shape = "9555"; // 9 segments, 555 docs per segment

BENCHMARK_CAPTURE(BM_FacetBuildIndex, build,              nDocs, shape);
BENCHMARK_CAPTURE(BM_Facet, u10k_i,            nDocs, shape, "all", "u10k_i", false);
BENCHMARK_CAPTURE(BM_Facet, u10k_i_para,       nDocs, shape, "all", "u10k_i", true);
BENCHMARK_CAPTURE(BM_Facet, short_u10k_s,      nDocs, shape, "all", "short_u10k_s", false);
BENCHMARK_CAPTURE(BM_Facet, short_u10k_s_para, nDocs, shape, "all", "short_u10k_s", true);