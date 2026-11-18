// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>

namespace luxir {

// Node-wide cap on running merges, shared by every collection's writer (each
// writer already runs at most one policy merge at a time).  Waiting is data,
// never a blocked thread: a writer that finds every slot taken leaves its
// Ticket in a FIFO queue and returns.  The release that frees a slot hands it
// to the oldest ticket and calls the ticket's wake callback, which must not
// block (a writer queues a task on its own graph that resubmits the merge, or
// hands the slot back if the merge is no longer due).
//
// Merges a client waits for (forced merges, and merges while a wait_for_merges
// commit is pending) take a slot at once through acquireNow(), even past the
// limit: they count, so natural merges wait behind them, but they never wait.
class MergeSlots {
public:
  using Clock = std::chrono::steady_clock;

  // One per writer.  State fields are guarded by MergeSlots::mutex.
  class Ticket {
    friend class MergeSlots;
    std::mutex callbackMutex;
    std::function<void()> wake;  // guarded by callbackMutex
    bool queued = false;
    bool granted = false;  // a released slot was handed over and not yet used
    Clock::time_point queuedAt{};

    void notify() {
      std::lock_guard lock(callbackMutex);
      if (wake) wake();
    }

  public:
    explicit Ticket(std::function<void()> wake) : wake(std::move(wake)) {}
    // Returns once no wake callback is running; none starts afterwards.
    void disable() {
      std::lock_guard lock(callbackMutex);
      wake = {};
    }
  };

  struct Stats {
    int limit = 0;
    int64_t running = 0;
    int64_t waiting = 0;
    uint64_t waits = 0;      // merges that waited for a slot
    uint64_t waitNanos = 0;  // their summed wait
  };

private:
  int limit;  // 0 = unlimited
  std::mutex mutex;
  int64_t running = 0;
  std::deque<std::shared_ptr<Ticket>> waiting;
  uint64_t waits = 0, waitNanos = 0;

public:
  explicit MergeSlots(int limit) : limit(limit) {}

  // Takes a slot (or the one handed to `ticket`) and returns true, or queues
  // `ticket` and returns false; its wake callback runs when a slot is handed
  // over.  A queued ticket keeps its place.
  bool tryAcquire(const std::shared_ptr<Ticket>& ticket) {
    std::lock_guard lock(mutex);
    if (ticket->granted) {
      ticket->granted = false;
      return true;
    }
    if (!ticket->queued && waiting.empty() && hasRoomLocked()) {
      running++;
      return true;
    }
    if (!ticket->queued) {
      ticket->queued = true;
      ticket->queuedAt = Clock::now();
      waiting.push_back(ticket);
    }
    return false;
  }

  // Takes a slot without waiting, past the limit if need be.
  void acquireNow() {
    std::lock_guard lock(mutex);
    running++;
  }

  bool queued(const Ticket& ticket) {
    std::lock_guard lock(mutex);
    return ticket.queued;
  }

  void release() {
    std::shared_ptr<Ticket> next;
    {
      std::lock_guard lock(mutex);
      running--;
      next = grantNextLocked();
    }
    if (next) next->notify();
  }

  // Hands back a slot granted to `ticket` that it did not use.
  void returnGrant(Ticket& ticket) {
    std::shared_ptr<Ticket> next;
    {
      std::lock_guard lock(mutex);
      if (!ticket.granted) return;
      ticket.granted = false;
      running--;
      next = grantNextLocked();
    }
    if (next) next->notify();
  }

  // Leaves the queue and hands back an unused grant (writer close).
  void cancel(Ticket& ticket) {
    {
      std::lock_guard lock(mutex);
      if (ticket.queued) {
        std::erase_if(waiting, [&](const auto& t) { return t.get() == &ticket; });
        ticket.queued = false;
      }
    }
    returnGrant(ticket);
  }

  Stats stats() {
    std::lock_guard lock(mutex);
    return {limit, running, (int64_t)waiting.size(), waits, waitNanos};
  }

private:
  bool hasRoomLocked() const { return limit == 0 || running < limit; }

  std::shared_ptr<Ticket> grantNextLocked() {
    if (waiting.empty() || !hasRoomLocked()) return nullptr;
    auto next = std::move(waiting.front());
    waiting.pop_front();
    next->queued = false;
    next->granted = true;
    running++;
    waits++;
    waitNanos += (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - next->queuedAt).count();
    return next;
  }
};

}
