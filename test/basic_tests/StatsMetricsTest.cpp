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
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <span>
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
  return renderPrometheus(response);
}
}

class StatsMetricsTest : public LuxirTest {};

namespace {
template<class> struct IsOptional : std::false_type {};
template<class U> struct IsOptional<std::optional<U>> : std::true_type {};
template<class> struct IsSpan : std::false_type {};
template<class U> struct IsSpan<std::span<U>> : std::true_type {};
template<class> struct IsMap : std::false_type {};
template<class K, class V> struct IsMap<api::map_view<K, V>> : std::true_type { using mapped = V; };

// Every number gets its own value, so its rendering can be found again.
// Repeated fields and maps get one entry.
struct DistinctFill {
  std::pmr::memory_resource& resource;
  int64_t next = 1001;

  template<class T>
  void operator()(T& x) {
    if constexpr (std::is_same_v<T, bool>) x = true;
    else if constexpr (std::is_enum_v<T>) x = (T)1;
    else if constexpr (std::is_arithmetic_v<T>) x = (T)next++;
    else if constexpr (std::is_same_v<T, std::string_view>) x = "s";
    else if constexpr (IsOptional<T>::value) (*this)(x.emplace());
    else if constexpr (IsSpan<T>::value) (*this)(*api::build::allocArray(x, 1, resource));
    else if constexpr (IsMap<T>::value) {
      std::span<const std::pair<std::string_view, typename IsMap<T>::mapped>> entries;
      auto* entry = api::build::allocArray(entries, 1, resource);
      entry->first = "k";
      (*this)(entry->second);
      x = {entries};
    } else {
      glz::for_each_field(x, [&](auto& member) { (*this)(member); });
    }
  }
};

// Each numeric JSON leaf, by its path without array positions.
void numericLeaves(const glz::generic_i64& json, const std::string& path,
                   const std::function<void(const std::string&, double)>& visit) {
  std::visit([&](const auto& value) {
    using V = std::decay_t<decltype(value)>;
    if constexpr (std::is_same_v<V, glz::generic_i64::object_t>) {
      for (const auto& [key, member] : value) numericLeaves(member, path.empty() ? key : path + "." + key, visit);
    } else if constexpr (std::is_same_v<V, glz::generic_i64::array_t>) {
      for (const auto& element : value) numericLeaves(element, path, visit);
    } else if constexpr (std::is_arithmetic_v<V> && !std::is_same_v<V, bool>) {
      visit(path, (double)value);
    }
  }, json.data);
}

// Every sample value and numeric label value in a Prometheus rendering.
std::set<double> renderedNumbers(const std::string& text) {
  std::set<double> numbers;
  auto number = [&](const std::string& s) {
    try { size_t used; double v = std::stod(s, &used); if (used == s.size()) numbers.insert(v); } catch (...) {}
  };
  std::istringstream lines(text);
  static const std::regex labelValue("=\"([^\"]*)\"");
  for (std::string line; std::getline(lines, line);) {
    if (line.empty() || line[0] == '#') continue;
    number(line.substr(line.rfind(' ') + 1));
    for (std::sregex_iterator it(line.begin(), line.end(), labelValue), end; it != end; ++it) number((*it)[1]);
  }
  return numbers;
}
} // namespace

// The renderer is a hand mapping of the response. A numeric field that is in
// neither the rendering nor this JSON-only list fails here, so a new field
// cannot silently miss the Prometheus format.
TEST_F(StatsMetricsTest, EveryNumericFieldIsRenderedOrJsonOnly) {
  static const std::set<std::string> jsonOnly = {
      // Generations, shard ids and segment layout: introspection, not monitoring.
      "collections.schema_gen", "collections.shards.shard_id", "collections.shards.index.index_gen",
      "collections.shards.index.core_gen", "collections.shards.index.schema_gen",
      "collections.shards.index.update_version", "collections.shards.index.aux_indexes.built_core_gen",
      "collections.shards.index.aux_indexes.bytes", "collections.shards.index.aux_indexes.commit_time",
      "collections.shards.index.segments.bytes", "collections.shards.index.segments.deleted_docs",
      "collections.shards.index.segments.first_commit_time", "collections.shards.index.segments.live_docs",
      "collections.shards.index.segments.max_doc", "collections.shards.index.segments.max_update_version",
      "collections.shards.index.segments.merge_level", "collections.shards.index.segments.min_update_version",
      "collections.shards.index.segments.overlays.built_core_gen", "collections.shards.index.segments.overlays.bytes",
      "collections.shards.index.segments.overlays.commit_time", "collections.shards.index.segments.schema_gen",
      // Sums of the per-index series, which Prometheus aggregates itself.
      "totals.bytes", "totals.collections", "totals.committed_bytes", "totals.committed_segments",
      "totals.deleted_docs", "totals.live_docs", "totals.max_docs", "totals.segments", "totals.shards",
      "collections.totals.bytes", "collections.totals.collections", "collections.totals.committed_bytes",
      "collections.totals.committed_segments", "collections.totals.deleted_docs", "collections.totals.live_docs",
      "collections.totals.max_docs", "collections.totals.segments", "collections.totals.shards",
      // Derivable from rendered series, or never set at this level.
      "collections.shards.index.totals.committed_segments", "collections.shards.index.totals.max_docs",
      "collections.shards.index.totals.collections", "collections.shards.index.totals.shards",
      "memory.field_metadata.bytes",
      // HTTP only.
      "service.grpc.connections", "service.grpc.io_threads",
  };
  std::pmr::monotonic_buffer_resource resource;
  api::StatsResponse response;
  DistinctFill{resource}(response);
  // An errored collection renders only its availability.
  const_cast<api::CollectionStats&>(response.collections[0]).error.reset();
  // Rendered in seconds, from microseconds and milliseconds.
  static const std::map<std::string, double> perSecond = {{"collections.shards.index.commit_time", 1e6},
                                                          {"scrub.last_pass_end", 1e3}};
  auto rendered = renderedNumbers(renderPrometheus(response));
  std::string json;
  ASSERT_TRUE(api::write_json(response, json));
  glz::generic_i64 parsed;
  ASSERT_FALSE(glz::read_json(parsed, json));
  std::set<std::string> missing, seen;
  numericLeaves(parsed, "", [&](const std::string& path, double value) {
    seen.insert(path);
    auto scale = perSecond.find(path);
    bool found = rendered.contains(scale == perSecond.end() ? value : value / scale->second);
    if (!jsonOnly.contains(path) && !found) missing.insert(path);
  });
  for (const auto& path : missing) ADD_FAILURE() << "neither rendered nor JSON-only: " << path;
  for (const auto& path : jsonOnly) EXPECT_TRUE(seen.contains(path)) << "stale JSON-only entry: " << path;
  for (const auto& [path, scale] : perSecond) EXPECT_TRUE(seen.contains(path)) << "stale per-second entry: " << path;
  // The fill reached the whole response, not just its top level.
  EXPECT_GT(seen.size(), 250u);
}

TEST_F(StatsMetricsTest, HttpTrafficAndIndexingOutcomes) {
  LuxirNode node;
  HttpServer server(node, 2, 0);
  server.start();
  auto port = server.getPort();
  auto update = httpRequest(port, http::verb::post, "/collections/metrics/_update",
      R"({"docs":[{"id":"one"},{"id":"two"}],"commit":{}})");
  ASSERT_EQ(200, update.result_int()) << update.body();
  // A commit-only update is not an index batch.
  auto commit = httpRequest(port, http::verb::post, "/collections/metrics/_update", R"({"commit":{}})");
  ASSERT_EQ(200, commit.result_int()) << commit.body();
  auto search = httpRequest(port, http::verb::post, "/collections/metrics/_search",
      R"({"ops":{"q":{"top_docs":{"limit":1,"get_number":true}}}})");
  ASSERT_EQ(200, search.result_int()) << search.body();
  ASSERT_EQ(400, httpRequest(port, http::verb::post, "/collections/metrics/_search", "{").result_int());
  ASSERT_EQ(404, httpRequest(port, http::verb::get, "/does-not-exist").result_int());
  ASSERT_EQ(405, httpRequest(port, http::verb::post, "/health").result_int());
  auto response = httpRequest(port, http::verb::get, scrape);
  ASSERT_EQ(200, response.result_int());
  EXPECT_EQ(prometheusContentType, response[http::field::content_type]);
  auto& text = response.body();
  EXPECT_EQ(2, sample(text, "luxir_requests_total" + httpLabels("update", "ok")));
  EXPECT_EQ(1, sample(text, "luxir_requests_total" + httpLabels("search", "ok")));
  EXPECT_EQ(1, sample(text, "luxir_requests_total" + httpLabels("search", "error")));
  EXPECT_EQ(1, sample(text, "luxir_requests_total" + httpLabels("health", "error")));
  EXPECT_EQ(2, sample(text, "luxir_ingest_documents_total{outcome=\"accepted\"}"));
  EXPECT_EQ(0, sample(text, "luxir_requests_in_flight{transport=\"http\",operation=\"search\"}"));
  EXPECT_GT(sample(text, "luxir_background_total{kind=\"commit\",outcome=\"ok\"}"), 0);
  EXPECT_GT(sample(text, "luxir_index_committed_bytes{tenant=\"t0\",collection=\"metrics\"}"), 0);
  EXPECT_EQ(2, sample(text, "luxir_index_documents{tenant=\"t0\",collection=\"metrics\",state=\"live\"}"));
  EXPECT_EQ(1, sample(text, "luxir_searches_total{outcome=\"ok\"}"));
  // The malformed search never reached the collection.
  const std::string collection = "tenant=\"t0\",collection=\"metrics\"";
  EXPECT_EQ(1, sample(text, "luxir_collection_searches_total{" + collection + ",outcome=\"ok\"}"));
  EXPECT_EQ(0, sample(text, "luxir_collection_searches_total{" + collection + ",outcome=\"error\"}"));
  EXPECT_EQ(2, sample(text, "luxir_collection_documents_total{" + collection + ",outcome=\"accepted\"}"));
  EXPECT_EQ(1, sample(text, "luxir_collection_index_batches_total{" + collection + ",outcome=\"ok\"}"));
  EXPECT_EQ(1, sample(text, "luxir_index_batches_total{outcome=\"ok\"}"));
  EXPECT_EQ(2, sample(text, "luxir_request_duration_seconds_count{transport=\"http\",operation=\"update\"}"));
  EXPECT_EQ(2, sample(text, "luxir_request_duration_seconds_bucket{transport=\"http\",operation=\"update\",le=\"+Inf\"}"));
  // Directory bytes need files=true; the cheap walk lists no directories.
  EXPECT_EQ(std::string::npos, text.find("luxir_index_directory_bytes"));
  // Stats requests are their own operation, so they never blur service traffic.
  auto again = httpRequest(port, http::verb::get, scrape).body();
  EXPECT_EQ(sample(text, "luxir_requests_total" + httpLabels("stats", "ok")) + 1,
            sample(again, "luxir_requests_total" + httpLabels("stats", "ok")));
  EXPECT_EQ(sample(text, "luxir_requests_total" + httpLabels("search", "ok")),
            sample(again, "luxir_requests_total" + httpLabels("search", "ok")));
  // /metrics is the same rendering with collections and system on by default.
  auto metrics = httpRequest(port, http::verb::get, "/metrics");
  ASSERT_EQ(200, metrics.result_int());
  EXPECT_EQ(prometheusContentType, metrics[http::field::content_type]);
  EXPECT_NE(std::string::npos, metrics.body().find("luxir_index_documents{"));
  EXPECT_NE(std::string::npos, metrics.body().find("\nprocess_threads "));
  auto bare = httpRequest(port, http::verb::get, "/metrics?collections=false&system=false").body();
  EXPECT_EQ(std::string::npos, bare.find("luxir_index_documents{"));
  EXPECT_EQ(std::string::npos, bare.find("\nprocess_threads "));
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
  auto partial = httpRequest(port, http::verb::post, "/collections/metrics/_update",
      R"({"docs":[{"id":"three","n":3},{"id":"bad","n":"not-a-number"}]})");
  ASSERT_EQ(200, partial.result_int()) << partial.body();
  auto rejected = httpRequest(port, http::verb::post, "/collections/metrics/_update",
      "{\"id\":\"bad\",\"n\":\"not-a-number\"}\n", "application/x-ndjson");
  ASSERT_EQ(200, rejected.result_int()) << rejected.body();
  auto text = httpRequest(port, http::verb::get, scrape).body();
  EXPECT_EQ(2, sample(text, "luxir_ingest_documents_total{outcome=\"accepted\"}"));
  EXPECT_EQ(5, sample(text, "luxir_ingest_documents_total{outcome=\"rejected\"}"));
  // The rolled-back document has no error of its own.
  EXPECT_EQ(4, sample(text, "luxir_ingest_document_errors_total{kind=\"invalid_request\"}"));
  // The atomic batch and the last stream applied nothing; the others applied
  // one of two documents.
  EXPECT_EQ(2, sample(text, "luxir_requests_total" + httpLabels("update", "error")));
  EXPECT_EQ(2, sample(text, "luxir_requests_total" + httpLabels("update", "ok")));
  EXPECT_EQ(2, sample(text, "luxir_request_errors_total{transport=\"http\",kind=\"invalid_request\"}"));
  EXPECT_EQ(2, sample(text, "luxir_index_batches_total{outcome=\"error\"}"));
}

TEST_F(StatsMetricsTest, ConcurrentScrapesAndCollectionRemoval) {
  LuxirNode node;
  HttpServer server(node, 2, 0);
  server.start();
  // Scrape continuously while the collection comes and goes.
  std::atomic<bool> done{false};
  std::atomic<int> scrapes{0};
  std::vector<std::future<int>> scrapers;
  for (int i = 0; i < 4; ++i) scrapers.push_back(std::async(std::launch::async, [&] {
    int failures = 0;
    do {
      auto response = httpRequest(server.getPort(), http::verb::get, scrape);
      if (response.result_int() != 200 ||
          response.body().find("# TYPE luxir_requests_total counter\n") == std::string::npos) failures++;
      scrapes++;
    } while (!done.load());
    return failures;
  }));
  for (int i = 0; i < 20 || scrapes.load() < 20; ++i) {
    node.createCollection("metrics");
    node.deleteCollection("metrics");
  }
  done = true;
  for (auto& scraper : scrapers) EXPECT_EQ(0, scraper.get());
  EXPECT_EQ(std::string::npos, render(node).find("collection=\"metrics\""));
}

TEST_F(StatsMetricsTest, EscapingAndHistogramBuckets) {
  Metrics metrics;
  auto activity = metricActivity(MetricStage::SEARCH_EXECUTION);
  metrics.complete(activity, MetricOutcome::OK, 1'000'000);        // 1 ms, on a bound
  metrics.complete(activity, MetricOutcome::OK, 150'000'000'000);  // past the last bound
  auto observed = metrics.counts().activities[activity];
  EXPECT_DOUBLE_EQ(150.001, (double)observed.nanos / 1e9);
  std::array<uint64_t, requestBounds.size() + 1> counts{};
  uint64_t cumulative = 0;
  for (size_t i = 0; i < counts.size(); ++i) counts[i] = cumulative += observed.buckets[i];
  PrometheusText text;
  text.histogram("test_seconds", "Latency.", requestBounds, {counts, (double)observed.nanos / 1e9, cumulative},
      PrometheusText::label("label", "quote\" slash\\ newline\n"));
  // Nothing observed: every bucket renders as zero.
  text.histogram("empty_seconds", "Latency.", requestBounds, {});
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
  auto text = renderPrometheus(cheap);
  EXPECT_EQ(std::string::npos, text.find("collection=\"metrics\""));
  EXPECT_EQ(std::string::npos, text.find("process_cpu_seconds_total"));
  EXPECT_NE(std::string::npos, text.find("luxir_index_ram_draining_bytes "));

  // One response, two renderings: the formats agree.
  api::StatsResponse walked;
  gatherStats(node, statsRequest(true), walked, resource);
  ASSERT_TRUE(walked.totals);
  ASSERT_EQ(node.collectionEntries().size(), walked.collections.size());
  text = renderPrometheus(walked);
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
  EXPECT_FALSE(index(walked).totals.bytes);
  api::StatsResponse listed;
  gatherStats(node, statsRequest(true, true), listed, resource);
  EXPECT_GT(index(listed).totals.bytes.value_or(0), 0u);
  EXPECT_NE(std::string::npos, renderPrometheus(listed).find("luxir_index_directory_bytes{"));

  // A collection scope reports only that collection, in either format.
  api::StatsResponse scoped;
  gatherStats(node, statsRequest(false, false, "metrics"), scoped, resource);
  EXPECT_FALSE(scoped.service || scoped.memory);
  ASSERT_EQ(1u, scoped.collections.size());
  text = renderPrometheus(scoped);
  EXPECT_NE(std::string::npos, text.find("luxir_index_segments{tenant=\"t0\",collection=\"metrics\"}"));
  EXPECT_EQ(std::string::npos, text.find("luxir_requests_total"));
  EXPECT_THROW(gatherStats(node, statsRequest(false, false, "metrics", true), scoped, resource), RequestError);
}

#ifdef __linux__
TEST_F(StatsMetricsTest, RealProcessAndSystemCounters) {
  std::pmr::monotonic_buffer_resource resource;
  api::SystemStats before;
  gatherSystemStats(&before, nullptr, "/tmp", resource);
  std::ifstream extra("/proc/self/status");
  ASSERT_TRUE(extra.good());
  std::promise<void> stop;
  auto future = stop.get_future();
  std::thread thread([&] { future.wait(); });
  api::SystemStats after;
  api::HostStats host;
  gatherSystemStats(&after, &host, "/tmp", resource);
  stop.set_value();
  thread.join();
  ASSERT_TRUE(before.process && after.process);
  EXPECT_GE(after.process->threads, before.process->threads + 1);
  EXPECT_GE(after.process->open_fds, before.process->open_fds + 1);
  EXPECT_GT(after.process->resident_bytes, 0u);
  EXPECT_GT(after.process->start_time_seconds, 0);
  ASSERT_TRUE(after.filesystem && host.memory && host.cpu);
  EXPECT_GT(after.filesystem->size_bytes, 0u);
  EXPECT_GT(host.memory->total_bytes, 0u);
  EXPECT_FALSE(host.cpu->cpus.empty());
  EXPECT_FALSE(host.network.empty());
  api::StatsResponse response;
  response.system = after;
  auto text = renderPrometheus(response);
  EXPECT_EQ(after.process->threads, sample(text, "process_threads"));
  EXPECT_EQ(1, sample(text, "luxir_stats_collector_success{collector=\"filesystem\"}"));
  EXPECT_EQ(std::string::npos, text.find("luxir_host_"));
  response.host = host;
  text = renderPrometheus(response);
  EXPECT_NE(std::string::npos, text.find("luxir_host_network_receive_bytes_total{device="));
  EXPECT_NE(std::string::npos, text.find("luxir_host_cpu_seconds_total{cpu="));
  EXPECT_EQ(std::string::npos, text.find("\nnode_"));

  api::SystemStats missing;
  gatherSystemStats(&missing, nullptr, "/nonexistent-luxir-stats-directory", resource);
  EXPECT_FALSE(missing.filesystem);
  response.system = missing;
  response.host.reset();
  text = renderPrometheus(response);
  EXPECT_EQ(0, sample(text, "luxir_stats_collector_success{collector=\"filesystem\"}"));
  EXPECT_EQ(std::string::npos, text.find("luxir_data_filesystem_size_bytes"));
}
#endif

class StatsMetricsGrpcTest : public GrpcLuxirTest {};
TEST_F(StatsMetricsGrpcTest, RpcCountersAndErrors) {
  auto channel = getChannel();
  auto activity = metricActivity(MetricTransport::GRPC, MetricOperation::SCHEMA);
  auto schemaCalls = [&] { return luxirNode->metrics->counts().activities[activity]; };
  auto failed = [&] { return schemaCalls().completed[(size_t)MetricOutcome::ERROR]; };
  auto errors = failed();
  api::SchemaRequest request;
  request.collection = "missing-metrics-collection";
  Reply<api::SchemaResponse> reply;
  grpc::ClientContext context;
  auto status = hppUnaryCall(channel.get(), rpc::GetSchema, &context, request, &reply);
  EXPECT_FALSE(status.ok());
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (failed() == errors && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  EXPECT_EQ(errors + 1, failed());
  auto calls = schemaCalls();
  EXPECT_EQ(calls.started, calls.completed[0] + calls.completed[1] + calls.completed[2]);

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
