// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <set>
#include <mutex>
#include "CollectionEvents.h"
#include "CollectionPublication.h"

namespace luxir {
// Discovery and follower acknowledgments, using only state from the ordered feed.
class ReplicationSource : public CollectionEvents {
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
  std::map<std::string, CollectionPublication, std::less<>> collections;
  std::map<uint64_t, std::function<void()>> watches;
  std::map<std::string, LiveFollower, std::less<>> followers;
  std::chrono::milliseconds liveness;
  Now now;
  // Set before use. Called after unlocking; waits may read follower state.
  std::function<void(std::string_view)> acknowledgmentsChanged;
  size_t rowsLocked() const;
  void expireLocked();
  void seenLocked(std::string_view follower);
  void pruneLocked(const std::string& name);
public:
  explicit ReplicationSource(std::chrono::milliseconds liveness, Now now = Clock::now);
  void onAcknowledgmentsChanged(std::function<void(std::string_view)> callback) { acknowledgmentsChanged = std::move(callback); }
  void registered(const std::string& name, const std::shared_ptr<Collection>& collection) noexcept override;
  void updated(const std::string& name, const Collection& collection) noexcept override;
  void removed(const std::string& name) noexcept override;
  uint64_t watch(std::string_view cursor, std::string_view follower, std::function<void()> completion);
  void cancel(uint64_t watch);
  api::ReplicationCatalog catalog(std::pmr::memory_resource& arena);
  void installed(const api::ReplicationInstalled& request);
  std::vector<Follower> status();
  void seen(std::string_view follower);
  void expire();
  std::optional<Clock::time_point> nextExpiry();
  Barrier capture(const api::ReplicaRequirement& requirement, std::string_view collection, const CommitId& id);
  api::ReplicaResult progress(const Barrier& barrier, std::string_view collection, const CommitId& id);
};
}
