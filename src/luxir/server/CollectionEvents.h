// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <memory>
#include <string>

namespace luxir {
class Collection;

// Ordered feed of a node's name -> collection map for node-level consumers
// (replication discovery, commit waits). Calls arrive in map-change order and
// must not throw or block: consumers update in-memory state and defer any
// completions until after they release their own locks.
class CollectionEvents {
public:
  virtual ~CollectionEvents() = default;
  // `collection` is now registered under `name`: created, replaced, or an
  // unavailable placeholder.
  virtual void registered(const std::string& name, const std::shared_ptr<Collection>& collection) noexcept = 0;
  // `collection` published a snapshot or changed availability. Consumers re-read
  // it, and ignore it unless it is the collection registered under `name`.
  virtual void updated(const std::string& name, const Collection& collection) noexcept = 0;
  // Nothing is registered under `name`.
  virtual void removed(const std::string& name) noexcept = 0;
};

}
