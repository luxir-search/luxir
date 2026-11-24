// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string_view>

#include "luxir/util/ApiError.h"
#include "luxir/util/DeadlineScheduler.h"

namespace luxir {

class IndexRamBudget;
struct ServerConfig;

// Node-wide memory pressure: a periodic sampler compares the memory the
// kernel cannot reclaim against the limit it enforces, and admission reads the
// resulting state with one relaxed load. Two thresholds with hysteresis:
//   SOFT  rejects new update requests and asks indexing to flush;
//   HARD  also rejects new searches and further batches of running update
//         streams.
// Accepted work is never cut: running searches, paused streaming production
// and their resumes continue. Control-plane calls are never checked.
//
// The measure excludes page cache, so mapped segment files never count:
//   under a cgroup v2 memory limit (the tightest one on the path to the root),
//     used = memory.current - active_file - inactive_file of that cgroup,
//     which is what the cgroup's OOM killer acts on;
//   otherwise used = this process's RssAnon (/proc/self/status), against
//     server.memory-limit-mb or system RAM.
// Allocator statistics are not used: they miss memory allocated outside
// jemalloc and overstate what dirty pages awaiting decay cost the kernel.
class MemoryPressure {
public:
  enum class State : uint8_t { NORMAL, SOFT, HARD };
  // What is being admitted: a new update request (HTTP buffered or NDJSON,
  // unary gRPC Update, the first message of an UpdateStream), a further batch
  // of an admitted update stream, or a new search.
  enum class Work : uint8_t { UPDATE, UPDATE_BATCH, SEARCH };
  enum class Source : uint8_t { NONE, CGROUP, PROCESS, INJECTED };
  struct Sample {
    uint64_t usedBytes = 0;
    uint64_t limitBytes = 0;
  };
  struct Snapshot {
    State state;
    Source source;
    Sample sample;
    uint64_t transitions;
    // Rejections by work and by the state that refused it.
    std::array<std::array<uint64_t, 3>, 3> rejected;
  };

private:
  std::atomic<State> state{State::NORMAL};
  std::atomic<uint64_t> transitions{0};
  std::array<std::array<std::atomic<uint64_t>, 3>, 3> rejectedCounts{};
  IndexRamBudget& indexRamBudget;
  const int64_t indexRamBytes;  // the configured indexing cap the hint restores
  const double softRatio;
  const double hardRatio;
  const double hysteresisRatio;
  const uint64_t configuredLimitBytes;
  const std::chrono::milliseconds interval;
  // Serializes evaluation (sampler and test injection); never on admission.
  mutable std::mutex mutex;
  Source source = Source::NONE;
  Sample last;
  std::optional<Sample> injected;
  bool flushHinted = false;
  std::optional<std::filesystem::path> cgroup;  // the limiting cgroup, if any
  DeadlineScheduler::Slot slot;

  void tick() noexcept;
  void evaluateLocked(Sample sample);
  std::optional<Sample> read();

public:
  // A zero interval disables sampling: the state stays NORMAL unless a test
  // injects a sample.
  MemoryPressure(const ServerConfig& config, IndexRamBudget& indexRamBudget, int64_t indexRamBytes);
  ~MemoryPressure();
  MemoryPressure(const MemoryPressure&) = delete;
  MemoryPressure& operator=(const MemoryPressure&) = delete;

  // The next state for `ratio` (used / limit) from `previous`: entering a
  // level takes its threshold, leaving it takes the threshold less the
  // hysteresis.
  static State next(State previous, double ratio, double soft, double hard, double hysteresis);
  static std::string_view name(State state);
  static std::string_view name(Source source);
  static std::string_view name(Work work);

  State current() const { return state.load(std::memory_order_relaxed); }
  double softThreshold() const { return softRatio; }
  double hardThreshold() const { return hardRatio; }

  // The memory_pressure error when `work` is refused in the current state.
  // One relaxed load when the node is not under pressure.
  std::optional<ErrorInfo> admit(Work work) {
    State s = current();
    if (s == State::NORMAL) [[likely]] return std::nullopt;
    if (s == State::SOFT && work != Work::UPDATE) return std::nullopt;
    return reject(work, s);
  }
  ErrorInfo reject(Work work, State s);

  Snapshot snapshot() const;

  // Replaces every later sample until reset with nullopt; evaluates at once.
  void injectSampleForTests(std::optional<Sample> sample);
};

// Seconds a client should wait before retrying a request refused for a
// transient node-wide condition, or 0 when the code is not one.
int retryAfterSeconds(std::string_view code);

}  // namespace luxir
