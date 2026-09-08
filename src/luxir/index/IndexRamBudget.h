// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

#include <boost/container/small_vector.hpp>

namespace luxir {

// A shared pool of indexing RAM that concurrent work reserves against before
// launching.  There is NO blocking acquire, condition variable, or thread that
// waits for capacity.  Callers that cannot reserve hold their work as data and
// register a callback that capacity releases ping.  The callback only retries
// admission or schedules reclaim work: work waits, threads never do.  That rule
// makes the budget safe to share across TBB task workers, where a thread parked
// on a full budget could be the only thread able to run the task that releases
// it.
//
// Reservation admission remains lock-free: total and reserved are relaxed
// atomics, and tryAcquire uses a CAS.  A small registry mutex serializes merge
// demand, draining-inverter accounting, and callback lifetime.  Callbacks are
// copied while holding that mutex and invoked only after it is released.  The
// lock order at clients is driver/index mutex -> registry mutex; the budget
// never takes a client mutex.
//
// Merge demand reserves dynamic headroom without statically partitioning the
// pool.  A draining inverter remains reserved until its memory is destroyed,
// but is discounted from pressure because that reservation is already on its
// way out.  The global pressure predicate is therefore:
//
//   reserved + pending merge demand - draining inverter RAM > total
//
// Demand is capped at total for this predicate: values beyond a whole pool are
// operationally identical and can arise from zero-stream partition range
// batches.  The comparison is performed by subtraction to avoid overflow.
//
// The cap is deliberately soft: forceAcquire() overdraws so a client that has
// nothing in flight can always make progress (and so a single reservation larger
// than the whole budget still runs, alone).  The overshoot is bounded at one
// force-admitted batch per active merge, so it still scales with the number of
// active collections.  The demand/admission registry added here is groundwork
// for a node-wide overdraft token, but the token remains deferred: an idle
// MergeAdmissionDriver that loses it has no task for task_group::wait() to wait
// on.  Passing the token without spinning or parking a TBB worker first requires
// a continuation-owned driver lifetime, not just another registry bit.
//
// Current reservation clients are parallel field merges and inverter RAM.
// Future reclaimable clients, such as vector index builds, can use the same
// pressure-listener mechanism.
class IndexRamBudget {
  struct CallbackSlot {
    std::mutex mutex;
    std::function<void()> callback;

    explicit CallbackSlot(std::function<void()>&& callback)
      : callback(std::move(callback)) {}

    void notify() noexcept {
      const std::lock_guard<std::mutex> lock(mutex);
      if (!callback) return;
      try {
        callback();
      } catch (...) {
        // Guard destruction and reservation release must remain noexcept.  The
        // next capacity or pressure transition will retry the notification.
      }
    }

    void disable() noexcept {
      const std::lock_guard<std::mutex> lock(mutex);
      callback = {};
    }
  };

  struct MergeDemandEntry {
    std::shared_ptr<CallbackSlot> callback;
    int64_t bytes = 0;
    bool registered = true;
  };

  struct PressureEntry {
    std::shared_ptr<CallbackSlot> callback;
    bool registered = true;
  };

public:
  class MergeDemandRegistration {
    IndexRamBudget* budget = nullptr;
    std::shared_ptr<MergeDemandEntry> entry;

    MergeDemandRegistration(IndexRamBudget& budget,
                            std::shared_ptr<MergeDemandEntry>&& entry)
      : budget(&budget), entry(std::move(entry)) {}

    friend class IndexRamBudget;

  public:
    MergeDemandRegistration() = default;
    MergeDemandRegistration(const MergeDemandRegistration&) = delete;
    MergeDemandRegistration& operator=(const MergeDemandRegistration&) = delete;

    MergeDemandRegistration(MergeDemandRegistration&& other) noexcept
      : budget(other.budget), entry(std::move(other.entry)) {
      other.budget = nullptr;
    }

    MergeDemandRegistration& operator=(MergeDemandRegistration&& other) noexcept {
      if (this != &other) {
        reset();
        budget = other.budget;
        entry = std::move(other.entry);
        other.budget = nullptr;
      }
      return *this;
    }

    ~MergeDemandRegistration() { reset(); }

    void publish(int64_t bytes) {
      assert(bytes >= 0);
      if (budget != nullptr) budget->publishDemand(entry, bytes);
    }

    void reset() noexcept {
      if (budget == nullptr) return;
      IndexRamBudget* target = budget;
      target->unregisterDemand(entry);
      entry->callback->disable();
      budget = nullptr;
      entry.reset();
    }

    explicit operator bool() const { return budget != nullptr; }
  };

  class PressureRegistration {
    IndexRamBudget* budget = nullptr;
    std::shared_ptr<PressureEntry> entry;

    PressureRegistration(IndexRamBudget& budget,
                         std::shared_ptr<PressureEntry>&& entry)
      : budget(&budget), entry(std::move(entry)) {}

    friend class IndexRamBudget;

  public:
    PressureRegistration() = default;
    PressureRegistration(const PressureRegistration&) = delete;
    PressureRegistration& operator=(const PressureRegistration&) = delete;

    PressureRegistration(PressureRegistration&& other) noexcept
      : budget(other.budget), entry(std::move(other.entry)) {
      other.budget = nullptr;
    }

    PressureRegistration& operator=(PressureRegistration&& other) noexcept {
      if (this != &other) {
        reset();
        budget = other.budget;
        entry = std::move(other.entry);
        other.budget = nullptr;
      }
      return *this;
    }

    ~PressureRegistration() { reset(); }

    void reset() noexcept {
      if (budget == nullptr) return;
      IndexRamBudget* target = budget;
      target->unregisterPressure(entry);
      entry->callback->disable();
      budget = nullptr;
      entry.reset();
    }

    explicit operator bool() const { return budget != nullptr; }
  };

  class Guard {
    IndexRamBudget* budget = nullptr;
    int64_t bytes = 0;
    bool draining = false;

  public:
    Guard() = default;
    Guard(IndexRamBudget& budget, int64_t bytes) : budget(&budget), bytes(bytes) {}
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;

    Guard(Guard&& other) noexcept
      : budget(other.budget), bytes(other.bytes), draining(other.draining) {
      other.budget = nullptr;
      other.bytes = 0;
      other.draining = false;
    }

    Guard& operator=(Guard&& other) noexcept {
      if (this != &other) {
        release();
        budget = other.budget;
        bytes = other.bytes;
        draining = other.draining;
        other.budget = nullptr;
        other.bytes = 0;
        other.draining = false;
      }
      return *this;
    }

    ~Guard() { release(); }

    void release() noexcept {
      if (budget != nullptr) {
        budget->releaseReservation(bytes, draining);
        budget = nullptr;
        bytes = 0;
        draining = false;
      }
    }

    bool tryResize(int64_t newBytes) {
      assert(newBytes >= 0);
      assert(budget != nullptr);
      assert(!draining);
      if (!budget->tryResize(bytes, newBytes)) return false;
      bytes = newBytes;
      return true;
    }

    // Unconditionally resizes this reservation, overdrawing the cap like
    // forceAcquire.  Returns true when the pool is over its cap afterwards
    // (never when uncapped), so a resync plus the over-budget check costs a
    // single reservation atomic op.
    bool forceResize(int64_t newBytes) {
      assert(newBytes >= 0);
      assert(budget != nullptr);
      assert(!draining);
      bool over = budget->forceAdjust(newBytes - bytes);
      bytes = newBytes;
      return over;
    }

    // Requested and commit flushes are unconditionally draining.  Pressure
    // flushes claim a victim only while global pressure still exists, so
    // concurrent writers cannot all act on the same stale pressure snapshot.
    void markDraining() {
      assert(budget != nullptr);
      assert(!draining);
      budget->markDraining(bytes);
      draining = true;
    }

    bool tryMarkDrainingForPressure() {
      assert(budget != nullptr);
      assert(!draining);
      if (!budget->tryMarkDrainingForPressure(bytes)) return false;
      draining = true;
      return true;
    }

    void clearDraining() noexcept {
      assert(budget != nullptr);
      assert(draining);
      budget->clearDraining(bytes);
      draining = false;
    }

    int64_t size() const { return bytes; }
    bool isDraining() const { return draining; }
    explicit operator bool() const { return budget != nullptr; }
  };

private:
  std::atomic<int64_t> total = 0;
  std::atomic<int64_t> reserved = 0;
  // Relaxed mirror of "any registered demand entry has nonzero bytes",
  // maintained under registryMutex, read lock-free by pressurePossible().
  std::atomic<int32_t> nonzeroDemands = 0;
  mutable std::mutex registryMutex;
  std::vector<std::shared_ptr<MergeDemandEntry>> mergeDemands;
  std::vector<std::shared_ptr<PressureEntry>> pressureListeners;
  int64_t drainingBytes = 0;

  using CallbackList = boost::container::small_vector<std::shared_ptr<CallbackSlot>, 8>;

  int64_t effectiveDemandLocked(int64_t cap) const {
    assert(cap > 0);
    int64_t demand = 0;
    for (const auto& entry : mergeDemands) {
      if (!entry->registered || entry->bytes == 0) continue;
      int64_t available = cap - demand;
      if (entry->bytes >= available) return cap;
      demand += entry->bytes;
    }
    return demand;
  }

  bool pressureNeededLocked() const {
    int64_t cap = total.load(std::memory_order_relaxed);
    if (cap == 0) return false;
    int64_t current = reserved.load(std::memory_order_relaxed);
    assert(current >= drainingBytes);
    int64_t live = current - drainingBytes;
    if (live > cap) return true;
    return effectiveDemandLocked(cap) > cap - live;
  }

  void collectAdmissionCallbacksLocked(CallbackList& callbacks) const {
    callbacks.reserve(callbacks.size() + mergeDemands.size());
    for (const auto& entry : mergeDemands) {
      if (entry->registered && entry->bytes != 0) {
        callbacks.push_back(entry->callback);
      }
    }
  }

  void collectPressureCallbacksLocked(CallbackList& callbacks) const {
    callbacks.reserve(callbacks.size() + pressureListeners.size());
    for (const auto& entry : pressureListeners) {
      if (entry->registered) callbacks.push_back(entry->callback);
    }
  }

  static void notifyCallbacks(CallbackList& callbacks) noexcept {
    for (const auto& callback : callbacks) callback->notify();
  }

  void notifyAfterReservationGrowth() noexcept {
    CallbackList pressure;
    try {
      {
        const std::lock_guard<std::mutex> lock(registryMutex);
        if (pressureNeededLocked()) collectPressureCallbacksLocked(pressure);
      }
      notifyCallbacks(pressure);
    } catch (...) {
    }
  }

  void notifyCapacityReleased() noexcept {
    CallbackList admissions;
    try {
      {
        const std::lock_guard<std::mutex> lock(registryMutex);
        collectAdmissionCallbacksLocked(admissions);
      }
      notifyCallbacks(admissions);
    } catch (...) {
    }
  }

  bool tryResize(int64_t oldBytes, int64_t newBytes) {
    assert(oldBytes >= 0 && newBytes >= 0);
    if (newBytes <= oldBytes) {
      [[maybe_unused]] int64_t prev =
          reserved.fetch_sub(oldBytes - newBytes, std::memory_order_relaxed);
      assert(prev >= oldBytes);
      if (newBytes != oldBytes) notifyCapacityReleased();
      return true;
    }
    int64_t cap = total.load(std::memory_order_relaxed);
    int64_t cur = reserved.load(std::memory_order_relaxed);
    do {
      assert(cur >= oldBytes);
      int64_t withoutGuard = cur - oldBytes;
      if (cap != 0 && (withoutGuard > cap || newBytes > cap - withoutGuard)) {
        return false;
      }
    } while (!reserved.compare_exchange_weak(cur, cur - oldBytes + newBytes,
                                             std::memory_order_relaxed));
    notifyAfterReservationGrowth();
    return true;
  }

  // Moves reserved by delta unconditionally; true when the pool ends up over
  // its cap (never when uncapped).
  bool forceAdjust(int64_t delta) {
    int64_t now = reserved.fetch_add(delta, std::memory_order_relaxed) + delta;
    assert(now >= 0);
    if (delta > 0) {
      notifyAfterReservationGrowth();
    } else if (delta < 0) {
      notifyCapacityReleased();
    }
    int64_t cap = total.load(std::memory_order_relaxed);
    return cap != 0 && now > cap;
  }

  void releaseReservation(int64_t bytes, bool draining) noexcept {
    CallbackList admissions;
    CallbackList pressure;
    try {
      {
        const std::lock_guard<std::mutex> lock(registryMutex);
        [[maybe_unused]] int64_t prev =
            reserved.fetch_sub(bytes, std::memory_order_relaxed);
        assert(prev >= bytes);
        if (draining) {
          assert(drainingBytes >= bytes);
          drainingBytes -= bytes;
        }
        collectAdmissionCallbacksLocked(admissions);
        if (pressureNeededLocked()) collectPressureCallbacksLocked(pressure);
      }
      notifyCallbacks(admissions);
      notifyCallbacks(pressure);
    } catch (...) {
    }
  }

  void markDraining(int64_t bytes) {
    const std::lock_guard<std::mutex> lock(registryMutex);
    assert(bytes >= 0);
    assert(reserved.load(std::memory_order_relaxed) >= drainingBytes + bytes);
    drainingBytes += bytes;
  }

  bool tryMarkDrainingForPressure(int64_t bytes) {
    const std::lock_guard<std::mutex> lock(registryMutex);
    assert(bytes >= 0);
    if (!pressureNeededLocked()) return false;
    assert(reserved.load(std::memory_order_relaxed) >= drainingBytes + bytes);
    drainingBytes += bytes;
    return true;
  }

  void clearDraining(int64_t bytes) noexcept {
    CallbackList pressure;
    try {
      {
        const std::lock_guard<std::mutex> lock(registryMutex);
        assert(drainingBytes >= bytes);
        drainingBytes -= bytes;
        if (pressureNeededLocked()) collectPressureCallbacksLocked(pressure);
      }
      notifyCallbacks(pressure);
    } catch (...) {
    }
  }

  void publishDemand(const std::shared_ptr<MergeDemandEntry>& entry, int64_t bytes) {
    CallbackList pressure;
    {
      const std::lock_guard<std::mutex> lock(registryMutex);
      if (!entry->registered) return;
      if ((entry->bytes == 0) != (bytes == 0)) {
        nonzeroDemands.fetch_add(bytes != 0 ? 1 : -1, std::memory_order_relaxed);
      }
      entry->bytes = bytes;
      if (pressureNeededLocked()) collectPressureCallbacksLocked(pressure);
    }
    notifyCallbacks(pressure);
  }

  void unregisterDemand(const std::shared_ptr<MergeDemandEntry>& entry) noexcept {
    try {
      const std::lock_guard<std::mutex> lock(registryMutex);
      if (entry->bytes != 0) {
        nonzeroDemands.fetch_sub(1, std::memory_order_relaxed);
      }
      entry->bytes = 0;
      entry->registered = false;
      std::erase(mergeDemands, entry);
    } catch (...) {
    }
  }

  void unregisterPressure(const std::shared_ptr<PressureEntry>& entry) noexcept {
    try {
      const std::lock_guard<std::mutex> lock(registryMutex);
      entry->registered = false;
      std::erase(pressureListeners, entry);
    } catch (...) {
    }
  }

public:
  explicit IndexRamBudget(int64_t totalBytes = 0) : total(totalBytes) {
    assert(totalBytes >= 0);
  }

  bool tryAcquire(int64_t bytes) {
    assert(bytes >= 0);
    return tryResize(0, bytes);
  }

  std::optional<Guard> tryAcquireGuard(int64_t bytes) {
    if (!tryAcquire(bytes)) return std::nullopt;
    return Guard(*this, bytes);
  }

  Guard forceAcquire(int64_t bytes) {
    assert(bytes >= 0);
    forceAdjust(bytes);
    return Guard(*this, bytes);
  }

  void release(int64_t bytes) noexcept {
    assert(bytes >= 0);
    releaseReservation(bytes, false);
  }

  MergeDemandRegistration registerMergeDemand(std::function<void()>&& callback) {
    auto slot = std::make_shared<CallbackSlot>(std::move(callback));
    auto entry = std::make_shared<MergeDemandEntry>();
    entry->callback = std::move(slot);
    {
      const std::lock_guard<std::mutex> lock(registryMutex);
      mergeDemands.push_back(entry);
    }
    return MergeDemandRegistration(*this, std::move(entry));
  }

  PressureRegistration registerPressureListener(std::function<void()>&& callback) {
    auto slot = std::make_shared<CallbackSlot>(std::move(callback));
    auto entry = std::make_shared<PressureEntry>();
    entry->callback = std::move(slot);
    bool notify = false;
    {
      const std::lock_guard<std::mutex> lock(registryMutex);
      pressureListeners.push_back(entry);
      notify = pressureNeededLocked();
    }
    PressureRegistration registration(*this, std::move(entry));
    if (notify) registration.entry->callback->notify();
    return registration;
  }

  // True when reservations exceed the cap (never when uncapped).  This does
  // not include pending demand or discount draining reservations.
  bool overBudget() const {
    int64_t cap = total.load(std::memory_order_relaxed);
    return cap != 0 && reserved.load(std::memory_order_relaxed) > cap;
  }

  bool pressureNeeded() const {
    const std::lock_guard<std::mutex> lock(registryMutex);
    return pressureNeededLocked();
  }

  // Conservative lock-free pre-check: false only when pressure is provably
  // absent (reservations within cap and no published merge demand), so callers
  // can skip victim scans and the registry mutex on per-release fast paths.
  // May return true where the locked predicate says no (draining discount,
  // demand under headroom) - it gates the probe, never decides a shed.  Safe
  // against staleness: same-thread reservation growth is visible to a
  // subsequent read, and cross-thread growth or demand publication pings the
  // pressure listeners, which re-enter the gated path with the gate true.
  bool pressurePossible() const {
    int64_t cap = total.load(std::memory_order_relaxed);
    if (cap == 0) return false;
    return reserved.load(std::memory_order_relaxed) > cap
        || nonzeroDemands.load(std::memory_order_relaxed) != 0;
  }

  void setTotalBytes(int64_t totalBytes) {
    assert(totalBytes >= 0);
    CallbackList admissions;
    CallbackList pressure;
    {
      const std::lock_guard<std::mutex> lock(registryMutex);
      int64_t old = total.load(std::memory_order_relaxed);
      total.store(totalBytes, std::memory_order_relaxed);
      if (totalBytes == 0 || (old != 0 && totalBytes > old)) {
        collectAdmissionCallbacksLocked(admissions);
      }
      if (pressureNeededLocked()) collectPressureCallbacksLocked(pressure);
    }
    notifyCallbacks(admissions);
    notifyCallbacks(pressure);
  }

  int64_t totalBytes() const {
    return total.load(std::memory_order_relaxed);
  }

  int64_t reservedBytes() const {
    return reserved.load(std::memory_order_relaxed);
  }

  int64_t drainingBytesCount() const {
    const std::lock_guard<std::mutex> lock(registryMutex);
    return drainingBytes;
  }

  int64_t pendingMergeDemandBytes() const {
    const std::lock_guard<std::mutex> lock(registryMutex);
    int64_t cap = total.load(std::memory_order_relaxed);
    int64_t limit = cap == 0 ? std::numeric_limits<int64_t>::max() : cap;
    int64_t demand = 0;
    for (const auto& entry : mergeDemands) {
      if (!entry->registered || entry->bytes == 0) continue;
      int64_t available = limit - demand;
      if (entry->bytes >= available) return limit;
      demand += entry->bytes;
    }
    return demand;
  }
};

} // namespace luxir
