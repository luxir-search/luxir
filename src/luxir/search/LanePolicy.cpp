// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "LanePolicy.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <stdexcept>

#include "luxir/server/LuxirNode.h"

namespace luxir {

namespace {

// Time a fan-out costs before it saves any, from waking workers and merging
// per-segment results.
constexpr double FANOUT_OVERHEAD_US = 150.0;
// Calibration samples serial requests priced above this, one in eight.
constexpr double MIN_SAMPLE_UNITS = 50000.0;
constexpr uint32_t SAMPLE_MASK = 7;
// Sums decay per sample, so large requests outweigh small ones whose fixed
// costs would bias the ratio. The prior keeps the first samples from swinging it.
constexpr double SAMPLE_DECAY = 0.98;
constexpr double PRIOR_UNITS = 1e7;
constexpr double MIN_NS_PER_UNIT = 0.25;
constexpr double MAX_NS_PER_UNIT = 4.0;

// Fans out when the predicted wall-clock saving reaches the budget. A
// request's largest segment bounds what parallelism can do for it.
class CostLanePolicy final : public LanePolicy {
  double minSavingUs;
  double fixedNsPerUnit;  // 0: calibrate from observed requests
  std::atomic<double> currentNsPerUnit;
  std::atomic<uint32_t> samples{0};
  std::mutex calibration;
  double sampledNanos = PRIOR_UNITS;
  double sampledUnits = PRIOR_UNITS;

  double saving(double units, double largest, int width, double ns) const {
    double serial = units * ns / 1000.0;
    return serial - (std::max(largest * ns / 1000.0, serial / width) + FANOUT_OVERHEAD_US);
  }

public:
  explicit CostLanePolicy(const SearchConfig& config)
    : minSavingUs((double)config.auto_parallel_min_saving_us),
      fixedNsPerUnit(config.auto_parallel_ns_per_unit),
      currentNsPerUnit(fixedNsPerUnit > 0 ? fixedNsPerUnit : 1.0) {}

  bool active() const override { return minSavingUs > 0; }
  double nsPerUnit() const override { return currentNsPerUnit.load(std::memory_order_relaxed); }

  LaneDecision decide(const LaneFacts& facts) override {
    double ns = nsPerUnit();
    int fullWidth = std::min(facts.viableSegments, facts.workers);
    if (fullWidth >= 2
        && saving(facts.requiredUnits, facts.largestRequiredUnits, fullWidth, ns) >= minSavingUs) {
      int width = std::min(facts.viableSegments, facts.spareWorkers);
      if (width >= 2
          && saving(facts.requiredUnits, facts.largestRequiredUnits, width, ns) >= minSavingUs) {
        return {Lane::PARALLEL, LaneReason::SAVING};
      }
      return serial(facts, LaneReason::NO_SPARE_WORKERS);
    }
    double worstUnits = facts.requiredUnits + facts.prunableUnits;
    double worstLargest = std::max(facts.largestRequiredUnits, facts.largestPrunableUnits);
    if (facts.prunableUnits > 0 && fullWidth >= 2
        && saving(worstUnits, worstLargest, fullWidth, ns) >= minSavingUs) {
      return {Lane::PROBE, LaneReason::UNKNOWN_WORK, (uint32_t)minSavingUs};
    }
    return serial(facts, LaneReason::BELOW_SAVING);
  }

  void observe(const LaneFacts& facts, const LaneDecision&, const LaneOutcome& outcome) override {
    std::lock_guard lock(calibration);
    sampledNanos = sampledNanos * SAMPLE_DECAY + (double)outcome.cpuNanos;
    sampledUnits = sampledUnits * SAMPLE_DECAY + facts.requiredUnits;
    currentNsPerUnit.store(std::clamp(sampledNanos / sampledUnits, MIN_NS_PER_UNIT, MAX_NS_PER_UNIT),
                           std::memory_order_relaxed);
  }

private:
  LaneDecision serial(const LaneFacts& facts, LaneReason reason) {
    LaneDecision decision{Lane::SERIAL, reason};
    decision.observe = fixedNsPerUnit <= 0 && facts.prunableUnits == 0
        && facts.requiredUnits >= MIN_SAMPLE_UNITS
        && (samples.fetch_add(1, std::memory_order_relaxed) & SAMPLE_MASK) == 0;
    return decision;
  }
};

// Every eligible request fans out. For tests and A/B baselines.
class AlwaysLanePolicy final : public LanePolicy {
public:
  LaneDecision decide(const LaneFacts&) override { return {Lane::PARALLEL, LaneReason::POLICY}; }
};

}  // namespace

LanePolicyRegistry::LanePolicyRegistry() {
  add("cost", [](LuxirNode& node) { return std::make_unique<CostLanePolicy>(node.getConfig().search); });
  add("always", [](LuxirNode&) { return std::make_unique<AlwaysLanePolicy>(); });
}

LanePolicyRegistry& LanePolicyRegistry::global() {
  static LanePolicyRegistry registry;
  return registry;
}

void LanePolicyRegistry::add(std::string name, Maker maker) {
  if (frozen) throw std::logic_error("lane policy registry is frozen");
  if (name.empty() || name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") != std::string::npos)
    throw std::invalid_argument("invalid lane policy name: " + name);
  if (!maker || !makers.emplace(name, std::move(maker)).second)
    throw std::invalid_argument("invalid or duplicate lane policy: " + name);
}

std::unique_ptr<LanePolicy> LanePolicyRegistry::make(std::string_view name, LuxirNode& node) const {
  auto found = makers.find(name);
  if (found == makers.end()) throw std::invalid_argument("unknown lane policy: " + std::string(name));
  return found->second(node);
}

}  // namespace luxir
