// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "luxir/util/DeadlineScheduler.h"

namespace luxir {

// Node-wide admission for searches. Every dispatched search takes a slot before
// it runs, on an arena worker or inline on its HTTP shard, so a burst of long
// searches cannot occupy every worker and stall the rest of the node.
//
// Limits:
//  - maxRunning: searches running at once on the node;
//  - maxRunningPerCollection: of those, searches of one collection, so long
//    searches on one collection always leave a slot for the others;
//  - maxRunningRemote: of those, searches of collections on remote storage,
//    which hold a worker while their reads wait on the network;
//  - maxQueued: searches waiting for a slot, node-wide. One more is refused
//    (QUEUE_FULL);
//  - maxQueueWait: how long a search may wait for a slot (queue time only,
//    never execution). Zero is unbounded. A search waiting longer is refused
//    (QUEUE_TIMEOUT).
//
// Waiting searches hold no thread. Each collection has a FIFO, and freed slots
// go to the collections in round-robin order, so one collection's backlog
// delays another's searches by at most one turn. The thread that frees a slot
// starts the next search; that start only hands work off.
//
// Resumed streaming production (an emitter paused by backpressure) re-admits
// through a priority lane: it is never refused, counts against maxRunning only,
// and is served before any queued search, so new searches cannot starve streams
// already under way. It holds its slot for one production burst.
class SearchAdmission {
public:
  using Clock = std::chrono::steady_clock;

  struct Limits {
    uint32_t maxRunning = 1;
    uint32_t maxRunningPerCollection = 1;
    uint32_t maxRunningRemote = 1;
    uint32_t maxQueued = 0;
    std::chrono::milliseconds maxQueueWait{0};
  };

  // Why a search never started.
  enum class Refusal { QUEUE_FULL, QUEUE_TIMEOUT, CANCELLED, SHUTDOWN };

private:
  struct Lane;

public:
  // A running search's (or resume burst's) claim on the gate. Dropping it frees
  // the slot and starts the next waiter, if any; it is released exactly once.
  class Slot {
    friend class SearchAdmission;
    SearchAdmission* gate = nullptr;
    Lane* lane = nullptr;  // null for a resume burst
    bool remote = false;
    bool queued = false;
    Slot(SearchAdmission* gate, Lane* lane, bool remote, bool queued)
        : gate(gate), lane(lane), remote(remote), queued(queued) {}

  public:
    Slot() = default;
    Slot(Slot&& other) noexcept
        : gate(std::exchange(other.gate, nullptr)), lane(other.lane), remote(other.remote), queued(other.queued) {}
    Slot& operator=(Slot&& other) noexcept {
      if (this != &other) {
        reset();
        gate = std::exchange(other.gate, nullptr);
        lane = other.lane;
        remote = other.remote;
        queued = other.queued;
      }
      return *this;
    }
    ~Slot() { reset(); }
    void reset() noexcept {
      if (auto* g = std::exchange(gate, nullptr)) g->release(lane, remote);
    }
    explicit operator bool() const { return gate != nullptr; }
    // The holder waited in the queue, and was started by the thread that freed
    // a slot rather than the one that asked for it.
    bool waited() const { return queued; }
  };

  struct CollectionSnapshot {
    std::string collection;
    uint64_t running = 0;
    uint64_t queued = 0;
  };

  struct Snapshot {
    Limits limits;
    uint64_t running = 0;          // searches and resume bursts holding a slot
    uint64_t runningRemote = 0;
    uint64_t queued = 0;           // searches waiting for a slot
    uint64_t resumesQueued = 0;    // resume bursts waiting for a slot
    uint64_t admitted = 0;         // searches started, queued or not
    uint64_t waited = 0;           // of those, the ones that queued first
    uint64_t waitNanos = 0;        // summed queue time of those
    uint64_t rejectedFull = 0;     // refused on arrival: queue full
    uint64_t rejectedTimeout = 0;  // refused after maxQueueWait in the queue
    uint64_t cancelled = 0;        // cancelled by their transport while queued
    uint64_t resumes = 0;          // resume bursts started
    uint64_t resumesWaited = 0;    // of those, the ones that queued first
    uint64_t downgraded = 0;       // max_parallel=-1 searches run adaptively
    // Collections with a search running or queued now.
    std::vector<CollectionSnapshot> collections;
  };

private:
  struct Waiter {
    Lane* lane = nullptr;  // null in the resume lane
    bool remote = false;
    bool inQueue = false;  // guarded by mutex
    Clock::time_point queuedAt;
    Clock::time_point deadline = Clock::time_point::max();
    std::function<void(Slot)> start;
    std::function<void(Refusal)> refuse;
    std::optional<std::stop_callback<std::function<void()>>> cancellation;
  };

  struct Lane {
    std::string collection;
    uint32_t running = 0;
    bool inRing = false;
    std::deque<std::shared_ptr<Waiter>> queue;
  };

  using Started = std::vector<std::pair<std::shared_ptr<Waiter>, Slot>>;

  std::mutex mutex;
  const Limits limits;
  // guarded by mutex:
  std::unordered_map<std::string, std::unique_ptr<Lane>> lanes;
  std::vector<Lane*> ring;  // lanes with waiters, served round robin
  size_t cursor = 0;        // the ring position served next
  std::deque<std::shared_ptr<Waiter>> resumeQueue;
  uint64_t running = 0;
  uint64_t runningRemote = 0;
  uint64_t queued = 0;
  uint64_t admitted = 0;
  uint64_t waited = 0;
  uint64_t waitNanos = 0;
  uint64_t rejectedFull = 0;
  uint64_t rejectedTimeout = 0;
  uint64_t cancelledCount = 0;
  uint64_t resumes = 0;
  uint64_t resumesWaited = 0;
  bool closing = false;
  // Lock-free mirrors for the per-search load check (crowded()).
  std::atomic<uint64_t> runningNow{0};
  std::atomic<uint64_t> queuedNow{0};
  std::atomic<uint64_t> downgradedCount{0};
  DeadlineScheduler::Slot timer{[this] { expire(); }};

  Lane& laneLocked(const std::string& collection);
  void dropLaneIfIdleLocked(Lane* lane);
  bool runnableLocked(const Lane& lane, bool remote) const;
  Slot claimLocked(Lane* lane, bool remote, bool wasQueued);
  void startWaitersLocked(Started& started);
  void unqueueLocked(const std::shared_ptr<Waiter>& waiter);
  void armLocked();
  void publishLocked();
  void release(Lane* lane, bool remote) noexcept;
  void cancel(const std::weak_ptr<Waiter>& waiter) noexcept;
  void expire() noexcept;
  static void start(Started& started) noexcept;
  static void refuse(const std::shared_ptr<Waiter>& waiter, Refusal why) noexcept;

public:
  explicit SearchAdmission(Limits limits);
  // Refuses every queued search (SHUTDOWN) and starts queued resumes without
  // a slot. Running searches must have released their slots.
  ~SearchAdmission();
  SearchAdmission(const SearchAdmission&) = delete;
  SearchAdmission& operator=(const SearchAdmission&) = delete;

  const Limits& getLimits() const { return limits; }

  // A slot now, or none: only when one is free and no search or resume waits.
  // The HTTP inline lane runs on the shard only with such a slot.
  Slot tryAcquire(const std::string& collection, bool remote);

  // Calls start(slot) on this thread when a slot is free for the collection and
  // none of its searches waits. Otherwise queues the search, to be started by
  // the thread that frees a slot, or calls refuse(QUEUE_FULL) on this thread
  // when the queue is full. A queued search leaves the queue with
  // refuse(CANCELLED) on the stopping thread when `stop` fires, or
  // refuse(QUEUE_TIMEOUT) on the timer thread after maxQueueWait. Exactly one of
  // the callbacks runs. Neither may throw (an escaping exception is logged and
  // swallowed, and the slot freed); both only hand work off or answer.
  void admit(const std::string& collection, bool remote, std::stop_token stop,
             std::function<void(Slot)> start, std::function<void(Refusal)> refuse);

  // The priority lane for resumed production: start(slot) runs on this thread
  // when a slot is free and no other resume waits, otherwise on the thread that
  // frees one, ahead of every queued search. Never refused.
  void resume(std::function<void(Slot)> start);

  // Whether fanning one search out over `width` threads would exceed the node's
  // capacity: searches are waiting, or the running ones (the caller's
  // included) plus the extra width pass maxRunning.
  bool crowded(uint32_t width) const;
  void countDowngrade() { downgradedCount.fetch_add(1, std::memory_order_relaxed); }

  Snapshot snapshot();
};

}  // namespace luxir
