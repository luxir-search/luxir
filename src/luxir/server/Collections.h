// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
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
// incarnation, removal), announced in order through CollectionEvents.
//
// Removal and activation own their name for their whole duration: a second
// transition of the same name fails fast instead of waiting, and no lock is
// held across I/O. Creation is exclusive through the map's lazy creation.
// Staged candidates hold leases on their incarnation directories, and
// retirement removes only directories nothing selects, leases or retains.
// Publications within an incarnation are not transitions; they reach the
// events feed from the collection's registry.
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

private:
  // Keeps a staged incarnation directory from retirement until released.
  struct Lease {
    Collections& owner;
    std::string name, incarnation;
    ~Lease() { owner.release(name, incarnation); }
  };

public:
  // An unregistered incarnation of a collection for an installer to fill.
  class Candidate {
    std::string name_;
    std::string incarnation_;
    std::shared_ptr<Collection> collection_;
    std::unique_ptr<Lease> lease;
    friend class Collections;
    Candidate(std::string name, std::string incarnation, std::shared_ptr<Collection> collection, std::unique_ptr<Lease> lease)
        : name_(std::move(name)), incarnation_(std::move(incarnation)), collection_(std::move(collection)), lease(std::move(lease)) {}
  public:
    Candidate(Candidate&&) = default;
    Candidate& operator=(Candidate&&) = default;
    const std::string& incarnation() const { return incarnation_; }
    Directory& dir() const { return collection_->getShard()->getSnapshots().dir; }
    // The snapshot an interrupted activation already committed, if any.
    std::shared_ptr<const CommitSnapshot> committed() const { return collection_->getShard()->getSnapshots().snapshot(); }
  };

  // A candidate whose snapshot is durable and published to its own, still
  // unregistered, reader view. Only prepare() makes one; install() consumes it.
  class Prepared {
    Candidate candidate;
    friend class Collections;
    explicit Prepared(Candidate candidate) : candidate(std::move(candidate)) {}
  public:
    Prepared(Prepared&&) = default;
  };

private:
  struct Slot {
    bool busy = false;                 // a removal or activation owns the name
    std::multiset<std::string> leased; // staged candidate incarnations
    std::set<std::string> retained;    // a local copy that could not be opened
  };
  // Owns a name for one transition; fails fast if another transition does.
  class Transition {
    Collections& owner;
    std::string name;
  public:
    Transition(Collections& owner, std::string name);
    ~Transition();
  };

  const LuxirConfig& config;
  CollectionEvents& events;
  IndexRamBudget& indexRamBudget;
  std::unique_ptr<DirectoryFactory> factory;
  SharedLazyMap<std::string, Collection> map;
  std::mutex slotsMutex;
  std::map<std::string, Slot, std::less<>> slots;

  std::shared_ptr<Collection> makeCollection(const std::string& name, std::shared_ptr<Directory> directory);
  std::shared_ptr<Collection> openCollection(const std::string& name, Role role, const std::optional<std::string>& selected);
  std::shared_ptr<Collection> initWriter(const std::string& name, std::shared_ptr<Schema> initialSchema = {});
  void observe(const std::string& name, Collection& collection);
  bool reclaimStorage();
  void release(const std::string& name, const std::string& incarnation) noexcept;
  // Removes incarnation directories that nothing selects, leases or retains.
  // The caller owns the name's transition.
  void retire(const std::string& name, const std::string& selected) noexcept;

public:
  Collections(const LuxirConfig& config, CollectionEvents& events, IndexRamBudget& indexRamBudget);
  ~Collections();

  static void validateName(std::string_view name);
  DirectoryFactory& storage() { return *factory; }

  // The single startup discovery path. Writers recover identities, remove
  // unselected leftovers and create the default collection; read-only nodes
  // never mutate; followers retain incomplete downloads. A collection that
  // cannot be opened is registered unavailable and all of its data is kept.
  std::vector<Opened> open(Role role);

  // The registered entry, which may be an unavailable placeholder.
  std::shared_ptr<Collection> get(std::string_view name) { return map.get(std::string(name)); }
  std::vector<Entry> entries();

  // Writers. getOrCreate() creates only when `autoCreate`.
  std::shared_ptr<Collection> getOrCreate(std::string_view name, bool autoCreate);
  std::shared_ptr<Collection> create(std::string_view name, const api::SchemaDef* schema = nullptr);

  // Removes the entry and all storage. Without `mustExist`, an absent entry
  // still removes leftover storage. Refused while a candidate is staged.
  void remove(std::string_view name, bool mustExist = true);

  Candidate stage(std::string_view name, std::string_view incarnation);
  // Makes `snapshot` durable in the candidate's directory, whose files the
  // caller verified and synced, and opens its readers. On failure the
  // candidate stays staged.
  Prepared prepare(Candidate& candidate, std::shared_ptr<const CommitSnapshot> snapshot);
  // Activates a prepared candidate in place of `expected` (null: absent):
  // CURRENT durably selects it, the map points at it, the events feed
  // announces it, and incarnations nothing else needs are retired.
  std::shared_ptr<Collection> install(Prepared prepared, const std::shared_ptr<Collection>& expected);
  // Closes an abandoned candidate and removes its directory unless something
  // selects, leases or retains it.
  void discard(Candidate& candidate) noexcept;
  // After a same-incarnation advance: retire leftover candidates of earlier runs.
  void retainSelected(const std::string& name) noexcept;
  // Registers an unavailable placeholder unless something is already registered.
  std::shared_ptr<Collection> unavailable(const std::string& name, std::string reason);
};

}
