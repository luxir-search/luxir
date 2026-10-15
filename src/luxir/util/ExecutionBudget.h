// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <stop_token>

namespace luxir {

// A search's execution budget: a steady-clock deadline plus the client's
// cancellation token.  Execution polls it at segment starts and match-window
// boundaries, never per document.  Once exhausted it stays exhausted, and the
// flags record why: a timeout ends collection early and the request reports
// its partial results, a cancellation turns the request into an error.
//
// Code that has the request in hand asks it directly (SearchRequest::
// shouldStop).  Free-standing loops (the Collector.h window drivers) consult
// the budget installed on the calling thread: SearchEngine installs it for the
// request body and task_group_run carries it into spawned tasks.
class ExecutionBudget {
public:
  using Clock = std::chrono::steady_clock;

  Clock::time_point deadline = Clock::time_point::max();
  std::stop_token cancellation;
  std::atomic<bool> timedOut{false};
  std::atomic<bool> cancelled{false};

  // The budget of the search whose work runs on this thread, or null.
  static inline thread_local ExecutionBudget* current = nullptr;

  // TEST-ONLY: when positive, a budget with a deadline reports a timeout at
  // the check that brings this count to zero, so tests can stop a query at a
  // deterministic point instead of racing the clock.
  static inline std::atomic<int64_t> expireAfterChecksForTests{0};

  bool hasDeadline() const { return deadline != Clock::time_point::max(); }

  // True once execution should stop: the deadline passed or the client went
  // away.  Cheap enough for a per-window check: two relaxed loads and a token
  // poll, plus a clock read only when a deadline is set.
  bool exhausted() {
    if (timedOut.load(std::memory_order_relaxed)
        || cancelled.load(std::memory_order_relaxed)) {
      return true;
    }
    if (cancellation.stop_requested()) {
      cancelled.store(true, std::memory_order_relaxed);
      return true;
    }
    if (!hasDeadline()) return false;
    if ((expireAfterChecksForTests.load(std::memory_order_relaxed) > 0
         && expireAfterChecksForTests.fetch_sub(1, std::memory_order_relaxed) == 1)
        || Clock::now() >= deadline) {
      timedOut.store(true, std::memory_order_relaxed);
      return true;
    }
    return false;
  }

  // The calling thread's budget is exhausted; false when none is installed.
  static bool currentExhausted() {
    ExecutionBudget* budget = current;
    return budget != nullptr && budget->exhausted();
  }

  // Installs `budget` (possibly null) as the calling thread's budget for the
  // scope's lifetime and restores the previous one after, so a thread that
  // runs another request's task while it waits keeps each budget apart.
  class Scope {
    ExecutionBudget* saved;

  public:
    explicit Scope(ExecutionBudget* budget) : saved(current) { current = budget; }
    ~Scope() { current = saved; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
  };
};

} // namespace luxir
