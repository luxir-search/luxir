// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <vector>

#include <oneapi/tbb/task_arena.h>

#include "AcquireJoin.h"

namespace luxir {

/// One round of batched acquires that a request waits on, either blocking
/// (wait()) or without holding a thread (resumeOn()).
///
/// Issue each file's batch with acquire(), then end the round with wait() or
/// resumeOn(), once, from the issuing thread. The round ends when every batch
/// completed, when `deadline` passes, or when `stop` is requested, whichever
/// comes first. Ending early leaves the batches running: their holds still
/// reach their `take`, so what they acquire is not lost, and keep() keeps the
/// owners `take` writes into alive until the last batch completed.
///
/// A round whose batches all complete inline (memory, a mapping, the default
/// InputFile batch) ends on the issuing thread with no wait and no hop.
class AcquireRound final : public AcquireBatches, public std::enable_shared_from_this<AcquireRound> {
public:
  using Clock = std::chrono::steady_clock;
  // Runs once with the round's failure, or null when every batch succeeded.
  using Then = std::function<void(std::exception_ptr)>;

private:
  struct Private {};
  friend class RoundDeadlines;

  const Clock::time_point deadline;
  const std::stop_token stop;
  // The batches in flight, plus one until the round is ended.
  std::atomic<size_t> pending{1};
  std::atomic<bool> fired{false};
  std::mutex mutex;
  std::condition_variable finished;
  std::exception_ptr error;    // the first batch's failure
  std::exception_ptr outcome;  // what ended the round
  bool ended = false;
  oneapi::tbb::task_arena* arena = nullptr;
  Then then;
  std::vector<std::shared_ptr<const void>> owners;
  std::optional<std::multimap<Clock::time_point, std::weak_ptr<AcquireRound>>::iterator> deadlineEntry;
  std::optional<std::stop_callback<std::function<void()>>> onStop;

  void complete(std::exception_ptr failure) noexcept;
  void fire(std::exception_ptr reason) noexcept;
  void runThen() noexcept;
  // Starts waiting for the deadline and the stop token; false when the round
  // ended meanwhile.
  void watch();

public:
  AcquireRound(Private, Clock::time_point deadline, std::stop_token stop)
      : deadline(deadline), stop(std::move(stop)) {}
  ~AcquireRound();

  /// A round that ends at `deadline` with DeadlineExceededError, or on `stop`
  /// with RequestCancelledError, if its batches have not completed by then.
  static std::shared_ptr<AcquireRound> create(Clock::time_point deadline = Clock::time_point::max(),
                                              std::stop_token stop = {});

  void acquire(InputFile& file, std::span<const ByteRange> ranges, Take take) override;

  /// Keeps `owner` alive until every batch completed.
  void keep(std::shared_ptr<const void> owner);

  /// Blocks until the round ends and returns why: null when every batch
  /// succeeded, else the first batch failure, DeadlineExceededError or
  /// RequestCancelledError.
  std::exception_ptr wait();

  /// Ends issuing. True when every batch already completed: the caller goes
  /// on on its own thread (failure() says how the batches went) and `then`
  /// never runs. Otherwise false: `then` runs once, as a task on `arena`, when
  /// the round ends, and the caller must not touch the state `then` uses.
  /// If that enqueue fails, `then` runs on the thread that ended the round.
  bool resumeOn(oneapi::tbb::task_arena& arena, Then then);

  /// How the round ended so far: see wait().
  std::exception_ptr failure();
};

}  // namespace luxir
