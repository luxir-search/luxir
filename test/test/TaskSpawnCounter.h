// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "luxir/util/thread.h"

namespace luxir::test {

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
