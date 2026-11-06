// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <memory>
#include <string>
#include <span>
#include <cstdint>
#include "luxir/store/CollectionId.h"

namespace luxir {
class Collection;

// Ordered feed of a node's name -> collection map for node-level consumers
// (replication discovery, commit waits). Calls arrive in map-change order and
// must not throw or block: consumers update in-memory state and defer any
// completions until after they release their own locks.
class CollectionEvents {
public:
  struct Binding { CollectionId id; std::shared_ptr<Collection> collection; };
  // One atomic mapping change; a null collection removes that name.
  virtual void remapped(std::string_view tenant, uint64_t revision, std::span<const Binding> changes) noexcept = 0;
  virtual ~CollectionEvents() = default;
  // `collection` is now registered under `id`: created, replaced, or an
  // unavailable placeholder.
  virtual void registered(const CollectionId& id, const std::shared_ptr<Collection>& collection, uint64_t revision = 0) noexcept = 0;
  // `collection` published a snapshot or changed availability. Consumers re-read
  // it, and ignore it unless it is the collection registered under `id`.
  virtual void updated(const CollectionId& id, const Collection& collection) noexcept = 0;
  // Nothing is registered under `id`.
  virtual void removed(const CollectionId& id, uint64_t revision = 0) noexcept = 0;
};

}
