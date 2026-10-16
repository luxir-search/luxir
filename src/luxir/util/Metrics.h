// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <string_view>

namespace luxir {

// Process-wide service counters, reported by _stats. Fixed dimensions:
// requests cannot manufacture series. No allocations, label lookups or locks
// on the request path.
enum class MetricOperation { SEARCH, UPDATE, SCHEMA, COLLECTIONS, STATS, HEALTH, OTHER, COUNT };
enum class MetricTransport { HTTP, GRPC, COUNT };
enum class MetricOutcome { OK, ERROR, CANCELLED, COUNT };
inline constexpr std::array<std::string_view, 7> metricOperations = {
    "search", "update", "schema", "collections", "stats", "health", "other"};
inline constexpr std::array<std::string_view, 3> metricOutcomes = {"ok", "error", "cancelled"};

class MetricHistogram {
public:
  static constexpr std::array<double, 14> bounds = {
      .0001, .0005, .001, .005, .01, .025, .05, .1, .25, .5, 1, 5, 30, 120};
  struct Snapshot {
    std::array<uint64_t, bounds.size() + 1> buckets{};
    double sum = 0;
  };
private:
  std::array<std::atomic<uint64_t>, bounds.size() + 1> buckets{};
  std::atomic<double> sum{0};
public:
  void observe(double seconds) noexcept {
    size_t bucket = 0;
    while (bucket < bounds.size() && seconds > bounds[bucket]) ++bucket;
    buckets[bucket].fetch_add(1, std::memory_order_relaxed);
    sum.fetch_add(seconds, std::memory_order_relaxed);
  }
  // Buckets are read individually, so a snapshot taken during observe() may
  // be off by one observation between the count and the sum.
  Snapshot snapshot() const {
    Snapshot out;
    for (size_t i = 0; i < buckets.size(); ++i) out.buckets[i] = buckets[i].load(std::memory_order_relaxed);
    out.sum = sum.load(std::memory_order_relaxed);
    return out;
  }
};

struct MetricActivity {
  std::atomic<int64_t> active{0};
  std::array<std::atomic<uint64_t>, 3> completed{};
  MetricHistogram duration;
};

// Explicit finish supports caught failures; unwinding or abandoned asynchronous
// work is never counted as a successful operation. Owners must serialize access.
class MetricScope {
  MetricActivity* activity = nullptr;
  std::chrono::steady_clock::time_point start;
public:
  MetricScope() = default;
  explicit MetricScope(MetricActivity* activity) { begin(activity); }
  MetricScope(const MetricScope&) = delete;
  MetricScope& operator=(const MetricScope&) = delete;
  ~MetricScope() { finish(MetricOutcome::CANCELLED); }
  void begin(MetricActivity* next) noexcept {
    finish(MetricOutcome::CANCELLED);
    activity = next;
    if (activity) {
      start = std::chrono::steady_clock::now();
      activity->active.fetch_add(1, std::memory_order_relaxed);
    }
  }
  void finish(MetricOutcome outcome = MetricOutcome::OK) noexcept {
    if (!activity) return;
    auto* current = activity;
    activity = nullptr;
    current->duration.observe(std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count());
    current->completed[(size_t)outcome].fetch_add(1, std::memory_order_relaxed);
    current->active.fetch_sub(1, std::memory_order_relaxed);
  }
};

// Scope for synchronous engine work; mark caught failures explicitly.
class MetricWork {
  MetricScope scope;
  int exceptions = std::uncaught_exceptions();
public:
  bool failed = false;
  explicit MetricWork(MetricActivity* activity) : scope(activity) {}
  ~MetricWork() { scope.finish(failed || std::uncaught_exceptions() > exceptions
      ? MetricOutcome::ERROR : MetricOutcome::OK); }
};

struct Metrics {
  std::array<std::array<MetricActivity, (size_t)MetricOperation::COUNT>, 2> requests;
  std::array<std::array<std::atomic<uint64_t>, 8>, 2> errors{};
  MetricActivity searches, updates, flushes, commits, merges, vectorBuilds, readerRefreshes;
  MetricHistogram searchExecution;
  std::atomic<uint64_t> searchResourceExhausted{0}, searchWarnings{0};
  std::atomic<uint64_t> documentsAccepted{0}, documentsRejected{0}, deletesAccepted{0};
  std::atomic<uint64_t> httpConnections{0};
  std::array<std::atomic<uint64_t>, 2> receivedBytes{}, sentBytes{}, streamPauses{};
  std::atomic<int64_t> httpThreads{0};
  MetricActivity& request(MetricTransport transport, MetricOperation operation) {
    return requests[(size_t)transport][(size_t)operation];
  }
};

} // namespace luxir
