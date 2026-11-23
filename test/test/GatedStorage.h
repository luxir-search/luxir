// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <ranges>
#include <set>
#include <stop_token>
#include <thread>
#include <vector>

#include "luxir/store/DelegatingDirectory.h"
#include "luxir/store/FileIOException.h"

namespace luxir::test {

// Holds back the completions of batched acquires until `expected` batches
// were submitted, then delivers them in reverse order on its own thread, as
// a remote backend finishing out of order would. If fewer arrive within
// `patience` (the caller waited on one before issuing the rest), the gate
// records the timeout and delivers what it holds, so the caller fails
// instead of hanging. Once open, completions pass straight through.
class BatchGate {
  struct Completion {
    AcquireDone done;
    std::vector<HeldRange> holds;
    std::exception_ptr error;
  };

  mutable std::mutex mutex;
  std::condition_variable_any reached;
  const size_t expected;
  size_t submitted = 0;
  size_t delivered = 0;
  bool open = false;
  bool expired = false;
  std::vector<Completion> held;
  std::jthread releaser;  // Last, so it stops and joins before the rest goes.

  // Counts first, so a caller woken by the completion sees it counted.
  void deliver(Completion& completion) noexcept {
    {
      std::lock_guard lock(mutex);
      ++delivered;
    }
    completion.done(std::move(completion.holds), std::move(completion.error));
  }

public:
  BatchGate(size_t expected, std::chrono::milliseconds patience) : expected(expected) {
    held.reserve(expected);
    releaser = std::jthread([this, patience](std::stop_token stop) {
      std::vector<Completion> release;
      {
        std::unique_lock lock(mutex);
        bool full = reached.wait_for(lock, stop, patience, [&] { return submitted >= this->expected; });
        expired = !full && !stop.stop_requested();
        open = true;
        release = std::move(held);
      }
      for (auto& completion : std::views::reverse(release)) deliver(completion);
    });
  }

  // After the batch went to the delegate, so an inline completion is held.
  void submit() noexcept {
    std::lock_guard lock(mutex);
    if (++submitted == expected) reached.notify_all();
  }

  void complete(AcquireDone done, std::vector<HeldRange>&& holds, std::exception_ptr error) noexcept {
    Completion completion{std::move(done), std::move(holds), std::move(error)};
    {
      std::lock_guard lock(mutex);
      if (!open) {
        held.push_back(std::move(completion));
        return;
      }
    }
    deliver(completion);
  }

  bool timedOut() const {
    std::lock_guard lock(mutex);
    return expired;
  }
  size_t submissions() const {
    std::lock_guard lock(mutex);
    return submitted;
  }
  size_t deliveries() const {
    std::lock_guard lock(mutex);
    return delivered;
  }
};

class GatedInputFile : public InputFile {
  std::shared_ptr<InputFile> delegate;
  BatchGate& gate;
  bool fail;  // Every batch fails, without reaching the delegate.

public:
  GatedInputFile(std::shared_ptr<InputFile> delegate, BatchGate& gate, bool fail)
      : delegate(std::move(delegate)), gate(gate), fail(fail) {}

  size_t size() override { return delegate->size(); }
  HeldRange acquire(ByteRange range) override { return delegate->acquire(range); }
  bool holdsFree() const override { return delegate->holdsFree(); }
  void prefetch(size_t offset, size_t length) override { delegate->prefetch(offset, length); }

  void acquireBatch(std::span<const ByteRange> ranges, AcquireDone done) noexcept override {
    if (fail) {
      gate.complete(std::move(done), {}, std::make_exception_ptr(FileIOException("injected batch failure")));
      gate.submit();
      return;
    }
    AcquireDone toGate;
    try {
      toGate = [target = &gate, done](std::vector<HeldRange>&& holds, std::exception_ptr error) mutable noexcept {
        target->complete(std::move(done), std::move(holds), std::move(error));
      };
    } catch (...) {
      done({}, std::current_exception());
      return;
    }
    delegate->acquireBatch(ranges, std::move(toGate));
    gate.submit();
  }
};

// Gates the batches of every file it opens; the gate must outlive them.
// Batches on the files named in `failing` fail.
class GatedDirectory : public DelegatingDirectory {
public:
  BatchGate gate;
  std::set<std::string, std::less<>> failing;

  GatedDirectory(std::shared_ptr<Directory> inner, size_t expected, std::chrono::milliseconds patience)
      : DelegatingDirectory(std::move(inner)), gate(expected, patience) {}

  std::shared_ptr<InputFile> openFile(std::string_view name, OpenOptions options = {}) override {
    auto file = delegate->openFile(name, options);
    if (!file) return {};
    return std::make_shared<GatedInputFile>(std::move(file), gate, failing.contains(name));
  }
};

} // namespace luxir::test
