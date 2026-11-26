// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

#include "ApiError.h"

namespace luxir {

/// Storage I/O of one request, counted by every thread that works for it.
/// The request owns the counters; they outlive all of its tasks.
///
/// Two sources count here. Core counts the ranges a segment acquires from a
/// file that hands out held copies (not RAM or mmap, whose views cost
/// nothing): `acquires` and `acquiredBytes`, on any such backend. A storage
/// backend or layer that issues requests of its own (an object-store GET, a
/// hedge, a cache miss) counts them in `requests`, `rounds` and
/// `fetchedBytes`. The counters are additive.
struct IoCounters {
  // Held ranges acquired from storage (InputFile::acquire), one per range.
  std::atomic<uint64_t> acquires{0};
  // Bytes those ranges cover.
  std::atomic<uint64_t> acquiredBytes{0};
  // Store requests a backend issued (one ranged GET each).
  std::atomic<uint64_t> requests{0};
  // Sequential waits on the store: requests issued together and awaited
  // together count as one round. A backend that issues one request at a time
  // counts a round per request; one that fans a read out counts one per wave.
  std::atomic<uint64_t> rounds{0};
  // Bytes those requests returned.
  std::atomic<uint64_t> fetchedBytes{0};

  struct Snapshot {
    uint64_t acquires = 0, acquiredBytes = 0, requests = 0, rounds = 0, fetchedBytes = 0;
    bool empty() const { return (acquires | acquiredBytes | requests | rounds | fetchedBytes) == 0; }
  };

  Snapshot snapshot() const {
    return {acquires.load(std::memory_order_relaxed), acquiredBytes.load(std::memory_order_relaxed),
            requests.load(std::memory_order_relaxed), rounds.load(std::memory_order_relaxed),
            fetchedBytes.load(std::memory_order_relaxed)};
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
/// (FanoutScope) and into a resumed streaming emitter.
/// Storage code (a DelegatingDirectory layer, a backend's InputFile::acquire)
/// reads it with IoContext::current(): null means the read is not done for a
/// request (merges, startup, reader refresh, replication, verification), and
/// such reads must behave as before.
///
///   if (const auto* io = luxir::IoContext::current()) {
///     if (!io->fillCache) ...;                // a scan: do not admit into a cache
///     if (io->remaining() < hedgeAfter) ...;  // little time left: hedge now
///     if (io->stopRequested()) ...;           // the client is gone: stop waiting
///     io->countFetch(bytes);                  // a request the layer issued itself
///   }
///
/// A context is a small value: tasks copy it, so a nested IoScope over a
/// modified copy (a scan inside a request, say) is cheap and never outlived.
/// Only `counters` points at request-owned state, and `stop` shares the
/// request's stop state.
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
  // core never fails a read on it. Set it with setDeadline(); storage waits
  // end at waitDeadline(), a margin before it.
  Clock::time_point deadline = Clock::time_point::max();
  // How long before `deadline` storage waits end (waitDeadline()); set with
  // it by setDeadline().
  Clock::duration waitMargin = Clock::duration::zero();
  // Requested when the client cancelled the request or the server is
  // stopping; empty when nothing can cancel it. A layer may stop waiting for
  // a fetch on it, as does a wait on a shared build.
  std::stop_token stop;
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
  bool stopRequested() const { return stop.stop_requested(); }

  /// The most a storage wait keeps back from the transport deadline, and its
  /// share of the budget a request starts with.
  static constexpr Clock::duration kMaxWaitMargin = std::chrono::milliseconds(500);
  static constexpr int kWaitMarginPercent = 5;

  /// Sets the client's deadline at request start (`now`), and the margin
  /// storage waits keep before it: min(kMaxWaitMargin, 5% of the budget).
  void setDeadline(Clock::time_point clientDeadline, Clock::time_point now = Clock::now()) {
    deadline = clientDeadline;
    auto budget = clientDeadline > now ? clientDeadline - now : Clock::duration::zero();
    waitMargin = std::min(kMaxWaitMargin, budget * kWaitMarginPercent / 100);
  }

  /// When storage waits for this request end (a wait on a shared build, a
  /// storage layer's own fetch): a margin before the transport deadline, so a
  /// deadline_exceeded error still reaches a client that is waiting for it,
  /// instead of losing the race to the client's own deadline. max() without
  /// a deadline.
  Clock::time_point waitDeadline() const {
    return hasDeadline() ? deadline - waitMargin : Clock::time_point::max();
  }
  /// Time left before the deadline (negative once passed); max() without one.
  Clock::duration remaining(Clock::time_point now = Clock::now()) const {
    return hasDeadline() ? deadline - now : Clock::duration::max();
  }

  /// `ranges` held ranges of `bytes` in all, acquired in one call. Today
  /// every acquire takes one range; an acquire that takes a batch of ranges
  /// calls this once per batch, which is where a count of batches (the
  /// rounds a request waits on in sequence) belongs.
  void countAcquire(uint64_t bytes, uint64_t ranges = 1) const {
    if (!counters) return;
    counters->acquires.fetch_add(ranges, std::memory_order_relaxed);
    counters->acquiredBytes.fetch_add(bytes, std::memory_order_relaxed);
  }
  /// One wave of `requests` concurrent store requests that returned `bytes`.
  void countFetch(uint64_t bytes, uint64_t requests = 1, uint64_t rounds = 1) const {
    if (!counters) return;
    counters->requests.fetch_add(requests, std::memory_order_relaxed);
    counters->rounds.fetch_add(rounds, std::memory_order_relaxed);
    counters->fetchedBytes.fetch_add(bytes, std::memory_order_relaxed);
  }

  /// The context for work shared with other requests (a SharedLazyBuild):
  /// its reads still count for the request that does them, but no one
  /// request's deadline, cancellation or fill hint applies to a result
  /// everyone takes.
  IoContext shared() const {
    IoContext io = *this;
    io.deadline = Clock::time_point::max();
    io.waitMargin = Clock::duration::zero();
    io.stop = {};
    io.fillCache = true;
    return io;
  }
};

/// Counts an acquire into the context of the request this thread works for,
/// if there is one.
inline void countAcquire(uint64_t bytes, uint64_t ranges = 1) {
  if (const IoContext* io = IoContext::current()) io->countAcquire(bytes, ranges);
}

/// The request was cancelled (IoContext::stop) before it finished.
class RequestCancelledError : public ApiError {
public:
  RequestCancelledError() : ApiError(ErrorKind::UNAVAILABLE, "cancelled", "the request was cancelled") {}
};

/// The request's deadline (IoContext::waitDeadline) passed while it waited.
class DeadlineExceededError : public ApiError {
public:
  explicit DeadlineExceededError(std::string message)
      : ApiError(ErrorKind::UNAVAILABLE, "deadline_exceeded", std::move(message)) {}
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
