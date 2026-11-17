// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "luxir/util/thread.h"

namespace luxir::test {

class FanoutGrainGuard {
  uint64_t saved;
  bool savedLoad;
public:
  explicit FanoutGrainGuard(uint64_t ns)
      : saved(fanoutGrainNs.exchange(ns, std::memory_order_relaxed)),
        savedLoad(LoadRegistry::enabled.exchange(ns != FANOUT_GRAIN_NEVER)) {}
  ~FanoutGrainGuard() {
    fanoutGrainNs.store(saved, std::memory_order_relaxed);
    LoadRegistry::enabled.store(savedLoad);
  }
};

class TaskClockGuard {
  int64_t saved;
public:
  explicit TaskClockGuard(int64_t now)
      : saved(taskClockNsForTests.exchange(now, std::memory_order_relaxed)) {}
  ~TaskClockGuard() { taskClockNsForTests.store(saved, std::memory_order_relaxed); }
};

class FanoutLoadGuard {
  // A real idle registry keeps publication tests independent of machine load.
  LoadRegistry registry{{256, 256}};
  LoadRegistry::Registration defaultOwner{registry};
  bool savedEnabled;
  LoadRegistry::Registration* savedOwner;
  LoadSlot* savedSlot;
  uint64_t savedSibling;
public:
  explicit FanoutLoadGuard(LoadRegistry::Registration* owner = nullptr,
                           uint64_t sibling = FANOUT_GRAIN_NEVER)
      : savedEnabled(LoadRegistry::enabled.exchange(true)),
        savedOwner(std::exchange(LoadRegistry::localForTests, owner ? owner : &defaultOwner)),
        savedSlot(std::exchange(LoadRegistry::localSlot, nullptr)),
        savedSibling(fanoutSiblingGrainNs.exchange(sibling)) {}
  ~FanoutLoadGuard() {
    LoadRegistry::enabled.store(savedEnabled);
    LoadRegistry::localForTests = savedOwner;
    LoadRegistry::localSlot = savedSlot;
    fanoutSiblingGrainNs.store(savedSibling);
  }
};

class TaskSpawnCounter {
  bool saved = countTaskSpawnsForTests.exchange(true, std::memory_order_relaxed);
  int64_t before = taskSpawnsForTests.load(std::memory_order_relaxed);

public:
  int64_t count() const {
    return taskSpawnsForTests.load(std::memory_order_relaxed) - before;
  }
  ~TaskSpawnCounter() {
    countTaskSpawnsForTests.store(saved, std::memory_order_relaxed);
  }
};

}
