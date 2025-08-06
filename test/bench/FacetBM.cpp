#include <charconv>
#include <latch>

#include "bench/solux_bench.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"

using namespace solux;
using namespace solux::test;

// buildBenchIndex is declared in solux_bench.h


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
    buildBenchIndex(helper, nDocs, docsPerSeg);
  }

  RSSWatcher watcher;

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
    auto& topDocsOps = *topDocs.mutable_ops();
    auto& facet = *topDocsOps["f"].mutable_field_facet();
    facet.set_field(ffield);
    facet.set_limit(5);

    lreq->engine.submit(*lreq, para);
    // LOG_DEBUG("ENGINE REQ: {}", lreq->toString());
    // add all the facet counts into the fingerprint "ret"
    // auto& facetResult = lreq->responses[0]->proto.ops().at("f").facet();  // top level facets
    auto& facetResult = lreq->responses[0]->proto.ops().at("q").docs().ops().at("f").facet();
    auto& counts = facetResult.counts();
    if (facetResult.bucket_ids().has_col_i()) {
      auto& bucketIds = facetResult.bucket_ids().col_i();
      for (int i = 0; i < facetResult.counts().size(); i++) {
        ret = ret * 31 + bucketIds.v(i) * counts[i];
      }
    } else if (facetResult.bucket_ids().has_col_s()) {
      auto& bucketIds = facetResult.bucket_ids().col_s();
      for (int i = 0; i < facetResult.counts().size(); i++) {
        ret = ret * 31 + std::hash<std::string_view>()(bucketIds.v(i)) * counts[i];
      }
    } else if (facetResult.bucket_ids().has_multi_i()) {
      auto& bucketIds = facetResult.bucket_ids().multi_i();
      for (int i = 0; i < facetResult.counts().size(); i++) {
        ret = ret * 31 + bucketIds.v(i).v(0) * counts[i];
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
  auto mem = watcher.getDeltaKB();
  state.counters["RSS_delta"] = mem.first / 1024;
  state.counters["RSS_max"] = mem.second / 1024;
}


constexpr int32_t nDocs = 10'000'000;
constexpr const char* shape = "9555"; // 9 segments, 555 docs per segment

SOLUX_BENCHMARK_CAPTURE(BM_Facet, u10_i,            nDocs, shape, "all", "u10_i", false);
SOLUX_BENCHMARK_CAPTURE(BM_Facet, u10_i_para,       nDocs, shape, "all", "u10_i", true);
SOLUX_BENCHMARK_CAPTURE(BM_Facet, u10k_i,            nDocs, shape, "all", "u10k_i", false);
SOLUX_BENCHMARK_CAPTURE(BM_Facet, u10k_i_para,       nDocs, shape, "all", "u10k_i", true);
SOLUX_BENCHMARK_CAPTURE(BM_Facet, u10m_i,            nDocs, shape, "all", "u10m_i", false);
SOLUX_BENCHMARK_CAPTURE(BM_Facet, u10m_i_para,       nDocs, shape, "all", "u10m_i", true);
SOLUX_BENCHMARK_CAPTURE(BM_Facet, short_u10k_s,      nDocs, shape, "all", "short_u10k_s", false);
SOLUX_BENCHMARK_CAPTURE(BM_Facet, short_u10k_s_para, nDocs, shape, "all", "short_u10k_s", true);
SOLUX_BENCHMARK_CAPTURE(BM_Facet, med_u10k_s,        nDocs, shape, "all", "med_u10k_s", false);
SOLUX_BENCHMARK_CAPTURE(BM_Facet, med_u10k_s_para,   nDocs, shape, "all", "med_u10k_s", true);
SOLUX_BENCHMARK_CAPTURE(BM_Facet, short_u1m_s,       nDocs, shape, "all", "short_u1m_s", false);
SOLUX_BENCHMARK_CAPTURE(BM_Facet, short_u1m_s_para,  nDocs, shape, "all", "short_u1m_s", true);
SOLUX_BENCHMARK_CAPTURE(BM_Facet, med_u1m_s,         nDocs, shape, "all", "med_u1m_s", false);
SOLUX_BENCHMARK_CAPTURE(BM_Facet, med_u1m_s_para,    nDocs, shape, "all", "med_u1m_s", true);

// test tiny domain
SOLUX_BENCHMARK_CAPTURE(BM_Facet, tinyD_u10_i,            nDocs, shape, "short_u1m_s", "u10_i", false);
SOLUX_BENCHMARK_CAPTURE(BM_Facet, tinyD_u10_i,            nDocs, shape, "short_u1m_s", "u10_i", true);
