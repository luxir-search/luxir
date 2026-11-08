// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>
#include <pthread.h>
#include <algorithm>
#include <oneapi/tbb/flow_graph.h>
#include <oneapi/tbb/info.h>
#include <oneapi/tbb/task_group.h>

namespace luxir {

/// Label the calling thread for ps/top/gdb (15-char kernel limit).
inline void nameThisThread(const char* name) {
#if defined(__APPLE__)
  pthread_setname_np(name);
#else
  pthread_setname_np(pthread_self(), name);
#endif
}


/// Threads that can compute at once in an arena of the given max_concurrency.
/// The node arena also reserves a slot per transport thread (see LuxirNode),
/// so its max_concurrency overstates parallelism; cap at the affinity width.
inline int computeWidth(int arenaConcurrency) {
  return std::max(1, std::min(arenaConcurrency, oneapi::tbb::info::default_concurrency()));
}


inline std::atomic<bool> countTaskSpawnsForTests{false};
inline std::atomic<int64_t> taskSpawnsForTests{0};

/// makeTask runs on the caller and may read borrowed state; its returned
/// callable owns its captures. Publish indices 0..n-2 and run n-1 here.
/// A null group runs every index here, in order.
template <typename MakeTask>
void task_group_fanout(oneapi::tbb::task_group* tg, size_t n, MakeTask&& makeTask) {
  for (size_t i = 0; i < n; i++) {
    if (tg && i + 1 < n) {
      tg->run(makeTask(i));
      if (countTaskSpawnsForTests.load(std::memory_order_relaxed)) {
        taskSpawnsForTests.fetch_add(1, std::memory_order_relaxed);
      }
    } else {
      makeTask(i)();
    }
  }
}


/// Scope guard for a maybe-parallel fan-out over one (lazily created)
/// task_group: pass group() to task_group_fanout, or run(spawn, f) to spawn f
/// or run it inline.  Call join() on the success path: it waits and
/// propagates the first task exception.  If the scope unwinds before join(),
/// the destructor cancels the group and waits while swallowing any parked
/// task exception, so the unwind never double-throws (a bare ~task_group
/// rethrowing a captured task exception during active unwinding would
/// std::terminate).
class TaskGroupRunner {
  std::optional<oneapi::tbb::task_group> tg;
  bool joined = false;

public:
  oneapi::tbb::task_group* group() {
    if (!tg) tg.emplace();
    return &*tg;
  }

  template <typename F>
  void run(bool spawn, F&& f) {
    if (spawn) {
      group()->run(std::forward<F>(f));
    } else {
      std::forward<F>(f)();
    }
  }

  void join() {
    joined = true;
    if (tg) tg->wait();
  }

  ~TaskGroupRunner() {
    if (joined || !tg) return;
    tg->cancel();
    try { tg->wait(); } catch (...) {}
  }
};


// A simple (but heavyweight) class like a latch so that one can block waiting on an async action to complete.
// When this class waits, it enters the TBB work stealing loop.
class Blocker {
  tbb::flow::graph g;

  // If we don't need to pass data, it might be simpler (or maybe faster) to
  // just use graph.reserve_wait() and graph.release_wait() directly.

public:
  Blocker() : g() {
    g.reserve_wait();
  }

  // If this blocker is used in a loop, call this again to cause the next call to wait() to block again.
  void reserve_wait() {
    g.reserve_wait();
  }

  // enter TBB work-stealing loop until someone else calls blocker.notify()
  void wait() {
    g.wait_for_all();  // this "blocks" (but enters TBB work stealing loop)
  }

  // call this to signal that the async work is done and that the wait() call can return.
  void notify() {
    g.release_wait(); // notifies flow graph that async work has been completed.
  }
};



// A simple (but heavyweight) class like a latch so that one can block waiting on an async action to complete.
// When this class waits, it enters the TBB work stealing loop.
class BlockerAsyncNode {
  using node_t = tbb::flow::async_node<void*, void*>;
  using gateway_t = node_t::gateway_type;
  tbb::flow::graph g;
  std::function<void()> action;
  node_t asyncNode;

  // If we don't need to pass data, it might be simpler (or maybe faster) to
  // just use graph.reserve_wait() and graph.release_wait() directly.

public:
  BlockerAsyncNode(std::function<void()> action) :
          action(std::move(action)),
          asyncNode(g, tbb::flow::unlimited, [this](void* input, gateway_t& gateway) {
            gateway.reserve_wait(); // notifies flow graph that async work has been submitted.  Do this first to avoid race conditions.
            this->action(); // do our action after so there is no possibility of notify() being called first!
          } )
  {
  }

  // call this to kick off the async work and then wait for someone to call notify()
  void wait() {
    asyncNode.try_put(nullptr);
    g.wait_for_all();  // this "blocks" (but enters TBB work stealing loop)
  }

  // call this to signal that the async work is done and that the wait() call can return.
  void notify() {
    asyncNode.gateway().release_wait(); // notifies flow graph that async work has been completed.
  }
};


}
