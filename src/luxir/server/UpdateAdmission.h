// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <vector>

#include "luxir/index/IndexRamBudget.h"

namespace luxir {

// Node-wide admission of update requests: how many may be in flight at once
// and how many raw request bytes they may hold.  A transport admits an update
// before it submits it to a writer and the ticket goes with the message, so
// the slot is released exactly once, when the message completes (done()) or
// is dropped on a failed submission.
//
// The update graph itself accepts every message (startUpdateNode is a
// queueing function_node), and indexing runs at low priority, so without this
// a client can queue updates faster than a search-loaded node indexes them
// until memory runs out.  Over the limit a transport refuses (unary gRPC:
// RESOURCE_EXHAUSTED, buffered HTTP: 429) or, for streams, stops reading.
//
// Nothing here blocks.  A paused stream parks a resume callback with
// whenAvailable(); a release that leaves capacity hands every parked callback
// back (outside the lock) to retry.  Callbacks must be short and must not
// block: they run on whatever thread released the slot, often an indexing
// worker.  Rejecting or deferring an update happens before the writer sees it,
// so it never fails a shared inverter or the writer.
//
// Admitted bytes are also charged to the IndexRamBudget as transient
// reservations (see forceAcquireTransient): they delay merge admission while
// ingest holds RAM, but do not count as pressure that sheds inverters.
class UpdateAdmission {
public:
  class Ticket {
    UpdateAdmission* owner = nullptr;
    int64_t bytes = 0;
    IndexRamBudget::Guard ram;

    friend class UpdateAdmission;
    Ticket(UpdateAdmission& owner, int64_t bytes, IndexRamBudget::Guard ram)
      : owner(&owner), bytes(bytes), ram(std::move(ram)) {}

  public:
    Ticket() = default;
    Ticket(const Ticket&) = delete;
    Ticket& operator=(const Ticket&) = delete;
    Ticket(Ticket&& other) noexcept : owner(other.owner), bytes(other.bytes), ram(std::move(other.ram)) {
      other.owner = nullptr;
      other.bytes = 0;
    }
    Ticket& operator=(Ticket&& other) noexcept {
      if (this != &other) {
        release();
        owner = other.owner;
        bytes = other.bytes;
        ram = std::move(other.ram);
        other.owner = nullptr;
        other.bytes = 0;
      }
      return *this;
    }
    ~Ticket() { release(); }

    // Idempotent.
    void release() noexcept {
      if (owner == nullptr) return;
      UpdateAdmission* target = owner;
      owner = nullptr;
      ram.release();
      target->released(bytes);
      bytes = 0;
    }

    int64_t size() const { return bytes; }
    explicit operator bool() const { return owner != nullptr; }
  };

  struct Snapshot {
    int64_t updates = 0;
    int64_t bytes = 0;
    int64_t maxUpdates = 0;
    int64_t maxBytes = 0;
    uint64_t rejected = 0;
    uint64_t paused = 0;
  };

private:
  IndexRamBudget* ram;
  mutable std::mutex mutex;
  // protected by mutex:
  int64_t maxUpdates;  // 0 = unlimited
  int64_t maxBytes;    // 0 = unlimited
  int64_t updates = 0;
  int64_t bytes = 0;
  std::vector<std::function<void()>> waiters;

  std::atomic<uint64_t> rejected{0};
  std::atomic<uint64_t> paused{0};

  // Room for one more update: a reader may start reading the next one.
  bool availableLocked() const {
    return (maxUpdates == 0 || updates < maxUpdates) && (maxBytes == 0 || bytes < maxBytes);
  }

  // An update larger than the whole byte limit is admitted alone, so the
  // limit never makes a valid request impossible.
  bool fitsLocked(int64_t size) const {
    if (maxUpdates != 0 && updates >= maxUpdates) return false;
    return maxBytes == 0 || bytes == 0 || size <= maxBytes - bytes;
  }

  Ticket chargeLocked(int64_t size) {
    updates++;
    bytes += size;
    return Ticket(*this, size, ram ? ram->forceAcquireTransient(size) : IndexRamBudget::Guard());
  }

  std::vector<std::function<void()>> takeWaitersLocked() {
    std::vector<std::function<void()>> out;
    if (!waiters.empty() && availableLocked()) out.swap(waiters);
    return out;
  }

  static void wake(std::vector<std::function<void()>>& resumes) noexcept {
    for (auto& resume : resumes) {
      try {
        resume();
      } catch (...) {
        // A resume that fails here is lost; it re-parks on its next attempt,
        // and the next release wakes it.
      }
    }
  }

  void released(int64_t size) noexcept {
    std::vector<std::function<void()>> resumes;
    {
      const std::lock_guard<std::mutex> lock(mutex);
      updates--;
      bytes -= size;
      resumes = takeWaitersLocked();
    }
    wake(resumes);
  }

public:
  // 0 is unlimited for either limit.
  UpdateAdmission(int64_t maxUpdates, int64_t maxBytes, IndexRamBudget* ram = nullptr)
    : ram(ram), maxUpdates(std::max<int64_t>(0, maxUpdates)), maxBytes(std::max<int64_t>(0, maxBytes)) {}
  UpdateAdmission(const UpdateAdmission&) = delete;
  UpdateAdmission& operator=(const UpdateAdmission&) = delete;

  // Changes the limits in place (tests, embedding); raising them wakes
  // parked streams.
  void setLimits(int64_t updateLimit, int64_t byteLimit) {
    std::vector<std::function<void()>> resumes;
    {
      const std::lock_guard<std::mutex> lock(mutex);
      maxUpdates = std::max<int64_t>(0, updateLimit);
      maxBytes = std::max<int64_t>(0, byteLimit);
      resumes = takeWaitersLocked();
    }
    wake(resumes);
  }

  // Admits an update of `size` raw bytes if it fits; otherwise counts a
  // rejection and returns nothing.
  std::optional<Ticket> tryAdmit(int64_t size) {
    {
      const std::lock_guard<std::mutex> lock(mutex);
      if (fitsLocked(size)) return chargeLocked(size);
    }
    rejected.fetch_add(1, std::memory_order_relaxed);
    return std::nullopt;
  }

  // Admits unconditionally: a stream message already read (its reader
  // checked whenAvailable() before reading it), or a stream's barrier batch
  // that must go ahead.  The overshoot is at most one message per stream.
  Ticket admit(int64_t size) {
    const std::lock_guard<std::mutex> lock(mutex);
    return chargeLocked(size);
  }

  // True when there is room to read another update now.  Otherwise parks
  // `resume`, to be called once (from a releasing thread) when room returns,
  // and returns false.  The caller retries then, and may park again.
  bool whenAvailable(std::function<void()> resume) {
    {
      const std::lock_guard<std::mutex> lock(mutex);
      if (availableLocked()) return true;
      try {
        waiters.push_back(std::move(resume));
      } catch (...) {
        return true;  // read on rather than strand the stream
      }
    }
    paused.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  bool available() const {
    const std::lock_guard<std::mutex> lock(mutex);
    return availableLocked();
  }

  Snapshot snapshot() const {
    Snapshot out;
    {
      const std::lock_guard<std::mutex> lock(mutex);
      out.updates = updates;
      out.bytes = bytes;
      out.maxUpdates = maxUpdates;
      out.maxBytes = maxBytes;
    }
    out.rejected = rejected.load(std::memory_order_relaxed);
    out.paused = paused.load(std::memory_order_relaxed);
    return out;
  }
};

}  // namespace luxir
