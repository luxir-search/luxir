// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <random>
#include <string>
#include <string_view>

#include "bench/luxir_bench.h"
#include "luxir/search/SearchOverrides.h"
#include "luxir/server/JsonRequest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"

using namespace luxir;
using namespace luxir::test;

namespace {

// Plain metric aggregations as an Elasticsearch gateway sends them: a stats
// aggregation is five sibling ops over one field.
constexpr std::string_view STATS = R"json({"n":"sum(count(fare_d))","lo":"min(fare_d)",
  "hi":"max(fare_d)","mean":"avg(fare_d)","total":"sum(fare_d)"})json";

struct MetricAggregateCase {
  std::string json;
  // The same request with sibling fusion turned off.
  bool unfused = false;
};

std::array<MetricAggregateCase, 4> cases() {
  std::string stats(STATS);
  return {{
    {R"json({"ops":)json" + stats + "}"},
    {R"json({"ops":)json" + stats + "}", true},
    {R"json({"ops":{"q":{"top_docs":{"limit":0,"query":"passengers_i:[2 TO 4]","ops":)json"
        + stats + "}}}}"},
    {R"json({"ops":{"q":{"top_docs":{"limit":0,"query":"passengers_i:[2 TO 4]","ops":)json"
        + stats + "}}}}", true},
  }};
}

struct MetricAggregateFixture {
  static constexpr std::string_view collectionName = "metric_aggregate_bm";
  static constexpr int32_t numSegments = 8;
  CollectionHelper helper{collectionName};
  int64_t numDocs = unit_tests ? LuxirTest::scaleTestWork(800) : 2000000;
  std::shared_ptr<IndexReader> reader;

  MetricAggregateFixture() {
    std::array<int32_t, numSegments> shape;
    for (int32_t segment = 0; segment < numSegments; segment++) {
      shape[segment] = (int32_t)(numDocs * (segment + 1) / numSegments
                                - numDocs * segment / numSegments);
    }
    if (helper.indexMatchesShape(shape)) {
      reader = helper.getIndexWriter()->snapshots.readers.getReader();
      return;
    }
    auto& node = helper.getNode();
    node.deleteCollection(collectionName);
    helper = CollectionHelper(node, collectionName);
    auto writer = helper.getIndexWriter();
    writer->mergePolicy->setMergeFactor(1000);
    std::mt19937_64 random(41);
    std::lognormal_distribution<double> fares(2.5, 0.8);
    std::uniform_int_distribution<int64_t> passengers(1, 6);
    std::uniform_int_distribution<int64_t> vendors(1, 8);
    int64_t doc = 0;
    for (int32_t segment = 0; segment < numSegments; segment++) {
      auto& inv = writer->obtainInverter();
      auto& id = inv.getIndexHandler("id");
      auto& fare = inv.getIndexHandler("fare_d");
      auto& distance = inv.getIndexHandler("distance_d");
      auto& passenger = inv.getIndexHandler("passengers_i");
      auto& vendor = inv.getIndexHandler("vendor_i");
      int64_t end = doc + shape[segment];
      for (; doc < end; doc++) {
        api::Val fareValue, distanceValue;
        fareValue.kind = fares(random);
        distanceValue.kind = std::get<double>(fareValue.kind) / 3.0;
        inv.startDoc();
        id.index(inv, std::to_string(doc));
        // One fare in sixteen is missing, as count() must then differ from
        // the domain size.
        if (doc % 16 != 0) fare.index(inv, fareValue);
        distance.index(inv, distanceValue);
        passenger.index(inv, passengers(random));
        vendor.index(inv, vendors(random));
        inv.finishDoc();
      }
      writer->releaseInverter(inv, true);
      helper.commit();
    }
    reader = writer->snapshots.readers.getReader();
  }
};

void BM_MetricAggregate(benchmark::State& state, size_t caseIndex) {
  MetricAggregateFixture fixture;
  const auto entry = cases().at(caseIndex);
  SearchOverridesGuard guard(disableAggregateSharingForTests);
  disableAggregateSharingForTests = entry.unfused;
  auto run = [&]() {
    auto req = localReq(fixture.helper.getSearchEngine());
    parseQueryRequest(entry.json, req->rawRequest(), req->mr);
    req->collection(MetricAggregateFixture::collectionName);
    req->execute(false);
    return req;
  };

  ASSERT_EQ(MetricAggregateFixture::numSegments, fixture.reader->segments().size());
  {
    auto req = run();
    ASSERT_TRUE(req->ok()) << req->errorMsg();
  }
  for (auto _ : state) {
    auto req = run();
    ASSERT_TRUE(req->ok()) << req->errorMsg();
    benchmark::DoNotOptimize(req->responses[0]->proto);
  }
  state.counters["docs"] = (double)fixture.numDocs;
  state.SetItemsProcessed((int64_t)state.iterations() * fixture.numDocs);
}

LUXIR_BENCHMARK_CAPTURE(BM_MetricAggregate, root_stats, 0);
LUXIR_BENCHMARK_CAPTURE(BM_MetricAggregate, root_stats_unfused, 1);
LUXIR_BENCHMARK_CAPTURE(BM_MetricAggregate, filtered_stats, 2);
LUXIR_BENCHMARK_CAPTURE(BM_MetricAggregate, filtered_stats_unfused, 3);

} // namespace
