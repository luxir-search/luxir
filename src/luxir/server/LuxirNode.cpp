// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/server/ReplicationCatalog.h"
#include "LuxirNode.h"
#include "ReplicationFollower.h"
#include "ReplicationState.h"
#include "Promotion.h"
#include "luxir/util/Uuid.h"
#include "luxir/schema/Schema.h"
#include "luxir/store/InputStream.h"
#include "luxir/store/Manifest.h"
#include "luxir/store/CheckedDirFactory.h"
#include "luxir/store/ReadOnlyDirectory.h"
#include "luxir/reader/Postings.h"
#include "luxir/api/padded_input.h"
#include "luxir/api/luxir_types.hpp"
#include "luxir/util/DateTime.h"
#include "luxir/util/Signal.h"

#include <algorithm>
#include <exception>
#include <memory_resource>
#include <span>

namespace luxir {

static constexpr std::string_view DELETING_REASON = "being deleted";

void LuxirNode::validateCollectionName(std::string_view name) {
  constexpr std::size_t kMaxCollectionNameBytes = 255;
  if (name.empty()) {
    throw InvalidCollectionNameError("collection name is empty");
  }
  if (name.size() > kMaxCollectionNameBytes) {
    throw InvalidCollectionNameError("collection '" + std::string(name) + "' exceeds maximum length");
  }
  if (name[0] == '_') {
    throw InvalidCollectionNameError("collection '" + std::string(name) + "' is reserved");
  }
  // Lowercase id names ([a-z][a-z0-9_]*): the name is the on-disk directory,
  // and lowercase keeps a data dir portable to case-insensitive filesystems.
  if (name[0] < 'a' || name[0] > 'z') {
    throw InvalidCollectionNameError("collection '" + std::string(name) +
                                     "' must start with a lowercase letter");
  }
  for (char c : name) {
    if ((c < 'a' || c > 'z') && (c < '0' || c > '9') && c != '_') {
      throw InvalidCollectionNameError("collection '" + std::string(name) +
          "' may only contain lowercase letters, digits, and underscores");
    }
  }
}

std::shared_ptr<Schema> Collection::updateSchema(const luxir::api::SchemaDef& def,
                                                 luxir::api::SchemaRequest_::Mode mode) {
  if (!shard->iw) throw ReadOnlyError("collection has no writer");
  return shard->iw->updateSchema([&](const Schema* current) {
    return Schema::fromProto(def, mode == api::SchemaRequest_::Mode::SET ? current : nullptr);
  });
}

void Collection::setSchema(std::shared_ptr<Schema> newSchema) {
  if (!shard->iw) throw ReadOnlyError("collection has no writer");
  shard->iw->setSchema(std::move(newSchema));
}

LuxirNode::LuxirNode(LuxirConfig config, Mode mode)
  : config(std::move(config)) {
  // A config assembled in code (tests, embedding) has not been through
  // normalize(), so resolve the RAM sentinels here too - before the writers
  // created by createSingletons() take a pointer to the budget.
  if (this->config.promote && !this->config.replication.source.empty()) throw std::invalid_argument("--promote cannot be combined with --replicate-from");
  if (this->config.promote && this->config.store.backend == "ram") throw std::invalid_argument("--promote requires the FS backend");
  this->config.resolveRamBudgets();
  auto& replicationConfig = this->config.replication;
  replicationConfig.validate();
  replication = std::make_shared<ReplicationCatalog>(std::chrono::milliseconds(
      replicationConfig.follower_timeout_ms));
  events = replication;
  indexRamBudget.setTotalBytes(this->config.index.max_ram_mb * 1024 * 1024);
  preWarmTimeZoneDatabase();
  createSingletons();
  searchEngine = std::make_unique<SearchEngine>(*this);
  if (follower && mode == Mode::SERVE) follower->start();
}

LuxirNode::~LuxirNode() {
  if (follower) follower->stop();
  replication->closeWaits();
}

std::shared_ptr<Collection> LuxirNode::getCollection(std::string_view name) {
  return getCollection(root.get(), name);
}

std::string Collection::getUnavailableReason() const {
  if (!unavailableReason.empty()) return unavailableReason;
  if (shard && shard->iw) {
    if (auto failure = shard->iw->getFailureReason()) return *failure;
  }
  return {};
}

std::shared_ptr<Collection> LuxirNode::checkLoaded(std::shared_ptr<Collection> collection) {
  if (collection) {
    auto reason = collection->getUnavailableReason();
    if (!reason.empty()) throw CollectionUnavailableError(
        "collection '" + collection->name + "' is unavailable: " + reason);
  }
  return collection;
}

std::shared_ptr<Collection> LuxirNode::getCollection(Library* library, std::string_view name) {
  Library* targetLibrary = library != nullptr ? library : root.get();
  if (targetLibrary == nullptr) {
    throw CollectionResolutionError(ErrorKind::INTERNAL, "internal", "root library is not initialized");
  }

  // Resolution is lookup-only on the hot path: an invalid name can never be in
  // the map, so the validation scan runs only on a miss, to tell "you wrote a
  // name that cannot exist" (INVALID_REQUEST) from "no such collection"
  // (NOT_FOUND) the same way every route does.
  std::string collectionName(name);
  if (auto collection = targetLibrary->collections.get(collectionName)) {
    return checkLoaded(std::move(collection));
  }

  validateCollectionName(collectionName);
  throw CollectionNotFoundError("collection '" + collectionName + "' does not exist");
}

std::shared_ptr<Collection> LuxirNode::resolveCollection(std::string_view name) {
  return getCollection(name.empty() ? kDefaultCollectionName : name);
}

std::shared_ptr<Collection> LuxirNode::getOrCreateCollection(std::string_view name) {
  return getOrCreateCollection(root.get(), name);
}

std::shared_ptr<Collection> LuxirNode::getOrCreateCollection(Library* library, std::string_view name) {
  if (following()) return getCollection(library, name);
  Library* targetLibrary = library != nullptr ? library : root.get();
  if (targetLibrary == nullptr) {
    throw CollectionResolutionError(ErrorKind::INTERNAL, "internal", "root library is not initialized");
  }

  std::string collectionName(name);
  validateCollectionName(collectionName);

  bool createdHere = false;
  auto collection = targetLibrary->collections.getOrCreate(collectionName, [&]() -> std::shared_ptr<Collection> {
    if (!config.ingest.auto_create_collection) {
      return nullptr;
    }
    auto created = initCollection(collectionName);
    createdHere = true;
    LOG_INFO("Created collection: {}", collectionName);
    return created;
  });
  if (!collection) {
    throw CollectionNotFoundError("collection '" + collectionName + "' does not exist");
  }
  if (createdHere) events->registered(collectionName, collection);
  return checkLoaded(std::move(collection));
}

std::shared_ptr<Collection> LuxirNode::resolveOrCreateCollection(std::string_view name) {
  return getOrCreateCollection(name.empty() ? kDefaultCollectionName : name);
}

std::vector<LuxirNode::CollectionEntry> LuxirNode::collectionEntries() {
  std::vector<CollectionEntry> entries;
  if (!root) return entries;

  using Pointer = SharedLazyMap<std::string, Collection>::Pointer;
  root->collections.dataMap.cvisit_all([&](const auto& elem) {
    if (auto* collection = std::get_if<Pointer>(&elem.second)) {
      entries.push_back({elem.first, *collection, (*collection)->getUnavailableReason()});
    }
  });
  std::sort(entries.begin(), entries.end(),
            [](const CollectionEntry& a, const CollectionEntry& b) { return a.name < b.name; });
  return entries;
}

std::map<std::string, CommitId> LuxirNode::replicationCollections() {
  std::map<std::string, CommitId> result;
  for (const auto& entry : collectionEntries()) {
    if (auto shard = entry.collection->getShard()) {
      if (auto snapshot = shard->getSnapshots().snapshot()) result.emplace(entry.name, snapshot->id);
    }
  }
  return result;
}

std::shared_ptr<Collection> LuxirNode::createCollection(
    Library* library, std::string_view name, const api::SchemaDef* schema) {
  if (following()) throw ReadOnlyError("cannot create collections on a follower");
  Library* targetLibrary = library != nullptr ? library : root.get();
  if (targetLibrary == nullptr) {
    throw CollectionResolutionError(ErrorKind::INTERNAL, "internal", "root library is not initialized");
  }

  std::string collectionName(name);
  validateCollectionName(collectionName);

  bool createdHere = false;
  std::exception_ptr createFailure;
  auto collection = targetLibrary->collections.getOrCreate(collectionName, [&]() {
    createdHere = true;
    std::shared_ptr<Collection> created;
    bool stored = false;
    try {
      auto initialSchema = Schema::createDefaultSchema();
      if (schema) initialSchema = Schema::fromProto(*schema, initialSchema.get());
      dirFactory->collection(collectionName).createExclusive();
      stored = true;
      created = initCollection(collectionName, std::move(initialSchema));
      LOG_INFO("Created collection: {}", collectionName);
      return created;
    } catch (...) {
      createFailure = std::current_exception();
      try {
        if (created && created->shard && created->shard->iw) {
          created->shard->iw->close();
        }
        if (stored) dirFactory->collection(collectionName).remove();
      } catch (const std::exception& e) {
        auto tombstone = std::make_shared<Collection>();
        tombstone->name = collectionName;
        tombstone->unavailableReason =
            "create failed and cleanup failed: " + std::string(e.what());
        return tombstone;
      } catch (...) {
        auto tombstone = std::make_shared<Collection>();
        tombstone->name = collectionName;
        tombstone->unavailableReason =
            "create failed and cleanup failed: unknown non-standard exception";
        return tombstone;
      }
      std::rethrow_exception(createFailure);
    }
  });
  if (createdHere) events->registered(collectionName, collection);
  if (createFailure) std::rethrow_exception(createFailure);
  if (!createdHere) {
    throw CollectionExistsError("collection '" + collectionName + "' already exists");
  }
  return collection;
}

void LuxirNode::deleteCollection(std::string_view name) {
  if (follower) { follower->deleteOrphan(name); return; }
  deleteLocalCollection(name);
}

void LuxirNode::deleteLocalCollection(std::string_view name) {
  if (!root) throw CollectionResolutionError(ErrorKind::INTERNAL, "internal", "root library is not initialized");

  // Empty means the request never named a collection - a malformed request,
  // not a missing collection.
  if (name.empty()) {
    throw InvalidCollectionNameError("collection name is empty");
  }

  // Otherwise lookup-only, no name validation: every map key is
  // filesystem-safe (from a validated create or a startup directory listing),
  // and a tombstoned legacy-named collection must stay deletable.
  std::string collectionName(name);
  auto collection = root->collections.get(collectionName);
  if (!collection) {
    validateCollectionName(collectionName);
    throw CollectionNotFoundError("collection '" + collectionName + "' does not exist");
  }
  if (collection->unavailableReason == DELETING_REASON) {
    throw CollectionUnavailableError(
        "collection '" + collectionName + "' is unavailable: being deleted");
  }

  auto tombstone = std::make_shared<Collection>();
  tombstone->name = collectionName;
  tombstone->unavailableReason = DELETING_REASON;
  if (!root->collections.replace(collectionName, collection, tombstone)) {
    throw CollectionUnavailableError(
        "collection '" + collectionName + "' changed while deletion started");
  }
  events->registered(collectionName, tombstone);

  try {
    if (collection->shard && collection->shard->iw) {
      collection->shard->iw->close();
    }
    if (collection->shard) collection->shard->snapshots->close();
    dirFactory->collection(collectionName).remove();
  } catch (const std::exception& e) {
    auto failed = std::make_shared<Collection>();
    failed->name = collectionName;
    failed->unavailableReason = "delete failed: " + std::string(e.what());
    if (root->collections.replace(collectionName, tombstone, failed)) events->registered(collectionName, failed);
    throw;
  } catch (...) {
    auto failed = std::make_shared<Collection>();
    failed->name = collectionName;
    failed->unavailableReason = "delete failed: unknown non-standard exception";
    if (root->collections.replace(collectionName, tombstone, failed)) events->registered(collectionName, failed);
    throw;
  }

  if (!root->collections.erase(collectionName, tombstone)) {
    throw std::runtime_error(
        "collection '" + collectionName + "' tombstone disappeared during deletion");
  }
  events->removed(collectionName);
}

std::shared_ptr<Collection> LuxirNode::initCollection(const std::string& name, std::shared_ptr<Schema> initialSchema) {
  auto storage = dirFactory->collection(name);
  auto selected = CollectionStorage::current(*dirFactory->container(name, !config.read_only));
  bool creating = !selected;
  if (creating) {
    if (storage.hasFiles() || !storage.incarnations().empty()) throw std::runtime_error("Collection files exist without CURRENT");
    if (config.read_only) throw ReadOnlyError("Collection has no CURRENT");
    selected = newUuid();
  }
  if (!creating && !config.read_only) {
    auto manifest = Manifest::load(*storage.open(*selected));
    if (manifest.bytes && manifest.generation != manifest.highestGeneration) {
      auto recovered = copySnapshot(storage, *selected, manifest);
      LOG_ERROR("Recovered collection '{}' from snapshot {} below {}; using new incarnation {}",
                name, manifest.generation, manifest.highestGeneration, recovered);
      storage.select(recovered);
      selected = std::move(recovered);
    }
  }
  auto col = makeCollection(name, creating ? storage.create(*selected) : storage.open(*selected));
  if (!creating && !Manifest::load(*col->shard->dir).bytes) throw std::runtime_error("CURRENT selects an empty index directory");
  if (config.read_only) {
    col->shard->snapshots->openLocalSnapshot();
  } else {
    col->shard->iw = std::make_shared<IndexWriter>(*col->shard->snapshots,
      std::move(initialSchema), &indexRamBudget,
      config.index.merge_factor, creating ? *selected : std::string{});
    col->shard->iw->perInverterRamBytes = (size_t)config.index.max_inverter_ram_mb * 1024 * 1024;
    col->shard->iw->pressureFlushFloorBytes = (size_t)config.index.pressure_flush_floor_mb * 1024 * 1024;
  }
  if (col->shard->snapshots->snapshot()->id.incarnation != *selected) throw std::runtime_error("Snapshot incarnation does not match CURRENT");
  if (creating) storage.select(*selected);
  if (!config.read_only) storage.retainOnly(*selected);
  observeCollection(name, *col);
  Signal::emit("collectionInitialized", col.get());
  return col;
}

std::shared_ptr<Collection> LuxirNode::makeCollection(const std::string& name, std::shared_ptr<Directory> directory) {
  auto col = std::make_shared<Collection>();
  col->name = name;
  col->shard = std::make_shared<Shard>(*col);
  col->shard->dir = std::move(directory);

  col->shard->snapshots = std::make_unique<CommitSnapshotRegistry>(*col->shard->dir,
      FilterCacheConfig{.maxBytes = config.queryCacheBytes});
  auto& snapshots = *col->shard->snapshots;
  const auto& policy = config.replication;
  snapshots.setPolicy({std::chrono::milliseconds(policy.pin_idle_timeout_ms), policy.pin_retained_bytes});
  return col;
}

void LuxirNode::observeCollection(const std::string& name, Collection& collection) {
  // The registry belongs to this collection, so the hook never outlives it.
  collection.getShard()->getSnapshots().onChange = [events = events, name, &collection]() noexcept {
    events->updated(name, collection);
  };
}

void LuxirNode::createSingletons() {
  root = std::make_shared<Library>();
  if (config.store.backend == "fs") {
    dirFactory = std::make_unique<FSDirFactory>(config.store.data_dir, config.read_only);
  } else {
    if (config.store.ram_limit_mb > UINT64_MAX / (1024 * 1024)) throw std::invalid_argument("store.ram_limit_mb is too large");
    dirFactory = std::make_unique<RAMDirFactory>(config.store.ram_limit_mb * 1024 * 1024, [weak = std::weak_ptr(root)] {
      auto library = weak.lock();
      if (!library) return false;
      std::vector<std::shared_ptr<Collection>> collections;
      using Pointer = SharedLazyMap<std::string, Collection>::Pointer;
      library->collections.dataMap.cvisit_all([&](const auto& entry) {
        if (auto col = std::get_if<Pointer>(&entry.second)) collections.push_back(*col);
      });
      std::shared_ptr<Collection> oldest;
      auto time = CommitSnapshotRegistry::Clock::time_point::max();
      for (const auto& col : collections) if (auto shard = col->getShard()) {
        auto created = shard->getSnapshots().oldestReclaimableReservation();
        if (created && *created < time) { time = *created; oldest = col; }
      }
      if (!oldest) return false;
      oldest->getShard()->getSnapshots().reclaimOldestReservation();
      return true; // A concurrent drop also warrants retrying the allocation.
    });
  }

  if (config.store.checked_dir.sync != "off") {
    auto mode = config.store.checked_dir.sync == "throw" ? CheckedDirMode::THROW : CheckedDirMode::WARN;
    dirFactory = std::make_unique<CheckedDirFactory>(std::move(dirFactory), mode);
  }

  // Outermost, so a mutation is refused before any wrapper does bookkeeping for it.
  if (config.read_only) {
    dirFactory = std::make_unique<ReadOnlyDirFactory>(std::move(dirFactory));
  }

  if (following()) {
    if (config.read_only) throw std::invalid_argument("replication.source cannot be combined with read-only");
    follower = std::make_unique<ReplicationFollower>(*this);
    return;
  }

  std::map<std::string, std::string> promotionErrors;
  // Promotion is a storage operation before any writer opens the replicas.
  // Hard links retain immutable data without copying it. CURRENT still selects
  // the old complete snapshot until the new root and directory are durable.
  if (!config.read_only && config.store.backend == "fs") {
    FSDirectory metadata(config.store.data_dir);
    if (auto binding = metadata.openFile("replication.json")) {
      if (!config.promote) throw std::runtime_error("Follower-bound data directory: use --promote to start a writer");
      auto state = ReplicationState::read(*binding);
      promotionErrors = promote(*dirFactory, metadata, state);
      if (promotionErrors.empty()) {
        LOG_INFO("promoted from follower of {}", state.source);
      } else {
        LOG_WARN("Promotion from follower of {} incomplete; restart with --promote to retry failed collections", state.source);
      }
    } else if (config.promote) {
      LOG_INFO("No follower binding; nothing to promote");
    }
  }

  // Discover existing collections from the store, or create default "main".
  auto existing = dirFactory->collections();
  bool createDefault = existing.empty();
  if (createDefault) existing.emplace_back(kDefaultCollectionName);

  for (const auto& name : existing) {
    try {
      validateCollectionName(name);
      if (auto failed = promotionErrors.find(name); failed != promotionErrors.end()) throw std::runtime_error(failed->second);
      auto storage = dirFactory->collection(name);
      if (!createDefault && !storage.current()) {
        if (!config.read_only && !storage.hasFiles() && std::ranges::all_of(storage.incarnations(), isUuid)) {
          storage.remove();
          LOG_INFO("Removed unselected collection: {}", name);
          continue;
        }
        throw std::runtime_error("Collection has no CURRENT");
      }
      auto collection = root->collections.getOrCreate(name, [&]() {
        return initCollection(name);
      });
      events->registered(name, collection);
      LOG_INFO("Loaded collection: {}", name);
    } catch (const std::exception& e) {
      // Keep the node up: register a tombstone so the name resolves to a clear
      // error instead of "does not exist", and can't be silently re-created
      // over the on-disk data.
      LOG_ERROR("Failed to load collection '{}': {}", name, e.what());
      auto tombstone = std::make_shared<Collection>();
      tombstone->name = name;
      tombstone->unavailableReason = "failed to load: " + std::string(e.what());
      events->registered(name, root->collections.getOrCreate(name, [&]() { return tombstone; }));
    }
  }
}

} // namespace luxir
