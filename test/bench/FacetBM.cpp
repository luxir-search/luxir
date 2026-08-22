#include <charconv>
#include <latch>
#include <variant>

#include <tbb/task_group.h>

#include "bench/luxir_bench.h"
#include "luxir/search/ops/FacetOp.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/SchemaBuilder.h"

using namespace luxir;
using namespace luxir::test;

// buildBenchIndex is declared in luxir_bench.h


//
// TODO: optionally go through grpc for searching to see how much overhead that adds.
//
static void BM_Facet(benchmark::State& state, int64_t nDocs, std::string_view shape, std::string_view qfield, std::string_view ffield, bool para, int64_t topLimit = 10) {
  int mergeFactor = 10;  // TODO: actually get from IW?

  if (luxir::unit_tests) {
    nDocs = LuxirTest::scaleTestWork(200);
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
  int64_t domainSize = -1;
  for (auto _ : state) {
    int64_t ret = 0;

    auto req = localReq(LuxirTest::luxirNode->getSearchEngine());
    req->collection("main");
    // match all docs query
    auto& topDocs = req->topDocs("q");

    if (qfield == "all") {
      topDocs.allQuery();
    } else if (qfield == "1%") {
      // No field has 100 distinct values, but sparse_u1k_s is populated on
      // exactly 1% of docs, so exists over it is a ~1% domain for the cost of
      // one docs-with-value bitset.  Intersecting two 10-valued fields reaches
      // the same domain but walks 2M postings to do it, which then dominates
      // the measurement.  This domain is sparse enough to stay an ARRAY (point
      // decoding) and large enough to measure; tinyD is 6 docs.
      topDocs.exprQuery("sparse_u1k_s:*");
    } else {
      topDocs.matchQuery(qfield, "0");
    }
    topDocs.limit(topLimit).getNumber(true).getScores(false).fields({"id"});

    // add the field we want to facet
    auto& facet = topDocs.facet("f", ffield);
    facet.limit(5);

    req->execute(para);
    // LOG_DEBUG("ENGINE REQ: {}", req->toString());
    // add all the facet counts into the fingerprint "ret"
    const auto* qDocs = req->responses[0]->proto.ops.at("q")->docList();
    const auto* facetResult = qDocs->ops.at("f")->facetResult();
    const auto& counts = facetResult->counts;
    if (std::holds_alternative<luxir::api::ColInt>(facetResult->bucket_ids->kind)) {
      const auto& bucketIds = std::get<luxir::api::ColInt>(facetResult->bucket_ids->kind);
      for (int i = 0; i < (int)counts.size(); i++) {
        ret = ret * 31 + bucketIds.v[i] + counts[i];
      }
    } else if (std::holds_alternative<luxir::api::ColStr>(facetResult->bucket_ids->kind)) {
      const auto& bucketIds = std::get<luxir::api::ColStr>(facetResult->bucket_ids->kind);
      for (int i = 0; i < (int)counts.size(); i++) {
        ret = ret * 31 + java_string_hashcode(bucketIds.v[i]) + counts[i];
      }
    } else if (std::holds_alternative<luxir::api::ArrArrInt>(facetResult->bucket_ids->kind)) {
      const auto& bucketIds = std::get<luxir::api::ArrArrInt>(facetResult->bucket_ids->kind);
      for (int i = 0; i < (int)counts.size(); i++) {
        ret = ret * 31 + bucketIds.v[i].v[0] + counts[i];
      }
    } else {
      LOG_ERROR("Unknown bucket ids type in facet result: {}", "<unknown>");
    }


    benchmark::DoNotOptimize(ret);

    domainSize = qDocs->found.value_or(-1);
    if (fp != -1) {
      ASSERT_EQ(fp, ret); // sanity check that we get the same result every time.
    }
    fp = ret;  // save the fingerprint for the next iteration
  }

  // Matching docs, so each row says which domain size it actually measured
  // rather than leaving it implied by the query field's cardinality.
  state.counters["domain"] = (double)domainSize;
  state.counters["fp"] = fp;  // sanity check.
  state.counters["reused"] = reuseIndex;; // did we reuse the index?
  state.counters["rate"] = benchmark::Counter(state.iterations(),benchmark::Counter::kIsRate);
  auto mem = watcher.getDeltaKB();
  state.counters["RSS_delta"] = mem.first / 1024;
  state.counters["RSS_max"] = mem.second / 1024;
}

static void buildRangeFacetBenchIndex(CollectionHelper& helper,
                                      std::span<const int32_t> docsPerSeg) {
  helper.clear();
  SchemaBuilder b;
  auto& field = b.field("range_bm_i");
  field.type = api::FieldDef::FieldClass::INT;
  field.index = api::FieldDef::IndexMode::RANGE;
  b.set(helper.collection());

  auto iw = helper.getIndexWriter();
  std::vector<Inverter*> inverters;
  std::vector<int64_t> starts;
  int64_t start = 0;
  for (int32_t docs : docsPerSeg) {
    inverters.push_back(&iw->obtainInverter());
    starts.push_back(start);
    start += docs;
  }
  tbb::task_group tg;
  for (size_t seg = 0; seg < docsPerSeg.size(); seg++) {
    tg.run([&, seg]() {
      Inverter& inverter = *inverters[seg];
      auto& id = inverter.getIndexHandler("id");
      auto& value = inverter.getIndexHandler("range_bm_i");
      int64_t doc = starts[seg];
      for (int32_t i = 0; i < docsPerSeg[seg]; i++, doc++) {
        inverter.startDoc();
        id.index(inverter, std::to_string(doc));
        value.index(inverter, doc % 10'000 * 1000);
        inverter.finishDoc();
      }
      iw->releaseInverter(inverter, true);
    });
  }
  tg.wait();
  helper.commit();
}

static void BM_RangeFacet(benchmark::State& state, int64_t nDocs,
                          std::string_view shape, bool forceWalk) {
  if (luxir::unit_tests) nDocs = LuxirTest::scaleTestWork(200);
  std::vector<int32_t> docsPerSeg;
  CollectionHelper::calcSegSizes(nDocs, 10, shape, docsPerSeg);
  CollectionHelper helper("facet_range_bm");
  bool reuseIndex = helper.indexMatchesShape(docsPerSeg);
  if (!reuseIndex) buildRangeFacetBenchIndex(helper, docsPerSeg);

  struct HookGuard {
    bool saved = IntFacetRangeReq::disablePointsRangeFacetForTests;
    ~HookGuard() { IntFacetRangeReq::disablePointsRangeFacetForTests = saved; }
  } hookGuard;
  IntFacetRangeReq::disablePointsRangeFacetForTests = forceWalk;
  int64_t fingerprint = -1;
  for (auto _ : state) {
    auto req = localReq(LuxirTest::luxirNode->getSearchEngine());
    req->collection("facet_range_bm");
    req->rangeFacet("f", "range_bm_i").range(-500, 10'000'500, 100'003);
    req->execute(false);
    const auto* result = req->responses[0]->proto.ops.at("f")->facetResult();
    int64_t current = 0;
    const auto& bounds = std::get<api::ArrArrInt>(result->bucket_ids->kind).v;
    for (size_t i = 0; i < result->counts.size(); i++) {
      current = current * 31 + bounds[i].v[0] + result->counts[i];
    }
    benchmark::DoNotOptimize(current);
    if (fingerprint != -1) {
      ASSERT_EQ(fingerprint, current);
    }
    fingerprint = current;
  }
  state.counters["fp"] = fingerprint;
  state.counters["reused"] = reuseIndex;
  state.counters["rate"] = benchmark::Counter(
      state.iterations(), benchmark::Counter::kIsRate);
}


constexpr int32_t nDocs = 10'000'000;
constexpr const char* shape = "9555"; // 9 segments, 555 docs per segment

LUXIR_BENCHMARK_CAPTURE(BM_Facet, u10_i,          nDocs, shape, "all", "u10_i", false);
LUXIR_BENCHMARK_CAPTURE(BM_Facet, u10_i_para,     nDocs, shape, "all", "u10_i", true);
LUXIR_BENCHMARK_CAPTURE(BM_Facet, u10k_i,         nDocs, shape, "all", "u10k_i", false);
LUXIR_BENCHMARK_CAPTURE(BM_Facet, u10k_i_para,    nDocs, shape, "all", "u10k_i", true);
LUXIR_BENCHMARK_CAPTURE(BM_Facet, u10m_i,         nDocs, shape, "all", "u10m_i", false);
LUXIR_BENCHMARK_CAPTURE(BM_Facet, u10m_i_para,    nDocs, shape, "all", "u10m_i", true);
// Multi-valued (1 value per doc, 2 on 1/64 of them).  Pairs with u10_i: same
// cardinality and domain, so the difference between the two rows is the cost
// of the multi-valued path - the endValueRank mono column read twice per doc.
LUXIR_BENCHMARK_CAPTURE(BM_Facet, u10_is,         nDocs, shape, "all", "u10_is", false);
LUXIR_BENCHMARK_CAPTURE(BM_Facet, u10_is_para,    nDocs, shape, "all", "u10_is", true);
LUXIR_BENCHMARK_CAPTURE(BM_Facet, u10k_s,         nDocs, shape, "all", "short_u10k_s", false);
LUXIR_BENCHMARK_CAPTURE(BM_Facet, u10k_s_para,    nDocs, shape, "all", "short_u10k_s", true);
LUXIR_BENCHMARK_CAPTURE(BM_Facet, u100k_s,        nDocs, shape, "all", "short_u100k_s", false);
LUXIR_BENCHMARK_CAPTURE(BM_Facet, u100k_s_para,   nDocs, shape, "all", "short_u100k_s", true);
LUXIR_BENCHMARK_CAPTURE(BM_Facet, u1m_s,          nDocs, shape, "all", "short_u1m_s", false);
LUXIR_BENCHMARK_CAPTURE(BM_Facet, u1m_s_para,     nDocs, shape, "all", "short_u1m_s", true);


// test different domain sizes
LUXIR_BENCHMARK_CAPTURE(BM_Facet, tinyD_u10_i,      nDocs, shape, "short_u1m_s", "u10_i", false);
LUXIR_BENCHMARK_CAPTURE(BM_Facet, tinyD_u10_i,      nDocs, shape, "short_u1m_s", "u10_i", true);
LUXIR_BENCHMARK_CAPTURE(BM_Facet, bigD_u10_i,       nDocs, shape, "short_u10_s", "u10_i", false);
LUXIR_BENCHMARK_CAPTURE(BM_Facet, bigD_u10_i,       nDocs, shape, "short_u10_s", "u10_i", true);
LUXIR_BENCHMARK_CAPTURE(BM_Facet, tinyD_u10_is,     nDocs, shape, "short_u1m_s", "u10_is", false);
LUXIR_BENCHMARK_CAPTURE(BM_Facet, bigD_u10_is,      nDocs, shape, "short_u10_s", "u10_is", false);
// ~1% domain: the sparse point-decode path at a size worth measuring.  tinyD
// matches ~10 docs and bigD is dense enough to take the bulk path.
LUXIR_BENCHMARK_CAPTURE(BM_Facet, midD_u10_i,       nDocs, shape, "1%", "u10_i", false);
LUXIR_BENCHMARK_CAPTURE(BM_Facet, midD_u10_is,      nDocs, shape, "1%", "u10_is", false);
// u10_i needs 4 bits, which the old lane-packed select never straddles a word
// for, so it had nothing to lose there.  u10k_i needs ~14, where roughly a
// third of values cross a 32-bit lane boundary and the old select paid a
// second load and a stitch branch - this is where the single-load point read
// should show up on a real column.
LUXIR_BENCHMARK_CAPTURE(BM_Facet, midD_u10k_i,      nDocs, shape, "1%", "u10k_i", false);
LUXIR_BENCHMARK_CAPTURE(BM_Facet, midD_u10k_s,      nDocs, shape, "1%", "short_u10k_s", false);
LUXIR_BENCHMARK_CAPTURE(BM_Facet, bigD_u10_s,       nDocs, shape, "short_u10_s", "med_u10_s", false);
LUXIR_BENCHMARK_CAPTURE(BM_Facet, bigD_u10_s,       nDocs, shape, "short_u10_s", "med_u10_s", true);
LUXIR_BENCHMARK_CAPTURE(BM_Facet, bigD_u10k_s,      nDocs, shape, "short_u10_s", "short_u10k_s", false);
LUXIR_BENCHMARK_CAPTURE(BM_Facet, bigD_u10k_s,      nDocs, shape, "short_u10_s", "short_u10k_s", true);
LUXIR_BENCHMARK_CAPTURE(BM_Facet, bigD_u100k_s,     nDocs, shape, "short_u10_s", "short_u100k_s", false);
LUXIR_BENCHMARK_CAPTURE(BM_Facet, bigD_u100k_s,     nDocs, shape, "short_u10_s", "short_u100k_s", true);
LUXIR_BENCHMARK_CAPTURE(BM_Facet, bigD_u1m_s,       nDocs, shape, "short_u10_s", "short_u1m_s", false);
LUXIR_BENCHMARK_CAPTURE(BM_Facet, bigD_u1m_s,       nDocs, shape, "short_u10_s", "short_u1m_s", true);

// Sparse field (~1% of docs have a value).  Doc-driven counting walks the whole
// domain regardless, so total postings stay far below the domain size - the one
// shape where per-term postings intersection should beat the column walk.
LUXIR_BENCHMARK_CAPTURE(BM_Facet, bigD_sparse_s,    nDocs, shape, "short_u10_s", "sparse_u1k_s", false);
LUXIR_BENCHMARK_CAPTURE(BM_Facet, tinyD_sparse_s,   nDocs, shape, "short_u1m_s", "sparse_u1k_s", false);
LUXIR_BENCHMARK_CAPTURE(BM_Facet, sparse_s,         nDocs, shape, "all", "sparse_u1k_s", false);

// limit=0: facet-only requests that return no documents, the shape a facet
// benchmark actually issues.  Worth its own cells because limit>0 and limit=0
// take different collection paths: with no top-K to fill, everything the
// request costs is domain and facet work.
LUXIR_BENCHMARK_CAPTURE(BM_Facet, lim0_u10_i,       nDocs, shape, "all", "u10_i", false, 0);
LUXIR_BENCHMARK_CAPTURE(BM_Facet, lim0_u1m_s,       nDocs, shape, "all", "short_u1m_s", false, 0);
LUXIR_BENCHMARK_CAPTURE(BM_Facet, lim0_bigD_u10_i,  nDocs, shape, "short_u10_s", "u10_i", false, 0);

LUXIR_BENCHMARK_CAPTURE(BM_RangeFacet, points,      nDocs, shape, false);
LUXIR_BENCHMARK_CAPTURE(BM_RangeFacet, forced_walk, nDocs, shape, true);
