// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "AcquireRound.h"

#include "luxir/util/DeadlineScheduler.h"
#include "luxir/util/IoContext.h"
#include "luxir/util/log.h"

namespace luxir {

// Ends rounds whose deadline passed. One scheduler slot serves every round,
// and it never goes away, so a round needs no slot of its own to detach.
class RoundDeadlines {
  using Entries = std::multimap<AcquireRound::Clock::time_point, std::weak_ptr<AcquireRound>>;
  std::mutex mutex;
  Entries entries;
  DeadlineScheduler::Slot slot{[this] { expire(); }};

  void expire() {
    std::vector<std::shared_ptr<AcquireRound>> due;
    std::optional<AcquireRound::Clock::time_point> next;
    {
      std::lock_guard lock(mutex);
      auto now = AcquireRound::Clock::now();
      while (!entries.empty() && entries.begin()->first <= now) {
        if (auto round = entries.begin()->second.lock()) {
          round->deadlineEntry.reset();
          due.push_back(std::move(round));
        }
        entries.erase(entries.begin());
      }
      if (!entries.empty()) next = entries.begin()->first;
    }
    if (next) DeadlineScheduler::global().arm(slot, *next);
    for (auto& round : due) {
      round->fire(std::make_exception_ptr(DeadlineExceededError("storage reads passed the request deadline")));
    }
  }

public:
  // Leaked: a static destructor could run while the scheduler thread fires.
  static RoundDeadlines& instance() {
    static auto* deadlines = new RoundDeadlines;
    return *deadlines;
  }

  void add(AcquireRound& round) {
    {
      std::lock_guard lock(mutex);
      round.deadlineEntry = entries.emplace(round.deadline, round.weak_from_this());
    }
    DeadlineScheduler::global().arm(slot, round.deadline);
  }

  void remove(AcquireRound& round) noexcept {
    std::lock_guard lock(mutex);
    if (round.deadlineEntry) entries.erase(*std::exchange(round.deadlineEntry, std::nullopt));
  }
};

std::shared_ptr<AcquireRound> AcquireRound::create(Clock::time_point deadline, std::stop_token stop) {
  return std::make_shared<AcquireRound>(Private{}, deadline, std::move(stop));
}

AcquireRound::~AcquireRound() {
  if (deadline != Clock::time_point::max()) RoundDeadlines::instance().remove(*this);
}

void AcquireRound::acquire(InputFile& file, std::span<const ByteRange> ranges, Take take) {
  // Built before the batch counts as pending: the conversion can allocate.
  AcquireDone done = [self = shared_from_this(), count = ranges.size(), take = std::move(take)](
                         std::vector<HeldRange>&& delivered, std::exception_ptr failure) noexcept {
    // Owned here, so what `take` leaves releases before the round completes.
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
    self->complete(std::move(failure));
  };
  pending.fetch_add(1, std::memory_order_relaxed);
  file.acquireBatch(ranges, std::move(done));
}

void AcquireRound::keep(std::shared_ptr<const void> owner) {
  std::lock_guard lock(mutex);
  owners.push_back(std::move(owner));
}

void AcquireRound::complete(std::exception_ptr failure) noexcept {
  if (failure) {
    std::lock_guard lock(mutex);
    if (!error) error = std::move(failure);
  }
  if (pending.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
  std::exception_ptr first;
  {
    std::lock_guard lock(mutex);
    first = error;
    owners.clear();  // every batch completed: nothing writes into them any more
  }
  fire(std::move(first));
}

void AcquireRound::fire(std::exception_ptr reason) noexcept {
  if (fired.exchange(true, std::memory_order_acq_rel)) return;
  if (deadline != Clock::time_point::max()) RoundDeadlines::instance().remove(*this);
  oneapi::tbb::task_arena* resumeArena;
  {
    std::lock_guard lock(mutex);
    outcome = std::move(reason);
    ended = true;
    resumeArena = arena;
  }
  finished.notify_all();
  if (!resumeArena) return;  // wait() wakes up
  try {
    resumeArena->enqueue([self = shared_from_this()] { self->runThen(); });
  } catch (...) {
    runThen();
  }
}

void AcquireRound::runThen() noexcept {
  auto run = std::move(then);  // releases its captures once it returns
  try {
    run(failure());
  } catch (const std::exception& e) {
    LOG_ERROR("acquire round continuation failed: {}", e.what());
  } catch (...) {
    LOG_ERROR("acquire round continuation failed");
  }
}

void AcquireRound::watch() {
  if (stop.stop_possible()) {
    onStop.emplace(stop, [this] { fire(std::make_exception_ptr(RequestCancelledError())); });
  }
  if (deadline != Clock::time_point::max() && !fired.load(std::memory_order_acquire)) {
    RoundDeadlines::instance().add(*this);
    // Ended while it registered: the entry would only expire late.
    if (fired.load(std::memory_order_acquire)) RoundDeadlines::instance().remove(*this);
  }
}

std::exception_ptr AcquireRound::wait() {
  if (pending.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    fired.store(true, std::memory_order_release);
    std::lock_guard lock(mutex);
    ended = true;
    owners.clear();
    return outcome = error;
  }
  watch();
  std::unique_lock lock(mutex);
  finished.wait(lock, [&] { return ended; });
  return outcome;
}

bool AcquireRound::resumeOn(oneapi::tbb::task_arena& resumeArena, Then continuation) {
  {
    std::lock_guard lock(mutex);
    arena = &resumeArena;
    then = std::move(continuation);
  }
  if (pending.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    fired.store(true, std::memory_order_release);
    std::lock_guard lock(mutex);
    ended = true;
    owners.clear();
    outcome = error;
    then = nullptr;
    return true;
  }
  watch();
  return false;
}

std::exception_ptr AcquireRound::failure() {
  std::lock_guard lock(mutex);
  return ended ? outcome : error;
}

}  // namespace luxir
