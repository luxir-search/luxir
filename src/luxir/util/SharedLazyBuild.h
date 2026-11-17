// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cassert>
#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <utility>
#include <tbb/task_arena.h>
#include <tbb/task_group.h>
#include "IoContext.h"
#include "Signal.h"

namespace luxir {

// One attempt, shared by its builder and waiters. Creation must not check a
// request's deadline or charge request-scoped memory: everyone takes the
// builder's outcome. The deferred handle holds the wait open until the builder
// runs it itself; spawning could strand it behind critical tasks or in another arena.
// For the same reason the build runs under IoContext::shared(): its reads count
// for the builder's request, but that request's deadline and fill hint do not
// apply to a value every waiter takes.
template <typename Value>
class SharedLazyBuild {
  using Pointer = std::shared_ptr<Value>;
  // Cancellation of one requester must not skip a build shared with others.
  tbb::task_group_context context{tbb::task_group_context::isolated,
                                  tbb::task_group_context::concurrent_wait};
  tbb::task_group group{context};
  Pointer value;
  std::exception_ptr error;
  // Set once value or error is, for waiters bounded by their request.
  std::mutex finishedMutex;
  std::condition_variable_any finishedChanged;
  bool finished = false;

#ifndef NDEBUG
  struct ActiveBuild;
  inline static thread_local ActiveBuild* activeBuild = nullptr;
  struct ActiveBuild {
    const SharedLazyBuild* build;
    ActiveBuild* previous;
    explicit ActiveBuild(const SharedLazyBuild* build)
        : build(build), previous(activeBuild) { activeBuild = this; }
    ~ActiveBuild() { activeBuild = previous; }
  };
#endif

  void checkRecursion() const {
#ifndef NDEBUG
    for (auto* active = activeBuild; active; active = active->previous) {
      assert(active->build != this && "recursive shared build");
    }
#endif
  }

  Pointer result() const {
    if (error) std::rethrow_exception(error);
    return value;
  }

public:
  template <typename CreateFunc>
  tbb::task_handle defer(CreateFunc&& createFunc) {
    auto io = captureIoContext();
    if (io) io = io->shared();
    return group.defer([this, io, create = std::forward<CreateFunc>(createFunc)] {
      IoScope scope(io ? &*io : nullptr);
      try {
        // A nested wait must not steal a task that joins this suspended build.
        value = tbb::this_task_arena::isolate([&] {
#ifndef NDEBUG
          ActiveBuild active(this);
#endif
          return create();
        });
      } catch (...) {
        error = std::current_exception();
      }
      {
        std::lock_guard lock(finishedMutex);
        finished = true;
      }
      finishedChanged.notify_all();
    });
  }

  Pointer run(tbb::task_handle&& build) {
    checkRecursion();
    group.run_and_wait(std::move(build));
    return result();
  }

  // Waits for the builder's outcome. A waiter working for a request with a
  // deadline or a stop token waits no longer than its request: it leaves
  // with a fetch_timeout or cancelled error while the build goes on for the
  // others.
  Pointer wait() {
    checkRecursion();
    Signal::emit("sharedLazyBuildWait", this);
    const IoContext* io = IoContext::current();
    if (!io || (!io->hasDeadline() && !io->stop.stop_possible())) {
      group.wait();
      return result();
    }
    std::unique_lock lock(finishedMutex);
    auto done = [this] { return finished; };
    if (!(io->hasDeadline() ? finishedChanged.wait_until(lock, io->stop, io->deadline, done)
                            : finishedChanged.wait(lock, io->stop, done))) {
      if (io->stopRequested()) throw RequestCancelledError();
      throw ApiError(ErrorKind::UNAVAILABLE, "fetch_timeout",
                     "the request's deadline passed while it waited for a build shared with other requests");
    }
    return result();
  }
};

} // namespace luxir
