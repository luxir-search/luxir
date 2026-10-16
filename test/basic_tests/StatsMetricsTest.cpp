// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#include "test/LuxirTest.h"
#include "test/HttpReq.h"
#include "test/GrpcClient.h"
#include "test/GrpcLuxirTest.h"
#include "luxir/server/HttpServer.h"
#include "luxir/server/Prometheus.h"
#include "luxir/server/Stats.h"
#include <cmath>
#include <fstream>
#include <future>
#include <thread>

namespace luxir::test {
namespace {
double sample(const std::string& text, const std::string& name) {
  auto pos = text.find("\n" + name + " ");
  if (pos == std::string::npos) {
    ADD_FAILURE() << "Missing metric: " << name;
    return std::nan("");
  }
  return std::stod(text.substr(pos + name.size() + 2));
}
std::string httpLabels(std::string_view operation, std::string_view outcome) {
  return "{transport=\"http\",operation=\"" + std::string(operation) + "\",outcome=\"" + std::string(outcome) + "\"}";
}
constexpr const char* scrape = "/_stats?collections=true&format=prometheus";
api::StatsRequest statsRequest(bool collections, bool files = false, std::string_view collection = {},
                               bool system = false) {
  api::StatsRequest request;
  request.collections = collections;
  request.files = files;
  request.collection = collection;
  request.system = system;
  return request;
}
std::string render(LuxirNode& node, api::StatsRequest request = statsRequest(true)) {
  std::pmr::monotonic_buffer_resource resource;
  api::StatsResponse response;
  gatherStats(node, request, response, resource);
  return renderPrometheus(response, request.files);
}
}

class StatsMetricsTest : public LuxirTest {};

TEST_F(StatsMetricsTest, HttpTrafficAndIndexingOutcomes) {
  LuxirNode node;
  HttpServer server(node, 2, 0);
  server.start();
  auto port = server.getPort();
  auto update = httpRequest(port, http::verb::post, "/collections/metrics/_update",
      R"({"docs":[{"id":"one"},{"id":"two"}],"commit":{}})");
  ASSERT_EQ(200, update.result_int()) << update.body();
  auto search = httpRequest(port, http::verb::post, "/collections/metrics/_search",
      R"({"ops":{"q":{"top_docs":{"limit":1,"get_number":true}}}})");
  ASSERT_EQ(200, search.result_int()) << search.body();
  ASSERT_EQ(400, httpRequest(port, http::verb::post, "/collections/metrics/_search", "{").result_int());
  ASSERT_EQ(404, httpRequest(port, http::verb::get, "/does-not-exist").result_int());
  ASSERT_EQ(405, httpRequest(port, http::verb::post, "/health").result_int());
  ASSERT_EQ(404, httpRequest(port, http::verb::get, "/metrics").result_int());
  auto response = httpRequest(port, http::verb::get, scrape);
  ASSERT_EQ(200, response.result_int());
  EXPECT_EQ(prometheusContentType, response[http::field::content_type]);
  auto& text = response.body();
  EXPECT_EQ(1, sample(text, "luxir_requests_total" + httpLabels("update", "ok")));
  EXPECT_EQ(1, sample(text, "luxir_requests_total" + httpLabels("search", "ok")));
  EXPECT_EQ(1, sample(text, "luxir_requests_total" + httpLabels("search", "error")));
  EXPECT_EQ(1, sample(text, "luxir_requests_total" + httpLabels("health", "error")));
  EXPECT_EQ(2, sample(text, "luxir_ingest_documents_total{outcome=\"accepted\"}"));
  EXPECT_EQ(0, sample(text, "luxir_requests_in_flight{transport=\"http\",operation=\"search\"}"));
  EXPECT_EQ(0, sample(text, "luxir_stats_collection_errors"));
  EXPECT_GT(sample(text, "luxir_background_total{kind=\"commit\",outcome=\"ok\"}"), 0);
  EXPECT_GT(sample(text, "luxir_index_committed_bytes{tenant=\"t0\",collection=\"metrics\"}"), 0);
  EXPECT_EQ(2, sample(text, "luxir_index_documents{tenant=\"t0\",collection=\"metrics\",state=\"live\"}"));
  EXPECT_EQ(1, sample(text, "luxir_requests_duration_seconds_count{transport=\"http\",operation=\"update\"}"));
  EXPECT_EQ(1, sample(text, "luxir_requests_duration_seconds_bucket{transport=\"http\",operation=\"update\",le=\"+Inf\"}"));
  // Directory bytes need files=true; the cheap walk lists no directories.
  EXPECT_EQ(std::string::npos, text.find("luxir_index_directory_bytes"));
  // Stats requests are their own operation, so they never blur service traffic.
  auto again = httpRequest(port, http::verb::get, scrape).body();
  EXPECT_EQ(sample(text, "luxir_requests_total" + httpLabels("stats", "ok")) + 1,
            sample(again, "luxir_requests_total" + httpLabels("stats", "ok")));
  EXPECT_EQ(sample(text, "luxir_requests_total" + httpLabels("search", "ok")),
            sample(again, "luxir_requests_total" + httpLabels("search", "ok")));
  server.shutdown();
  EXPECT_EQ(0, node.metrics->httpThreads.load());
}

TEST_F(StatsMetricsTest, StreamingFailuresAndAtomicRollback) {
  LuxirNode node;
  HttpServer server(node, 1, 0);
  server.start();
  auto port = server.getPort();
  auto schema = httpRequest(port, http::verb::post, "/collections/_create",
      R"({"name":"metrics","schema":{"fields":{"id":{"type":"id"},"n":{"type":"int"}}}})");
  ASSERT_EQ(200, schema.result_int()) << schema.body();
  auto atomic = httpRequest(port, http::verb::post, "/collections/metrics/_update",
      R"({"all_or_none":true,"docs":[{"id":"one","n":1},{"id":"bad","n":"not-a-number"}]})");
  ASSERT_EQ(200, atomic.result_int()) << atomic.body();
  auto stream = httpRequest(port, http::verb::post, "/collections/metrics/_update",
      "{\"id\":\"good\",\"n\":2}\n{\"id\":\"bad\",\"n\":\"not-a-number\"}\n", "application/x-ndjson");
  ASSERT_EQ(200, stream.result_int()) << stream.body();
  auto text = httpRequest(port, http::verb::get, scrape).body();
  EXPECT_EQ(1, sample(text, "luxir_ingest_documents_total{outcome=\"accepted\"}"));
  EXPECT_EQ(3, sample(text, "luxir_ingest_documents_total{outcome=\"rejected\"}"));
  EXPECT_EQ(2, sample(text, "luxir_requests_total" + httpLabels("update", "error")));
}

TEST_F(StatsMetricsTest, ConcurrentScrapesAndCollectionRemoval) {
  LuxirNode node;
  node.createCollection("metrics");
  HttpServer server(node, 2, 0);
  server.start();
  std::vector<std::future<std::string>> scrapes;
  for (int i = 0; i < 4; ++i) scrapes.push_back(std::async(std::launch::async, [&] {
    return httpRequest(server.getPort(), http::verb::get, scrape).body();
  }));
  node.deleteCollection("metrics");
  for (auto& response : scrapes) {
    auto text = response.get();
    EXPECT_NE(std::string::npos, text.find("# TYPE luxir_requests_total counter\n"));
  }
  EXPECT_EQ(std::string::npos, render(node).find("collection=\"metrics\""));
}

TEST_F(StatsMetricsTest, EscapingAndHistogramBuckets) {
  MetricHistogram histogram;
  histogram.observe(.001);
  histogram.observe(150);
  auto snapshot = histogram.snapshot();
  EXPECT_DOUBLE_EQ(150.001, snapshot.sum);
  std::array<uint64_t, MetricHistogram::bounds.size() + 1> counts{};
  uint64_t cumulative = 0;
  for (size_t i = 0; i < counts.size(); ++i) counts[i] = cumulative += snapshot.buckets[i];
  PrometheusText text;
  text.histogram("test_seconds", "Latency.", MetricHistogram::bounds, {counts, snapshot.sum, cumulative},
      PrometheusText::label("label", "quote\" slash\\ newline\n"));
  // Nothing observed: every bucket renders as zero.
  text.histogram("empty_seconds", "Latency.", MetricHistogram::bounds, {});
  auto rendered = text.str();
  EXPECT_NE(std::string::npos, rendered.find("label=\"quote\\\" slash\\\\ newline\\n\""));
  EXPECT_NE(std::string::npos, rendered.find("le=\"+Inf\"} 2\n"));
  EXPECT_NE(std::string::npos, rendered.find("le=\"0.001\"} 1\n"));
  EXPECT_NE(std::string::npos, rendered.find("empty_seconds_bucket{le=\"+Inf\"} 0\n"));
  EXPECT_NE(std::string::npos, rendered.find("empty_seconds_count 0\n"));
}

TEST_F(StatsMetricsTest, CheapDefaultsAndOptInSections) {
  LuxirNode node;
  node.createCollection("metrics");
  std::pmr::monotonic_buffer_resource resource;
  api::StatsResponse cheap;
  gatherStats(node, {}, cheap, resource);
  EXPECT_TRUE(cheap.service && cheap.memory);
  EXPECT_FALSE(cheap.totals || cheap.system);
  EXPECT_TRUE(cheap.collections.empty());
  auto text = renderPrometheus(cheap, false);
  EXPECT_EQ(std::string::npos, text.find("collection=\"metrics\""));
  EXPECT_EQ(std::string::npos, text.find("process_cpu_seconds_total"));
  EXPECT_NE(std::string::npos, text.find("luxir_index_ram_draining_bytes "));

  // One response, two renderings: the formats agree.
  api::StatsResponse walked;
  gatherStats(node, statsRequest(true), walked, resource);
  ASSERT_TRUE(walked.totals);
  ASSERT_EQ(node.collectionEntries().size(), walked.collections.size());
  text = renderPrometheus(walked, false);
  EXPECT_EQ(walked.totals->collections, sample(text, "luxir_collections{state=\"available\"}"));
  EXPECT_EQ(walked.memory->indexing_budget.limit_bytes, sample(text, "luxir_index_ram_budget_bytes"));
  std::string json;
  ASSERT_TRUE(api::write_json(walked, json));
  glz::generic_i64 parsed;
  ASSERT_FALSE(glz::read_json(parsed, json)) << json;
  EXPECT_TRUE(parsed["service"]["http"]["operations"].contains("search"));
  EXPECT_TRUE(parsed["service"]["http"]["errors"].contains("not_found"));

  // Directory listings are opt-in; committed bytes come from the snapshot.
  auto index = [&](const api::StatsResponse& r) -> const api::IndexStats& {
    for (const auto& c : r.collections) if (c.name == "metrics") return c.shards[0].index;
    throw std::runtime_error("no metrics collection");
  };
  EXPECT_EQ(0u, index(walked).totals.bytes);
  api::StatsResponse listed;
  gatherStats(node, statsRequest(true, true), listed, resource);
  EXPECT_GT(index(listed).totals.bytes, 0u);
  EXPECT_NE(std::string::npos, renderPrometheus(listed, true).find("luxir_index_directory_bytes{"));

  // A collection scope reports only that collection, in either format.
  api::StatsResponse scoped;
  gatherStats(node, statsRequest(false, false, "metrics"), scoped, resource);
  EXPECT_FALSE(scoped.service || scoped.memory);
  ASSERT_EQ(1u, scoped.collections.size());
  text = renderPrometheus(scoped, false);
  EXPECT_NE(std::string::npos, text.find("luxir_index_segments{tenant=\"t0\",collection=\"metrics\"}"));
  EXPECT_EQ(std::string::npos, text.find("luxir_requests_total"));
  EXPECT_THROW(gatherStats(node, statsRequest(false, false, "metrics", true), scoped, resource), RequestError);
}

#ifdef __linux__
TEST_F(StatsMetricsTest, RealProcessAndSystemCounters) {
  std::pmr::monotonic_buffer_resource resource;
  api::SystemStats before;
  gatherSystemStats(before, "/tmp", resource);
  std::ifstream extra("/proc/self/status");
  ASSERT_TRUE(extra.good());
  std::promise<void> stop;
  auto future = stop.get_future();
  std::thread thread([&] { future.wait(); });
  api::SystemStats after;
  gatherSystemStats(after, "/tmp", resource);
  stop.set_value();
  thread.join();
  ASSERT_TRUE(before.process && after.process);
  EXPECT_GE(after.process->threads, before.process->threads + 1);
  EXPECT_GE(after.process->open_fds, before.process->open_fds + 1);
  EXPECT_GT(after.process->resident_bytes, 0u);
  EXPECT_GT(after.process->start_time_seconds, 0);
  ASSERT_TRUE(after.memory && after.cpu && after.filesystem);
  EXPECT_GT(after.memory->total_bytes, 0u);
  EXPECT_FALSE(after.cpu->cpus.empty());
  EXPECT_GT(after.filesystem->size_bytes, 0u);
  EXPECT_FALSE(after.network.empty());
  api::StatsResponse response;
  response.system = after;
  auto text = renderPrometheus(response, false);
  EXPECT_EQ(after.process->threads, sample(text, "process_threads"));
  EXPECT_EQ(1, sample(text, "luxir_stats_collector_success{collector=\"filesystem\"}"));
  EXPECT_NE(std::string::npos, text.find("node_network_receive_bytes_total{device="));
  EXPECT_NE(std::string::npos, text.find("node_cpu_seconds_total{cpu="));

  api::SystemStats missing;
  gatherSystemStats(missing, "/nonexistent-luxir-stats-directory", resource);
  EXPECT_FALSE(missing.filesystem);
  response.system = missing;
  text = renderPrometheus(response, false);
  EXPECT_EQ(0, sample(text, "luxir_stats_collector_success{collector=\"filesystem\"}"));
  EXPECT_EQ(std::string::npos, text.find("luxir_data_filesystem_size_bytes"));
}
#endif

class StatsMetricsGrpcTest : public GrpcLuxirTest {};
TEST_F(StatsMetricsGrpcTest, RpcCountersAndErrors) {
  auto channel = getChannel();
  auto& metric = luxirNode->metrics->request(MetricTransport::GRPC, MetricOperation::SCHEMA);
  auto errors = metric.completed[(size_t)MetricOutcome::ERROR].load();
  api::SchemaRequest request;
  request.collection = "missing-metrics-collection";
  Reply<api::SchemaResponse> reply;
  grpc::ClientContext context;
  auto status = hppUnaryCall(channel.get(), rpc::GetSchema, &context, request, &reply);
  EXPECT_FALSE(status.ok());
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (metric.completed[(size_t)MetricOutcome::ERROR].load() == errors && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  EXPECT_EQ(errors + 1, metric.completed[(size_t)MetricOutcome::ERROR].load());
  EXPECT_EQ(0, metric.active.load());

  // The structured stats RPC carries the same service counters.
  api::StatsRequest statsRequest;
  Reply<api::StatsResponse> stats;
  grpc::ClientContext statsContext;
  ASSERT_TRUE(hppUnaryCall(channel.get(), rpc::Stats, &statsContext, statsRequest, &stats).ok());
  ASSERT_TRUE(stats.msg.service.has_value());
  auto* schema = stats.msg.service->grpc.operations.find("schema");
  ASSERT_NE(nullptr, schema);
  EXPECT_GE(schema->error, errors + 1);
}
} // namespace luxir::test
