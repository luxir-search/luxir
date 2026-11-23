// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

#include "OutputStream.h"

namespace luxir {

/// Where batched acquires go: a caller issues one batch per file and each
/// batch's holds reach its `take`, on the thread that completed the batch.
/// AcquireJoin waits for them on the caller's thread; AcquireRound resumes a
/// request once they complete, without a thread waiting.
class AcquireBatches {
public:
  using Take = std::function<void(std::vector<HeldRange>&&)>;

  /// Starts one batch on `file`, which must stay open until the batch
  /// completed. A batch acquire that throws instead of completing counts as
  /// that batch failing.
  virtual void acquire(InputFile& file, std::span<const ByteRange> ranges, Take take) = 0;

protected:
  ~AcquireBatches() = default;
};

/// Joins batched acquires across files, so a caller issues them together and
/// waits once for all of them.
///
/// Call acquire() for each file's batch, then wait(). Each batch's holds go
/// to its `take`, on the thread that completed the batch, before wait()
/// returns. A batch that completes inline (memory, a mapping, the default
/// InputFile batch) runs `take` before acquire() returns, and wait() then
/// returns without blocking. wait() rethrows the first error a batch, or a
/// `take`, reported.
///
/// The destructor waits for batches still in flight, so a caller that throws
/// between acquire() and wait() leaves no completion running on freed state.
class AcquireJoin final : public AcquireBatches {
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
  AcquireJoin() = default;
  AcquireJoin(const AcquireJoin&) = delete;
  AcquireJoin& operator=(const AcquireJoin&) = delete;
  ~AcquireJoin() { await(); }

  /// Starts one batch on `file`. A batch acquire that throws instead of
  /// completing counts as that batch failing. `file` must outlive wait().
  void acquire(InputFile& file, std::span<const ByteRange> ranges, Take take) override {
    {
      std::lock_guard lock(mutex);
      ++pending;
    }
    auto once = std::make_shared<std::atomic<bool>>(false);
    try {
      file.acquire(ranges, [this, once, take = std::move(take)](std::vector<HeldRange>&& holds,
                                                                 std::exception_ptr failure) {
        if (once->exchange(true)) return;
        if (!failure) {
          try {
            take(std::move(holds));
          } catch (...) {
            failure = std::current_exception();
          }
        }
        complete(std::move(failure));
      });
    } catch (...) {
      if (!once->exchange(true)) complete(std::current_exception());
    }
  }

  /// Blocks until every batch completed, then rethrows the first error.
  void wait() {
    await();
    std::lock_guard lock(mutex);
    if (error) std::rethrow_exception(std::exchange(error, nullptr));
  }
};

} // namespace luxir
