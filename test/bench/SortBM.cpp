// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "bench/luxir_bench.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/QueryBuild.h"
#include "test/SchemaBuilder.h"
#include "luxir/index/IndexWriter.h"
#include "luxir/index/Inverter.h"
#include "luxir/query/QueryPrep.h"
#include "luxir/reader/SkipStats.h"
#include "luxir/search/FieldSortCollector.h"
#include "luxir/search/SortField.h"
#include "luxir/util/random.h"
#include <format>
#include <optional>

using namespace luxir;
using namespace luxir::test;

static void BM_StringSort(benchmark::State& state, int64_t nDocs,
                          std::string_view shape, qb::SortDir direction,
                          StringSortMode mode) {
  class ModeGuard {
    StringSortMode saved;
  public:
    explicit ModeGuard(StringSortMode mode)
        : saved(SortField::setStringSortModeForTests(mode)) {}
    ~ModeGuard() { SortField::setStringSortModeForTests(saved); }
  } guard(mode);

  if (luxir::unit_tests) nDocs = LuxirTest::scaleTestWork(200);
  std::vector<int32_t> docsPerSeg;
  CollectionHelper::calcSegSizes(nDocs, 10, shape, docsPerSeg);

  CollectionHelper helper("string_sort_bm");
  bool reused = helper.indexMatchesShape(docsPerSeg);
  if (!reused) buildBenchIndex(helper, nDocs, docsPerSeg);

  int64_t fingerprint = -1;
  for (auto _ : state) {
    auto req = localReq(LuxirTest::luxirNode->getSearchEngine());
    req->collection("string_sort_bm");
    auto& top = req->topDocs("q").countLimit().allQuery().limit(100)
        .fields({"id", "short_u10k_s"});
    qb::sort(top, "short_u10k_s", direction);
    req->execute(false);
    const auto* docs = req->docList("q");
    int64_t current = docs->found.value_or(0);
    for (const auto& batch : req->responses) {
      const auto* list = batch->proto.ops.at("q")->docList();
      const auto& ids = std::get<api::ColStr>(list->columns.at("id").kind).v;
      const auto& values =
          std::get<api::ColStr>(list->columns.at("short_u10k_s").kind).v;
      for (size_t i = 0; i < ids.size(); i++) {
        current = current * 31 + java_string_hashcode(ids[i]);
        current = current * 31 + java_string_hashcode(values[i]);
      }
    }
    benchmark::DoNotOptimize(current);
    if (fingerprint != -1) {
      ASSERT_EQ(fingerprint, current);
    }
    fingerprint = current;
  }
  state.counters["fp"] = fingerprint;
  state.counters["reused"] = reused;
  state.counters["rate"] = benchmark::Counter(
      state.iterations(), benchmark::Counter::kIsRate);
}

static constexpr int64_t STRING_SORT_DOCS = 10'000'000;
static constexpr const char* STRING_SORT_SHAPE = "9555";
static constexpr int64_t INT_SORT_SS_DOCS = 5'000'000;

// One instrumented pass's pruning-attribution SkipStats, exposed as counters:
// competitiveRanges vs irreducibleBlocks is the reordering headroom,
// lastAdmission/docsGathered is threshold maturity.
static void recordFieldSortCounters(benchmark::State& state) {
  state.counters["warmupRanges"] = (double)SkipStats::fieldSortWarmupRanges;
  state.counters["competitiveRanges"] =
      (double)SkipStats::fieldSortCompetitiveRanges;
  state.counters["blocksSkipped"] = (double)SkipStats::fieldSortBlocksSkipped;
  state.counters["leavesSkipped"] = (double)SkipStats::fieldSortLeavesSkipped;
  state.counters["docsGathered"] = (double)SkipStats::fieldSortDocsGathered;
  state.counters["lastAdmission"] =
      (double)SkipStats::fieldSortGatherAtLastAdmission;
  state.counters["irreducibleBlocks"] =
      (double)SkipStats::fieldSortIrreducibleBlocks;
  state.counters["bulkCollections"] =
      (double)SkipStats::fieldSortBulkCollections;
  state.counters["requiredBlocks"] =
      (double)SkipStats::fieldSortRequiredBlocks;
  state.counters["irreducibleLeaves"] =
      (double)SkipStats::fieldSortIrreducibleLeaves;
  state.counters["requiredLeaves"] =
      (double)SkipStats::fieldSortRequiredLeaves;
  state.counters["bfActivations"] =
      (double)SkipStats::fieldSortBestFirstActivations;
  state.counters["bfLeaves"] = (double)SkipStats::fieldSortBestFirstLeaves;
  state.counters["bfTerminations"] =
      (double)SkipStats::fieldSortBestFirstTerminations;
  state.counters["bfFallbacks"] =
      (double)SkipStats::fieldSortBestFirstFallbacks;
  state.counters["seedActivations"] =
      (double)SkipStats::fieldSortSeededActivations;
  state.counters["seedLeaves"] = (double)SkipStats::fieldSortSeedLeaves;
  state.counters["seedClassifiedOut"] =
      (double)SkipStats::fieldSortSeedClassifiedOut;
  state.counters["seedFillAborts"] =
      (double)SkipStats::fieldSortSeedFillAborts;
  state.counters["seedPass2Skips"] =
      (double)SkipStats::fieldSortSeedPass2Skips;
  state.counters["domainScans"] = (double)SkipStats::fieldSortDomainScans;
  state.counters["sweepLeaves"] = (double)SkipStats::fieldSortSweepLeaves;
  state.counters["wholeBestFirst"] =
      (double)SkipStats::wholeFieldSortBestFirstActivations;
  state.counters["extActivations"] =
      (double)SkipStats::fieldSortExternalApproxActivations;
  state.counters["extCandidates"] =
      (double)SkipStats::fieldSortExternalApproxCandidates;
  state.counters["extBoundRejects"] =
      (double)SkipStats::fieldSortExternalApproxBoundRejects;
  state.counters["extVerifications"] =
      (double)SkipStats::fieldSortExternalApproxVerifications;
}

// Near-unique int sort under a cached filter: the serverbench sort10
// red-cell shape. filterBelow selects u10k_i < filterBelow (0 = unfiltered;
// 1000 ~= 10% density, 100 ~= 1%, 10 ~= 0.1%).
static void BM_IntSortFiltered(benchmark::State& state, int64_t nDocs,
                               int64_t filterBelow, bool singleSeg = false) {
  if (luxir::unit_tests) nDocs = LuxirTest::scaleTestWork(200);
  std::vector<int32_t> docsPerSeg;
  if (singleSeg) {
    // The serverbench sort-grid posture: one segment, so the irreducible
    // floor is measured against the true final bottom rather than summed
    // per segment against a maturing one.
    docsPerSeg.push_back((int32_t)nDocs);
  } else {
    CollectionHelper::calcSegSizes(nDocs, 10, STRING_SORT_SHAPE, docsPerSeg);
  }

  const char* collection = singleSeg ? "int_sort_bm" : "string_sort_bm";
  CollectionHelper helper(collection);
  bool reused = helper.indexMatchesShape(docsPerSeg);
  if (!reused) buildBenchIndex(helper, nDocs, docsPerSeg);

  auto execute = [&]() -> int64_t {
    auto req = localReq(LuxirTest::luxirNode->getSearchEngine());
    req->collection(collection);
    auto& top = req->topDocs("q").allQuery().limit(10).fields({"id"});
    qb::sort(top, "u10m_i", qb::ASC);
    if (filterBelow > 0) {
      top.filter(qb::range(top.mr(), "u10k_i", nullptr, nullptr, nullptr,
                           qb::valI64(top.mr(), filterBelow)));
    }
    req->execute(false);
    int64_t current = 0;
    for (const auto& batch : req->responses) {
      const auto* list = batch->proto.ops.at("q")->docList();
      const auto& ids = std::get<api::ColStr>(list->columns.at("id").kind).v;
      for (const auto& id : ids) {
        current = current * 31 + java_string_hashcode(id);
      }
    }
    return current;
  };

  int64_t fingerprint = execute();  // warms the filter cache too

  {
    SkipStatsScope stats;
    ASSERT_EQ(fingerprint, execute());
    recordFieldSortCounters(state);
  }

  for (auto _ : state) {
    int64_t current = execute();
    benchmark::DoNotOptimize(current);
    ASSERT_EQ(fingerprint, current);
  }
  state.counters["fp"] = fingerprint;
  state.counters["reused"] = reused;
  state.counters["rate"] = benchmark::Counter(
      state.iterations(), benchmark::Counter::kIsRate);
}

LUXIR_BENCHMARK_CAPTURE(BM_StringSort, global_asc, STRING_SORT_DOCS,
                        STRING_SORT_SHAPE, qb::ASC, StringSortMode::GLOBAL);
LUXIR_BENCHMARK_CAPTURE(BM_StringSort, global_desc, STRING_SORT_DOCS,
                        STRING_SORT_SHAPE, qb::DESC, StringSortMode::GLOBAL);
LUXIR_BENCHMARK_CAPTURE(BM_StringSort, segment_asc, STRING_SORT_DOCS,
                        STRING_SORT_SHAPE, qb::ASC, StringSortMode::SEGMENT);
LUXIR_BENCHMARK_CAPTURE(BM_StringSort, segment_desc, STRING_SORT_DOCS,
                        STRING_SORT_SHAPE, qb::DESC, StringSortMode::SEGMENT);
// Near-unique int sort where the selectivity predicate IS the main query
// (a term match, uncached): the GRID_FILTER_MODE=query sort10 posture. The
// term postings are the domain - no materialized set exists, so the exact-
// domain best-first route cannot fire and collection rides the pruned
// doc-order window walk. queryField "short_u10_s" ~= 10% density per term,
// "short_u100_s" ~= 1%, "short_u10k_s" ~= 0.01% (sparse control).
// `descent` forces the walk's leaf descent on (1) or off (0) to measure both
// arms of FieldSortCollector::descendsToLeaves (ExactDomainSortCosts::
// kWalkLeafCost was calibrated so); -1 leaves it priced.
struct DescentGuard {
  std::optional<bool> saved = FieldSortCollector::forceLeafDescentForTests;
  explicit DescentGuard(int descent) {
    if (descent >= 0) {
      FieldSortCollector::forceLeafDescentForTests = descent == 1;
    }
  }
  ~DescentGuard() { FieldSortCollector::forceLeafDescentForTests = saved; }
};

// Top `limit` by u10m_i ascending over the query `setQuery` installs on the
// op, fingerprinted by the returned ids.
template <typename SetQuery>
static void runIntSort(benchmark::State& state, std::string_view collection,
                       bool reused, int32_t limit, SetQuery&& setQuery) {
  auto execute = [&]() -> int64_t {
    auto req = localReq(LuxirTest::luxirNode->getSearchEngine());
    req->collection(collection);
    auto& top = req->topDocs("q").limit(limit).fields({"id"});
    setQuery(top);
    qb::sort(top, "u10m_i", qb::ASC);
    req->execute(false);
    int64_t current = 0;
    for (const auto& batch : req->responses) {
      const auto* list = batch->proto.ops.at("q")->docList();
      // The unit-test corpus is small enough for a sparse term to match
      // nothing at all.
      if (list == nullptr) continue;
      const auto* column = list->columns.find("id");
      if (column == nullptr) continue;
      for (const auto& id : std::get<api::ColStr>(column->kind).v) {
        current = current * 31 + java_string_hashcode(id);
      }
    }
    return current;
  };

  int64_t fingerprint = execute();

  {
    SkipStatsScope stats;
    ASSERT_EQ(fingerprint, execute());
    recordFieldSortCounters(state);
  }

  for (auto _ : state) {
    int64_t current = execute();
    benchmark::DoNotOptimize(current);
    ASSERT_EQ(fingerprint, current);
  }
  state.counters["fp"] = fingerprint;
  state.counters["reused"] = reused;
  state.counters["rate"] = benchmark::Counter(
      state.iterations(), benchmark::Counter::kIsRate);
}

// One single-segment collection per size, so sizes never rebuild each
// other's index.
static std::string singleSegmentCollection(int64_t nDocs) {
  return nDocs == INT_SORT_SS_DOCS ? "int_sort_bm"
      : std::format("int_sort_bm_{}", nDocs);
}

static void BM_IntSortQuery(benchmark::State& state, int64_t nDocs,
                            const char* queryField, bool singleSeg = false,
                            int descent = -1, int32_t limit = 10) {
  DescentGuard descentGuard(descent);
  if (luxir::unit_tests) nDocs = LuxirTest::scaleTestWork(200);
  std::vector<int32_t> docsPerSeg;
  if (singleSeg) {
    docsPerSeg.push_back((int32_t)nDocs);
  } else {
    CollectionHelper::calcSegSizes(nDocs, 10, STRING_SORT_SHAPE, docsPerSeg);
  }

  std::string collection =
      singleSeg ? singleSegmentCollection(nDocs) : "string_sort_bm";
  CollectionHelper helper(collection);
  bool reused = helper.indexMatchesShape(docsPerSeg);
  if (!reused) buildBenchIndex(helper, nDocs, docsPerSeg);
  runIntSort(state, collection, reused, limit,
             [&](OpCursor& top) { top.matchQuery(queryField, "3"); });
}

// One segment of phrase fields beside u10m_i. body32_w holds four tokens
// drawn uniformly from 32 words; bodyhh_w eight, each w0 or w1 with chance
// 0.3 and otherwise one of 100 rarer words, so "w0 w1" is a high-high phrase.
static void buildSortPhraseIndex(CollectionHelper& helper, int64_t nDocs) {
  helper.clear();
  auto iw = helper.getIndexWriter();
  Inverter& inverter = iw->obtainInverter();
  Inverter::InputHandler& id = inverter.getIndexHandler("id");
  Inverter::InputHandler& sortKey = inverter.getIndexHandler("u10m_i");
  Inverter::InputHandler& body32 = inverter.getIndexHandler("body32_w");
  Inverter::InputHandler& bodyHH = inverter.getIndexHandler("bodyhh_w");
  std::string text;
  for (int64_t doc = 0; doc < nDocs; doc++) {
    SplitMix64 r(doc);
    inverter.startDoc();
    id.index(inverter, std::to_string(doc));
    sortKey.index(inverter, r.rint(10000000));
    text.clear();
    for (int i = 0; i < 4; i++) {
      if (i > 0) text.push_back(' ');
      text.append("w").append(std::to_string(r.rint(32)));
    }
    body32.index(inverter, text);
    text.clear();
    for (int i = 0; i < 8; i++) {
      if (i > 0) text.push_back(' ');
      int64_t pick = r.rint(1000);
      text.append(pick < 300 ? "w0" : pick < 600 ? "w1"
                  : "r" + std::to_string(r.rint(100)));
    }
    bodyHH.index(inverter, text);
    inverter.finishDoc();
  }
  iw->releaseInverter(inverter, true);
  helper.commit();
}

// Expensive query shapes over one 10M-doc segment, where a skipped leaf
// saves more than its output docs: conjunctions whose lead is denser than
// their output (the bulk walk, which pays a multi-clause window per split
// range), and phrases whose approximation is (the external two-phase gate).
enum class IntSortShape {
  AND_1_10,     // short_u100_s AND short_u10_s: 0.1% out, 1% lead
  AND_01_10,    // short_u1000_s AND short_u10_s: 0.01% out, 0.1% lead
  AND_10_10,    // short_u10_s AND med_u10_s: 1% out, 10% lead
  PHRASE_MID,   // "w3 w7" in body32_w: ~0.3% out, ~1.3% approximation
  PHRASE_HH,    // "w0 w1" in bodyhh_w: both words in ~94% of docs, ~50% out
};

// The whole-membership plan stays off, so each repetition runs the query's
// own scorer: a repeated query is otherwise served from a resident copy of
// its matches by the exact-domain driver, and never walks.
static void BM_IntSortShape(benchmark::State& state, IntSortShape shape,
                            int descent = -1) {
  DescentGuard descentGuard(descent);
  struct MembershipGuard {
    bool saved = QueryPrep::disableWholeMembershipPlanForTests;
    MembershipGuard() { QueryPrep::disableWholeMembershipPlanForTests = true; }
    ~MembershipGuard() { QueryPrep::disableWholeMembershipPlanForTests = saved; }
  } membershipGuard;
  int64_t nDocs = 10'000'000;
  if (luxir::unit_tests) nDocs = LuxirTest::scaleTestWork(200);
  std::vector<int32_t> docsPerSeg{(int32_t)nDocs};
  bool phrase = shape == IntSortShape::PHRASE_MID
      || shape == IntSortShape::PHRASE_HH;
  std::string collection = phrase ? "sort_phrase_bm"
                                  : singleSegmentCollection(nDocs);
  CollectionHelper helper(collection);
  bool reused = helper.indexMatchesShape(docsPerSeg);
  if (!reused) {
    if (phrase) buildSortPhraseIndex(helper, nDocs);
    else buildBenchIndex(helper, nDocs, docsPerSeg);
  }
  runIntSort(state, collection, reused, 10, [&](OpCursor& top) {
    auto& mr = top.mr();
    switch (shape) {
      case IntSortShape::AND_1_10:
        top.rawQuery() = qb::boolean(mr, {qb::match(mr, "short_u100_s", "3"),
                                          qb::match(mr, "short_u10_s", "3")});
        break;
      case IntSortShape::AND_01_10:
        top.rawQuery() = qb::boolean(mr, {qb::match(mr, "short_u1000_s", "3"),
                                          qb::match(mr, "short_u10_s", "3")});
        break;
      case IntSortShape::AND_10_10:
        top.rawQuery() = qb::boolean(
            mr, {qb::match(mr, "short_u10_s", "3"),
                 qb::match(mr, "med_u10_s", "3medium_length_string_no_SSO")});
        break;
      case IntSortShape::PHRASE_MID:
        top.rawQuery() = qb::phraseWords(mr, "body32_w", {"w3", "w7"});
        break;
      case IntSortShape::PHRASE_HH:
        top.rawQuery() = qb::phraseWords(mr, "bodyhh_w", {"w0", "w1"});
        break;
    }
  });
}

LUXIR_BENCHMARK_CAPTURE(BM_IntSortFiltered, all, STRING_SORT_DOCS, 0);
LUXIR_BENCHMARK_CAPTURE(BM_IntSortFiltered, f10pct, STRING_SORT_DOCS, 1000);
LUXIR_BENCHMARK_CAPTURE(BM_IntSortFiltered, f1pct, STRING_SORT_DOCS, 100);
LUXIR_BENCHMARK_CAPTURE(BM_IntSortFiltered, f0p1pct, STRING_SORT_DOCS, 10);
LUXIR_BENCHMARK_CAPTURE(BM_IntSortFiltered, ss_all, INT_SORT_SS_DOCS, 0, true);
LUXIR_BENCHMARK_CAPTURE(BM_IntSortFiltered, ss_f10pct, INT_SORT_SS_DOCS, 1000, true);
LUXIR_BENCHMARK_CAPTURE(BM_IntSortFiltered, ss_f1pct, INT_SORT_SS_DOCS, 100, true);
LUXIR_BENCHMARK_CAPTURE(BM_IntSortFiltered, ss_f0p1pct, INT_SORT_SS_DOCS, 10, true);
LUXIR_BENCHMARK_CAPTURE(BM_IntSortQuery, q10pct, STRING_SORT_DOCS, "short_u10_s");
LUXIR_BENCHMARK_CAPTURE(BM_IntSortQuery, ss_q10pct, INT_SORT_SS_DOCS,
                        "short_u10_s", true);
LUXIR_BENCHMARK_CAPTURE(BM_IntSortQuery, ss_q1pct, INT_SORT_SS_DOCS,
                        "short_u100_s", true);
LUXIR_BENCHMARK_CAPTURE(BM_IntSortQuery, ss_q0p1pct, INT_SORT_SS_DOCS,
                        "short_u1000_s", true);
LUXIR_BENCHMARK_CAPTURE(BM_IntSortQuery, ss_q0p01pct, INT_SORT_SS_DOCS,
                        "short_u10k_s", true);
// The grid's 10M-doc segment: at 0.1% its 10k-doc domain admits 40% of the
// leaves with half a domain doc in each, where leaf descent loses.
static constexpr int64_t INT_SORT_SS10M_DOCS = 10'000'000;
LUXIR_BENCHMARK_CAPTURE(BM_IntSortQuery, ss10m_q1pct, INT_SORT_SS10M_DOCS,
                        "short_u100_s", true);
LUXIR_BENCHMARK_CAPTURE(BM_IntSortQuery, ss10m_q0p1pct, INT_SORT_SS10M_DOCS,
                        "short_u1000_s", true);
// Each shape priced, and with descent forced off and on: the priced arm
// should track the faster one.
#define INT_SORT_SHAPE_CAPTURES(name, shape) \
  LUXIR_BENCHMARK_CAPTURE(BM_IntSortShape, name, shape, -1); \
  LUXIR_BENCHMARK_CAPTURE(BM_IntSortShape, name##_off, shape, 0); \
  LUXIR_BENCHMARK_CAPTURE(BM_IntSortShape, name##_on, shape, 1);
INT_SORT_SHAPE_CAPTURES(and_1_10, IntSortShape::AND_1_10)
INT_SORT_SHAPE_CAPTURES(and_01_10, IntSortShape::AND_01_10)
INT_SORT_SHAPE_CAPTURES(and_10_10, IntSortShape::AND_10_10)
INT_SORT_SHAPE_CAPTURES(phrase_mid, IntSortShape::PHRASE_MID)
INT_SORT_SHAPE_CAPTURES(phrase_hh, IntSortShape::PHRASE_HH)

// A multi-valued int sort over one segment of docs holding eight random
// values each: SET edge reads and ARRAY scans, bare fields and reducers.
static void BM_MultiIntSort(benchmark::State& state, const char* sortExpr,
                            qb::SortDir direction, bool array) {
  int64_t nDocs = 2'000'000;
  if (luxir::unit_tests) nDocs = LuxirTest::scaleTestWork(200);
  std::string collection = std::format("multi_int_sort_bm_{}_{}", array ? "array" : "set", nDocs);
  CollectionHelper helper(collection);
  std::vector<int32_t> docsPerSeg{(int32_t)nDocs};
  bool reused = helper.indexMatchesShape(docsPerSeg);
  if (!reused) {
    helper.clear();
    SchemaBuilder schema;
    auto& field = schema.field("many_is");
    field.type = api::FieldDef::FieldClass::INT;
    field.multi = array ? api::FieldDef::MultiMode::ARRAY : api::FieldDef::MultiMode::SET;
    schema.set(helper.collection());
    auto iw = helper.getIndexWriter();
    Inverter& inverter = iw->obtainInverter();
    Inverter::InputHandler& id = inverter.getIndexHandler("id");
    Inverter::InputHandler& many = inverter.getIndexHandler("many_is");
    int64_t values[8];
    for (int64_t doc = 0; doc < nDocs; doc++) {
      SplitMix64 r(doc);
      inverter.startDoc();
      id.index(inverter, std::to_string(doc));
      for (int64_t& value : values) value = r.rint(1'000'000'000);
      many.index(inverter, std::span<const int64_t>(values));
      inverter.finishDoc();
    }
    iw->releaseInverter(inverter, true);
    helper.commit();
  }

  std::optional<uint64_t> fingerprint;
  for (auto _ : state) {
    auto req = localReq(LuxirTest::luxirNode->getSearchEngine());
    req->collection(collection);
    auto& top = req->topDocs("q").allQuery().limit(10).fields({"id"});
    qb::sort(top, sortExpr, direction);
    req->execute(false);
    ASSERT_TRUE(req->ok()) << req->errorMsg();
    uint64_t current = 0;
    for (const auto& batch : req->responses) {
      const auto* list = batch->proto.ops.at("q")->docList();
      for (const auto& docId : std::get<api::ColStr>(list->columns.at("id").kind).v) {
        current = current * 31 + java_string_hashcode(docId);
      }
    }
    benchmark::DoNotOptimize(current);
    if (fingerprint) {
      ASSERT_EQ(*fingerprint, current);
    }
    fingerprint = current;
  }
  state.counters["fp"] = (int64_t)fingerprint.value_or(0);
  state.counters["reused"] = reused;
}

LUXIR_BENCHMARK_CAPTURE(BM_MultiIntSort, field_asc, "many_is", qb::ASC, false);
LUXIR_BENCHMARK_CAPTURE(BM_MultiIntSort, field_desc, "many_is", qb::DESC, false);
LUXIR_BENCHMARK_CAPTURE(BM_MultiIntSort, min_asc, "min(many_is)", qb::ASC, false);
LUXIR_BENCHMARK_CAPTURE(BM_MultiIntSort, max_desc, "max(many_is)", qb::DESC, false);

LUXIR_BENCHMARK_CAPTURE(BM_MultiIntSort, array_field_asc, "many_is", qb::ASC, true);
LUXIR_BENCHMARK_CAPTURE(BM_MultiIntSort, array_field_desc, "many_is", qb::DESC, true);
LUXIR_BENCHMARK_CAPTURE(BM_MultiIntSort, array_min_asc, "min(many_is)", qb::ASC, true);
LUXIR_BENCHMARK_CAPTURE(BM_MultiIntSort, array_max_desc, "max(many_is)", qb::DESC, true);
