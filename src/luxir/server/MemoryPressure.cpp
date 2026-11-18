// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "MemoryPressure.h"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <fstream>
#include <string>

#include <fmt/format.h>

#include "Stats.h"
#include "luxir/LuxirConfig.h"
#include "luxir/index/IndexRamBudget.h"
#include "luxir/util/log.h"

namespace luxir {
namespace {

constexpr int64_t MiB = 1024 * 1024;
// Below this the indexing reservations are not worth a flush.
constexpr int64_t FLUSH_HINT_MIN_BYTES = 4 * MiB;

// One "key value" field of a procfs or cgroupfs file; nullopt when missing.
std::optional<uint64_t> field(const std::filesystem::path& path, std::string_view key) {
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.starts_with(key)) continue;
    std::string_view rest = std::string_view(line).substr(key.size());
    if (rest.starts_with(':')) rest.remove_prefix(1);
    if (rest.empty() || (rest[0] != ' ' && rest[0] != '\t')) continue;
    return std::strtoull(std::string(rest).c_str(), nullptr, 10);
  }
  return std::nullopt;
}

// A one-number cgroup file; nullopt for "max" or when unreadable.
std::optional<uint64_t> scalar(const std::filesystem::path& path) {
  std::ifstream in(path);
  uint64_t value;
  if (in >> value) return value;
  return std::nullopt;
}

// The cgroup whose memory.max binds this process: the smallest limit on the
// path from its own cgroup to the mount root. nullopt without one.
std::optional<std::filesystem::path> limitingCgroup() {
#ifdef __linux__
  std::filesystem::path dir;
  try { dir = cgroupV2Directory(); } catch (const std::exception&) { return std::nullopt; }
  std::optional<std::filesystem::path> best;
  uint64_t bestLimit = 0;
  for (auto path = dir; ; path = path.parent_path()) {
    if (!std::filesystem::exists(path / "memory.current")) break;
    if (auto limit = scalar(path / "memory.max"); limit && (!best || *limit < bestLimit)) {
      best = path;
      bestLimit = *limit;
    }
    if (path == path.parent_path()) break;
  }
  return best;
#else
  return std::nullopt;
#endif
}

}  // namespace

MemoryPressure::MemoryPressure(const ServerConfig& config, IndexRamBudget& indexRamBudget, int64_t indexRamBytes)
    : indexRamBudget(indexRamBudget), indexRamBytes(indexRamBytes),
      softRatio(config.memory_pressure_soft_percent / 100.0),
      hardRatio(config.memory_pressure_hard_percent / 100.0),
      hysteresisRatio(config.memory_pressure_hysteresis_percent / 100.0),
      configuredLimitBytes((uint64_t)std::max<int64_t>(0, config.memory_limit_mb) * MiB),
      interval(config.memory_pressure_sample_ms),
      slot([this] { tick(); }) {
  if (interval.count() <= 0) return;
  cgroup = limitingCgroup();
  tick();
}

MemoryPressure::~MemoryPressure() {
  DeadlineScheduler::global().detach(slot);
  // Leave the indexing budget as configured for whoever outlives the monitor.
  const std::lock_guard<std::mutex> lock(mutex);
  if (flushHinted) indexRamBudget.setTotalBytes(indexRamBytes);
}

MemoryPressure::State MemoryPressure::next(State previous, double ratio, double soft, double hard,
                                           double hysteresis) {
  if (ratio >= hard || (previous == State::HARD && ratio >= hard - hysteresis)) return State::HARD;
  if (ratio >= soft || (previous != State::NORMAL && ratio >= soft - hysteresis)) return State::SOFT;
  return State::NORMAL;
}

std::string_view MemoryPressure::name(State s) {
  switch (s) {
    case State::NORMAL: return "normal";
    case State::SOFT: return "soft";
    case State::HARD: return "hard";
  }
  return "normal";
}

std::string_view MemoryPressure::name(Source s) {
  switch (s) {
    case Source::NONE: return "none";
    case Source::CGROUP: return "cgroup";
    case Source::PROCESS: return "process";
    case Source::INJECTED: return "injected";
  }
  return "none";
}

std::string_view MemoryPressure::name(Work w) {
  switch (w) {
    case Work::UPDATE: return "update";
    case Work::UPDATE_BATCH: return "update_batch";
    case Work::SEARCH: return "search";
  }
  return "update";
}

std::optional<MemoryPressure::Sample> MemoryPressure::read() {
  if (cgroup) {
    auto current = scalar(*cgroup / "memory.current");
    auto limit = scalar(*cgroup / "memory.max");
    auto active = field(*cgroup / "memory.stat", "active_file");
    auto inactive = field(*cgroup / "memory.stat", "inactive_file");
    if (current && limit && active && inactive) {
      uint64_t file = *active + *inactive;
      uint64_t cap = configuredLimitBytes ? std::min(*limit, configuredLimitBytes) : *limit;
      source = Source::CGROUP;
      return Sample{*current > file ? *current - file : 0, cap};
    }
  }
#ifdef __linux__
  auto anonKb = field("/proc/self/status", "RssAnon");
  if (!anonKb) return std::nullopt;
  uint64_t cap = configuredLimitBytes ? configuredLimitBytes : (uint64_t)std::max<int64_t>(0, systemRamBytes());
  if (cap == 0) return std::nullopt;
  source = Source::PROCESS;
  return Sample{*anonKb * 1024, cap};
#else
  return std::nullopt;
#endif
}

void MemoryPressure::tick() noexcept {
  try {
    const std::lock_guard<std::mutex> lock(mutex);
    if (injected) {
      evaluateLocked(*injected);
    } else if (auto sample = read()) {
      evaluateLocked(*sample);
    }
  } catch (const std::exception& e) {
    LOG_ERROR("memory pressure sample failed: {}", e.what());
  }
  DeadlineScheduler::global().arm(slot, DeadlineScheduler::Clock::now() + interval);
}

void MemoryPressure::evaluateLocked(Sample sample) {
  last = sample;
  double ratio = sample.limitBytes ? (double)sample.usedBytes / (double)sample.limitBytes : 0;
  State previous = current();
  State s = next(previous, ratio, softRatio, hardRatio, hysteresisRatio);
  if (s != previous) {
    state.store(s, std::memory_order_relaxed);
    transitions.fetch_add(1, std::memory_order_relaxed);
    LOG_WARN("memory pressure {} -> {}: {} of {} bytes in use ({:.1f}%, source {})", name(previous), name(s),
             sample.usedBytes, sample.limitBytes, ratio * 100, name(injected ? Source::INJECTED : source));
  }
  // Under pressure, ask indexing to flush: halving the shared indexing cap
  // below what is reserved wakes the writers' pressure listeners, which flush
  // their idle inverters on the index arena. Nothing here waits for them.
  // While the hint stands, the lowered cap also slows new indexing work.
  if (s != State::NORMAL && !flushHinted) {
    int64_t reserved = indexRamBudget.reservedBytes();
    if (reserved >= FLUSH_HINT_MIN_BYTES) {
      int64_t cap = reserved / 2;
      if (indexRamBytes > 0) cap = std::min(cap, indexRamBytes);
      indexRamBudget.setTotalBytes(cap);
      flushHinted = true;
    }
  } else if (s == State::NORMAL && flushHinted) {
    indexRamBudget.setTotalBytes(indexRamBytes);
    flushHinted = false;
  }
}

ErrorInfo MemoryPressure::reject(Work work, State s) {
  rejectedCounts[(size_t)work][(size_t)s].fetch_add(1, std::memory_order_relaxed);
  return {ErrorKind::RESOURCE_EXHAUSTED, "memory_pressure",
          fmt::format("node is under {} memory pressure; {} rejected, retry later", name(s), name(work))};
}

MemoryPressure::Snapshot MemoryPressure::snapshot() const {
  Snapshot out{};
  {
    const std::lock_guard<std::mutex> lock(mutex);
    out.source = injected ? Source::INJECTED : source;
    out.sample = last;
  }
  out.state = current();
  out.transitions = transitions.load(std::memory_order_relaxed);
  for (size_t w = 0; w < out.rejected.size(); w++)
    for (size_t s = 0; s < out.rejected[w].size(); s++)
      out.rejected[w][s] = rejectedCounts[w][s].load(std::memory_order_relaxed);
  return out;
}

void MemoryPressure::injectSampleForTests(std::optional<Sample> sample) {
  const std::lock_guard<std::mutex> lock(mutex);
  injected = sample;
  if (sample) evaluateLocked(*sample);
}

int retryAfterSeconds(std::string_view code) {
  return code == "memory_pressure" || code == "node_request_memory_exceeded" ? 1 : 0;
}

}  // namespace luxir
