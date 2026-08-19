#pragma once

#include <atomic>
#include <cassert>
#include <cstdint>
#include <optional>

namespace luxir {

// A shared pool of indexing RAM that concurrent work reserves against before
// launching.  Deliberately just counters: there is NO blocking acquire, no waiter
// queue, and no condition variable.  Callers that cannot reserve must hold their
// work as data and retry when capacity is released (typically from a completed
// task's release path) - work waits, threads never do.  That rule is what makes
// the budget safe to share across TBB task workers: a thread parked on a full
// budget could be the only thread able to run the task that would release it.
//
// Lock-free: both counters are relaxed atomics.  The budget is advisory
// accounting - no data is published through it - so no ordering is needed.
// The one pairing the old mutex provided that atomics do not is
// setTotalBytes() racing an admission check, which can admit against the
// stale cap; the soft cap (below) already tolerates that.
//
// The cap is deliberately soft: forceAcquire() overdraws so a client that has
// nothing in flight can always make progress (and so a single reservation larger
// than the whole budget still runs, alone).  The overshoot is bounded at one
// force-admitted batch per active merge - but that bound SCALES WITH THE NUMBER
// OF ACTIVELY-MERGING COLLECTIONS, and the forced batches can be the huge ones.
// TODO: gate overdrafts globally - a single node-wide overdraft token
// (tryForceAcquire), with starved drivers registering an admission callback that
// release() pings so the token passes to the next starved merge.  Keeps the
// no-thread-ever-waits rule; turns worst-case overshoot from sum-over-merges
// into one batch.  (With inverter RAM folded into reserved, over-cap is the
// steady state under heavy indexing load - the token's grant condition must not
// key on raw reserved-vs-total or it would never grant.)
//
// Current reservation clients: parallel field merges (SegmentMerger), and
// inverter RAM (each Inverter carries a Guard that IndexWriter::releaseInverter
// resyncs to memSize() once per batch, so accumulated inverter RAM shrinks merge
// headroom and vice versa; an over-budget release sheds that writer's largest
// idle inverter).  Pressure shedding is per-writer: another writer's idle
// inverters are reclaimed only by its own releases or commits - node-wide
// victim selection would need a writer registry (same cluster as the
// overdraft-token TODO).  Planned: vector index builds as reservations.
class IndexRamBudget {
public:
  class Guard {
    IndexRamBudget* budget = nullptr;
    int64_t bytes = 0;

  public:
    Guard() = default;
    Guard(IndexRamBudget& budget, int64_t bytes) : budget(&budget), bytes(bytes) {}
    Guard(const Guard&) = delete;
    Guard& operator=(const Guard&) = delete;

    Guard(Guard&& other) noexcept : budget(other.budget), bytes(other.bytes) {
      other.budget = nullptr;
      other.bytes = 0;
    }

    Guard& operator=(Guard&& other) noexcept {
      if (this != &other) {
        release();
        budget = other.budget;
        bytes = other.bytes;
        other.budget = nullptr;
        other.bytes = 0;
      }
      return *this;
    }

    ~Guard() {
      release();
    }

    void release() {
      if (budget != nullptr) {
        budget->release(bytes);
        budget = nullptr;
        bytes = 0;
      }
    }

    bool tryResize(int64_t newBytes) {
      assert(newBytes >= 0);
      assert(budget != nullptr);
      if (!budget->tryResize(bytes, newBytes)) return false;
      bytes = newBytes;
      return true;
    }

    // Unconditionally resizes this reservation, overdrawing the cap like
    // forceAcquire.  Returns true when the pool is over its cap afterwards
    // (never when uncapped), so a resync plus the over-budget check costs a
    // single atomic op.
    bool forceResize(int64_t newBytes) {
      assert(newBytes >= 0);
      assert(budget != nullptr);
      bool over = budget->forceAdjust(newBytes - bytes);
      bytes = newBytes;
      return over;
    }

    int64_t size() const {
      return bytes;
    }

    explicit operator bool() const {
      return budget != nullptr;
    }
  };

private:
  std::atomic<int64_t> total = 0;
  std::atomic<int64_t> reserved = 0;

  bool tryResize(int64_t oldBytes, int64_t newBytes) {
    assert(oldBytes >= 0 && newBytes >= 0);
    if (newBytes <= oldBytes) {
      [[maybe_unused]] int64_t prev =
          reserved.fetch_sub(oldBytes - newBytes, std::memory_order_relaxed);
      assert(prev >= oldBytes);
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
    return true;
  }

  // Moves reserved by delta unconditionally; true when the pool ends up over
  // its cap (never when uncapped).
  bool forceAdjust(int64_t delta) {
    int64_t now = reserved.fetch_add(delta, std::memory_order_relaxed) + delta;
    assert(now >= 0);
    int64_t cap = total.load(std::memory_order_relaxed);
    return cap != 0 && now > cap;
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
    if (!tryAcquire(bytes)) {
      return std::nullopt;
    }
    return Guard(*this, bytes);
  }

  Guard forceAcquire(int64_t bytes) {
    assert(bytes >= 0);
    forceAdjust(bytes);
    return Guard(*this, bytes);
  }

  void release(int64_t bytes) {
    assert(bytes >= 0);
    [[maybe_unused]] int64_t prev = reserved.fetch_sub(bytes, std::memory_order_relaxed);
    assert(prev >= bytes);
  }

  // True when reservations exceed the cap (never when uncapped).
  bool overBudget() const {
    int64_t cap = total.load(std::memory_order_relaxed);
    return cap != 0 && reserved.load(std::memory_order_relaxed) > cap;
  }

  void setTotalBytes(int64_t totalBytes) {
    assert(totalBytes >= 0);
    total.store(totalBytes, std::memory_order_relaxed);
  }

  int64_t totalBytes() const {
    return total.load(std::memory_order_relaxed);
  }

  int64_t reservedBytes() const {
    return reserved.load(std::memory_order_relaxed);
  }
};

} // namespace luxir
