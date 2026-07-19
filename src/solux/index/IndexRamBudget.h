#pragma once

#include <cassert>
#include <cstdint>
#include <mutex>
#include <optional>

namespace solux {

// A shared pool of indexing RAM that concurrent work reserves against before
// launching.  Deliberately just counters: there is NO blocking acquire, no waiter
// queue, and no condition variable.  Callers that cannot reserve must hold their
// work as data and retry when capacity is released (typically from a completed
// task's release path) - work waits, threads never do.  That rule is what makes
// the budget safe to share across TBB task workers: a thread parked on a full
// budget could be the only thread able to run the task that would release it.
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
// into one batch.
//
// Current reservation client: parallel field merges (SegmentMerger).  Planned:
// inverter RAM as tracked (non-reserved) usage that shrinks merge headroom so
// flushes win under pressure, and vector index builds as reservations.
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

    int64_t size() const {
      return bytes;
    }

    explicit operator bool() const {
      return budget != nullptr;
    }
  };

private:
  mutable std::mutex mutex;
  int64_t total = 0;
  int64_t reserved = 0;

  bool tryResize(int64_t oldBytes, int64_t newBytes) {
    const std::lock_guard<std::mutex> lock(mutex);
    assert(oldBytes >= 0 && newBytes >= 0 && reserved >= oldBytes);
    int64_t withoutGuard = reserved - oldBytes;
    if (newBytes > oldBytes && total != 0
        && (withoutGuard > total || newBytes > total - withoutGuard)) {
      return false;
    }
    reserved = withoutGuard + newBytes;
    return true;
  }

public:
  explicit IndexRamBudget(int64_t totalBytes = 0) : total(totalBytes) {
    assert(totalBytes >= 0);
  }

  bool tryAcquire(int64_t bytes) {
    assert(bytes >= 0);
    const std::lock_guard<std::mutex> lock(mutex);
    if (total != 0 && reserved + bytes > total) {
      return false;
    }
    reserved += bytes;
    return true;
  }

  std::optional<Guard> tryAcquireGuard(int64_t bytes) {
    if (!tryAcquire(bytes)) {
      return std::nullopt;
    }
    return Guard(*this, bytes);
  }

  Guard forceAcquire(int64_t bytes) {
    assert(bytes >= 0);
    const std::lock_guard<std::mutex> lock(mutex);
    reserved += bytes;
    return Guard(*this, bytes);
  }

  void release(int64_t bytes) {
    assert(bytes >= 0);
    const std::lock_guard<std::mutex> lock(mutex);
    assert(reserved >= bytes);
    reserved -= bytes;
  }

  void setTotalBytes(int64_t totalBytes) {
    assert(totalBytes >= 0);
    const std::lock_guard<std::mutex> lock(mutex);
    total = totalBytes;
  }

  int64_t totalBytes() const {
    const std::lock_guard<std::mutex> lock(mutex);
    return total;
  }

  int64_t reservedBytes() const {
    const std::lock_guard<std::mutex> lock(mutex);
    return reserved;
  }
};

} // namespace solux
