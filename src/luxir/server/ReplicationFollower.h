// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <memory>
#include <memory_resource>
#include <string_view>

namespace luxir {
class LuxirNode;
namespace api { struct ReplicationStatus; }

// Owns discovery and a bounded set of transfer workers. Ordinary writer nodes
// do not construct one. Local publication is independent of network waits.
class ReplicationFollower {
  struct Impl;
  std::unique_ptr<Impl> impl;
public:
  explicit ReplicationFollower(LuxirNode& node);
  ~ReplicationFollower();
  void start();
  void stop();
  void deleteOrphan(std::string_view name);
  void stats(api::ReplicationStatus& out, std::pmr::memory_resource& arena);
};
}
