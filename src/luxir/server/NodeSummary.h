// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "luxir/util/Metrics.h"

namespace luxir {

class LuxirNode;

// Logs one INFO line per interval with what the node did since the previous
// line: search rate and latency, ingest rate and rejections, background
// stage counts and times, indexing RAM, RSS, and host pressure (PSI, cgroup
// CPU throttling). Reads only the process-wide counters and a few /proc and
// cgroup files, so it costs nothing on the request path.
class NodeSummary {
  LuxirNode& node;
  const std::chrono::seconds interval;
  std::mutex mutex;
  std::condition_variable wake;
  bool stopping = false;  // mutex
  MetricCounts previous{};
  std::chrono::steady_clock::time_point previousAt;
  std::optional<uint64_t> previousThrottledMicros;
  std::thread thread;

  void run();

public:
  NodeSummary(LuxirNode& node, std::chrono::seconds interval);
  ~NodeSummary();
  NodeSummary(const NodeSummary&) = delete;
  NodeSummary& operator=(const NodeSummary&) = delete;

  // The line for the counts accumulated since the previous call (exposed for tests).
  std::string report();
};

} // namespace luxir
