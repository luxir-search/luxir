// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "ControlExecutor.h"

#include <algorithm>
#include <cstdio>
#include <stdexcept>

#include "luxir/util/log.h"
#include "luxir/util/thread.h"

namespace luxir {

ControlExecutor::ControlExecutor(int n) {
  n = std::max(1, n);
  width = n;
  live = n;
  threads.reserve((size_t)n);
  for (int i = 0; i < n; i++) threads.emplace_back([this, i] { runThread(i); });
}

void ControlExecutor::push(std::move_only_function<void()> run) {
  {
    std::lock_guard lock(mutex);
    if (live == 0) throw std::runtime_error("control executor is stopped");
    tasks.push_back(std::move(run));
  }
  wake.notify_one();
}

void ControlExecutor::runThread(int index) {
  char name[16];
  snprintf(name, sizeof(name), "luxir_ctl_%d", index);
  nameThisThread(name);
  std::unique_lock lock(mutex);
  for (;;) {
    wake.wait(lock, [&] { return stopping || !tasks.empty(); });
    if (tasks.empty()) {  // stopping, and the queue is drained
      live--;
      return;
    }
    auto task = std::move(tasks.front());
    tasks.pop_front();
    busy.fetch_add(1, std::memory_order_relaxed);
    lock.unlock();
    try {
      task();
    } catch (const std::exception& e) {
      LOG_ERROR("control task failed: {}", e.what());
    } catch (...) {
      LOG_ERROR("control task failed: unknown exception");
    }
    // Destroy the task's captures before reporting it done.
    task = nullptr;
    completed.fetch_add(1, std::memory_order_relaxed);
    busy.fetch_sub(1, std::memory_order_relaxed);
    lock.lock();
  }
}

void ControlExecutor::stop() {
  {
    std::lock_guard lock(mutex);
    if (stopping) return;
    stopping = true;
  }
  wake.notify_all();
  // A thread exits only on an empty queue, and enqueue refuses only once
  // none is left, so a task queued meanwhile (by a running task or a writer's
  // completion) always runs.
  for (auto& thread : threads) thread.join();
}

ControlExecutor::Stats ControlExecutor::stats() const {
  Stats out;
  out.threads = (uint64_t)width;
  {
    std::lock_guard lock(mutex);
    out.queued = tasks.size();
  }
  out.busy = busy.load(std::memory_order_relaxed);
  out.completed = completed.load(std::memory_order_relaxed);
  return out;
}

}  // namespace luxir
