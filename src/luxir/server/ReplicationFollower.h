// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <iosfwd>
#include <memory>
#include <memory_resource>
#include <span>
#include <string_view>
#include <vector>
#include "Collections.h"

namespace luxir {
class LuxirNode;
namespace api { struct ReplicationStatus; }

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
  void stop();
  void deleteOrphan(const CollectionId& id);
  // After Collections::fail: repair the failed collection, or a replacement
  // still holding one of its suspect descriptors. Unrelated replacements stay
  // available. Repair downloads marker-named files and verifies other files.
  void repair(const CollectionId& id, const std::shared_ptr<Collection>& collection, const std::string& reason,
              std::span<const FileDescriptor> suspects);
  void stats(api::ReplicationStatus& out, std::pmr::memory_resource& arena);
};
}
