// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

namespace luxir {

/// Storage I/O of one request, counted by every thread that works for it.
/// The request owns the counters; they outlive all of its tasks.
///
/// Core reads no storage over a network, so it counts nothing itself: storage
/// backends count their store reads (a backend's lazy files count each fetch,
/// the bytes readers touch and whole-file reads), and a layer that issues
/// requests of its own, or splits one read into several, adds them. The
/// counters are additive.
struct IoCounters {
  // Store requests issued (one ranged GET each).
  std::atomic<uint64_t> requests{0};
  // Sequential waits on the store: requests issued together and awaited
  // together count as one round. A backend that issues one request at a time
  // counts a round per request; one that fans a read out counts one per wave.
  std::atomic<uint64_t> rounds{0};
  // Bytes those requests returned.
  std::atomic<uint64_t> fetchedBytes{0};
  // Bytes of lazy files readers resolved (rangeStream, read()), whether
  // resident or fetched. A reader that keeps a range it resolved earlier
  // reads it again through a plain pointer, without counting.
  std::atomic<uint64_t> touchedBytes{0};
  // Reads that needed a whole lazy file at once (InputFile::read() and
  // getInputStream()), the reads that defeat range fetching.
  std::atomic<uint64_t> wholeFileReads{0};

  struct Snapshot {
    uint64_t requests = 0, rounds = 0, fetchedBytes = 0, touchedBytes = 0, wholeFileReads = 0;
    bool empty() const { return (requests | rounds | fetchedBytes | touchedBytes | wholeFileReads) == 0; }
  };

  Snapshot snapshot() const {
    return {requests.load(std::memory_order_relaxed), rounds.load(std::memory_order_relaxed),
            fetchedBytes.load(std::memory_order_relaxed), touchedBytes.load(std::memory_order_relaxed),
            wholeFileReads.load(std::memory_order_relaxed)};
  }
};

struct IoContext;

namespace detail {
// Constant-initialized: reading it is one thread-local load, no init guard.
inline constinit thread_local const IoContext* currentIoContext = nullptr;
}  // namespace detail

/// What the storage layer may know about the request a read is done for.
///
/// Search code installs the request's context with an IoScope before it reads
/// segments, and core carries it into every task the request fans out
/// (task_group_fanout, TaskGroupRunner) and into a resumed streaming emitter.
/// Storage code (a DelegatingDirectory layer, a backend's store reads) reads
/// it with IoContext::current(): null means the read is not done for a
/// request (merges, startup, replication, verification), and such reads must
/// behave as before.
///
///   if (const auto* io = luxir::IoContext::current()) {
///     if (!io->fillCache) ...;                // a scan: do not admit into a cache
///     if (io->remaining() < hedgeAfter) ...;  // little time left: hedge now
///     io->countFetch(bytes);                  // a request the layer issued itself
///   }
///
/// A context is a small value: tasks copy it, so a nested IoScope over a
/// modified copy (a scan inside a request, say) is cheap and never outlived.
/// Only `counters` points at request-owned state.
struct IoContext {
  using Clock = std::chrono::steady_clock;

  // Unique per request in this process, for correlating a layer's own logs
  // and metrics with a request.
  uint64_t queryId = 0;
  // The client's SearchRequest.request_id; may be empty. Valid while the
  // request lives, which covers every read done for it.
  std::string_view requestId;
  // When the client stops waiting; Clock::time_point::max() when it set no
  // deadline. A hint for hedging and for giving up on a slow request early:
  // core never fails a read on it.
  Clock::time_point deadline = Clock::time_point::max();
  // False when the reads should not fill a cache: they are expected not to
  // repeat (a scan or a bulk export), so admitting them would evict data that
  // will be read again. A hint; a read must return the same bytes either way.
  bool fillCache = true;
  // The request's counters; null counts nothing.
  IoCounters* counters = nullptr;

  /// The context of the request this thread is working for, or null.
  static const IoContext* current() { return detail::currentIoContext; }

  /// A process-unique query id.
  static uint64_t newQueryId() {
    static std::atomic<uint64_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
  }

  bool hasDeadline() const { return deadline != Clock::time_point::max(); }
  /// Time left before the deadline (negative once passed); max() without one.
  Clock::duration remaining(Clock::time_point now = Clock::now()) const {
    return hasDeadline() ? deadline - now : Clock::duration::max();
  }

  /// One wave of `requests` concurrent requests that returned `bytes` in all.
  void countFetch(uint64_t bytes, uint64_t requests = 1, uint64_t rounds = 1) const {
    if (!counters) return;
    counters->requests.fetch_add(requests, std::memory_order_relaxed);
    counters->rounds.fetch_add(rounds, std::memory_order_relaxed);
    counters->fetchedBytes.fetch_add(bytes, std::memory_order_relaxed);
  }
  void countTouch(uint64_t bytes) const {
    if (counters) counters->touchedBytes.fetch_add(bytes, std::memory_order_relaxed);
  }
  void countWholeFileRead() const {
    if (counters) counters->wholeFileReads.fetch_add(1, std::memory_order_relaxed);
  }

  /// The context for work shared with other requests (a SharedLazyBuild):
  /// its reads still count for the request that does them, but no one
  /// request's deadline or fill hint applies to a result everyone takes.
  IoContext shared() const {
    IoContext io = *this;
    io.deadline = Clock::time_point::max();
    io.fillCache = true;
    return io;
  }
};

/// Installs a context on this thread for the scope and restores the previous
/// one (usually null) when it ends, also on unwind. Null clears the context:
/// use it for work that must not be attributed to the enclosing request.
class IoScope {
  const IoContext* previous;

public:
  explicit IoScope(const IoContext* io) : previous(detail::currentIoContext) { detail::currentIoContext = io; }
  ~IoScope() { detail::currentIoContext = previous; }
  IoScope(const IoScope&) = delete;
  IoScope& operator=(const IoScope&) = delete;
};

/// A copy of this thread's context to carry into work run elsewhere.
inline std::optional<IoContext> captureIoContext() {
  const IoContext* io = IoContext::current();
  return io ? std::optional<IoContext>(*io) : std::nullopt;
}

/// Wraps `f` to run under the context current here, on whichever thread runs
/// it, and to leave that thread's own context as it found it. Use it to hand
/// request work to another thread or pool (a hedged read, a prefetch):
/// without it the work runs with no context. The wrapper is callable as
/// const, as tbb::task_group::run requires.
template <typename F>
auto bindIoContext(F&& f) {
  return [io = captureIoContext(), f = std::forward<F>(f)]() {
    IoScope scope(io ? &*io : nullptr);
    f();
  };
}

}  // namespace luxir
