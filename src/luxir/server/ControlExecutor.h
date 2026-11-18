// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace luxir {

// The node's control plane: administration (collection lifecycle, schema
// changes), replication transfers and timers, plugin routes and expensive
// stats run here, FIFO, on a few dedicated OS threads (server.control_threads).
//
// Not a TBB arena on purpose. Control work blocks: fsync, catalog writes, a
// writer's close() draining its update graph, a replication file read. In an
// arena, a blocked task holds one of the global pool's workers, so the search
// arena (whose workers come from the same pool) would lose it, and a control
// arena large enough not to queue behind such a block would take more. These
// threads are outside the pool: they never take a worker from searches or
// indexing, and searches never delay them. A control thread that enters an
// arena (a writer's close() waiting on its graph, a plugin route running a
// search) uses one of the seats the node reserves for it, as a transport
// thread does.
class ControlExecutor {
public:
  struct Stats {
    uint64_t threads = 0;
    uint64_t busy = 0;       // tasks running now
    uint64_t queued = 0;     // tasks waiting for a thread
    uint64_t completed = 0;  // tasks finished, including those that threw
  };

private:
  mutable std::mutex mutex;
  std::condition_variable wake;
  std::deque<std::move_only_function<void()>> tasks;
  bool stopping = false;
  int width = 0;
  int live = 0;  // threads not yet exited
  std::atomic<uint64_t> busy{0};
  std::atomic<uint64_t> completed{0};
  std::vector<std::thread> threads;

  void push(std::move_only_function<void()> run);
  void runThread(int index);

public:
  explicit ControlExecutor(int n);  // threads, at least one
  ~ControlExecutor() { stop(); }
  ControlExecutor(const ControlExecutor&) = delete;
  ControlExecutor& operator=(const ControlExecutor&) = delete;

  // Queues f; it runs on a control thread. A throwing task is logged. Tasks
  // queued before or during stop() still run; enqueue once stop() has
  // drained the queue throws std::runtime_error.
  template <class F>
  void enqueue(F&& f) { push(std::move_only_function<void()>(std::forward<F>(f))); }

  // Runs every queued task, including ones they queue, then joins.
  void stop();
  Stats stats() const;
};

}  // namespace luxir
