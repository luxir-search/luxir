// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <mutex>
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
  std::map<uint64_t, std::function<void()>> watches;
  std::map<std::string, LiveFollower, std::less<>> followers;
  std::chrono::milliseconds liveness;
  Now now;
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
};

}
