// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cassert>
#include <exception>
#include <memory>
#include <utility>
#include <tbb/task_arena.h>
#include <tbb/task_group.h>
#include "Signal.h"

namespace luxir {

// One attempt, shared by its builder and waiters. Creation must not check a
// request's deadline or charge request-scoped memory: everyone takes the
// builder's outcome. The deferred handle holds the wait open until the builder
// runs it itself; spawning could strand it behind critical tasks or in another arena.
template <typename Value>
class SharedLazyBuild {
  using Pointer = std::shared_ptr<Value>;
  // Cancellation of one requester must not skip a build shared with others.
  tbb::task_group_context context{tbb::task_group_context::isolated,
                                  tbb::task_group_context::concurrent_wait};
  tbb::task_group group{context};
  Pointer value;
  std::exception_ptr error;

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
    return group.defer([this, create = std::forward<CreateFunc>(createFunc)] {
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
    });
  }

  Pointer run(tbb::task_handle&& build) {
    checkRecursion();
    group.run_and_wait(std::move(build));
    return result();
  }

  Pointer wait() {
    checkRecursion();
    Signal::emit("sharedLazyBuildWait", this);
    group.wait();
    return result();
  }
};

} // namespace luxir
