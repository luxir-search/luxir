// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <map>
#include <mutex>
#include <string_view>
#include "oneapi/tbb/task_arena.h"
#include "oneapi/tbb/task_scheduler_observer.h"

namespace luxir {

// Raises a pool worker's nice by `increment` while it runs a background
// arena's work and restores it when the worker leaves the arena.  Workers move
// between arenas, and inline HTTP searches never use the pool, so arena
// priority alone does not keep background work off their CPUs.  Only workers:
// a thread that enters through execute() is a transport or closing thread.
// Construct only with an increment resolve() returned, so the restore is known
// to be permitted.  Workers still in the arena when it is destroyed get no exit
// callback, and the pool outlives the node, so the destructor restores them.
// The indexing arena uses one; any other background arena can add its own.
class ArenaNice : public oneapi::tbb::task_scheduler_observer {
  int increment;
  std::mutex mutex;
  std::map<long, int> niced;  // thread id -> nice before entry

public:
  ArenaNice(oneapi::tbb::task_arena& arena, int increment)
    : oneapi::tbb::task_scheduler_observer(arena), increment(increment) {
    observe(true);
  }
  ~ArenaNice() override;
  void on_scheduler_entry(bool worker) override;
  void on_scheduler_exit(bool worker) override;

  // The increment to apply for a configured value (`option` names it in
  // logs): 0 = off, -1 = `autoIncrement`, else the value; 0 when a thread
  // cannot take it and return to its base afterwards.  Lowering priority is
  // always allowed; raising it back needs CAP_SYS_NICE or a high enough
  // RLIMIT_NICE.  Probed on a scratch thread, so no pool worker is ever left
  // niced.  Linux only.
  static int resolve(int configured, int autoIncrement, std::string_view option);
};

}
