// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stop_token>
#include <thread>

namespace luxir {

class Collections;
struct StoreConfig;

// Re-reads the files of every collection's published snapshot and checks each
// against its manifest size and xxh3. A mismatch is logged and makes that
// collection unavailable (Collection::markCorrupt) until it is reopened. One
// low-priority thread, paced by a byte rate, scrubs every `interval`.
class Scrubber {
public:
  struct Stats {
    uint64_t passes = 0;
    uint64_t files = 0;
    uint64_t bytes = 0;
    uint64_t mismatches = 0;
    uint64_t lastPassEnd = 0; // Unix milliseconds; zero before the first pass
  };

private:
  Collections& collections;
  std::chrono::seconds interval;
  uint64_t bytesPerSecond;
  std::mutex mutex; // serializes passes
  std::condition_variable_any wake;
  std::mutex wakeMutex;
  std::atomic<uint64_t> passes{0}, files{0}, bytes{0}, mismatches{0}, lastPassEnd{0};
  std::jthread thread;

  void run(std::stop_token stop);

public:
  Scrubber(Collections& collections, const StoreConfig& config);
  ~Scrubber();
  // Starts the periodic thread when an interval is configured.
  void start();
  void stop();
  // One full pass over the current snapshots on the calling thread, paced like
  // the background pass. Returns the number of collections found corrupt.
  size_t scrub(std::stop_token stop = {});
  bool enabled() const { return interval.count() > 0; }
  Stats stats() const;
};

}
