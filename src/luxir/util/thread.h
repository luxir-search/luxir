// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <thread>
#include <utility>
#include <type_traits>
#include <vector>
#include "TaskWork.h"
#include "LoadSignal.h"
#include "luxir_util.h"
#include <pthread.h>
#include <algorithm>
#include <oneapi/tbb/flow_graph.h>
#include <oneapi/tbb/info.h>
#include <oneapi/tbb/task_arena.h>
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

enum class FanoutPolicy : uint8_t { ADAPTIVE, EVERY };

// Owns lazy publication, cancellation and joining. Null scope pointers still
// mean that the entire subtree must run synchronously on the calling thread.
class FanoutScope {
  oneapi::tbb::task_arena* arena;
  FanoutPolicy policy;
  bool isolatedContext;
  std::atomic<bool> joined{false};
  uint64_t grain;
  // oneTBB's cancellation query is non-const.
  mutable std::optional<oneapi::tbb::task_group_context> context;
  std::optional<oneapi::tbb::isolated_task_group> group;
  // execute() resets the ambient cancellation context even in the same arena.
  // Remember scope-managed entries and tasks so nested groups can bind to the
  // running share's context instead of the arena's default context.
  inline static thread_local oneapi::tbb::task_arena* currentArena = nullptr;

  FanoutScope(oneapi::tbb::task_arena* arena, FanoutPolicy policy,
              bool isolatedContext, uint64_t grain)
      : arena(arena), policy(policy), isolatedContext(isolatedContext),
        grain(grain == 0 && policy == FanoutPolicy::ADAPTIVE ? FANOUT_GRAIN_NEVER : grain) {}

public:
  FanoutScope(oneapi::tbb::task_arena* arena, FanoutPolicy policy, bool isolatedContext = false)
      : FanoutScope(arena, policy, isolatedContext, fanoutGrainNs.load(std::memory_order_relaxed)) {}
  FanoutScope(const FanoutScope&) = delete;
  FanoutScope& operator=(const FanoutScope&) = delete;

  FanoutScope child(bool isolatedContext = false) const {
    return FanoutScope(arena, policy, isolatedContext, grain);
  }
  oneapi::tbb::task_arena* getArena() const { return arena; }
  FanoutPolicy getPolicy() const { return policy; }
  bool publishesEvery() const { return policy == FanoutPolicy::EVERY; }
  uint64_t grainNs() const { return grain; }
  bool published() const { return group.has_value(); }
  bool canceled() const { return context && context->is_group_execution_cancelled(); }
  void cancel() { if (context) context->cancel_group_execution(); }

  template <typename F>
  void enter(F&& f) {
    if (!arena || currentArena == arena) {
      std::forward<F>(f)();
    } else {
      arena->execute([&] {
        auto previous = std::exchange(currentArena, arena);
        auto restore = scope_guard([&] { currentArena = previous; });
        std::forward<F>(f)();
      });
    }
  }

  template <typename F>
  void spawn(F&& f) {
    enter([&] {
      // Only the owning thread can see an absent group: no share can run
      // request code until this first spawn has fully constructed the group.
      if (!group) {
        context.emplace(isolatedContext ? oneapi::tbb::task_group_context::isolated
                                        : oneapi::tbb::task_group_context::bound);
        group.emplace(*context);
      }
      // A scope can be reused after join (the stored-field fallback does so).
      // Shares can also spawn while the owner is joining, hence the atomic.
      joined.store(false, std::memory_order_relaxed);
      group->run([f = std::forward<F>(f), taskArena = currentArena] {
        auto previous = std::exchange(currentArena, taskArena);
        auto restore = scope_guard([&] { currentArena = previous; });
        f();
      });
    });
  }

  void join() {
    if (!group) return;
    enter([&] {
      // TBB drains and resets its context even when wait rethrows a task error.
      auto finish = scope_guard([&] { joined.store(true, std::memory_order_relaxed); });
      group->wait();
    });
  }

  ~FanoutScope() {
    if (!group || joined.load(std::memory_order_relaxed)) return;
    cancel();
    try { join(); } catch (...) {}
  }
};

namespace detail {

class FanoutShareTiming {
  uint64_t startNs;
  uint64_t estimatedNs;
public:
  FanoutShareTiming(uint64_t publishedNs, std::thread::id publisher, uint64_t estimatedNs)
      : startNs(TaskClock{}()), estimatedNs(estimatedNs) {
    uint64_t delay = startNs - publishedNs;
    fanoutShareStartDelayNsSum.fetch_add(delay, std::memory_order_relaxed);
    fanoutShareStartDelayCount.fetch_add(1, std::memory_order_relaxed);
    uint64_t maximum = fanoutShareStartDelayNsMax.load(std::memory_order_relaxed);
    while (maximum < delay && !fanoutShareStartDelayNsMax.compare_exchange_weak(
        maximum, delay, std::memory_order_relaxed)) {}
    if (publisher == std::this_thread::get_id()) {
      fanoutSharePublisherRuns.fetch_add(1, std::memory_order_relaxed);
    }
  }
  ~FanoutShareTiming() {
    if (estimatedNs) {
      uint64_t elapsed = TaskClock{}() - startNs;
      fanoutShareRunNsSum.fetch_add(elapsed, std::memory_order_relaxed);
      fanoutShareEstimatedNsSum.fetch_add(estimatedNs, std::memory_order_relaxed);
    }
  }
};

// Construct owned children on the caller before handing the share to TBB.
template <typename MakeTask>
void publishFanoutShare(FanoutScope* tg, size_t begin, size_t end,
                        uint64_t estimatedNs, MakeTask& makeTask) {
  using Task = std::decay_t<std::invoke_result_t<MakeTask&, size_t>>;
  auto publish = [&](auto task) {
    tg->spawn([task = std::move(task), publishedNs = TaskClock{}(),
             publisher = std::this_thread::get_id(), estimatedNs] {
      FanoutShareTiming timing(publishedNs, publisher, estimatedNs);
      LoadScope load(LoadKind::SHARE);
      FanoutCreditScope credit;
      task();
    });
  };
  if (begin + 1 == end) {
    publish(makeTask(begin));
  } else {
    std::vector<Task> tasks;
    tasks.reserve(end - begin);
    for (size_t i = begin; i < end; i++) tasks.push_back(makeTask(i));
    publish([tasks = std::move(tasks), tg] {
      for (size_t i = tasks.size(); i-- > 0;) {
        if (tg->canceled()) break;
        tasks[i]();
      }
    });
  }
  fanoutPublishedShares.fetch_add(1, std::memory_order_relaxed);
  if (countTaskSpawnsForTests.load(std::memory_order_relaxed)) {
    taskSpawnsForTests.fetch_add(1, std::memory_order_relaxed);
  }
}

}

struct FanoutSize { uint64_t units = 0; uint64_t steal = 0; };

enum class FanoutKind {
  EXHAUSTIVE,  // Order-free; the last child is the cheapest probe.
  PRUNED,      // Ranked, with a shared competitive bound; worst to best.
  SERIAL       // Caller order under a grain; no publication.
};

// True when the sized fan-out starts the caller at the last child.
inline bool fanoutCallerStartsAtBack(const FanoutScope* tg) {
  return tg && (tg->publishesEvery() || tg->grainNs() != FANOUT_GRAIN_NEVER);
}

/// Children are listed so the LAST is what the caller should run first.
/// sizeOf(i) supplies relative work and extra steal cost in site-local units,
/// only after credit is earned. makeTask runs on the caller, may read borrowed
/// state, and returns a callable that owns its captures. Returns shares published.
template <typename SizeOf, typename MakeTask>
size_t task_group_fanout(FanoutScope* tg, size_t n, FanoutKind kind,
                         SizeOf&& sizeOf, MakeTask&& makeTask) {
  if (!tg) {
    for (size_t i = 0; i < n; i++) makeTask(i)();
    return 0;
  }
  if (n < 2) {
    if (n) makeTask(0)();
    return 0;
  }
  if (tg->publishesEvery()) {
    tg->enter([&] {
      for (size_t i = 0; i + 1 < n; i++) detail::publishFanoutShare(tg, i, i + 1, 0, makeTask);
    });
    makeTask(n - 1)();
    return n - 1;
  }
  uint64_t grain = tg->grainNs();
  if (grain == FANOUT_GRAIN_NEVER) {
    for (size_t i = 0; i < n; i++) makeTask(i)();
    return 0;
  }
  if (n == 2 || kind == FanoutKind::SERIAL) {
    for (size_t i = n; i-- > 0;) {
      if (tg->canceled()) break;
      makeTask(i)();
    }
    return 0;
  }

  std::optional<FanoutCreditScope> credit;
  if (!fanoutTaskStart) credit.emplace();
  size_t begin = 0, end = n;
  uint64_t mark = TaskClock{}();
  size_t markEnd = n;
  size_t ran = 0, nextCheck = 1, published = 0;
  bool earnedCredit = false;
  boost::container::small_vector<FanoutSize, 16> sizes;
  boost::container::small_vector<TaskWork, 16> costs;
  while (begin < end) {
    if (tg->canceled()) break;
    makeTask(--end)();
    ++ran;
    // Two children left is the last point a share can be published, so it is
    // always checked.
    if (end - begin < 2 || (ran != nextCheck && end - begin != 2)) continue;
    uint64_t now = TaskClock{}();
    uint64_t creditElapsed = now - *fanoutTaskStart;
    // Before credit, sample after 1, 2, 4, ... children. Once earned, keep
    // measuring every child, including after a publish or load denial.
    earnedCredit |= creditElapsed >= grain;
    nextCheck = earnedCredit ? ran + 1 : ran * 2;
    uint64_t dt = now - mark;
    size_t from = markEnd;
    mark = now;
    markEnd = end;
    if (kind == FanoutKind::PRUNED && ran == 1) continue;
    if (creditElapsed < grain) continue;
    if (sizes.empty()) {
      sizes.reserve(n);
      for (size_t i = 0; i < n; i++) sizes.push_back(sizeOf(i));
    }
    __uint128_t units = 0;
    for (size_t i = end; i < from; i++) units += sizes[i].units;
    if (units == 0 || dt == 0) continue;
    fanoutEvaluations.fetch_add(1, std::memory_order_relaxed);
    auto scale = [&](uint64_t value) {
      return (uint64_t)std::min<__uint128_t>(
          (__uint128_t)value * dt / units, UINT64_MAX);
    };
    costs.clear();
    costs.reserve(end - begin);
    for (size_t i = begin; i < end; i++) {
      costs.push_back({scale(sizes[i].units), scale(sizes[i].steal)});
    }
    auto shares = planTaskShares(costs, grain, n);
    if (shares.empty()) continue;
    auto budget = LoadRegistry::fanoutBudget(grain);
    fanoutTaskStart = now;
    if (budget.grainNs == FANOUT_GRAIN_NEVER) {
      fanoutLoadDenials[budget.cls == LoadClass::SIBLINGS_ONLY ? 0 : 1].fetch_add(1, std::memory_order_relaxed);
      continue;
    }
    shares = planTaskShares(costs, budget.grainNs, (size_t)budget.maxPublished + 1);
    if (shares.empty()) continue;
    size_t keep = shares.size() - 1;
    if (kind == FanoutKind::EXHAUSTIVE) {
      for (size_t s = 0; s < shares.size(); s++) if (shares[s].work >= shares[keep].work) keep = s;
    }
    fanoutShareCreditNsSum.fetch_add(creditElapsed, std::memory_order_relaxed);
    fanoutShareCreditCount.fetch_add(1, std::memory_order_relaxed);
    fanoutShareDecisions[std::min(shares.size(), size_t{5}) - 2].fetch_add(1, std::memory_order_relaxed);
    fanoutShareKeptEstimatedNsSum.fetch_add(shares[keep].work, std::memory_order_relaxed);
    uint64_t largestPublished = 0;
    tg->enter([&] {
      for (size_t s = 0; s < shares.size(); s++) {
        if (s == keep) continue;
        largestPublished = std::max(largestPublished, shares[s].work);
        detail::publishFanoutShare(tg, begin + shares[s].begin, begin + shares[s].end, shares[s].work, makeTask);
        ++published;
      }
    });
    fanoutShareLargestPublishedEstimatedNsSum.fetch_add(largestPublished, std::memory_order_relaxed);
    size_t newBegin = begin + shares[keep].begin;
    end = begin + shares[keep].end;
    begin = newBegin;
    mark = now;
    markEnd = end;
  }
  return published;
}

/// Unsized sites publish only under EVERY; all others run forward.
template <typename MakeTask>
size_t task_group_fanout(FanoutScope* tg, size_t n, MakeTask&& makeTask) {
  if (!tg || n < 2 || !tg->publishesEvery()) {
    for (size_t i = 0; i < n; i++) makeTask(i)();
    return 0;
  }
  tg->enter([&] {
    for (size_t i = 0; i + 1 < n; i++) detail::publishFanoutShare(tg, i, i + 1, 0, makeTask);
  });
  makeTask(n - 1)();
  return n - 1;
}


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
