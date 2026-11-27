// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "NodeSummary.h"

#include <algorithm>
#include <fstream>
#include <limits>
#include <sstream>
#include <unistd.h>

#include "LuxirNode.h"
#include "luxir/util/log.h"
#include "luxir/util/thread.h"

namespace luxir {

namespace {

struct ActivityDelta {
  uint64_t completed = 0;
  uint64_t errors = 0;
  uint64_t nanos = 0;
  std::array<uint64_t, METRIC_BUCKETS> buckets{};
};

ActivityDelta delta(const MetricCounts& now, const MetricCounts& before, MetricStage stage) {
  size_t activity = metricActivity(stage);
  const auto& a = now.activities[activity];
  const auto& b = before.activities[activity];
  ActivityDelta d;
  for (size_t i = 0; i < a.completed.size(); i++) d.completed += a.completed[i] - b.completed[i];
  d.errors = a.completed[(size_t)MetricOutcome::ERROR] - b.completed[(size_t)MetricOutcome::ERROR];
  d.nanos = a.nanos - b.nanos;
  for (size_t i = 0; i < d.buckets.size(); i++) d.buckets[i] = a.buckets[i] - b.buckets[i];
  return d;
}

double meanMs(const ActivityDelta& d) { return d.completed ? logs::millis(d.nanos / d.completed) : 0.0; }

// Upper bound of the histogram bucket holding quantile q, as text ("<=250ms"),
// or ">60000ms" past the last bound.
std::string quantile(const ActivityDelta& d, MetricStage stage, double q) {
  if (!d.completed) return "-";
  auto bounds = metricBounds(metricActivity(stage));
  uint64_t rank = (uint64_t)((double)d.completed * q);
  if (rank >= d.completed) rank = d.completed - 1;
  uint64_t seen = 0;
  for (size_t i = 0; i < bounds.size(); i++) {
    seen += d.buckets[i];
    if (seen > rank) return fmt::format("<={:g}ms", bounds[i] * 1000);
  }
  return fmt::format(">{:g}ms", bounds.back() * 1000);
}

uint64_t counter(const MetricCounts& now, const MetricCounts& before, MetricCounter c) {
  return now.counters[(size_t)c] - before.counters[(size_t)c];
}

uint64_t transportCounter(const MetricCounts& now, const MetricCounts& before, MetricTransportCounter c) {
  uint64_t sum = 0;
  for (size_t t = 0; t < (size_t)MetricTransport::COUNT; t++) sum += now.transport[(size_t)c][t] - before.transport[(size_t)c][t];
  return sum;
}

uint64_t requestErrors(const MetricCounts& now, const MetricCounts& before) {
  uint64_t sum = 0;
  for (size_t t = 0; t < now.requestErrors.size(); t++) {
    for (size_t k = 0; k < now.requestErrors[t].size(); k++) sum += now.requestErrors[t][k] - before.requestErrors[t][k];
  }
  return sum;
}

std::optional<uint64_t> rssBytes() {
  std::ifstream in("/proc/self/statm");
  uint64_t size = 0, resident = 0;
  if (!(in >> size >> resident)) return std::nullopt;
  return resident * (uint64_t)sysconf(_SC_PAGESIZE);
}

// "some avg10" of a /proc/pressure file: the share of the last 10 s in which
// at least one task stalled on that resource.
std::optional<double> pressureAvg10(const char* resource) {
  std::ifstream in(std::string("/proc/pressure/") + resource);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.starts_with("some ")) continue;
    auto at = line.find("avg10=");
    if (at == std::string::npos) return std::nullopt;
    try { return std::stod(line.substr(at + 6)); } catch (...) { return std::nullopt; }
  }
  return std::nullopt;
}

// Cumulative CPU throttling of this process's cgroup (v2), in microseconds.
std::optional<uint64_t> throttledMicros() {
  std::ifstream in("/sys/fs/cgroup/cpu.stat");
  std::string key;
  uint64_t value = 0;
  while (in >> key >> value) {
    if (key == "throttled_usec") return value;
  }
  return std::nullopt;
}

std::string optionalPercent(std::optional<double> value) {
  return value ? fmt::format("{:.1f}%", *value) : std::string("-");
}

} // namespace

NodeSummary::NodeSummary(LuxirNode& node, std::chrono::seconds interval)
    : node(node), interval(interval), previous(node.metrics->counts()),
      previousAt(std::chrono::steady_clock::now()), previousThrottledMicros(throttledMicros()) {
  thread = std::thread([this] { run(); });
}

NodeSummary::~NodeSummary() {
  {
    std::lock_guard lock(mutex);
    stopping = true;
  }
  wake.notify_all();
  thread.join();
}

void NodeSummary::run() {
  nameThisThread("luxir_summary");
  std::unique_lock lock(mutex);
  while (!wake.wait_for(lock, interval, [this] { return stopping; })) {
    lock.unlock();
    try {
      auto line = report();
      LOG_INFO("{}", line);
    } catch (const std::exception& e) {
      LOG_WARN("node summary failed: {}", e.what());
    }
    lock.lock();
  }
}

std::string NodeSummary::report() {
  auto now = node.metrics->counts();
  auto at = std::chrono::steady_clock::now();
  double seconds = std::max(1e-3, std::chrono::duration<double>(at - previousAt).count());
  const auto& before = previous;

  auto search = delta(now, before, MetricStage::SEARCH);
  auto batches = delta(now, before, MetricStage::INDEX_BATCH);
  auto flush = delta(now, before, MetricStage::FLUSH);
  auto commit = delta(now, before, MetricStage::COMMIT);
  auto deletes = delta(now, before, MetricStage::DELETE_PASS);
  auto merge = delta(now, before, MetricStage::MERGE);
  auto vectors = delta(now, before, MetricStage::VECTOR_BUILD);
  auto refresh = delta(now, before, MetricStage::READER_REFRESH);
  uint64_t accepted = counter(now, before, MetricCounter::DOCUMENTS_ACCEPTED);
  uint64_t rejected = counter(now, before, MetricCounter::DOCUMENTS_REJECTED);

  auto& budget = node.getIndexRamBudget();
  auto throttled = throttledMicros();
  std::string throttledText = "-";
  if (throttled && previousThrottledMicros) {
    throttledText = fmt::format("{:.1f}%", (double)(*throttled - *previousThrottledMicros) / 1e4 / seconds);
  }
  auto rss = rssBytes();

  std::string line = fmt::format(
      "node summary {:.0f}s: "
      "search n={} rate={:.1f}/s mean={:.1f}ms p50{} p99{} errors={} exhausted={}; "
      "ingest batches={} docs={} rate={:.0f}/s rejected={} batch_mean={:.1f}ms batch_p99{}; "
      "flush n={} mean={:.0f}ms; commit n={} mean={:.0f}ms p99{}; delete_pass n={} mean={:.0f}ms; "
      "merge n={} mean={:.0f}ms errors={}; vector_build n={} mean={:.0f}ms; reader_refresh n={} mean={:.1f}ms; "
      "request_errors={} stream_pauses={} http_connections={}; "
      "index_ram={} rss={}; psi_some_avg10 cpu={} io={} memory={}; cpu_throttled={}",
      seconds,
      search.completed, (double)search.completed / seconds, meanMs(search),
      quantile(search, MetricStage::SEARCH, 0.5), quantile(search, MetricStage::SEARCH, 0.99), search.errors,
      counter(now, before, MetricCounter::SEARCH_RESOURCE_EXHAUSTED),
      batches.completed, accepted, (double)accepted / seconds, rejected, meanMs(batches),
      quantile(batches, MetricStage::INDEX_BATCH, 0.99),
      flush.completed, meanMs(flush), commit.completed, meanMs(commit), quantile(commit, MetricStage::COMMIT, 0.99),
      deletes.completed, meanMs(deletes),
      merge.completed, meanMs(merge), merge.errors, vectors.completed, meanMs(vectors), refresh.completed, meanMs(refresh),
      requestErrors(now, before), transportCounter(now, before, MetricTransportCounter::STREAM_PAUSES),
      node.metrics->httpConnections.load(std::memory_order_relaxed),
      budget.totalBytes() > 0
          ? fmt::format("{:.0f}/{:.0f}MiB", logs::mib((uint64_t)std::max<int64_t>(0, budget.reservedBytes())),
                        logs::mib((uint64_t)budget.totalBytes()))
          : fmt::format("{:.0f}MiB/unlimited", logs::mib((uint64_t)std::max<int64_t>(0, budget.reservedBytes()))),
      rss ? fmt::format("{:.0f}MiB", logs::mib(*rss)) : std::string("-"),
      optionalPercent(pressureAvg10("cpu")), optionalPercent(pressureAvg10("io")), optionalPercent(pressureAvg10("memory")),
      throttledText);

  previous = now;
  previousAt = at;
  previousThrottledMicros = throttled;
  return line;
}

} // namespace luxir
