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
// Creation, removal and activation own their name for their whole duration
// and announce their result before releasing it: a second transition of the
// same name fails fast instead of waiting, and no lock is held across I/O.
// Concurrent auto-creators wait on the map's creation slot instead.
// Staged candidates hold leases on their incarnation directories, and
// retirement removes only directories nothing selects, leases or retains.
// Publications within an incarnation are not transitions; they reach the
// events feed from the collection's registry.
class Collections {
public:
  static constexpr std::string_view kDefaultCollectionName = "main";
  enum class Role { WRITER, READ_ONLY, FOLLOWER };

  struct Entry {
    CollectionId id;
    std::shared_ptr<Collection> collection;
    std::string error;
  };
  // A startup row. A follower collection without CURRENT has neither a
  // collection nor an error: its incarnations are retained download candidates.
  struct Opened {
    CollectionId id;
    std::shared_ptr<Collection> collection; // null for a staged-only follower collection
    std::optional<std::string> incarnation; // CURRENT, when it could be read
    std::string error;
  };

private:
  // Keeps a staged incarnation directory from retirement until released.
  struct Lease {
    Collections& owner;
    CollectionId id;
    std::string incarnation;
    bool active = false;
    ~Lease() { if (active) owner.release(id, incarnation); }
  };

public:
  // An unregistered incarnation of a collection for an installer to fill.
  class Candidate {
    CollectionId id_;
    std::string incarnation_;
    std::shared_ptr<Collection> collection_;
    std::unique_ptr<Lease> lease;
    friend class Collections;
    Candidate(CollectionId id, std::string incarnation, std::shared_ptr<Collection> collection, std::unique_ptr<Lease> lease)
        : id_(std::move(id)), incarnation_(std::move(incarnation)), collection_(std::move(collection)), lease(std::move(lease)) {}
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
    CollectionId name;
  public:
    Transition(Collections& owner, CollectionId name);
    ~Transition();
  };

  const LuxirConfig& config;
  CollectionEvents& events;
  IndexRamBudget& indexRamBudget;
  std::unique_ptr<DirectoryFactory> factory;
  SharedLazyMap<CollectionId, Collection> map;
  std::mutex slotsMutex;
  std::map<CollectionId, Slot> slots;
  // Announced collections by tenant and name, changed only with their
  // announcements, so a listing reads its tenant without visiting the others.
  std::mutex listingMutex;
  std::map<std::string, std::map<std::string, std::shared_ptr<Collection>, std::less<>>, std::less<>> listing;

  std::shared_ptr<Collection> makeCollection(const CollectionId& id, std::shared_ptr<Directory> directory);
  std::shared_ptr<Collection> openCollection(const CollectionId& id, Role role, const std::optional<std::string>& selected);
  std::shared_ptr<Collection> initWriter(const CollectionId& id, std::shared_ptr<Schema> initialSchema = {});
  void observe(const CollectionId& id, Collection& collection);
  // Lists and announces a registration, or a removal.
  void registered(const CollectionId& id, const std::shared_ptr<Collection>& collection);
  void removed(const CollectionId& id);
  bool reclaimStorage();
  void release(const CollectionId& id, const std::string& incarnation) noexcept;
  // Removes incarnation directories that nothing selects, leases or retains.
  // The caller owns the name's transition.
  void retire(const CollectionId& id, const std::string& selected) noexcept;

public:
  Collections(const LuxirConfig& config, CollectionEvents& events, IndexRamBudget& indexRamBudget);
  ~Collections();

  // A tenant or collection name: [a-z][a-z0-9_]*, at most 255 bytes.
  static void validateName(std::string_view name, std::string_view kind = "collection");
  static void validate(const CollectionId& id);
  DirectoryFactory& storage() { return *factory; }

  // The single startup discovery path. Writers recover identities, remove
  // unselected leftovers and create the default collection; read-only nodes
  // never mutate; followers retain incomplete downloads. A collection that
  // cannot be opened is registered unavailable and all of its data is kept.
  std::vector<Opened> open(Role role);

  // The registered entry, which may be an unavailable placeholder.
  std::shared_ptr<Collection> get(const CollectionId& id) { return map.get(id); }
  // Announced collections in tenant and name order: all, or one tenant's.
  std::vector<Entry> entries();
  std::vector<Entry> entries(std::string_view tenant);
  // Tenants with an announced collection, in order.
  std::vector<std::string> tenants();

  // Writers. getOrCreate() creates only when `autoCreate`.
  std::shared_ptr<Collection> getOrCreate(const CollectionId& id, bool autoCreate);
  std::shared_ptr<Collection> create(const CollectionId& id, const api::SchemaDef* schema = nullptr);

  // Removes the entry and all storage. Without `mustExist`, an absent entry
  // still removes leftover storage. Refused while a candidate is staged.
  void remove(const CollectionId& id, bool mustExist = true);

  Candidate stage(const CollectionId& id, std::string_view incarnation);
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
  void retainSelected(const CollectionId& id) noexcept;
  // Registers an unavailable placeholder unless something is already registered.
  std::shared_ptr<Collection> unavailable(const CollectionId& id, std::string reason);
};

}
