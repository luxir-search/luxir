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

class LuxirNode;

// Re-reads the files of every collection's published snapshot and checks each
// against its manifest size and xxh3. Files are read with their own
// descriptors and O_DIRECT where the filesystem allows (never through the
// mmap path), so a pass reads the media rather than the page cache and leaves
// the cache to the queries. A mismatch is logged and fails that collection
// (Collections::fail): a writer stops publishing, the suspect file is
// recorded for the next startup, and a follower repairs the copy in place.
//
// One thread scrubs every `interval`, paced by a byte rate; the rate is the
// real throttle (the thread's nice level only yields CPU, and a scrub is I/O
// bound). An on-demand scrub() preempts a background pass at its next chunk
// and runs instead; the background pass resumes with its next interval.
class Scrubber {
public:
  using Clock = std::chrono::steady_clock;
  struct Stats {
    uint64_t passes = 0;
    uint64_t files = 0;
    uint64_t bytes = 0;
    uint64_t mismatches = 0;
    uint64_t errors = 0; // other failures to check a file; retried next pass
    uint64_t lastPassEnd = 0; // Unix milliseconds; zero before the first pass
  };

private:
  LuxirNode& node;
  std::chrono::seconds interval;
  uint64_t bytesPerSecond;
  std::mutex mutex; // serializes passes
  std::atomic<unsigned> waiting{0}; // on-demand callers waiting for the pass mutex
  std::condition_variable_any wake;
  std::mutex wakeMutex;
  std::atomic<uint64_t> passes{0}, files{0}, bytes{0}, mismatches{0}, errors{0}, lastPassEnd{0};
  std::jthread thread;

  void run(std::stop_token stop);
  size_t pass(std::stop_token stop, bool preemptible);

public:
  explicit Scrubber(LuxirNode& node);
  ~Scrubber();
  // Starts the periodic thread when an interval is configured.
  void start();
  void stop();
  // One full pass over the current snapshots on the calling thread, paced like
  // the background pass; `stop` abandons it. Returns the number of collections
  // found corrupt.
  size_t scrub(std::stop_token stop = {});
  bool enabled() const { return interval.count() > 0; }
  Stats stats() const;
  // Pure pacing step: a slow read earns no credit against later chunks.
  static Clock::time_point nextDeadline(Clock::time_point due, Clock::time_point now,
                                        size_t length, uint64_t bytesPerSecond);
};

}
