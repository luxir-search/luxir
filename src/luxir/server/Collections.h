// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "Collection.h"
#include "CollectionEvents.h"
#include "luxir/LuxirConfig.h"
#include "luxir/index/IndexRamBudget.h"
#include "luxir/store/DirectoryFactory.h"
#include "luxir/util/SharedLazyMap.h"

namespace luxir {

namespace api { struct SchemaDef; }

// Owns a node's collections: storage, the name -> collection map, and every
// transition of it (startup discovery, creation, activation of a new
// incarnation, removal). Each transition holds the name's lifecycle lock while
// it changes the map and announces the change to CollectionEvents, in order.
// Publications within an incarnation are not lifecycle transitions; they reach
// the events feed from the collection's registry.
class Collections {
public:
  static constexpr std::string_view kDefaultCollectionName = "main";
  enum class Role { WRITER, READ_ONLY, FOLLOWER };

  struct Entry {
    std::string name;
    std::shared_ptr<Collection> collection;
    std::string error;
  };
  // A startup row. A follower collection without CURRENT has neither a
  // collection nor an error: its incarnations are retained download candidates.
  struct Opened {
    std::string name;
    std::shared_ptr<Collection> collection; // null for a staged-only follower collection
    std::optional<std::string> incarnation; // CURRENT, when it could be read
    std::string error;
  };

  // An unregistered incarnation of `name` for an installer to fill. It becomes
  // installable once its registry holds a committed snapshot of incarnation().
  class Candidate {
    std::string name_;
    std::string incarnation_;
    std::shared_ptr<Collection> collection_;
    friend class Collections;
    Candidate(std::string name, std::string incarnation, std::shared_ptr<Collection> collection)
        : name_(std::move(name)), incarnation_(std::move(incarnation)), collection_(std::move(collection)) {}
  public:
    Candidate(Candidate&&) = default;
    Candidate& operator=(Candidate&&) = default;
    const std::string& incarnation() const { return incarnation_; }
    CommitSnapshotRegistry& snapshots() const { return collection_->getShard()->getSnapshots(); }
  };

private:
  const LuxirConfig& config;
  CollectionEvents& events;
  IndexRamBudget& indexRamBudget;
  std::unique_ptr<DirectoryFactory> factory;
  SharedLazyMap<std::string, Collection> map;
  std::array<std::mutex, 64> lifecycleLocks;

  std::mutex& lifecycleLock(std::string_view name);
  std::shared_ptr<Collection> makeCollection(const std::string& name, std::shared_ptr<Directory> directory);
  std::shared_ptr<Collection> openCollection(const std::string& name, Role role, const std::optional<std::string>& selected);
  std::shared_ptr<Collection> initWriter(const std::string& name, std::shared_ptr<Schema> initialSchema = {});
  void observe(const std::string& name, Collection& collection);
  bool reclaimStorage();

public:
  Collections(const LuxirConfig& config, CollectionEvents& events, IndexRamBudget& indexRamBudget);
  ~Collections();

  static void validateName(std::string_view name);
  DirectoryFactory& storage() { return *factory; }

  // The single startup discovery path. Writers recover identities, remove
  // unselected leftovers and create the default collection; read-only nodes
  // never mutate; followers retain incomplete downloads. A collection that
  // cannot be opened is registered unavailable and its data is kept.
  std::vector<Opened> open(Role role, const std::map<std::string, std::string>& failures = {});

  // The registered entry, which may be an unavailable placeholder.
  std::shared_ptr<Collection> get(std::string_view name) { return map.get(std::string(name)); }
  std::vector<Entry> entries();

  // Writers. getOrCreate() creates only when `autoCreate`.
  std::shared_ptr<Collection> getOrCreate(std::string_view name, bool autoCreate);
  std::shared_ptr<Collection> create(std::string_view name, const api::SchemaDef* schema = nullptr);

  // Removes the entry and all storage. Without `mustExist`, an absent entry
  // still removes leftover storage.
  void remove(std::string_view name, bool mustExist = true);

  Candidate stage(std::string_view name, std::string_view incarnation);
  // Activates a committed candidate in place of `expected` (null: absent):
  // CURRENT selects it, the map points at it, the events feed announces it, and
  // the replaced incarnation and any other incarnation directories are retired.
  std::shared_ptr<Collection> install(Candidate candidate, const std::shared_ptr<Collection>& expected);
  // Closes an abandoned candidate and removes its storage unless CURRENT
  // selects it (a retained copy that could not be opened).
  void discard(Candidate& candidate) noexcept;
  // Retires every incarnation directory CURRENT does not select, such as
  // candidates retained across a restart. Only for names with no live candidate.
  void retainSelected(const std::string& name) noexcept;
  // Registers an unavailable placeholder unless something is already registered.
  std::shared_ptr<Collection> unavailable(const std::string& name, std::string reason);
};

}
