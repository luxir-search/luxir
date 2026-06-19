#include <algorithm>
#include <charconv>
#include <cmath>
#include <string>
#include <vector>

#include <tbb/task_group.h>

#include "bench/solux_bench.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"

using namespace solux;
using namespace solux::test;

//
// Full-text field faceting benchmark.
//
// FacetBM already covers faceting on synthetic _s / _i column fields.  This
// benchmark targets the path FacetBM cannot reach: faceting ON a full-text field
// (FullTextFacetReq, term-driven counting over the inverted index), across a
// range of domain densities.
//
// The domain is produced by a full-text query, and the queried term's document
// frequency sets the domain size.  body_w is Zipfian over a 50K vocab (rank 0 =
// most frequent token), so the query term string dials density directly:
//
//   "all"    match-all          -> 1M docs
//   "t0"     head term (dense)   -> ~790K docs (many full 64K roaring buckets)
//   "t100"   mid term            -> ~16K docs
//   "t10000" tail term (sparse)  -> ~170 docs (sparse ArrDocSet)
//
// 1M docs in shape "5555" expands to ~5 segments of ~181K docs down to tiny
// sparse segments, so the per-segment count + cross-segment merge is exercised
// across every container regime.
//

namespace {

// Precomputed Zipf CDF: rank r (1-based) has weight 1/r^s.  O(vocab) to build,
// O(log vocab) per draw.  Shared read-only across the parallel segment builders.
struct ZipfTable {
  std::vector<double> cdf;  // cumulative weights, cdf.back() == total
  double total = 0;

  ZipfTable(int vocab, double s) {
    cdf.resize(vocab);
    double sum = 0;
    for (int r = 1; r <= vocab; r++) {
      sum += 1.0 / std::pow((double)r, s);
      cdf[r - 1] = sum;
    }
    total = sum;
  }

  // Map a uniform 64-bit value to a Zipf-distributed rank in [0, vocab).
  // Rank 0 is the most frequent token, so "t0" is the densest query term.
  int sample(uint64_t bits) const {
    double u = (double)(bits >> 11) * 0x1.0p-53;  // [0, 1)
    double target = u * total;
    auto it = std::lower_bound(cdf.begin(), cdf.end(), target);
    int idx = (int)(it - cdf.begin());
    if (idx >= (int)cdf.size()) idx = (int)cdf.size() - 1;
    return idx;
  }
};

constexpr int kBodyVocab = 50000;  // body_w vocabulary size
constexpr double kZipfS = 1.0;     // ~natural-language exponent

}  // namespace

namespace solux {

void buildFullTextBenchIndex(CollectionHelper& helper, int64_t nDocs, std::span<const int32_t> docsPerSeg) {
  unused(nDocs);
  helper.clear();

  // Built once; persists for the process.  ~400KB of doubles, read-only below.
  static const ZipfTable bodyZipf(kBodyVocab, kZipfS);

  auto iw = helper.getIndexWriter();

  // Pre-obtain all inverters we need for parallel segment building.
  std::vector<Inverter*> inverters;
  inverters.reserve(docsPerSeg.size());
  for (size_t i = 0; i < docsPerSeg.size(); i++) {
    inverters.push_back(&iw->obtainInverter());
  }

  // Calculate starting document ID for each segment.
  std::vector<int64_t> segmentStartIds;
  segmentStartIds.reserve(docsPerSeg.size());
  int64_t idNum = 0;
  for (size_t i = 0; i < docsPerSeg.size(); i++) {
    segmentStartIds.push_back(idNum);
    idNum += docsPerSeg[i];
  }

  tbb::task_group tg;
  for (size_t segnum = 0; segnum < docsPerSeg.size(); segnum++) {
    tg.run([&, segnum]() {
      int segDocs = docsPerSeg[segnum];
      Inverter& inverter = *inverters[segnum];
      int64_t localIdNum = segmentStartIds[segnum];

      Inverter::IndexHandler& hId   = inverter.getIndexHandler("id");
      Inverter::IndexHandler& hBody = inverter.getIndexHandler("body_w");

      std::string body;
      for (int i = 0; i < segDocs; i++) {
        SplitMix64 r(localIdNum);  // make each doc predictable

        inverter.startDoc();
        hId.index(inverter, std::to_string(localIdNum++));

        // Tweet-length body: 8..30 Zipfian tokens "t<rank>" joined by spaces.
        int nTok = 8 + (int)r.rint(23);
        body.resize(0);
        for (int t = 0; t < nTok; t++) {
          if (t) body.push_back(' ');
          body.push_back('t');
          body.append(std::to_string(bodyZipf.sample(r())));
        }
        hBody.index(inverter, body);

        inverter.finishDoc();
      }
      iw->releaseInverter(inverter, true);  // immediately request a flush of the segment.
    });
  }
  tg.wait();

  helper.commit();

  if (!solux::unit_tests) {
    malloc_trim(0);
    std::println(std::cerr, "Post buildFullTextBenchIndex - Peak RSS: {} KB, current RSS: {} KB", peakRSSKB(), currentRSSKB());
  }
}

}  // namespace solux

//
// Faceting ON the full-text field body_w (FullTextFacetReq, term-driven counting).
// qterm: "all" for a match-all domain, otherwise a body_w term whose document
//        frequency sets the domain density (e.g. "t0" dense, "t10000" sparse).
//
static void BM_FullTextFacet(benchmark::State& state, int64_t nDocs, std::string_view shape,
                             std::string_view qterm, bool para) {
  int mergeFactor = 10;  // TODO: actually get from IW?

  if (solux::unit_tests) {
    nDocs = 200;
  }

  std::vector<int32_t> docsPerSeg;
  CollectionHelper::calcSegSizes(nDocs, mergeFactor, shape, docsPerSeg);

  CollectionHelper helper;
  // The node has a single shared collection, but benchmarks run in a predictable
  // order: these full-text variants are registered consecutively and share one
  // build, and the only other corpora (FacetBM/QueryBM) use a different shape, so
  // a shape match here is always our text corpus.  (If a same-shaped column
  // corpus is ever added, reuse would need a body_w field check too.)
  bool reuseIndex = helper.indexMatchesShape(docsPerSeg);
  if (!reuseIndex) {
    buildFullTextBenchIndex(helper, nDocs, docsPerSeg);
  }

  RSSWatcher watcher;

  int64_t matches = 0;
  int64_t fp = -1;
  for (auto _ : state) {
    int64_t ret = 0;

    auto* lreq = LocalReq::create(SoluxTest::soluxNode->getSearchEngine());
    lreq->proto.mutable_collection()->add_name("main");
    lreq->proto.set_request_id("myrequestid");
    auto& ops = *lreq->proto.mutable_ops();
    auto& topDocs = *ops["q"].mutable_top_docs();

    if (qterm == "all") {
      topDocs.mutable_query()->set_all(true);
    } else {
      auto& matchQuery = *topDocs.mutable_query()->mutable_match();
      matchQuery.set_field("body_w");
      matchQuery.mutable_val()->set_s(std::string(qterm));
    }
    topDocs.set_get_number(true);
    topDocs.set_get_scores(false);

    // facet on the full-text field itself
    auto& topDocsOps = *topDocs.mutable_ops();
    auto& facet = *topDocsOps["f"].mutable_field_facet();
    facet.set_field("body_w");
    facet.set_limit(5);

    lreq->engine.submit(*lreq, para);

    const auto& qDocs = lreq->responses[0]->proto.ops().at("q").docs();
    matches = qDocs.matches();
    auto& facetResult = qDocs.ops().at("f").facet();
    auto& counts = facetResult.counts();
    // Text faceting buckets are terms (col_s).
    if (facetResult.bucket_ids().has_col_s()) {
      auto& bucketIds = facetResult.bucket_ids().col_s();
      for (int i = 0; i < counts.size(); i++) {
        ret = ret * 31 + java_string_hashcode(bucketIds.v(i)) + counts[i];
      }
    } else {
      LOG_ERROR("Unexpected bucket ids type in text facet result: {}", facetResult.bucket_ids().DebugString());
    }

    lreq->done();
    benchmark::DoNotOptimize(ret);

    if (fp != -1) {
      ASSERT_EQ(fp, ret);  // sanity check that we get the same result every time.
    }
    fp = ret;  // save the fingerprint for the next iteration
  }

  state.counters["fp"] = fp;            // sanity check.
  state.counters["matches"] = matches;  // domain size, so density is visible per variant.
  state.counters["reused"] = reuseIndex;
  state.counters["rate"] = benchmark::Counter(state.iterations(), benchmark::Counter::kIsRate);
  auto mem = watcher.getDeltaKB();
  state.counters["RSS_delta"] = mem.first / 1024;
  state.counters["RSS_max"] = mem.second / 1024;
}


constexpr int32_t nDocs = 1'000'000;
constexpr const char* shape = "5555";  // ~5 segs of ~181K docs down to tiny sparse segs

// Faceting ON the full-text field (body_w) via FullTextFacetReq term-driven
// counting, sweeping the domain density through the queried term's docFreq:
// match-all -> head (dense) -> mid -> tail (sparse), serial and parallel.
SOLUX_BENCHMARK_CAPTURE(BM_FullTextFacet, all_body,       nDocs, shape, "all",    false);
SOLUX_BENCHMARK_CAPTURE(BM_FullTextFacet, all_body_para,  nDocs, shape, "all",    true);
SOLUX_BENCHMARK_CAPTURE(BM_FullTextFacet, head_body,      nDocs, shape, "t0",     false);
SOLUX_BENCHMARK_CAPTURE(BM_FullTextFacet, head_body_para, nDocs, shape, "t0",     true);
SOLUX_BENCHMARK_CAPTURE(BM_FullTextFacet, mid_body,       nDocs, shape, "t100",   false);
SOLUX_BENCHMARK_CAPTURE(BM_FullTextFacet, mid_body_para,  nDocs, shape, "t100",   true);
SOLUX_BENCHMARK_CAPTURE(BM_FullTextFacet, tail_body,      nDocs, shape, "t10000", false);
SOLUX_BENCHMARK_CAPTURE(BM_FullTextFacet, tail_body_para, nDocs, shape, "t10000", true);
