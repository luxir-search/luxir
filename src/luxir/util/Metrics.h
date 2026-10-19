// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <span>
#include <string_view>
#include <utility>

#include "luxir/util/ApiError.h"
#include "luxir/util/Striped.h"

namespace luxir {

// Process-wide service counters, reported by _stats. Fixed dimensions:
// requests cannot manufacture series. No allocations, label lookups or locks
// on the request path, and each thread counts into its own stripe.
enum class MetricOperation { SEARCH, UPDATE, SCHEMA, COLLECTIONS, STATS, HEALTH, OTHER, COUNT };
enum class MetricTransport { HTTP, GRPC, COUNT };
enum class MetricOutcome { OK, ERROR, CANCELLED, COUNT };
// Timed engine work. Searches, their execution and index batches are
// request-sized; the rest runs in the background.
enum class MetricStage { SEARCH, SEARCH_EXECUTION, INDEX_BATCH, FLUSH, COMMIT, MERGE, VECTOR_BUILD, READER_REFRESH, COUNT };
enum class MetricCounter {
  SEARCH_RESOURCE_EXHAUSTED, SEARCH_WARNINGS, DOCUMENTS_ACCEPTED, DOCUMENTS_REJECTED, DELETE_IDS_ACCEPTED, COUNT
};
enum class MetricTransportCounter { RECEIVED_BYTES, SENT_BYTES, STREAM_PAUSES, COUNT };

inline constexpr std::array<std::string_view, (size_t)MetricOperation::COUNT> metricOperations = {
    "search", "update", "schema", "collections", "stats", "health", "other"};
inline constexpr std::array<std::string_view, (size_t)MetricOutcome::COUNT> metricOutcomes = {
    "ok", "error", "cancelled"};

// Histogram upper bounds in seconds; a +Inf bucket follows. Requests run from
// microsecond searches to slow administration, background stages from reader
// refreshes to long merges.
inline constexpr std::array<double, 21> requestBounds = {
    .00001, .000025, .00005, .0001, .00025, .0005, .001, .0025, .005, .01, .025, .05,
    .1, .25, .5, 1, 2.5, 5, 10, 30, 60};
inline constexpr std::array<double, 14> backgroundBounds = {
    .0001, .001, .01, .1, .5, 1, 5, 10, 30, 60, 300, 900, 1800, 3600};
inline constexpr size_t METRIC_BUCKETS = requestBounds.size() + 1;
static_assert(backgroundBounds.size() < METRIC_BUCKETS);

// Timed activities: requests by transport and operation, then engine stages.
inline constexpr size_t REQUEST_ACTIVITIES = (size_t)MetricTransport::COUNT * (size_t)MetricOperation::COUNT;
inline constexpr size_t METRIC_ACTIVITIES = REQUEST_ACTIVITIES + (size_t)MetricStage::COUNT;
constexpr size_t metricActivity(MetricTransport transport, MetricOperation operation) {
  return (size_t)transport * (size_t)MetricOperation::COUNT + (size_t)operation;
}
constexpr size_t metricActivity(MetricStage stage) { return REQUEST_ACTIVITIES + (size_t)stage; }
constexpr std::span<const double> metricBounds(size_t activity) {
  if (activity < metricActivity(MetricStage::FLUSH)) return requestBounds;
  return backgroundBounds;
}

template<class C>
struct ActivityCellsT {
  C started{};
  std::array<C, (size_t)MetricOutcome::COUNT> completed{};
  std::array<C, METRIC_BUCKETS> buckets{};  // completions per bucket, not cumulative
  C nanos{};                                 // summed duration of completions
};

template<class C>
struct MetricCellsT {
  std::array<ActivityCellsT<C>, METRIC_ACTIVITIES> activities;
  std::array<C, (size_t)MetricCounter::COUNT> counters{};
  std::array<std::array<C, (size_t)MetricTransport::COUNT>, (size_t)MetricTransportCounter::COUNT> transport{};
  // Failed requests by transport and the kind of their first error.
  std::array<std::array<C, ERROR_KINDS>, (size_t)MetricTransport::COUNT> requestErrors{};
  // Documents rejected with an error of their own, by kind.
  std::array<C, ERROR_KINDS> documentErrors{};
};

// One thread's stripe, and the sum of all stripes.
using MetricCells = MetricCellsT<std::atomic<uint64_t>>;
using MetricCounts = MetricCellsT<uint64_t>;
using ActivityCounts = ActivityCellsT<uint64_t>;

inline void addCells(uint64_t& dst, const std::atomic<uint64_t>& src) {
  dst += src.load(std::memory_order_relaxed);
}
inline void addCells(ActivityCounts& dst, const ActivityCellsT<std::atomic<uint64_t>>& src);
template<class D, class S, size_t N>
void addCells(std::array<D, N>& dst, const std::array<S, N>& src) {
  for (size_t i = 0; i < N; i++) addCells(dst[i], src[i]);
}
inline void addCells(ActivityCounts& dst, const ActivityCellsT<std::atomic<uint64_t>>& src) {
  addCells(dst.started, src.started);
  addCells(dst.completed, src.completed);
  addCells(dst.buckets, src.buckets);
  addCells(dst.nanos, src.nanos);
}
inline void addCells(MetricCounts& dst, const MetricCells& src) {
  addCells(dst.activities, src.activities);
  addCells(dst.counters, src.counters);
  addCells(dst.transport, src.transport);
  addCells(dst.requestErrors, src.requestErrors);
  addCells(dst.documentErrors, src.documentErrors);
}

inline uint64_t nanosSince(std::chrono::steady_clock::time_point start) {
  auto elapsed = std::chrono::steady_clock::now() - start;
  return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count();
}

// Error kinds index counters; an unknown wire value counts as UNKNOWN.
constexpr size_t errorKindIndex(ErrorKind kind) {
  return (size_t)kind < ERROR_KINDS ? (size_t)kind : (size_t)ErrorKind::UNKNOWN;
}

class Metrics {
  Striped<MetricCells> cells;

public:
  // Gauges that move per connection or thread, not per request.
  std::atomic<int64_t> httpConnections{0};
  std::atomic<int64_t> httpThreads{0};

  MetricCells& local() { return cells.local(); }

  void add(MetricCounter counter, uint64_t delta = 1) {
    local().counters[(size_t)counter].fetch_add(delta, std::memory_order_relaxed);
  }
  void add(MetricTransportCounter counter, MetricTransport transport, uint64_t delta = 1) {
    local().transport[(size_t)counter][(size_t)transport].fetch_add(delta, std::memory_order_relaxed);
  }
  void requestError(MetricTransport transport, ErrorKind kind) {
    local().requestErrors[(size_t)transport][errorKindIndex(kind)].fetch_add(1, std::memory_order_relaxed);
  }
  void documentError(ErrorKind kind) {
    local().documentErrors[errorKindIndex(kind)].fetch_add(1, std::memory_order_relaxed);
  }

  void start(size_t activity) {
    local().activities[activity].started.fetch_add(1, std::memory_order_relaxed);
  }
  void complete(size_t activity, MetricOutcome outcome, uint64_t nanos) {
    auto& cell = local().activities[activity];
    auto bounds = metricBounds(activity);
    double seconds = (double)nanos / 1e9;
    size_t bucket = 0;
    while (bucket < bounds.size() && seconds > bounds[bucket]) ++bucket;
    cell.completed[(size_t)outcome].fetch_add(1, std::memory_order_relaxed);
    cell.buckets[bucket].fetch_add(1, std::memory_order_relaxed);
    cell.nanos.fetch_add(nanos, std::memory_order_relaxed);
  }

  // Stripes are read one at a time, so a sum taken during updates may split an
  // observation between its count and its duration.
  MetricCounts counts() const {
    MetricCounts out{};
    cells.forEach([&](const MetricCells& stripe) { addCells(out, stripe); });
    return out;
  }

  // For engine objects created outside a node (tests, tools): counts that no
  // stats request reads.
  static const std::shared_ptr<Metrics>& detached() {
    static const auto metrics = std::make_shared<Metrics>();
    return metrics;
  }
};

// Engine work for one collection, reported with its stats. Counts only the
// work that reached the collection; the node counters keep the totals, which
// survive the collection's deletion. Durations are sums, without buckets.
template<class C>
struct CollectionCellsT {
  std::array<C, (size_t)MetricOutcome::COUNT> searches{};
  C searchNanos{};
  std::array<C, (size_t)MetricOutcome::COUNT> batches{};
  C batchNanos{};
  C documentsAccepted{};
  C documentsRejected{};
  C deleteIdsAccepted{};
};

using CollectionCells = CollectionCellsT<std::atomic<uint64_t>>;
using CollectionCounts = CollectionCellsT<uint64_t>;

inline void addCells(CollectionCounts& dst, const CollectionCells& src) {
  addCells(dst.searches, src.searches);
  addCells(dst.searchNanos, src.searchNanos);
  addCells(dst.batches, src.batches);
  addCells(dst.batchNanos, src.batchNanos);
  addCells(dst.documentsAccepted, src.documentsAccepted);
  addCells(dst.documentsRejected, src.documentsRejected);
  addCells(dst.deleteIdsAccepted, src.deleteIdsAccepted);
}

class CollectionMetrics {
  Striped<CollectionCells> cells;

public:
  void search(MetricOutcome outcome, uint64_t nanos) {
    auto& cell = cells.local();
    cell.searches[(size_t)outcome].fetch_add(1, std::memory_order_relaxed);
    cell.searchNanos.fetch_add(nanos, std::memory_order_relaxed);
  }
  void batch(MetricOutcome outcome, uint64_t nanos, uint64_t accepted, uint64_t rejected, uint64_t deleteIds) {
    auto& cell = cells.local();
    cell.batches[(size_t)outcome].fetch_add(1, std::memory_order_relaxed);
    cell.batchNanos.fetch_add(nanos, std::memory_order_relaxed);
    cell.documentsAccepted.fetch_add(accepted, std::memory_order_relaxed);
    cell.documentsRejected.fetch_add(rejected, std::memory_order_relaxed);
    cell.deleteIdsAccepted.fetch_add(deleteIds, std::memory_order_relaxed);
  }

  CollectionCounts counts() const {
    CollectionCounts out{};
    cells.forEach([&](const CollectionCells& stripe) { addCells(out, stripe); });
    return out;
  }

  // For writers created outside a collection (tests, tools).
  static const std::shared_ptr<CollectionMetrics>& detached() {
    static const auto metrics = std::make_shared<CollectionMetrics>();
    return metrics;
  }
};

// One timed activity. begin and finish may run on different threads, but the
// owner serializes them. A scope destroyed unfinished counts as cancelled, so
// unwinding or abandoned asynchronous work never counts as a success.
class MetricScope {
  Metrics* metrics = nullptr;
  size_t activity = 0;
  std::chrono::steady_clock::time_point start;

public:
  MetricScope() = default;
  MetricScope(Metrics& metrics, size_t activity) { begin(metrics, activity); }
  MetricScope(const MetricScope&) = delete;
  MetricScope& operator=(const MetricScope&) = delete;
  ~MetricScope() { finish(MetricOutcome::CANCELLED); }

  bool active() const { return metrics != nullptr; }
  void begin(Metrics& next, size_t nextActivity) noexcept {
    finish(MetricOutcome::CANCELLED);
    metrics = &next;
    activity = nextActivity;
    start = std::chrono::steady_clock::now();
    metrics->start(activity);
  }
  void finish(MetricOutcome outcome = MetricOutcome::OK) noexcept {
    if (!metrics) return;
    auto* current = std::exchange(metrics, nullptr);
    current->complete(activity, outcome, nanosSince(start));
  }
};

// Synchronous engine work: an escaping exception or `failed` counts as an error.
class MetricWork {
  MetricScope scope;
  int exceptions = std::uncaught_exceptions();

public:
  bool failed = false;
  MetricWork(Metrics& metrics, MetricStage stage) : scope(metrics, metricActivity(stage)) {}
  ~MetricWork() {
    scope.finish(failed || std::uncaught_exceptions() > exceptions ? MetricOutcome::ERROR : MetricOutcome::OK);
  }
};

} // namespace luxir
