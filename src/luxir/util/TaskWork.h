// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <boost/container/small_vector.hpp>

namespace luxir {

inline constexpr uint64_t FANOUT_GRAIN_NEVER = UINT64_MAX;
// Credit and minimum share in nanoseconds; NEVER disables adaptive publication.
inline std::atomic<uint64_t> fanoutGrainNs{50'000};
inline std::atomic<uint64_t> fanoutSiblingGrainNs{1'000'000};
// Process counters reserved for fan-out measurements.
inline std::atomic<uint64_t> fanoutEvaluations{0};
inline std::atomic<uint64_t> fanoutPublishedShares{0};
inline std::atomic<uint64_t> fanoutLoadDenials[2]{};  // Siblings only, saturated.
inline std::atomic<uint64_t> fanoutShareStartDelayNsSum{0};
inline std::atomic<uint64_t> fanoutShareStartDelayCount{0};
inline std::atomic<uint64_t> fanoutShareStartDelayNsMax{0};
inline std::atomic<uint64_t> fanoutShareRunNsSum{0};
inline std::atomic<uint64_t> fanoutShareEstimatedNsSum{0};
inline std::atomic<uint64_t> fanoutSharePublisherRuns{0};
inline std::atomic<uint64_t> fanoutShareCreditNsSum{0};
inline std::atomic<uint64_t> fanoutShareCreditCount{0};
inline std::atomic<uint64_t> fanoutShareDecisions[4]{};  // 2, 3, 4, 5+ shares, including the caller.
inline std::atomic<uint64_t> fanoutShareKeptEstimatedNsSum{0};
inline std::atomic<uint64_t> fanoutShareLargestPublishedEstimatedNsSum{0};
inline std::atomic<uint64_t> fanoutShareWaitNsSum{0};
inline std::atomic<uint64_t> fanoutShareWaitCount{0};

struct TaskWork {
  uint64_t work = 0;
  uint64_t steal = 0;
  TaskWork(uint64_t work = 0, uint64_t steal = 0) : work(work), steal(steal) {}
};

inline uint64_t estimateTaskWork(uint64_t units, uint64_t nsPerUnit) {
  return nsPerUnit && units > std::numeric_limits<uint64_t>::max() / nsPerUnit
      ? std::numeric_limits<uint64_t>::max() : units * nsPerUnit;
}

inline uint64_t taskWorkAdd(uint64_t a, uint64_t b) {
  return a + std::min(b, std::numeric_limits<uint64_t>::max() - a);
}

struct TaskShare { size_t begin, end; uint64_t work, steal; };
using TaskShares = boost::container::small_vector<TaskShare, 16>;

// Reserve shares from the end, rounding to the nearer child boundary.
// Every share must cover the minimum plus the extra accumulator and merge a
// stolen share can need: any share can end up on a worker.
// Merge shares that miss those floors into their neighbor in one pass.
// Keep the caller above half the final average, even with unequal children.
inline TaskShares planTaskShares(std::span<const TaskWork> costs, uint64_t minimum, size_t maxShares) {
  __uint128_t total = 0;
  for (auto cost : costs) total += cost.work;
  size_t k = minimum ? (size_t)std::min<__uint128_t>(costs.size(), total / minimum) : costs.size();
  k = std::min(k, maxShares);
  if (k >= 2) {
    TaskShares shares(k);
    size_t end = costs.size();
    __uint128_t remaining = total;
    for (size_t part = k; part-- > 0;) {
      size_t begin = part ? end - 1 : 0;
      __uint128_t work = 0;
      uint64_t steal = 0;
      for (size_t i = begin; i < end; i++) {
        work += costs[i].work;
        steal = std::max(steal, costs[i].steal);
      }
      __uint128_t target = remaining / (part + 1);
      while (part && begin > part && work < target) {
        __uint128_t next = work + costs[begin-1].work;
        if (next > target && next - target > target - work) break;
        work = next;
        steal = std::max(steal, costs[--begin].steal);
      }
      shares[part] = {begin, end, (uint64_t)std::min<__uint128_t>(work, std::numeric_limits<uint64_t>::max()), steal};
      remaining -= work;
      end = begin;
    }
    size_t out = 0;
    TaskShare carry = shares[0];
    for (size_t i = 1; i < shares.size(); i++) {
      if (carry.work >= taskWorkAdd(minimum, carry.steal)) {
        shares[out++] = carry;
        carry = shares[i];
      } else {
        carry.end = shares[i].end;
        carry.work = taskWorkAdd(carry.work, shares[i].work);
        carry.steal = std::max(carry.steal, shares[i].steal);
      }
    }
    while (out && carry.work < std::max<__uint128_t>(taskWorkAdd(minimum, carry.steal), total / (2 * (out + 1)))) {
      const auto& previous = shares[--out];
      carry.begin = previous.begin;
      carry.work = taskWorkAdd(carry.work, previous.work);
      carry.steal = std::max(carry.steal, previous.steal);
    }
    shares[out++] = carry;
    shares.resize(out);
    if (out >= 2) return shares;
  }
  return {};
}

inline std::atomic<int64_t> taskClockNsForTests{-1};
inline std::atomic<uint64_t> taskClockReadsForTests{0};

struct TaskClock {
  uint64_t operator()() const {
    int64_t testTime = taskClockNsForTests.load(std::memory_order_relaxed);
    if (testTime >= 0) {
      taskClockReadsForTests.fetch_add(1, std::memory_order_relaxed);
      return (uint64_t)testTime;
    }
    return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
  }
};

// Start of this thread's credited inline work, absent outside a task scope.
inline thread_local std::optional<uint64_t> fanoutTaskStart;
class FanoutCreditScope {
  std::optional<uint64_t> previous;
  explicit FanoutCreditScope(std::optional<uint64_t> startNs) : previous(fanoutTaskStart) {
    uint64_t grain = fanoutGrainNs.load(std::memory_order_relaxed);
    if (grain == FANOUT_GRAIN_NEVER) fanoutTaskStart.reset();
    else fanoutTaskStart = startNs ? *startNs : TaskClock{}();
  }
public:
  FanoutCreditScope() : FanoutCreditScope(std::nullopt) {}
  explicit FanoutCreditScope(uint64_t startNs) : FanoutCreditScope(std::optional<uint64_t>{startNs}) {}
  ~FanoutCreditScope() { fanoutTaskStart = previous; }
  FanoutCreditScope(const FanoutCreditScope&) = delete;
  FanoutCreditScope& operator=(const FanoutCreditScope&) = delete;
};

}
