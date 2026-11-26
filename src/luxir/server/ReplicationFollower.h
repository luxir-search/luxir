// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <iosfwd>
#include <memory>
#include <optional>
#include <memory_resource>
#include <span>
#include <string_view>
#include <vector>
#include "Collections.h"
#include "ReplicationSubscription.h"

namespace luxir {
class LuxirNode;
namespace api { struct ReplicationStatus; struct FollowerSubscription; struct SetFollowerSubscriptionRequest; }

// Owns discovery and a bounded set of transfer workers. Ordinary writer nodes
// do not construct one. Local publication is independent of network waits.
class ReplicationFollower {
  struct Impl;
  std::unique_ptr<Impl> impl;
public:
  // Binds the data directory to this follower; refuses one it does not own.
  explicit ReplicationFollower(LuxirNode& node);
  ~ReplicationFollower();
  // Adopts the collections opened from that directory, with their history.
  void seed(const std::vector<Collections::Opened>& opened);
  // One catalog pass, before start(), with the same installation rules.
  // Does not acknowledge serving traffic. Returns false if any collection failed.
  bool pull(std::ostream& output);
  void start();
  // Final: a stopped follower does not start or resubscribe again.
  void stop();
  struct Subscription {
    std::string source; // http://host:port
    ReplicationSubscription subscription;
  };
  Subscription subscription();
  // Changes the source and/or the subscription of a running follower; absent
  // keeps the current value. Persisted, so a restart keeps it unless the
  // configuration changed. Validates before stopping anything.
  void resubscribe(const std::optional<std::string>& source, const std::optional<ReplicationSubscription>& subscription);
  // The API forms of subscription() and resubscribe().
  void describe(api::FollowerSubscription& out, std::pmr::memory_resource& arena);
  void resubscribe(const api::SetFollowerSubscriptionRequest& request);
  void deleteOrphan(const CollectionId& id);
  // After Collections::fail: repair the failed collection, or a replacement
  // still holding one of its suspect descriptors. Unrelated replacements stay
  // available. Repair downloads marker-named files and verifies other files.
  void repair(const CollectionId& id, const std::shared_ptr<Collection>& collection, const std::string& reason,
              std::span<const FileDescriptor> suspects);
  void stats(api::ReplicationStatus& out, std::pmr::memory_resource& arena);
};
}
