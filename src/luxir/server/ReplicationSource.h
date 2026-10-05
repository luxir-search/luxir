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
    CollectionId collection;
    CommitId commit;
    uint64_t lastSeen;
    std::optional<uint64_t> lag;
  };
  struct Barrier {
    bool all;
    uint32_t wanted;
    std::map<std::string, uint64_t> members; // follower -> acknowledgment membership
  };
private:
  struct Acknowledgment {
    CommitId commit;
    uint64_t membership = 0; // advances only when a row is newly admitted
  };
  struct LiveFollower {
    uint64_t lastSeen;
    Clock::time_point seen;
    std::set<std::string> tenants; // empty subscribes to all
    std::map<CollectionId, Acknowledgment> commits;
    bool includes(const CollectionId& id) const { return tenants.empty() || tenants.contains(id.tenant); }
  };
  std::mutex mutex;
  std::string boot;
  uint64_t revision = 0;
  uint64_t nextWatch = 0;
  std::map<CollectionId, CollectionPublication> collections;
  struct Watch {
    std::set<std::string> tenants;
    std::function<void()> completion;
  };
  struct TenantWatches {
    uint64_t revision = 0; // retained after removal, for stale cursors
    std::set<uint64_t> watches;
  };
  std::map<uint64_t, Watch> watches;
  std::set<uint64_t> allWatches;
  std::map<std::string, TenantWatches, std::less<>> tenantWatches;
  size_t acknowledgments = 0;
  uint64_t nextMembership = 0;
  const uint64_t maxAcknowledgments;
  std::map<std::string, LiveFollower, std::less<>> followers;
  std::chrono::milliseconds liveness;
  Now now;
  // Set before use. Called after unlocking with the collection whose
  // acknowledgments changed, or null for any; waits may read follower state.
  std::function<void(const CollectionId*)> acknowledgmentsChanged;
  Watch takeWatchLocked(uint64_t id);
  std::map<uint64_t, std::function<void()>> changedLocked(const std::string& tenant);
  void expireLocked();
  void seenLocked(std::string_view follower);
  void pruneLocked(const CollectionId& id);
public:
  explicit ReplicationSource(std::chrono::milliseconds liveness, Now now = Clock::now, uint64_t maxAcknowledgments = 262144);
  void onAcknowledgmentsChanged(std::function<void(const CollectionId*)> callback) { acknowledgmentsChanged = std::move(callback); }
  void registered(const CollectionId& id, const std::shared_ptr<Collection>& collection) noexcept override;
  void updated(const CollectionId& id, const Collection& collection) noexcept override;
  void removed(const CollectionId& id) noexcept override;
  uint64_t watch(std::string_view cursor, std::string_view follower, std::function<void()> completion, std::span<const std::string> tenants = {});
  void cancel(uint64_t watch);
  // Every collection, or only those of `tenants` when not empty. The cursor
  // covers every tenant.
  api::ReplicationCatalog catalog(std::pmr::memory_resource& arena, std::span<const std::string> tenants = {});
  void installed(const api::ReplicationInstalled& request);
  std::vector<Follower> status();
  void seen(std::string_view follower);
  void expire();
  std::optional<Clock::time_point> nextExpiry();
  Barrier capture(const api::ReplicaRequirement& requirement, const CollectionId& collection, const CommitId& id);
  api::ReplicaResult progress(const Barrier& barrier, const CollectionId& collection, const CommitId& id);
};
}
