// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "luxir/util/log.h"

namespace luxir {

// Process-wide deadline service: one lazily-started thread fires per-owner Slot
// callbacks at (or shortly after) their armed deadline.  Built for coarse,
// low-rate deadlines (deferred commits), not high-resolution timing.
//
// Contract:
//  - arm() only ever tightens: the slot fires at the earliest deadline it has been
//    armed with since it last fired.  Firing disarms the slot; the callback (or its
//    consequences) re-arms if more work is scheduled.  A fire can therefore be
//    early relative to the owner's current deadline (a stale tighter arm); owners
//    re-check their own state and re-arm.
//  - The callback runs on the scheduler thread with no scheduler lock held.  It
//    must be brief and must not call detach() on its own slot.
//  - detach() disarms and waits out any in-flight callback: after it returns the
//    callback will never run again, so the owner may be destroyed.  Owners that
//    ever armed MUST detach before destruction.
class DeadlineScheduler {
public:
  using Clock = std::chrono::steady_clock;

  class Slot {
    friend class DeadlineScheduler;
    std::function<void()> fn;
    Clock::time_point due = Clock::time_point::max();  // max = disarmed
    bool registered = false;
    bool running = false;
    bool detached = false;  // terminal: arm() refuses, so an in-flight callback
                            // racing detach() cannot re-register the slot

  public:
    explicit Slot(std::function<void()> fn) : fn(std::move(fn)) {}
  };

private:
  std::mutex mutex;
  std::condition_variable cv;
  std::vector<Slot*> slots;  // registered slots, scanned for the earliest due
  std::thread worker;        // started on first arm
  bool stopping = false;

public:
  static DeadlineScheduler& global() {
    static DeadlineScheduler instance;
    return instance;
  }

  void arm(Slot& slot, Clock::time_point due) {
    const std::lock_guard<std::mutex> lock(mutex);
    if (stopping || slot.detached) return;
    bool changed = false;
    if (!slot.registered) {
      slots.push_back(&slot);
      slot.registered = true;
      changed = true;
    }
    if (due < slot.due) {
      slot.due = due;
      changed = true;
    }
    if (!worker.joinable()) worker = std::thread([this] { run(); });
    if (changed) cv.notify_all();  // wake the worker only for a new earliest-candidate
  }

  void detach(Slot& slot) {
    std::unique_lock<std::mutex> lock(mutex);
    slot.detached = true;
    if (slot.registered) {
      std::erase(slots, &slot);
      slot.registered = false;
    }
    slot.due = Clock::time_point::max();
    cv.wait(lock, [&] { return !slot.running; });
  }

private:
  ~DeadlineScheduler() {
    {
      const std::lock_guard<std::mutex> lock(mutex);
      stopping = true;
    }
    cv.notify_all();
    if (worker.joinable()) worker.join();
  }

  void run() {
    std::unique_lock<std::mutex> lock(mutex);
    while (!stopping) {
      // Few slots (one per writer with a pending deferred commit): a scan beats
      // heap upkeep, and re-scanning after every wait keeps arms/detaches simple.
      Slot* next = nullptr;
      for (Slot* s : slots) {
        if (s->due != Clock::time_point::max() && (next == nullptr || s->due < next->due)) {
          next = s;
        }
      }
      if (next == nullptr) {
        cv.wait(lock);
        continue;
      }
      // Copy the deadline before waiting: wait_until releases the mutex, and a
      // detach() during the wait may free the slot (running is false here, so
      // detach does not block).  After waking we re-scan; `next` is never
      // touched again.
      const auto due = next->due;
      if (Clock::now() < due) {
        cv.wait_until(lock, due);
        continue;
      }
      next->due = Clock::time_point::max();
      next->running = true;
      lock.unlock();
      // detach() blocks on `running`, so the slot stays valid here.  A throwing
      // callback would std::terminate the worker; contain it.
      try {
        next->fn();
      } catch (const std::exception& e) {
        LOG_ERROR("DeadlineScheduler callback threw: {}", e.what());
      } catch (...) {
        LOG_ERROR("DeadlineScheduler callback threw an unknown exception");
      }
      lock.lock();
      next->running = false;
      cv.notify_all();  // wake detach waiters
    }
  }
};

}  // namespace luxir
