// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>

namespace luxir {

class LuxirNode;

// Where a max_parallel 0 request runs. PROBE starts serially and fans the
// remaining segments out once the request outlasts its budget.
enum class Lane : uint8_t { SERIAL, PARALLEL, PROBE };
enum class LaneReason : uint8_t {
  BELOW_SAVING,      // fanning out would not save enough time
  NO_SPARE_WORKERS,  // it would, but other searches hold the workers
  SAVING,
  UNKNOWN_WORK,      // pruning decides the cost; execution will tell
  POLICY,            // a policy's own reason
};

// What the engine knows about a request once it is planned.
struct LaneFacts {
  std::string_view tenant = {};
  std::string_view collection = {};
  double requiredUnits = 0;
  double largestRequiredUnits = 0;
  double prunableUnits = 0;
  double largestPrunableUnits = 0;
  int viableSegments = 0;  // segments with any work
  int workers = 0;
  int spareWorkers = 0;  // workers not claimed by other searches
};

struct LaneDecision {
  Lane lane = Lane::SERIAL;
  LaneReason reason = LaneReason::POLICY;
  uint32_t probeBudgetUs = 0;  // PROBE only
  bool observe = false;        // report this request to LanePolicy::observe
};

struct LaneOutcome {
  uint64_t wallNanos = 0;
  uint64_t cpuNanos = 0;  // the executing thread's CPU; complete only when serial
  bool escalated = false;
};

// Chooses the lane of max_parallel 0 requests. One instance per node, selected
// by search.lane-policy; decide and observe run concurrently on search
// threads, on the request path, and must not block. Lanes never change a
// request's matches, order, scores or exact counts.
class LanePolicy {
public:
  virtual ~LanePolicy() = default;
  // False skips pricing requests entirely.
  virtual bool active() const { return true; }
  virtual LaneDecision decide(const LaneFacts& facts) = 0;
  virtual void observe(const LaneFacts&, const LaneDecision&, const LaneOutcome&) {}
  // Current nanoseconds per work unit, for stats; 0 when not applicable.
  virtual double nsPerUnit() const { return 0; }
};

class LanePolicyRegistry {
public:
  using Maker = std::function<std::unique_ptr<LanePolicy>(LuxirNode&)>;

private:
  std::map<std::string, Maker, std::less<>> makers;
  bool frozen = false;
  LanePolicyRegistry();

public:
  static LanePolicyRegistry& global();
  void freeze() { frozen = true; }
  void add(std::string name, Maker maker);
  std::unique_ptr<LanePolicy> make(std::string_view name, LuxirNode& node) const;
};

}  // namespace luxir
