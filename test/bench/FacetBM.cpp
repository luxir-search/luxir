// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <charconv>
#include <latch>
#include <numeric>
#include <random>
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

// Shared DATA corpus. Setup stays outside the timed search loop. In addition
// to the current lanes, keep a time-correlated 20-year minute span for narrow
// filters, separated parent windows, and distinct keys for the streaming lanes.
struct RangeDataBenchFixture {
  CollectionHelper helper;
  bool reused;
  int64_t numDocs;
  static constexpr int64_t epoch = 1577836800000LL;
  static constexpr int64_t retention = 20LL * 365 * 86400000;

  explicit RangeDataBenchFixture(int64_t docs, std::string_view shape,
                                 std::string_view name = "facet_range_data_bm") : helper(name) {
    if (unit_tests) docs = LuxirTest::scaleTestWork(200);
    numDocs = docs;
    std::vector<int32_t> docsPerSeg;
    CollectionHelper::calcSegSizes(docs, 10, shape, docsPerSeg);
    reused = helper.indexMatchesShape(docsPerSeg);
    if (reused) return;
    helper.clear();
    SchemaBuilder schema;
    for (auto name : {"data_i", "date_dt", "retention_dt", "group_i"}) {
      auto& field = schema.field(name);
      field.type = std::string_view(name).ends_with("_dt")
          ? api::FieldDef::FieldClass::DATE : api::FieldDef::FieldClass::INT;
      field.index = api::FieldDef::IndexMode::RANGE;
    }
    if (name == "facet_range_data_drop_bm") schema.field("drop_group_i").type = api::FieldDef::FieldClass::INT;
    schema.field("parent_s").type = api::FieldDef::FieldClass::STRING;
    if (name != "facet_range_data_bm") schema.field("parent_i").type = api::FieldDef::FieldClass::INT;
    schema.set(helper.collection());
    auto iw = helper.getIndexWriter();
    int64_t doc = 0;
    std::mt19937 random(37);
    for (int32_t size : docsPerSeg) {
      auto& inv = iw->obtainInverter();
      auto& id = inv.getIndexHandler("id");
      auto& value = inv.getIndexHandler("data_i");
      auto& date = inv.getIndexHandler("date_dt");
      auto& retained = inv.getIndexHandler("retention_dt");
      auto& parent = inv.getIndexHandler("parent_s");
      auto& group = inv.getIndexHandler("group_i");
      auto* parentNumber = name != "facet_range_data_bm" ? &inv.getIndexHandler("parent_i") : nullptr;
      auto* dropGroup = name == "facet_range_data_drop_bm" ? &inv.getIndexHandler("drop_group_i") : nullptr;
      for (int32_t i = 0; i < size; i++, doc++) {
        inv.startDoc();
        id.index(inv, std::to_string(doc));
        value.index(inv, doc % 10000 * 1000);
        int64_t cell = name == "facet_range_data_shuffled_bm" ? random() % (365 * 24) : doc % (365 * 24);
        date.index(inv, epoch + cell * 3600000);
        retained.index(inv, epoch + (int64_t)((__int128)doc * retention / docs));
        parent.index(inv, "p" + std::to_string(doc * 32 / docs));
        group.index(inv, doc % 4096);
        if (dropGroup && doc % 4 != 0) dropGroup->index(inv, doc % 4096);
        if (parentNumber) parentNumber->index(inv, doc * 32 / docs);
        inv.finishDoc();
      }
      iw->releaseInverter(inv, true);
    }
    helper.commit();
  }
};

enum class RangeDataBenchMode {
  INT, DATE, IANA, IANA_SHIFTED, IANA_DAILY, IANA_DAILY_6H, CALENDAR_DAY,
  NARROW, DISTINCT, DISTINCT_WIDE, DISTINCT_DROP, NESTED
};

// Logical source values per search, including values handled by point ordinals
// and values whose distinct key is dropped. RSS includes the resident corpus;
// peak_bytes measures only allocations charged to the request tracker.
static void rangeDataBenchCounters(benchmark::State& state, int64_t values,
                                  size_t buckets, size_t peak, RSSWatcher& rss) {
  state.counters["values"] = (double)values;
  state.counters["values_per_second"] = benchmark::Counter(
      (double)values, benchmark::Counter::kIsIterationInvariantRate);
  state.counters["buckets"] = (double)buckets;
  state.counters["peak_bytes"] = (double)peak;
  state.counters["rss_bytes"] = (double)(std::max(rss.startRSS, rss.getDeltaKB().second) * 1024);
}

static void BM_RangeFacetData(benchmark::State& state, int64_t nDocs,
                              std::string_view shape, RangeDataBenchMode mode,
                              bool forceWalk = false, bool parallel = false,
                              bool explicitBounds = false, bool shuffled = false) {
  bool drop = mode == RangeDataBenchMode::DISTINCT_DROP;
  std::string_view collection = drop ? "facet_range_data_drop_bm"
      : shuffled ? "facet_range_data_shuffled_bm" : "facet_range_data_bm";
  RangeDataBenchFixture fixture(nDocs, shape, collection);
  int64_t explicitStart = 0, explicitEnd = 0;
  if (explicitBounds) {
    auto zone = *resolveTimeZone("America/New_York");
    int64_t gap = mode == RangeDataBenchMode::IANA_DAILY || mode == RangeDataBenchMode::CALENDAR_DAY
        ? 86400000 : 3600000;
    int64_t origin = mode == RangeDataBenchMode::IANA_SHIFTED ? 17 * 60000 : 0;
    auto grid = mode == RangeDataBenchMode::CALENDAR_DAY ? RangeFacetGrid::calendar(CalendarUnit::DAY, 1, zone)
        : RangeFacetGrid::fixed(FieldType::DATE, gap, origin, zone);
    explicitStart = grid.floor(RangeDataBenchFixture::epoch).key;
    explicitEnd = grid.next(grid.floor(RangeDataBenchFixture::epoch + (365 * 24 - 1LL) * 3600000)).key;
  }
  RSSWatcher rss;
  bool savedWalk = IntFacetRangeReq::disablePointsRangeFacetForTests;
  auto* savedCounter = facetBucketBlockCounter;
  size_t blocks = 0, peak = 0, buckets = 0;
  int64_t values = fixture.numDocs;
  IntFacetRangeReq::disablePointsRangeFacetForTests = forceWalk;
  facetBucketBlockCounter = &blocks;
  auto restore = scope_guard([&]() noexcept {
    IntFacetRangeReq::disablePointsRangeFacetForTests = savedWalk;
    facetBucketBlockCounter = savedCounter;
  });
  std::optional<uint64_t> fingerprint;
  for (auto _ : state) {
    blocks = 0;
    auto req = localReq(fixture.helper.getSearchEngine());
    req->collection(collection);
    bool iana = mode == RangeDataBenchMode::IANA || mode == RangeDataBenchMode::IANA_SHIFTED
        || mode == RangeDataBenchMode::IANA_DAILY || mode == RangeDataBenchMode::IANA_DAILY_6H
        || mode == RangeDataBenchMode::CALENDAR_DAY;
    if (iana) req->timeZone("America/New_York");
    OpCursor* facet;
    if (mode == RangeDataBenchMode::NARROW) {
      auto& top = req->topDocs("q").limit(0).exprQuery("retention_dt:[2025-01-01T00:00Z TO 2025-01-02T00:00Z}");
      facet = &top.rangeFacet("f", "retention_dt").rangeData(60000);
    } else if (mode == RangeDataBenchMode::NESTED) {
      facet = &req->facet("parents", "parent_s").limit(-1)
          .rangeFacet("f", "retention_dt").rangeData(3600000);
    } else if (mode == RangeDataBenchMode::CALENDAR_DAY) {
      facet = &req->rangeFacet("f", "date_dt").rangeDataCalendar(1, api::CalendarGap_::Unit::DAY);
    } else {
      bool date = mode == RangeDataBenchMode::DATE || iana;
      bool daily = mode == RangeDataBenchMode::IANA_DAILY || mode == RangeDataBenchMode::IANA_DAILY_6H;
      int64_t gap = date ? (daily ? 86400000 : 3600000)
          : mode == RangeDataBenchMode::DISTINCT_WIDE ? 100 : 100000;
      int64_t origin = mode == RangeDataBenchMode::IANA_SHIFTED ? 17 * 60000
          : mode == RangeDataBenchMode::IANA_DAILY_6H ? 6 * 3600000 : 0;
      facet = &req->rangeFacet("f", date ? "date_dt" : "data_i").rangeData(gap, origin);
    }
    if (explicitBounds) {
      facet->range(explicitStart, explicitEnd,
          mode == RangeDataBenchMode::IANA_DAILY || mode == RangeDataBenchMode::CALENDAR_DAY ? 86400000 : 3600000);
      auto& range = std::get<api::RangeFacet>(facet->rawOp().kind);
      range.bounds = api::RangeFacet_::Bounds::EXPLICIT;
      range.origin.reset();
      if (mode == RangeDataBenchMode::CALENDAR_DAY) {
        auto& calendar = range.gap_kind.emplace<api::CalendarGap>();
        calendar.n = 1;
        calendar.unit = api::CalendarGap_::Unit::DAY;
      }
    }
    if (mode == RangeDataBenchMode::DISTINCT || mode == RangeDataBenchMode::DISTINCT_WIDE || drop) {
      auto& range = std::get<api::RangeFacet>(facet->rawOp().kind);
      auto* key = api::build::allocMessage<api::Collapse>(req->mr);
      *api::build::allocArray(key->field, 1, req->mr) = drop ? "drop_group_i" : "group_i";
      if (drop) key->missing = api::Collapse_::MissingPolicy::DROP;
      range.count_distinct = key;
    }
    req->execute(parallel ? -1 : 1);
    ASSERT_TRUE(req->ok()) << req->errorMsg();
    uint64_t current = 0;
    buckets = 0;
    auto consume = [&](const api::FacetResult& result) {
      const auto& bounds = std::get<api::ArrArrInt>(result.bucket_ids->kind).v;
      buckets += result.counts.size();
      for (size_t i = 0; i < result.counts.size(); i++) {
        current = current * 31 + (uint64_t)bounds[i].v[0] + result.counts[i];
      }
    };
    if (mode == RangeDataBenchMode::NESTED) {
      const auto* parents = req->responses[0]->proto.ops.at("parents")->facetResult();
      for (const auto& child : std::get<api::ArrVal>(parents->ops.at("f")->kind).v) consume(*child.facetResult());
    } else if (mode == RangeDataBenchMode::NARROW) {
      const auto& result = *req->docList()->ops.at("f")->facetResult();
      values = std::accumulate(result.counts.begin(), result.counts.end(), int64_t{0});
      consume(result);
    } else consume(*req->responses[0]->proto.ops.at("f")->facetResult());
    benchmark::DoNotOptimize(current);
    if (fingerprint) { ASSERT_EQ(*fingerprint, current); }
    fingerprint = current;
    peak = std::max(peak, req->memoryTracker.peak());
  }
  state.counters["fp"] = (double)fingerprint.value_or(0);
  state.counters["reused"] = fixture.reused;
  state.counters["blocks"] = (double)blocks;
  rangeDataBenchCounters(state, values, buckets, peak, rss);
}

// Existing nested fixed-bucket shapes. Keep these independent of DATA so the
// resident-byte scheduling change can be measured against the old estimate.
static void BM_FixedBucketResident(benchmark::State& state, bool query, bool metric) {
  RangeDataBenchFixture fixture(10'000'000, "9555");
  size_t blocks = 0;
  auto* savedCounter = facetBucketBlockCounter;
  facetBucketBlockCounter = &blocks;
  auto restore = scope_guard([&]() noexcept { facetBucketBlockCounter = savedCounter; });
  uint64_t fingerprint = 0;
  size_t peak = 0;
  for (auto _ : state) {
    blocks = 0;
    auto req = localReq(fixture.helper.getSearchEngine());
    auto& parent = req->collection("facet_range_data_bm").facet("parents", "parent_s").limit(-1);
    auto& child = parent.rangeFacet("child", "data_i");
    if (query) {
      auto& facet = child.rawOp().kind.emplace<api::QueryFacet>();
      auto* buckets = api::build::allocArray(facet.buckets, 64, req->mr);
      for (size_t i = 0; i < 64; i++) {
        buckets[i].name = api::build::arenaStr(req->mr, std::to_string(i));
        auto* predicate = api::build::allocMessage<api::Query>(req->mr);
        predicate->kind = true;
        buckets[i].query = predicate;
      }
      if (metric) api::build::mapSlot(facet.ops, 1, "mean", req->mr)->kind.emplace<api::ExprOp>().expr = "avg(group_i)";
    } else {
      child.range(0, 10'000'000, metric ? 10000 : 1000);
    }
    if (metric && !query) child.avg("mean", "group_i");
    req->execute(false);
    ASSERT_TRUE(req->ok()) << req->errorMsg();
    const auto& result = *req->responses[0]->proto.ops.at("parents")->facetResult();
    uint64_t current = 0;
    for (auto count : result.counts) current = current * 31 + count;
    const auto& children = std::get<api::ArrVal>(result.ops.at("child")->kind).v;
    for (const auto& value : children) {
      const auto& nested = *value.facetResult();
      for (auto count : nested.counts) current = current * 31 + count;
      if (metric) {
        const auto& means = std::get<api::ArrVal>(nested.ops.at("mean")->kind).v;
        for (const auto& mean : means) {
          if (!mean.isNull()) current = current * 31 + (uint64_t)mean.asDouble();
        }
      }
    }
    if (fingerprint) { ASSERT_EQ(fingerprint, current); }
    fingerprint = current;
    peak = std::max(peak, req->memoryTracker.peak());
    benchmark::DoNotOptimize(current);
  }
  state.counters["blocks"] = (double)blocks;
  state.counters["peak_bytes"] = (double)peak;
  state.counters["fp"] = (double)fingerprint;
}

LUXIR_BENCHMARK_CAPTURE(BM_FixedBucketResident, range_counts, false, false);
LUXIR_BENCHMARK_CAPTURE(BM_FixedBucketResident, range_metric, false, true);
LUXIR_BENCHMARK_CAPTURE(BM_FixedBucketResident, query_counts, true, false);
LUXIR_BENCHMARK_CAPTURE(BM_FixedBucketResident, query_metric, true, true);

// Three observed cells occupy a year-wide directory. Put the endpoints in
// the first segment so the wide state stays resident while another segment
// is still due. mincount=1 bounds output and permits avg under the sub-op cap.
struct RangeFanoutBenchFixture {
  CollectionHelper helper{"facet_range_data_fanout_bm"};
  int parents;
  explicit RangeFanoutBenchFixture() : parents(unit_tests ? 8 : 10000) {
    std::array<int32_t, 2> shape{parents * 2, parents};
    if (helper.indexMatchesShape(shape)) return;
    helper.clear();
    SchemaBuilder schema;
    schema.field("parent_s").type = api::FieldDef::FieldClass::STRING;
    schema.field("parent_i").type = api::FieldDef::FieldClass::INT;
    schema.field("date_dt").type = api::FieldDef::FieldClass::DATE;
    schema.field("score_i").type = api::FieldDef::FieldClass::INT;
    schema.set(helper.collection());
    for (int segment = 0; segment < 2; segment++) {
      std::vector<Doc> docs;
      for (int parent = 0; parent < parents; parent++) {
        for (int cell : segment == 0 ? std::vector<int>{0, 8759} : std::vector<int>{4380}) {
          docs.push_back(flatdoc("id", std::to_string(parent) + ":" + std::to_string(cell),
              "parent_s", std::to_string(parent), "parent_i", parent,
              "date_dt", RangeDataBenchFixture::epoch + (int64_t)cell * 3600000,
              "score_i", parent + cell));
        }
      }
      auto result = helper.indexAll(docs, UpdateMessage::COMMIT);
      if (!result.success) throw std::runtime_error(result.error_message);
    }
  }
};

static void BM_RangeFacetDataNested(benchmark::State& state, bool numeric,
                                     bool metric, bool parallel, bool fanout) {
  std::unique_ptr<RangeDataBenchFixture> regular;
  std::unique_ptr<RangeFanoutBenchFixture> large;
  if (fanout) large = std::make_unique<RangeFanoutBenchFixture>();
  else regular = std::make_unique<RangeDataBenchFixture>(10000000, "9555", "facet_range_data_nested_bm");
  auto& helper = fanout ? large->helper : regular->helper;
  RSSWatcher rss;
  size_t blocks = 0, peak = 0, emitted = 0;
  auto* saved = facetBucketBlockCounter;
  facetBucketBlockCounter = &blocks;
  auto restore = scope_guard([&]() noexcept { facetBucketBlockCounter = saved; });
  std::optional<uint64_t> fingerprint;
  for (auto _ : state) {
    blocks = 0;
    emitted = 0;
    auto req = localReq(helper.getSearchEngine());
    req->collection(fanout ? "facet_range_data_fanout_bm" : "facet_range_data_nested_bm");
    auto& parent = req->facet("parents", numeric ? "parent_i" : "parent_s").limit(-1);
    auto& child = parent.rangeFacet("hist", fanout ? "date_dt" : "retention_dt")
        .rangeData(fanout ? 3600000 : 86400000).mincount(1);
    if (metric) child.avg("avg", fanout ? "score_i" : "group_i");
    req->execute(parallel ? -1 : 1);
    ASSERT_TRUE(req->ok()) << req->errorMsg();
    const auto& p = *req->responses[0]->proto.ops.at("parents")->facetResult();
    uint64_t fp = 0;
    for (const auto& row : std::get<api::ArrVal>(p.ops.at("hist")->kind).v) {
      const auto& h = *row.facetResult();
      emitted += h.counts.size();
      for (auto count : h.counts) fp = fp * 31 + count;
      if (metric) for (const auto& mean : std::get<api::ArrVal>(h.ops.at("avg")->kind).v) {
        if (!mean.isNull()) fp = fp * 31 + (uint64_t)mean.asDouble();
      }
    }
    if (fingerprint) { ASSERT_EQ(*fingerprint, fp); }
    fingerprint = fp;
    peak = std::max(peak, req->memoryTracker.peak());
    benchmark::DoNotOptimize(fp);
  }
  state.counters["blocks"] = (double)blocks;
  state.counters["emitted"] = (double)emitted;
  rangeDataBenchCounters(state, fanout ? large->parents * 3 : regular->numDocs, emitted, peak, rss);
}

LUXIR_BENCHMARK_CAPTURE(BM_RangeFacetDataNested, string_counts, false, false, false, false);
LUXIR_BENCHMARK_CAPTURE(BM_RangeFacetDataNested, string_avg, false, true, false, false);
LUXIR_BENCHMARK_CAPTURE(BM_RangeFacetDataNested, string_counts_parallel, false, false, true, false);
LUXIR_BENCHMARK_CAPTURE(BM_RangeFacetDataNested, string_avg_parallel, false, true, true, false);
LUXIR_BENCHMARK_CAPTURE(BM_RangeFacetDataNested, numeric_counts, true, false, false, false);
LUXIR_BENCHMARK_CAPTURE(BM_RangeFacetDataNested, numeric_avg, true, true, false, false);
LUXIR_BENCHMARK_CAPTURE(BM_RangeFacetDataNested, numeric_counts_parallel, true, false, true, false);
LUXIR_BENCHMARK_CAPTURE(BM_RangeFacetDataNested, numeric_avg_parallel, true, true, true, false);
LUXIR_BENCHMARK_CAPTURE(BM_RangeFacetDataNested, fanout_string_counts, false, false, false, true);
LUXIR_BENCHMARK_CAPTURE(BM_RangeFacetDataNested, fanout_string_avg, false, true, false, true);
LUXIR_BENCHMARK_CAPTURE(BM_RangeFacetDataNested, fanout_string_counts_parallel, false, false, true, true);
LUXIR_BENCHMARK_CAPTURE(BM_RangeFacetDataNested, fanout_string_avg_parallel, false, true, true, true);
LUXIR_BENCHMARK_CAPTURE(BM_RangeFacetDataNested, fanout_numeric_counts, true, false, false, true);
LUXIR_BENCHMARK_CAPTURE(BM_RangeFacetDataNested, fanout_numeric_avg, true, true, false, true);
LUXIR_BENCHMARK_CAPTURE(BM_RangeFacetDataNested, fanout_numeric_counts_parallel, true, false, true, true);
LUXIR_BENCHMARK_CAPTURE(BM_RangeFacetDataNested, fanout_numeric_avg_parallel, true, true, true, true);

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
    auto* fieldReader = postings.fieldInfo(field);
    if (!(fieldReader != nullptr)) continue;
    const auto& info = *fieldReader;
    OrdColReader column(postings, info);
    int64_t missing = 0;
    int64_t previous = -1;
    forEachOrdValue(nullptr, column, OrdMap::SegToGlobal::identity(),
                    postings.maxDoc(), missing,
                    [&](int32_t docid, int64_t ord) {
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
LUXIR_BENCHMARK_CAPTURE(BM_RangeFacetData, unfiltered_int, nDocs, shape, RangeDataBenchMode::INT);
LUXIR_BENCHMARK_CAPTURE(BM_RangeFacetData, unfiltered_int_walk, nDocs, shape, RangeDataBenchMode::INT, true);
LUXIR_BENCHMARK_CAPTURE(BM_RangeFacetData, unfiltered_date, nDocs, shape, RangeDataBenchMode::DATE);
LUXIR_BENCHMARK_CAPTURE(BM_RangeFacetData, unfiltered_date_walk, nDocs, shape, RangeDataBenchMode::DATE, true);
LUXIR_BENCHMARK_CAPTURE(BM_RangeFacetData, iana_hourly, nDocs, shape, RangeDataBenchMode::IANA);
LUXIR_BENCHMARK_CAPTURE(BM_RangeFacetData, iana_hourly_walk, nDocs, shape, RangeDataBenchMode::IANA, true);
LUXIR_BENCHMARK_CAPTURE(BM_RangeFacetData, iana_hourly_17m, nDocs, shape, RangeDataBenchMode::IANA_SHIFTED);
LUXIR_BENCHMARK_CAPTURE(BM_RangeFacetData, iana_hourly_17m_walk, nDocs, shape, RangeDataBenchMode::IANA_SHIFTED, true);
LUXIR_BENCHMARK_CAPTURE(BM_RangeFacetData, iana_daily, nDocs, shape, RangeDataBenchMode::IANA_DAILY);
LUXIR_BENCHMARK_CAPTURE(BM_RangeFacetData, iana_daily_walk, nDocs, shape, RangeDataBenchMode::IANA_DAILY, true);
LUXIR_BENCHMARK_CAPTURE(BM_RangeFacetData, iana_daily_6h, nDocs, shape, RangeDataBenchMode::IANA_DAILY_6H);
LUXIR_BENCHMARK_CAPTURE(BM_RangeFacetData, iana_daily_6h_walk, nDocs, shape, RangeDataBenchMode::IANA_DAILY_6H, true);
LUXIR_BENCHMARK_CAPTURE(BM_RangeFacetData, calendar_day, nDocs, shape, RangeDataBenchMode::CALENDAR_DAY);
LUXIR_BENCHMARK_CAPTURE(BM_RangeFacetData, calendar_day_walk, nDocs, shape, RangeDataBenchMode::CALENDAR_DAY, true);
LUXIR_BENCHMARK_CAPTURE(BM_RangeFacetData, explicit_iana_hourly_walk, nDocs, shape, RangeDataBenchMode::IANA, true, false, true);
LUXIR_BENCHMARK_CAPTURE(BM_RangeFacetData, explicit_iana_hourly_17m_walk, nDocs, shape, RangeDataBenchMode::IANA_SHIFTED, true, false, true);
LUXIR_BENCHMARK_CAPTURE(BM_RangeFacetData, explicit_iana_daily_walk, nDocs, shape, RangeDataBenchMode::IANA_DAILY, true, false, true);
LUXIR_BENCHMARK_CAPTURE(BM_RangeFacetData, explicit_calendar_day_walk, nDocs, shape, RangeDataBenchMode::CALENDAR_DAY, true, false, true);
LUXIR_BENCHMARK_CAPTURE(BM_RangeFacetData, shuffled_iana_hourly_walk, nDocs, shape, RangeDataBenchMode::IANA, true, false, false, true);
LUXIR_BENCHMARK_CAPTURE(BM_RangeFacetData, shuffled_iana_hourly_17m_walk, nDocs, shape, RangeDataBenchMode::IANA_SHIFTED, true, false, false, true);
LUXIR_BENCHMARK_CAPTURE(BM_RangeFacetData, shuffled_iana_daily_walk, nDocs, shape, RangeDataBenchMode::IANA_DAILY, true, false, false, true);
LUXIR_BENCHMARK_CAPTURE(BM_RangeFacetData, shuffled_calendar_day_walk, nDocs, shape, RangeDataBenchMode::CALENDAR_DAY, true, false, false, true);
LUXIR_BENCHMARK_CAPTURE(BM_RangeFacetData, narrow_retention, nDocs, shape, RangeDataBenchMode::NARROW);
LUXIR_BENCHMARK_CAPTURE(BM_RangeFacetData, distinct_int, nDocs, shape, RangeDataBenchMode::DISTINCT);
LUXIR_BENCHMARK_CAPTURE(BM_RangeFacetData, distinct_int_parallel, nDocs, shape, RangeDataBenchMode::DISTINCT, false, true);
LUXIR_BENCHMARK_CAPTURE(BM_RangeFacetData, distinct_wide, nDocs, shape, RangeDataBenchMode::DISTINCT_WIDE);
LUXIR_BENCHMARK_CAPTURE(BM_RangeFacetData, distinct_wide_parallel, nDocs, shape, RangeDataBenchMode::DISTINCT_WIDE, false, true);
LUXIR_BENCHMARK_CAPTURE(BM_RangeFacetData, distinct_drop, nDocs, shape, RangeDataBenchMode::DISTINCT_DROP);
LUXIR_BENCHMARK_CAPTURE(BM_RangeFacetData, distinct_drop_parallel, nDocs, shape, RangeDataBenchMode::DISTINCT_DROP, false, true);
LUXIR_BENCHMARK_CAPTURE(BM_RangeFacetData, nested_retention, nDocs, shape, RangeDataBenchMode::NESTED);
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
