// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stop_token>
#include "ReplicationSource.h"
#include "luxir/util/DeadlineScheduler.h"

namespace luxir {
// Node-local publication floors and replica visibility barriers. Callbacks run
// after unlocking and must only dispatch work, never block the publication feed.
class CommitWaits : public CollectionEvents {
public:
  using Clock = ReplicationSource::Clock;
  using Now = ReplicationSource::Now;
  struct ReplicaWait {
    CollectionId name;
    CommitId commit;
    Clock::time_point deadline;
    ReplicationSource::Barrier barrier;
  };
  struct CommitResult {
    std::shared_ptr<Collection> collection;
    std::optional<ErrorInfo> error;
  };
private:
  struct Local {
    std::function<void(CommitResult)> complete;
    CommitResult result;
  };
  struct Replicas {
    ReplicationSource::Barrier barrier;
    std::function<void(api::ReplicaResult)> complete;
    api::ReplicaResult result;
  };
  struct Wait {
    uint64_t id = 0;
    CollectionId name;
    CommitId commit;
    Clock::time_point deadline;
    std::variant<Local, Replicas> kind;
    std::optional<std::stop_callback<std::function<void()>>> cancellation;
  };
  using Pending = std::vector<std::shared_ptr<Wait>>;
  std::mutex mutex;
  std::map<CollectionId, CollectionPublication> collections;
  std::map<CollectionId, Pending> waits;
  std::map<std::pair<Clock::time_point, uint64_t>, CollectionId> deadlines;
  ReplicationSource& source;
  bool following;
  Now now;
  bool closed = false;
  uint64_t nextWait = 0;
  size_t allWaits = 0;
  DeadlineScheduler::Slot timer{[this] { poll(); }};
  bool evaluate(Wait& wait, bool cancelled = false, bool removed = false);
  void retireLocked(const Wait& wait);
  void checkLocked(const CollectionId& name, Pending& ready, bool removed = false);
  void admit(std::shared_ptr<Wait> wait, std::stop_token stop);
  void cancel(const CollectionId& name, uint64_t id);
  void armLocked();
  static void finish(Pending& ready);
public:
  CommitWaits(ReplicationSource& source, bool following, Now now = Clock::now)
      : source(source), following(following), now(std::move(now)) {}
  ~CommitWaits();
  Clock::time_point deadlineAfter(uint64_t timeoutMs) const;
  void awaitCommit(CollectionId name, CommitId floor, Clock::time_point deadline,
                   std::stop_token stop, std::function<void(CommitResult)> complete);
  ReplicaWait prepareReplicas(CollectionId name, CommitId commit, const api::ReplicaRequirement& requirement,
                             Clock::time_point deadline);
  void awaitReplicas(ReplicaWait spec, std::stop_token stop, std::function<void(api::ReplicaResult)> complete);
  void awaitReplicas(CollectionId name, CommitId commit, const api::ReplicaRequirement& requirement,
                     Clock::time_point deadline, std::stop_token stop, std::function<void(api::ReplicaResult)> complete);
  void registered(const CollectionId& name, const std::shared_ptr<Collection>& collection) noexcept override;
  void updated(const CollectionId& name, const Collection& collection) noexcept override;
  void removed(const CollectionId& name) noexcept override;
  // Called for acks and expiry, and after advancing an injected clock in tests.
  // `name` null: acknowledgments of any collection changed (e.g. expiry).
  void acknowledged(const CollectionId* name);
  void poll();
  void close();
};
}
