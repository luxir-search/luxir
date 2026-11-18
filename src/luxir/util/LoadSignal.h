// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "TaskWork.h"

namespace luxir {

enum class LoadClass { IDLE_CORE, SIBLINGS_ONLY, SATURATED };
enum class LoadKind { REQUEST, SHARE };

struct FanoutBudget {
  uint64_t grainNs;
  uint32_t maxPublished;
  LoadClass cls;
};

struct LoadTopology { uint32_t physical, logical; };
inline constexpr std::array<uint64_t, 4> LOAD_WINDOWS_NS{0, 250'000, 1'000'000, 4'000'000};

struct alignas(64) LoadSlot {
  std::atomic<uint32_t> busy{0};
  std::atomic<uint64_t> lastEndNs{UINT64_MAX};  // No end yet; zero is a valid test time.
  std::atomic<bool> claimed{false};
  uint32_t depth = 0;        // Owner-private, including when a wait executes another task.

  void begin() {
    if (depth++ == 0) busy.store(1, std::memory_order_relaxed);
  }
  void end(bool recency) {
    if (depth && --depth == 0) {
      if (recency) lastEndNs.store(TaskClock{}(), std::memory_order_relaxed);
      // An acquiring snapshot that observes the clear also sees lastEndNs.
      busy.store(0, std::memory_order_release);
    }
  }
};

// Tiers fan-out using request bodies, published shares and index-arena workers.
// Recency bridges request gaps only. Excludes
// resumed streaming production, shard-thread update parsing, gRPC pollers and
// all work outside this process. Index workers include their arena linger.
class LoadRegistry {
  friend class LoadScope;
public:
  class ArenaActivity;
private:
  struct Counts {
    std::array<std::atomic<uint64_t>, 3> classes{};
    std::atomic<uint64_t> hot{0};
  };
  static constexpr size_t SLOTS = 256;
  std::array<LoadSlot, SLOTS> slots;
  std::atomic<size_t> highWater{0};
  std::atomic<uint64_t> slotsExhausted{0};
  std::array<Counts, 4> counts;
  LoadTopology topology;
  mutable std::mutex arenasMutex;
  std::vector<ArenaActivity*> arenas;

  LoadSlot* claim();
  void release(LoadSlot& slot);
  std::array<uint32_t, 4> snapshot(const LoadSlot* self) const;
  void record(const std::array<uint32_t, 4>& hot);
  static LoadSlot* claimLocal();

public:
  static inline std::atomic<bool> enabled{true};

  // An owning thread keeps its registration until exit, so idle history is
  // available between requests. Explicit registrations also allow isolated tests.
  class Registration {
    friend class LoadRegistry;
    friend class ArenaActivity;
    friend class LoadScope;
    LoadRegistry& registry;
    LoadSlot* slot = nullptr;
    LoadSlot* getOrClaim();
  public:
    explicit Registration(LoadRegistry& registry) : registry(registry) {}
    ~Registration() { if (slot) registry.release(*slot); }
    bool begin();
    void end(bool recency);
    void sample();
    std::array<uint32_t, 4> snapshot() const { return registry.snapshot(slot); }
    Registration(const Registration&) = delete;
    Registration& operator=(const Registration&) = delete;
  };

  // Arena presence has a separate lifetime from task nesting: detaching a TBB
  // observer need not send balancing exits to workers still inside the arena.
  class ArenaActivity {
    friend class LoadRegistry;
    struct alignas(64) Presence { std::atomic<bool> busy{false}; };
    LoadRegistry& registry;
    std::array<Presence, SLOTS> presence;
  public:
    explicit ArenaActivity(LoadRegistry& registry);
    ~ArenaActivity();
    void begin(Registration& owner);
    void end(Registration& owner);
    ArenaActivity(const ArenaActivity&) = delete;
    ArenaActivity& operator=(const ArenaActivity&) = delete;
  };

  explicit LoadRegistry(LoadTopology topology);
  static LoadRegistry& global();
  static Registration& local();
  // The scope hot path reads only this pointer. Registration and its TLS
  // destructor are initialized by claimLocal(), outside the hot path.
  static inline thread_local LoadSlot* localSlot = nullptr;
  // Explicit registrations let tests inject topology and load on this thread.
  static inline thread_local Registration* localForTests = nullptr;
  static LoadClass classify(uint32_t hot, LoadTopology topology);
  // One snapshot supplies both the budget and the FANOUT_DECISION sample.
  static FanoutBudget fanoutBudget(uint64_t grainNs);
  static void sample() {
    if (enabled.load(std::memory_order_relaxed)) local().sample();
  }
  std::string debugText() const;
  LoadRegistry(const LoadRegistry&) = delete;
  LoadRegistry& operator=(const LoadRegistry&) = delete;
};

class LoadScope {
  LoadSlot* slot = nullptr;
  LoadKind kind;
public:
  explicit LoadScope(LoadKind kind) : kind(kind) {
    // Disabled scopes do not touch TLS, initialize the registry or read a clock.
    if (LoadRegistry::enabled.load(std::memory_order_relaxed)) {
      slot = LoadRegistry::localSlot;
      if (!slot) slot = LoadRegistry::claimLocal();
      if (slot) slot->begin();
    }
  }
  explicit LoadScope(LoadRegistry::Registration& owner, LoadKind kind)
      : slot(owner.getOrClaim()), kind(kind) {
    if (slot) slot->begin();
  }
  ~LoadScope() { if (slot) slot->end(kind == LoadKind::REQUEST); }
  LoadScope(const LoadScope&) = delete;
  LoadScope& operator=(const LoadScope&) = delete;
};

std::string loadSignalDebugText();

}
