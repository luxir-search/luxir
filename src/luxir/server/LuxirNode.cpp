// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "LuxirNode.h"
#include "luxir/schema/Schema.h"
#include "luxir/store/InputStream.h"
#include "luxir/store/CheckedDirFactory.h"
#include "luxir/store/ReadOnlyDirectory.h"
#include "luxir/reader/Postings.h"
#include "luxir/api/padded_input.h"
#include "luxir/api/luxir_types.hpp"
#include "luxir/util/DateTime.h"

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

LuxirNode::LuxirNode(LuxirConfig config)
  : config(std::move(config)) {
  // A config assembled in code (tests, embedding) has not been through
  // normalize(), so resolve the RAM sentinels here too - before the writers
  // created by createSingletons() take a pointer to the budget.
  this->config.resolveRamBudgets();
  indexRamBudget.setTotalBytes(this->config.index.max_ram_mb * 1024 * 1024);
  preWarmTimeZoneDatabase();
  createSingletons();
  searchEngine = std::make_unique<SearchEngine>(*this);
}

LuxirNode::~LuxirNode() {
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
  Library* targetLibrary = library != nullptr ? library : root.get();
  if (targetLibrary == nullptr) {
    throw CollectionResolutionError(ErrorKind::INTERNAL, "internal", "root library is not initialized");
  }

  std::string collectionName(name);
  validateCollectionName(collectionName);

  auto collection = targetLibrary->collections.getOrCreate(collectionName, [&]() -> std::shared_ptr<Collection> {
    if (!config.ingest.auto_create_collection) {
      return nullptr;
    }
    auto created = initCollection(collectionName);
    LOG_INFO("Created collection: {}", collectionName);
    return created;
  });
  if (!collection) {
    throw CollectionNotFoundError("collection '" + collectionName + "' does not exist");
  }
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

std::shared_ptr<Collection> LuxirNode::createCollection(
    Library* library, std::string_view name, const api::SchemaDef* schema) {
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
    std::shared_ptr<Directory> directory;
    try {
      auto initialSchema = Schema::createDefaultSchema();
      if (schema) initialSchema = Schema::fromProto(*schema, initialSchema.get());
      directory = dirFactory->create(collectionName, true);
      created = initCollection(collectionName, std::move(initialSchema), directory);
      LOG_INFO("Created collection: {}", collectionName);
      return created;
    } catch (...) {
      createFailure = std::current_exception();
      try {
        if (created && created->shard && created->shard->iw) {
          created->shard->iw->close();
        }
        if (directory) dirFactory->remove(collectionName);
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
  if (createFailure) std::rethrow_exception(createFailure);
  if (!createdHere) {
    throw CollectionExistsError("collection '" + collectionName + "' already exists");
  }
  return collection;
}

void LuxirNode::deleteCollection(std::string_view name) {
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

  try {
    if (collection->shard && collection->shard->iw) {
      collection->shard->iw->close();
    }
    if (collection->shard) collection->shard->snapshots->close();
    dirFactory->remove(collectionName);
  } catch (const std::exception& e) {
    auto failed = std::make_shared<Collection>();
    failed->name = collectionName;
    failed->unavailableReason = "delete failed: " + std::string(e.what());
    root->collections.replace(collectionName, tombstone, std::move(failed));
    throw;
  } catch (...) {
    auto failed = std::make_shared<Collection>();
    failed->name = collectionName;
    failed->unavailableReason = "delete failed: unknown non-standard exception";
    root->collections.replace(collectionName, tombstone, std::move(failed));
    throw;
  }

  if (!root->collections.erase(collectionName, tombstone)) {
    throw std::runtime_error(
        "collection '" + collectionName + "' tombstone disappeared during deletion");
  }
}

std::shared_ptr<Collection> LuxirNode::initCollection(const std::string& name, std::shared_ptr<Schema> initialSchema,
                                                       std::shared_ptr<Directory> directory) {
  auto col = std::make_shared<Collection>();
  col->name = name;
  col->shard = std::make_shared<Shard>(*col);
  col->shard->dir = directory ? std::move(directory) : dirFactory->create(name);

  col->shard->snapshots = std::make_unique<CommitSnapshotRegistry>(*col->shard->dir,
      FilterCacheConfig{.maxBytes = config.queryCacheBytes});
  if (config.read_only) {
    col->shard->snapshots->openLocalSnapshot();
    return col;
  }
  col->shard->iw = std::make_shared<IndexWriter>(*col->shard->snapshots,
    std::move(initialSchema), &indexRamBudget,
    config.index.merge_factor);
  col->shard->iw->perInverterRamBytes = (size_t)config.index.max_inverter_ram_mb * 1024 * 1024;
  col->shard->iw->pressureFlushFloorBytes = (size_t)config.index.pressure_flush_floor_mb * 1024 * 1024;

  return col;
}

void LuxirNode::createSingletons() {
  if (config.store.backend == "fs") {
    dirFactory = std::make_unique<FSDirFactory>(config.store.data_dir, config.read_only);
  } else {
    dirFactory = std::make_unique<RAMDirFactory>();
  }

  if (config.store.checked_dir.sync != "off") {
    auto mode = config.store.checked_dir.sync == "throw" ? CheckedDirMode::THROW : CheckedDirMode::WARN;
    dirFactory = std::make_unique<CheckedDirFactory>(std::move(dirFactory), mode);
  }

  // Outermost, so a mutation is refused before any wrapper does bookkeeping for it.
  if (config.read_only) {
    dirFactory = std::make_unique<ReadOnlyDirFactory>(std::move(dirFactory));
  }

  root = std::make_shared<Library>();

  // Discover existing collections from the store, or create default "main".
  auto existing = dirFactory->listCollections();
  if (existing.empty()) {
    existing.emplace_back(kDefaultCollectionName);
  }

  for (const auto& name : existing) {
    try {
      validateCollectionName(name);
      root->collections.getOrCreate(name, [&]() {
        return initCollection(name);
      });
      LOG_INFO("Loaded collection: {}", name);
    } catch (const std::exception& e) {
      // Keep the node up: register a tombstone so the name resolves to a clear
      // error instead of "does not exist", and can't be silently re-created
      // over the on-disk data.
      LOG_ERROR("Failed to load collection '{}': {}", name, e.what());
      auto tombstone = std::make_shared<Collection>();
      tombstone->name = name;
      tombstone->unavailableReason = "failed to load: " + std::string(e.what());
      root->collections.getOrCreate(name, [&]() { return tombstone; });
    }
  }
}

} // namespace luxir
