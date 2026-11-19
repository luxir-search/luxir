// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "LoadSignal.h"
#include "TaskWork.h"

#include <algorithm>
#include <fstream>
#include <set>
#include <oneapi/tbb/info.h>
#ifdef __linux__
#include <sched.h>
#endif
#ifdef __APPLE__
#include <sys/sysctl.h>
#endif

namespace luxir {
namespace {

LoadTopology detectTopology() {
  uint32_t logical = (uint32_t)std::max(1, oneapi::tbb::info::default_concurrency());
  uint32_t physical = logical;
#ifdef __linux__
  // A fixed mask that cannot hold this machine's CPU IDs makes affinity fail;
  // in that case use the conservative P = L fallback, never a partial mask.
  cpu_set_t affinity;
  CPU_ZERO(&affinity);
  if (sched_getaffinity(0, sizeof(affinity), &affinity) == 0) {
    std::set<std::string> cores;
    bool valid = true;
    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++) {
      if (!CPU_ISSET(cpu, &affinity)) continue;
      std::ifstream in("/sys/devices/system/cpu/cpu" + std::to_string(cpu)
          + "/topology/thread_siblings_list");
      std::string siblings;
      if (!std::getline(in, siblings) || siblings.empty()) {
        valid = false;
        break;
      }
      cores.insert(std::move(siblings));
    }
    if (valid && !cores.empty()) physical = (uint32_t)cores.size();
  }
#endif
#ifdef __APPLE__
  uint32_t cores = 0;
  size_t size = sizeof(cores);
  if (sysctlbyname("hw.physicalcpu", &cores, &size, nullptr, 0) == 0 && cores > 0) physical = cores;
#endif
  return {std::min(physical, logical), logical};
}

}

LoadRegistry::LoadRegistry(LoadTopology topology) : topology(topology) {
  this->topology.logical = std::max(1u, topology.logical);
  this->topology.physical = std::clamp(topology.physical, 1u, this->topology.logical);
}

LoadRegistry& LoadRegistry::global() {
  // TLS registrations (including TBB workers) may outlive static destructors.
  // Keep this small process registry alive until process exit.
  static auto* registry = new LoadRegistry(detectTopology());
  return *registry;
}

LoadRegistry::Registration& LoadRegistry::local() {
  if (localForTests) return *localForTests;
  static thread_local Registration registration(global());
  return registration;
}

LoadSlot* LoadRegistry::claimLocal() {
  return localSlot = local().getOrClaim();
}

LoadSlot* LoadRegistry::claim() {
  for (size_t i = 0; i < slots.size(); i++) {
    bool expected = false;
    if (!slots[i].claimed.compare_exchange_strong(expected, true, std::memory_order_acquire,
                                                 std::memory_order_relaxed)) continue;
    // The previous owner cleared its atomic fields before releasing the slot.
    slots[i].depth = 0;
    size_t water = highWater.load(std::memory_order_relaxed);
    while (water < i + 1 && !highWater.compare_exchange_weak(water, i + 1,
        std::memory_order_release, std::memory_order_relaxed)) {}
    return &slots[i];
  }
  slotsExhausted.fetch_add(1, std::memory_order_relaxed);
  return nullptr;
}

void LoadRegistry::release(LoadSlot& slot) {
  // A skipped arena exit must not follow this slot into its next owner.
  std::lock_guard lock(arenasMutex);
  for (auto* arena : arenas) arena->presence[(size_t)(&slot - slots.data())].busy.store(false, std::memory_order_relaxed);
  slot.busy.store(0, std::memory_order_relaxed);
  slot.lastEndNs.store(UINT64_MAX, std::memory_order_relaxed);
  slot.claimed.store(false, std::memory_order_release);
}

LoadSlot* LoadRegistry::Registration::getOrClaim() {
  if (!slot) slot = registry.claim();
  return slot;
}

bool LoadRegistry::Registration::begin() {
  if (!getOrClaim()) return false;
  slot->begin();
  return true;
}

void LoadRegistry::Registration::end(bool recency) {
  if (slot) slot->end(recency);
}

std::array<uint32_t, 4> LoadRegistry::snapshot(const LoadSlot* self) const {
  std::array<uint32_t, 4> hot{};
  uint64_t now = TaskClock{}();
  size_t water = highWater.load(std::memory_order_acquire);
  // Arena addition/removal and thread exit are rare; the lock protects arena
  // lifetimes, never the request begin/end stores or arena entry/exit stores.
  std::lock_guard lock(arenasMutex);
  // Approximate concurrent snapshot: owners keep writing while we scan.
  for (size_t i = 0; i < water; i++) {
    const auto& slot = slots[i];
    if (&slot == self || !slot.claimed.load(std::memory_order_acquire)) continue;
    bool busy = slot.busy.load(std::memory_order_acquire) != 0;
    for (const auto* arena : arenas) {
      if (arena->presence[i].busy.load(std::memory_order_acquire)) busy = true;
    }
    uint64_t ended = slot.lastEndNs.load(std::memory_order_relaxed);
    // An owner can finish after 'now' was read; count that as age zero.
    uint64_t age = now >= ended ? now - ended : 0;
    for (size_t w = 0; w < hot.size(); w++) {
      if (busy || (ended != UINT64_MAX && LOAD_WINDOWS_NS[w] != 0 && age < LOAD_WINDOWS_NS[w])) ++hot[w];
    }
  }
  return hot;
}

LoadRegistry::ArenaActivity::ArenaActivity(LoadRegistry& registry) : registry(registry) {
  std::lock_guard lock(registry.arenasMutex);
  registry.arenas.push_back(this);
}

LoadRegistry::ArenaActivity::~ArenaActivity() {
  std::lock_guard lock(registry.arenasMutex);
  std::erase(registry.arenas, this);
}

void LoadRegistry::ArenaActivity::begin(Registration& owner) {
  if (auto* slot = owner.getOrClaim()) {
    presence[(size_t)(slot - registry.slots.data())].busy.store(true, std::memory_order_relaxed);
  }
}

void LoadRegistry::ArenaActivity::end(Registration& owner) {
  if (auto* slot = owner.slot) {
    auto& busy = presence[(size_t)(slot - registry.slots.data())].busy;
    if (!busy.load(std::memory_order_relaxed)) return;
    busy.store(false, std::memory_order_release);
  }
}

LoadClass LoadRegistry::classify(uint32_t hot, LoadTopology topology) {
  uint64_t total = (uint64_t)hot + 1;
  return total < topology.physical ? LoadClass::IDLE_CORE
      : total < topology.logical ? LoadClass::SIBLINGS_ONLY : LoadClass::SATURATED;
}

void LoadRegistry::record(const std::array<uint32_t, 4>& hot) {
  for (size_t w = 0; w < hot.size(); w++) {
    auto& count = counts[w];
    count.classes[(size_t)classify(hot[w], topology)].fetch_add(1, std::memory_order_relaxed);
    count.hot.fetch_add(hot[w], std::memory_order_relaxed);
  }
}

void LoadRegistry::Registration::sample() {
  // Utility callers may evaluate a fan-out outside a request or share scope.
  bool temporary = !slot || slot->depth == 0;
  if (temporary && !begin()) return;
  registry.record(registry.snapshot(slot));
  if (temporary) end(false);
}

FanoutBudget LoadRegistry::fanoutBudget(uint64_t grainNs) {
  auto& owner = local();
  auto& registry = owner.registry;
  auto hot = registry.snapshot(owner.getOrClaim());
  registry.record(hot);
  LoadClass cls = classify(hot[1], registry.topology);
  uint64_t total = (uint64_t)hot[1] + 1;
  if (cls == LoadClass::IDLE_CORE) return {grainNs, (uint32_t)(registry.topology.physical - total), cls};
  uint64_t sibling = fanoutSiblingGrainNs.load(std::memory_order_relaxed);
  if (cls == LoadClass::SIBLINGS_ONLY && sibling != FANOUT_GRAIN_NEVER) {
    return {std::max(grainNs, sibling), (uint32_t)(registry.topology.logical - total), cls};
  }
  return {FANOUT_GRAIN_NEVER, 0, cls};
}

std::string LoadRegistry::debugText() const {
  constexpr std::array<const char*, 3> classes{"idle_core", "siblings_only", "saturated"};
  std::string out;
  for (size_t w = 0; w < LOAD_WINDOWS_NS.size(); w++) {
    std::string labels = "point=\"fanout_decision\",window_us=\""
        + std::to_string(LOAD_WINDOWS_NS[w] / 1000) + "\"";
    const auto& count = counts[w];
    for (size_t c = 0; c < classes.size(); c++) {
      out += "luxir_debug_load_samples_total{" + labels + ",class=\"" + classes[c] + "\"} "
          + std::to_string(count.classes[c].load(std::memory_order_relaxed)) + "\n";
    }
    out += "luxir_debug_load_hot_threads_sum{" + labels + "} "
        + std::to_string(count.hot.load(std::memory_order_relaxed)) + "\n";
  }
  out += "luxir_debug_load_cpus{kind=\"physical\"} " + std::to_string(topology.physical) + "\n";
  out += "luxir_debug_load_cpus{kind=\"logical\"} " + std::to_string(topology.logical) + "\n";
  out += "luxir_debug_load_slots_exhausted_total "
      + std::to_string(slotsExhausted.load(std::memory_order_relaxed)) + "\n";
  return out;
}

std::string loadSignalDebugText() {
  std::string out;
  if (LoadRegistry::enabled.load(std::memory_order_relaxed)) out = LoadRegistry::global().debugText();
  auto shareCounter = [&](const char* name, const std::atomic<uint64_t>& counter) {
    out += "luxir_debug_fanout_share_" + std::string(name) + " "
        + std::to_string(counter.load(std::memory_order_relaxed)) + "\n";
  };
  shareCounter("start_delay_ns_sum", fanoutShareStartDelayNsSum);
  shareCounter("start_delay_count", fanoutShareStartDelayCount);
  shareCounter("start_delay_ns_max", fanoutShareStartDelayNsMax);
  shareCounter("run_ns_sum", fanoutShareRunNsSum);
  shareCounter("estimated_ns_sum", fanoutShareEstimatedNsSum);
  shareCounter("publisher_runs_total", fanoutSharePublisherRuns);
  shareCounter("credit_ns_sum", fanoutShareCreditNsSum);
  // Every admitted plan adds its credit and lands in one width bucket, so the
  // buckets' sum is the credit count.
  constexpr std::array<const char*, 4> shares{"2", "3", "4", "5+"};
  for (size_t s = 0; s < shares.size(); s++) {
    out += "luxir_debug_fanout_share_decisions_total{shares=\"" + std::string(shares[s]) + "\"} "
        + std::to_string(fanoutShareDecisions[s].load(std::memory_order_relaxed)) + "\n";
  }
  shareCounter("kept_estimated_ns_sum", fanoutShareKeptEstimatedNsSum);
  shareCounter("largest_published_estimated_ns_sum", fanoutShareLargestPublishedEstimatedNsSum);
  shareCounter("wait_ns_sum", fanoutShareWaitNsSum);
  shareCounter("wait_count", fanoutShareWaitCount);
  return out;
}

}
