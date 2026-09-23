// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <charconv>
#include <latch>
#include <variant>

#include <tbb/task_group.h>

#include "bench/luxir_bench.h"
#include "luxir/search/SearchOverrides.h"
#include "luxir/search/ops/FacetOp.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/QueryBuild.h"
#include "test/SchemaBuilder.h"
#include "test/TestUtils.h"

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

enum class AggregateFacetBenchMode {
  BUCKET_DOMAIN,
  INLINE_SORT,
  INLINE_SORT_DENSE,
  INLINE_SORT_SPARSE,
  INLINE_SORT_GENERIC
};

struct AggregateFacetRunStats {
  int64_t values = 0;
  int64_t runs = 0;
};

static AggregateFacetRunStats measureFacetRuns(
    IndexReader& reader, std::string_view field) {
  AggregateFacetRunStats stats;
  for (auto& segment : reader.segments()) {
    auto& postings = segment.postingsReader();
    FieldReader fieldReader(postings);
    if (!fieldReader.seek(field)) continue;
    SegFieldInfo info;
    fieldReader.readFieldInfo(info);
    OrdColReader column(postings, info);
    int64_t missing = 0;
    int32_t previous = -1;
    forEachOrdValue(nullptr, column, postings.maxDoc(), missing,
                    [&](int32_t docid, int32_t ord) {
      unused(docid);
      stats.values++;
      if (ord != previous) stats.runs++;
      previous = ord;
    });
  }
  return stats;
}

// A deliberately small corpus keeps these aggregate hot-path lanes useful in
// routine --bench runs. BUCKET_DOMAIN measures the ordinary aggregate
// calculator over selected facet domains; INLINE_SORT measures per-document
// causal expression-metric accumulation, the DENSE/SPARSE modes pin the entry
// representation, and INLINE_SORT_GENERIC keeps the same lane on the fallback
// state layout.
static void BM_AggregateFacet(benchmark::State& state,
                              AggregateFacetBenchMode mode,
                              int32_t bucketCardinality) {
  constexpr int64_t DOCS = 200'000;
  constexpr std::string_view SHAPE = "9555";
  std::vector<int32_t> docsPerSeg;
  CollectionHelper::calcSegSizes(DOCS, 10, SHAPE, docsPerSeg);
  CollectionHelper helper;
  bool reused = helper.indexMatchesShape(docsPerSeg);
  if (!reused) buildBenchIndex(helper, DOCS, docsPerSeg);

  SearchOverridesGuard guard(forcedFacetFeedStrategy,
                              forcedFacetSubOpInline,
                              inlineAggregateStatsForTests,
                              inlineFacetEntryStatsForTests,
                              disableDenseFacetStateForTests,
                              forcedInlineFacetEntryMode);
  forcedFacetFeedStrategy = FacetFeedStrategy::BUCKET_DOMAINS;
  forcedFacetSubOpInline = mode != AggregateFacetBenchMode::BUCKET_DOMAIN
      ? FacetSubOpInlineMode::ALL
      : FacetSubOpInlineMode::SORT_KEY_ONLY;
  disableDenseFacetStateForTests =
      mode == AggregateFacetBenchMode::INLINE_SORT_GENERIC;
  forcedInlineFacetEntryMode =
      mode == AggregateFacetBenchMode::INLINE_SORT_DENSE
          ? InlineFacetEntryMode::FORCE_DENSE
      : mode == AggregateFacetBenchMode::INLINE_SORT_SPARSE
          ? InlineFacetEntryMode::FORCE_SPARSE
          : InlineFacetEntryMode::AUTO;
  InlineAggregateStats inlineStats;
  InlineFacetEntryStats entryStats;
  inlineAggregateStatsForTests = mode != AggregateFacetBenchMode::BUCKET_DOMAIN
      ? &inlineStats : nullptr;
  inlineFacetEntryStatsForTests = mode != AggregateFacetBenchMode::BUCKET_DOMAIN
      ? &entryStats : nullptr;

  int64_t fingerprint = -1;
  for (auto _ : state) {
    auto req = localReq(LuxirTest::luxirNode->getSearchEngine());
    std::string_view facetField;
    if (mode == AggregateFacetBenchMode::BUCKET_DOMAIN
        || bucketCardinality == 10) {
      facetField = "short_u10_s";
    } else if (bucketCardinality == 1000) {
      facetField = "short_u1000_s";
    } else {
      assert(bucketCardinality == 10'000);
      facetField = "short_u10k_s";
    }
    auto& facet = req->collection("main").facet("f", facetField).limit(10);
    facet.expr("metric", "avg(u10k_i)");
    if (mode != AggregateFacetBenchMode::BUCKET_DOMAIN) {
      qb::sort(facet, "metric", qb::DESC);
    }
    req->execute(false);
    if (!req->ok()) {
      state.SkipWithError(req->errorMsg());
      break;
    }

    const auto* result = req->responses[0]->proto.ops.at("f")->facetResult();
    const auto& ids = std::get<api::ColStr>(result->bucket_ids->kind).v;
    const auto& metrics =
        std::get<api::ArrVal>(result->ops.at("metric")->kind).v;
    int64_t current = 0;
    for (size_t i = 0; i < ids.size(); i++) {
      current = current * 31 + java_string_hashcode(ids[i]);
      current = current * 31 + (int64_t)metrics[i].asDouble();
    }
    benchmark::DoNotOptimize(current);
    if (fingerprint != -1) {
      ASSERT_EQ(fingerprint, current);
    }
    fingerprint = current;
  }
  state.counters["docs"] = (double)DOCS;
  state.counters["fp"] = fingerprint;
  state.counters["reused"] = reused;
  state.counters["rate"] = benchmark::Counter(
      state.iterations(), benchmark::Counter::kIsRate);
  if (mode != AggregateFacetBenchMode::BUCKET_DOMAIN) {
    auto runStats = measureFacetRuns(*helper.getIndexWriter()->snapshots.readers.getReader(),
                                     bucketCardinality == 10
                                         ? "short_u10_s"
                                         : bucketCardinality == 1000
                                             ? "short_u1000_s"
                                             : "short_u10k_s");
    state.counters["mean_run"] = (double)runStats.values / runStats.runs;
    state.counters["same_ord_pct"] = 100.0
        * (double)(runStats.values - runStats.runs) / runStats.values;
    size_t finalizedPeak = (size_t)bucketCardinality * sizeof(uint64_t)
        + ((size_t)bucketCardinality + 63) / 64 * sizeof(uint64_t);
    ASSERT_EQ(0, inlineStats.finalizedBytes.load());
    ASSERT_EQ(finalizedPeak, inlineStats.peakFinalizedBytes.load());
    ASSERT_EQ(mode != AggregateFacetBenchMode::INLINE_SORT_GENERIC ? 17 : 26,
              inlineStats.stateBytesPerBucket.load());
    ASSERT_EQ(sizeof(int64_t) + inlineStats.stateBytesPerBucket.load(),
              entryStats.entryStride.load());
    state.counters["finalized_peak"] =
        (double)inlineStats.peakFinalizedBytes.load();
    state.counters["state_bytes"] =
        (double)inlineStats.stateBytesPerBucket.load();
    state.counters["entry_stride"] =
        (double)entryStats.entryStride.load();
    state.counters["dense_tables"] =
        (double)entryStats.denseTables.load();
    state.counters["sparse_tables"] =
        (double)entryStats.sparseTables.load();
  }
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
LUXIR_BENCHMARK_CAPTURE(BM_AggregateFacet, bucket_domain,
                        AggregateFacetBenchMode::BUCKET_DOMAIN, 10);
LUXIR_BENCHMARK_CAPTURE(BM_AggregateFacet, inline_sort_10,
                        AggregateFacetBenchMode::INLINE_SORT, 10);
LUXIR_BENCHMARK_CAPTURE(BM_AggregateFacet, inline_sort_1k,
                        AggregateFacetBenchMode::INLINE_SORT, 1000);
LUXIR_BENCHMARK_CAPTURE(BM_AggregateFacet, inline_sort_10k,
                        AggregateFacetBenchMode::INLINE_SORT, 10'000);
LUXIR_BENCHMARK_CAPTURE(BM_AggregateFacet, inline_sort_dense_10,
                        AggregateFacetBenchMode::INLINE_SORT_DENSE, 10);
LUXIR_BENCHMARK_CAPTURE(BM_AggregateFacet, inline_sort_dense_1k,
                        AggregateFacetBenchMode::INLINE_SORT_DENSE, 1000);
LUXIR_BENCHMARK_CAPTURE(BM_AggregateFacet, inline_sort_dense_10k,
                        AggregateFacetBenchMode::INLINE_SORT_DENSE, 10'000);
LUXIR_BENCHMARK_CAPTURE(BM_AggregateFacet, inline_sort_sparse_10,
                        AggregateFacetBenchMode::INLINE_SORT_SPARSE, 10);
LUXIR_BENCHMARK_CAPTURE(BM_AggregateFacet, inline_sort_sparse_1k,
                        AggregateFacetBenchMode::INLINE_SORT_SPARSE, 1000);
LUXIR_BENCHMARK_CAPTURE(BM_AggregateFacet, inline_sort_sparse_10k,
                        AggregateFacetBenchMode::INLINE_SORT_SPARSE, 10'000);
LUXIR_BENCHMARK_CAPTURE(BM_AggregateFacet, inline_sort_generic,
                        AggregateFacetBenchMode::INLINE_SORT_GENERIC, 10'000);
