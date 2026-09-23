// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <set>
#include <mutex>
#include <stop_token>
#include "luxir/util/DeadlineScheduler.h"
#include "luxir/index/CommitSnapshot.h"

namespace luxir {
class LuxirNode;

// Process-local discovery. Collection state is read from the node while forming
// a response; only revisions, waiters and liveness live here.
class ReplicationCatalog {
public:
  using Clock = std::chrono::steady_clock;
  using Now = std::function<Clock::time_point()>;
  struct Follower {
    std::string follower;
    std::string collection;
    CommitId commit;
    uint64_t lastSeen;
    std::optional<uint64_t> lag;
  };
  struct Barrier {
    bool all;
    uint32_t wanted;
    std::set<std::string> members;
  };
  enum class Event { CHECK, DEADLINE, CANCELLED, REMOVED, RECREATED, CHANGED };
private:
  struct LiveFollower {
    uint64_t lastSeen;
    Clock::time_point seen;
    std::map<std::string, CommitId, std::less<>> commits;
  };
  std::mutex mutex;
  std::string boot;
  uint64_t revision = 0;
  uint64_t nextWatch = 0;
  struct Wait {
    uint64_t id;
    Clock::time_point deadline;
    bool all;
    std::string incarnation;
    std::function<bool(Event)> ready;
    std::function<void()> complete;
    std::optional<std::stop_callback<std::function<void()>>> cancellation;
  };
  using Pending = std::vector<std::shared_ptr<Wait>>;
  std::map<std::string, Pending, std::less<>> waits;
  std::map<std::pair<Clock::time_point, uint64_t>, std::string> deadlines;
  uint64_t nextWait = 0;
  bool waitsClosed = false;
  DeadlineScheduler::Slot deadline{[this] { tick(); }};
  std::map<uint64_t, std::function<void()>> watches;
  std::map<std::string, LiveFollower, std::less<>> followers;
  std::chrono::milliseconds liveness;
  Now now;
  void tick();
  void checkLocked(std::string_view collection, Event event, Pending& ready, bool allOnly = false, std::string_view incarnation = {});
  static void finish(Pending& ready);
  void armLocked();
  void cancelWait(std::string_view collection, uint64_t id);
  api::ReplicaResult progressLocked(const Barrier& barrier, std::string_view collection, const CommitId& id);
  size_t rowsLocked() const;
  void expireLocked();
  void pruneLocked(const std::map<std::string, CommitId>& collections);
  void seenLocked(std::string_view follower);
public:
  explicit ReplicationCatalog(std::chrono::milliseconds liveness, Now now = Clock::now);
  void changed(std::string_view name = {}, std::string_view incarnation = {}) noexcept;
  void remove(const std::string& name) noexcept;
  // Check and registration share a lock. Completions only schedule I/O work.
  uint64_t watch(std::string_view cursor, std::string_view follower, std::function<void()> completion);
  void cancel(uint64_t watch);
  std::string catalog(LuxirNode& node);
  void installed(LuxirNode& node, std::string_view body);
  std::vector<Follower> stats(LuxirNode& node);
  // Watches renew liveness; all captures only followers serving this incarnation.
  void seen(std::string_view follower);
  void expire();
  static uint32_t replicaCount(std::string_view value);
  Barrier barrier(std::string_view wanted, std::string_view collection, const CommitId& id);
  // Only requests with a floor/barrier enter this service. Completions must
  // schedule their work; neither the deadline thread nor publication waits.
  // Returns true when already satisfied; the caller can continue inline.
  bool await(std::string collection, std::function<bool(Event)> ready, std::function<void()> complete,
             uint64_t timeoutMs, std::stop_token stop = {}, bool all = false, std::string incarnation = {});
  void awaitBarrier(LuxirNode& node, std::string collection, CommitId id, std::string_view wanted, uint64_t timeoutMs,
                    std::function<void(api::ReplicaResult, Event)> complete, std::stop_token stop = {});
  void closeWaits();
  ~ReplicationCatalog();
};

}
