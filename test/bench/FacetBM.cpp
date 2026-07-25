#include <charconv>
#include <latch>
#include <variant>

#include <tbb/task_group.h>

#include "bench/solux_bench.h"
#include "solux/search/ops/FacetOp.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/SchemaBuilder.h"

using namespace solux;
using namespace solux::test;

// buildBenchIndex is declared in solux_bench.h


//
// TODO: optionally go through grpc for searching to see how much overhead that adds.
//
static void BM_Facet(benchmark::State& state, int64_t nDocs, std::string_view shape, std::string_view qfield, std::string_view ffield, bool para) {
  int mergeFactor = 10;  // TODO: actually get from IW?

  if (solux::unit_tests) {
    nDocs = SoluxTest::scaleTestWork(200);
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

    auto req = localReq(SoluxTest::soluxNode->getSearchEngine());
    req->collection("main");
    // match all docs query
    auto& topDocs = req->topDocs("q");

    if (qfield == "all") {
      topDocs.allQuery();
    } else {
      topDocs.matchQuery(qfield, "0");
    }
    topDocs.getNumber(true).getScores(false);

    // add the field we want to facet
    auto& facet = topDocs.facet("f", ffield);
    facet.limit(5);

    req->execute(para);
    // LOG_DEBUG("ENGINE REQ: {}", req->toString());
    // add all the facet counts into the fingerprint "ret"
    const auto* qDocs = req->responses[0]->proto.ops.at("q")->docList();
    const auto* facetResult = qDocs->ops.at("f")->facetResult();
    const auto& counts = facetResult->counts;
    if (std::holds_alternative<solux::api::ColInt>(facetResult->bucket_ids->kind)) {
      const auto& bucketIds = std::get<solux::api::ColInt>(facetResult->bucket_ids->kind);
      for (int i = 0; i < (int)counts.size(); i++) {
        ret = ret * 31 + bucketIds.v[i] + counts[i];
      }
    } else if (std::holds_alternative<solux::api::ColStr>(facetResult->bucket_ids->kind)) {
      const auto& bucketIds = std::get<solux::api::ColStr>(facetResult->bucket_ids->kind);
      for (int i = 0; i < (int)counts.size(); i++) {
        ret = ret * 31 + java_string_hashcode(bucketIds.v[i]) + counts[i];
      }
    } else if (std::holds_alternative<solux::api::ArrArrInt>(facetResult->bucket_ids->kind)) {
      const auto& bucketIds = std::get<solux::api::ArrArrInt>(facetResult->bucket_ids->kind);
      for (int i = 0; i < (int)counts.size(); i++) {
        ret = ret * 31 + bucketIds.v[i].v[0] + counts[i];
      }
    } else {
      LOG_ERROR("Unknown bucket ids type in facet result: {}", "<unknown>");
    }


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
  if (solux::unit_tests) nDocs = SoluxTest::scaleTestWork(200);
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
    auto req = localReq(SoluxTest::soluxNode->getSearchEngine());
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

SOLUX_BENCHMARK_CAPTURE(BM_Facet, u10_i,          nDocs, shape, "all", "u10_i", false);
SOLUX_BENCHMARK_CAPTURE(BM_Facet, u10_i_para,     nDocs, shape, "all", "u10_i", true);
SOLUX_BENCHMARK_CAPTURE(BM_Facet, u10k_i,         nDocs, shape, "all", "u10k_i", false);
SOLUX_BENCHMARK_CAPTURE(BM_Facet, u10k_i_para,    nDocs, shape, "all", "u10k_i", true);
SOLUX_BENCHMARK_CAPTURE(BM_Facet, u10m_i,         nDocs, shape, "all", "u10m_i", false);
SOLUX_BENCHMARK_CAPTURE(BM_Facet, u10m_i_para,    nDocs, shape, "all", "u10m_i", true);
SOLUX_BENCHMARK_CAPTURE(BM_Facet, u10k_s,         nDocs, shape, "all", "short_u10k_s", false);
SOLUX_BENCHMARK_CAPTURE(BM_Facet, u10k_s_para,    nDocs, shape, "all", "short_u10k_s", true);
SOLUX_BENCHMARK_CAPTURE(BM_Facet, u100k_s,        nDocs, shape, "all", "short_u100k_s", false);
SOLUX_BENCHMARK_CAPTURE(BM_Facet, u100k_s_para,   nDocs, shape, "all", "short_u100k_s", true);
SOLUX_BENCHMARK_CAPTURE(BM_Facet, u1m_s,          nDocs, shape, "all", "short_u1m_s", false);
SOLUX_BENCHMARK_CAPTURE(BM_Facet, u1m_s_para,     nDocs, shape, "all", "short_u1m_s", true);


// test different domain sizes
SOLUX_BENCHMARK_CAPTURE(BM_Facet, tinyD_u10_i,      nDocs, shape, "short_u1m_s", "u10_i", false);
SOLUX_BENCHMARK_CAPTURE(BM_Facet, tinyD_u10_i,      nDocs, shape, "short_u1m_s", "u10_i", true);
SOLUX_BENCHMARK_CAPTURE(BM_Facet, bigD_u10_i,       nDocs, shape, "short_u10_s", "u10_i", false);
SOLUX_BENCHMARK_CAPTURE(BM_Facet, bigD_u10_i,       nDocs, shape, "short_u10_s", "u10_i", true);
SOLUX_BENCHMARK_CAPTURE(BM_Facet, bigD_u10_s,       nDocs, shape, "short_u10_s", "med_u10_s", false);
SOLUX_BENCHMARK_CAPTURE(BM_Facet, bigD_u10_s,       nDocs, shape, "short_u10_s", "med_u10_s", true);
SOLUX_BENCHMARK_CAPTURE(BM_Facet, bigD_u10k_s,      nDocs, shape, "short_u10_s", "short_u10k_s", false);
SOLUX_BENCHMARK_CAPTURE(BM_Facet, bigD_u10k_s,      nDocs, shape, "short_u10_s", "short_u10k_s", true);
SOLUX_BENCHMARK_CAPTURE(BM_Facet, bigD_u100k_s,     nDocs, shape, "short_u10_s", "short_u100k_s", false);
SOLUX_BENCHMARK_CAPTURE(BM_Facet, bigD_u100k_s,     nDocs, shape, "short_u10_s", "short_u100k_s", true);
SOLUX_BENCHMARK_CAPTURE(BM_Facet, bigD_u1m_s,       nDocs, shape, "short_u10_s", "short_u1m_s", false);
SOLUX_BENCHMARK_CAPTURE(BM_Facet, bigD_u1m_s,       nDocs, shape, "short_u10_s", "short_u1m_s", true);

SOLUX_BENCHMARK_CAPTURE(BM_RangeFacet, points,      nDocs, shape, false);
SOLUX_BENCHMARK_CAPTURE(BM_RangeFacet, forced_walk, nDocs, shape, true);
