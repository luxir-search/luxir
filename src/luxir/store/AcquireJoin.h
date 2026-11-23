// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <condition_variable>
#include <cstddef>
#include <exception>
#include <functional>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

#include "FileIOException.h"
#include "OutputStream.h"

namespace luxir {

/// Joins batched acquires across files, so a caller issues them together and
/// waits once for all of them.
///
/// Call acquire() for each file's batch, then wait(). Each batch's holds go
/// to its `take`, on the thread that completed the batch, before wait()
/// returns; `take` gets exactly one hold per range requested. Holds `take`
/// leaves in the vector are released before the batch counts as complete. A
/// batch that completes inline (memory, a mapping, the default batch) runs
/// `take` before acquire() returns, and wait() then returns without blocking.
/// wait() rethrows the first error a batch, or a `take`, reported.
///
/// The destructor waits for batches still in flight, so a caller that throws
/// between acquire() and wait() leaves no completion running on freed state.
class AcquireJoin {
  std::mutex mutex;
  std::condition_variable finished;
  size_t pending = 0;
  std::exception_ptr error;

  void complete(std::exception_ptr failure) noexcept {
    std::lock_guard lock(mutex);
    if (failure && !error) error = std::move(failure);
    if (--pending == 0) finished.notify_all();
  }

  void await() noexcept {
    std::unique_lock lock(mutex);
    finished.wait(lock, [&] { return pending == 0; });
  }

public:
  using Take = std::function<void(std::vector<HeldRange>&&)>;

  AcquireJoin() = default;
  AcquireJoin(const AcquireJoin&) = delete;
  AcquireJoin& operator=(const AcquireJoin&) = delete;
  ~AcquireJoin() { await(); }

  /// Starts one batch on `file`, which must outlive wait(). A throw (only
  /// allocation) leaves the batch unissued.
  void acquire(InputFile& file, std::span<const ByteRange> ranges, Take take) {
    // Built before the batch counts as pending: the conversion can allocate.
    AcquireDone done = [this, count = ranges.size(), take = std::move(take)](
                           std::vector<HeldRange>&& delivered, std::exception_ptr failure) noexcept {
      // Owned here, so what `take` leaves releases before the waiter wakes
      // and can destroy the file.
      auto holds = std::move(delivered);
      if (!failure) {
        try {
          if (holds.size() != count) throw FileIOException("batched acquire returned the wrong number of holds");
          take(std::move(holds));
        } catch (...) {
          failure = std::current_exception();
        }
      }
      holds.clear();
      complete(std::move(failure));
    };
    {
      std::lock_guard lock(mutex);
      ++pending;
    }
    file.acquireBatch(ranges, std::move(done));
  }

  /// Blocks until every batch completed, then rethrows the first error.
  void wait() {
    await();
    std::lock_guard lock(mutex);
    if (error) std::rethrow_exception(std::exchange(error, nullptr));
  }
};

} // namespace luxir
