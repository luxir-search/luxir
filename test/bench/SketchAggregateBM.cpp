// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <random>
#include <string>
#include <string_view>

#include "bench/luxir_bench.h"
#include "luxir/server/JsonRequest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"

using namespace luxir;
using namespace luxir::test;

namespace {

struct SketchAggregateCase {
  std::string_view json;
  bool filtered = false;
  int32_t buckets = 0;
};

// Keep the complete requests together so alternate API surfaces can use the
// same corpus, execution loop, and registrations.
constexpr std::array<SketchAggregateCase, 11> cases{{
  {R"json({"ops":{"metric":"cardinality(user_s)"}})json"},
  {R"json({"ops":{"metric":"cardinality(size_i)"}})json"},
  {R"json({"ops":{"metric":"percentile(latency_d,99)"}})json"},
  {R"json({"ops":{"metric":"percentile_rank(latency_d,100)"}})json"},
  {R"json({"ops":{"p50":"percentile(latency_d,50)","p95":"percentile(latency_d,95)","p99":"percentile(latency_d,99)"}})json"},
  {R"json({"query":"size_i:[0 TO 4999]","limit":0,"count_limit":-1,"ops":{"metric":"cardinality(user_s)"}})json", true},
  {R"json({"query":"size_i:[0 TO 4999]","limit":0,"count_limit":-1,"ops":{"metric":"percentile(latency_d,99)"}})json", true},
  {R"json({"ops":{"hosts":{"field_facet":{"field":"host_s","limit":10,"ops":{"metric":"cardinality(user_s)"}}}}})json", false, 10},
  {R"json({"ops":{"hosts":{"field_facet":{"field":"host_s","limit":10,"ops":{"metric":"percentile(latency_d,99)"}}}}})json", false, 10},
  {R"json({"ops":{"hosts":{"field_facet":{"field":"host_s","limit":-1,"ops":{"metric":"cardinality(user_s)"}}}}})json", false, 100},
  {R"json({"ops":{"hosts":{"field_facet":{"field":"host_s","limit":-1,"ops":{"metric":"percentile(latency_d,99)"}}}}})json", false, 100},
}};

struct SketchAggregateValues {
  int64_t user;
  int64_t size;
  double latency;
};

struct SketchAggregateGenerator {
  std::mt19937_64 random{37};
  std::uniform_int_distribution<int64_t> users{0, 199999};
  std::uniform_int_distribution<int64_t> sizes{0, 49999};
  std::lognormal_distribution<double> latencies{3.0, 1.0};

  SketchAggregateValues next() {
    return {users(random), sizes(random), latencies(random)};
  }
};

struct SketchAggregateFixture {
  static constexpr std::string_view collectionName = "sketch_aggregate_bm";
  static constexpr int32_t numSegments = 8;
  CollectionHelper helper{collectionName};
  int64_t numDocs = unit_tests ? LuxirTest::scaleTestWork(800) : 1000000;
  int64_t filteredDocs = 0;
  std::shared_ptr<IndexReader> reader;

  SketchAggregateFixture() {
    std::array<int32_t, numSegments> shape;
    for (int32_t segment = 0; segment < numSegments; segment++) {
      shape[segment] = (int32_t)(numDocs * (segment + 1) / numSegments
                                - numDocs * segment / numSegments);
    }
    bool reused = helper.indexMatchesShape(shape);
    LOG_DEBUG("Sketch aggregate corpus: reused={} docs={}", reused, numDocs);
    SketchAggregateGenerator generator;
    if (reused) {
      for (int64_t doc = 0; doc < numDocs; doc++) filteredDocs += generator.next().size < 5000;
      reader = helper.getIndexWriter()->snapshots.readers.getReader();
      return;
    }
    auto& node = helper.getNode();
    node.deleteCollection(collectionName);
    helper = CollectionHelper(node, collectionName);
    auto writer = helper.getIndexWriter();
    // Eight explicit flushes cannot trigger a background merge at this factor.
    writer->mergePolicy->setMergeFactor(1000);
    std::array<std::string, 100> hosts;
    for (size_t i = 0; i < hosts.size(); i++) hosts[i] = "host-" + std::to_string(i);

    int64_t doc = 0;
    for (int32_t segment = 0; segment < numSegments; segment++) {
      auto& inv = writer->obtainInverter();
      auto& id = inv.getIndexHandler("id");
      auto& host = inv.getIndexHandler("host_s");
      auto& user = inv.getIndexHandler("user_s");
      auto& size = inv.getIndexHandler("size_i");
      auto& latency = inv.getIndexHandler("latency_d");
      int64_t end = doc + shape[segment];
      for (; doc < end; doc++) {
        auto values = generator.next();
        api::Val latencyValue;
        latencyValue.kind = values.latency;
        filteredDocs += values.size < 5000;
        inv.startDoc();
        id.index(inv, std::to_string(doc));
        host.index(inv, hosts[doc % hosts.size()]);
        // Independent users keep host buckets above the HLL exact threshold
        // at benchmark scale, rather than assigning each user to one host.
        user.index(inv, "user-" + std::to_string(values.user));
        size.index(inv, values.size);
        latency.index(inv, latencyValue);
        inv.finishDoc();
      }
      writer->releaseInverter(inv, true);
      helper.commit();
    }
    reader = writer->snapshots.readers.getReader();
  }
};

void BM_SketchAggregate(benchmark::State& state, size_t caseIndex) {
  SketchAggregateFixture fixture;
  const auto& entry = cases.at(caseIndex);
  auto run = [&]() {
    auto req = localReq(fixture.helper.getSearchEngine());
    parseQueryRequest(entry.json, req->rawRequest(), req->mr);
    req->collection(SketchAggregateFixture::collectionName);
    req->execute(false);
    return req;
  };

  ASSERT_EQ(SketchAggregateFixture::numSegments, fixture.reader->segments().size());
  ASSERT_EQ(fixture.numDocs, fixture.reader->liveDocs());
  // Prime field/ordinal/filter caches outside timing. Each case measures a
  // steady-state request, including JSON parsing and response construction.
  {
    auto req = run();
    ASSERT_TRUE(req->ok()) << req->errorMsg();
    ASSERT_EQ(fixture.reader.get(), req->reader.get());
    if (entry.filtered) {
      ASSERT_EQ(fixture.filteredDocs, req->docList("q")->found);
    }
    if (entry.buckets) {
      const auto& facet = *req->responses[0]->proto.ops.at("hosts")->facetResult();
      ASSERT_EQ(entry.buckets, facet.counts.size());
    }
  }

  for (auto _ : state) {
    auto req = run();
    ASSERT_TRUE(req->ok()) << req->errorMsg();
    ASSERT_EQ(fixture.reader.get(), req->reader.get());
    benchmark::DoNotOptimize(req->responses[0]->proto);
  }
  int64_t domain = entry.filtered ? fixture.filteredDocs : fixture.numDocs;
  state.counters["docs"] = (double)fixture.numDocs;
  state.counters["domain"] = (double)domain;
  state.counters["segments"] = (double)fixture.reader->segments().size();
  if (entry.buckets) state.counters["buckets"] = (double)entry.buckets;
  state.counters["rate"] = benchmark::Counter((double)state.iterations(), benchmark::Counter::kIsRate);
  state.SetItemsProcessed((int64_t)state.iterations() * domain);
}

LUXIR_BENCHMARK_CAPTURE(BM_SketchAggregate, root_cardinality_user, 0);
LUXIR_BENCHMARK_CAPTURE(BM_SketchAggregate, root_cardinality_size, 1);
LUXIR_BENCHMARK_CAPTURE(BM_SketchAggregate, root_percentile_99, 2);
LUXIR_BENCHMARK_CAPTURE(BM_SketchAggregate, root_percentile_rank_100, 3);
LUXIR_BENCHMARK_CAPTURE(BM_SketchAggregate, root_percentiles_50_95_99, 4);
LUXIR_BENCHMARK_CAPTURE(BM_SketchAggregate, filtered_cardinality_user, 5);
LUXIR_BENCHMARK_CAPTURE(BM_SketchAggregate, filtered_percentile_99, 6);
LUXIR_BENCHMARK_CAPTURE(BM_SketchAggregate, facet_10_cardinality_user, 7);
LUXIR_BENCHMARK_CAPTURE(BM_SketchAggregate, facet_10_percentile_99, 8);
LUXIR_BENCHMARK_CAPTURE(BM_SketchAggregate, facet_all_cardinality_user, 9);
LUXIR_BENCHMARK_CAPTURE(BM_SketchAggregate, facet_all_percentile_99, 10);

} // namespace
