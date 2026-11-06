// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <utility>

namespace luxir {

// Node-wide admission for searches of collections on remote storage
// (Directory::REMOTE). Such a search holds an arena worker while its reads
// wait on the network, and it competes for workers with every gRPC search,
// max_parallel=1, min_commit and fan-out work: without a cap, enough cold
// searches occupy the whole pool. At most `slots` of them run at once; the
// rest wait in FIFO order without holding a thread. A waiting search is never
// rejected, only cancelled when its transport asks.
class RemoteSearchGate {
public:
  using Clock = std::chrono::steady_clock;

  // An admitted search's slot. Dropping it frees the slot and starts the next
  // waiting search, if any.
  class Slot {
    friend class RemoteSearchGate;
    RemoteSearchGate* gate = nullptr;
    explicit Slot(RemoteSearchGate* gate) : gate(gate) {}

  public:
    Slot() = default;
    Slot(Slot&& other) noexcept : gate(std::exchange(other.gate, nullptr)) {}
    Slot& operator=(Slot&& other) noexcept {
      if (this != &other) {
        reset();
        gate = std::exchange(other.gate, nullptr);
      }
      return *this;
    }
    ~Slot() { reset(); }
    void reset() noexcept {
      if (auto* g = std::exchange(gate, nullptr)) g->release();
    }
    explicit operator bool() const { return gate != nullptr; }
  };

  struct Snapshot {
    uint64_t slots = 0;
    uint64_t running = 0;
    uint64_t queued = 0;
    uint64_t admitted = 0;       // searches started, queued or not
    uint64_t waited = 0;         // of those, the ones that queued first
    uint64_t cancelled = 0;      // cancelled while queued, never started
    uint64_t waitNanos = 0;      // summed queue time of the searches started
  };

private:
  struct Waiter {
    uint64_t id = 0;
    Clock::time_point queuedAt;
    std::function<void(Slot)> start;
    std::function<void()> cancelled;
    std::optional<std::stop_callback<std::function<void()>>> cancellation;
  };

  std::mutex mutex;
  uint64_t slots;
  // guarded by mutex:
  uint64_t running = 0;
  uint64_t nextId = 0;
  std::deque<std::shared_ptr<Waiter>> queue;
  uint64_t admitted = 0;
  uint64_t waited = 0;
  uint64_t cancelledCount = 0;
  uint64_t waitNanos = 0;

  void release() noexcept;
  void cancel(uint64_t id) noexcept;

public:
  explicit RemoteSearchGate(uint64_t slots) : slots(slots < 1 ? 1 : slots) {}
  RemoteSearchGate(const RemoteSearchGate&) = delete;
  RemoteSearchGate& operator=(const RemoteSearchGate&) = delete;

  // Calls start(slot) on this thread when a slot is free and nobody waits,
  // otherwise queues it to be called by the thread that frees one. If `stop`
  // fires first, the search leaves the queue and cancelled() runs instead, on
  // the stopping thread. Neither callback may throw; both only hand work off.
  void admit(std::stop_token stop, std::function<void(Slot)> start, std::function<void()> cancelled);

  Snapshot snapshot();
};

}  // namespace luxir
