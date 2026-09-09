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

static constexpr std::string_view SCHEMA_PREFIX = "_schema_";
static constexpr std::string_view DELETING_REASON = "being deleted";

static std::string schemaFileName(uint64_t gen) {
  return std::string(SCHEMA_PREFIX) + Postings::getSortableString(gen);
}

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
  // One transaction per collection: read-current -> apply -> resolve ->
  // persist -> swap.  Without the lock, two concurrent SETs could each build
  // from the same base and the second swap would silently drop the first's
  // fields (and their persistence passes would delete each other's files).
  std::lock_guard lock(schemaMutex_);
  std::shared_ptr<Schema> newSchema;
  if (mode == luxir::api::SchemaRequest_::Mode::SET) {
    auto current = getSchema();
    newSchema = Schema::fromProto(def, current.get());
  } else {
    newSchema = Schema::fromProto(def);
  }
  setSchemaLocked(newSchema);
  return newSchema;
}

void Collection::setSchema(std::shared_ptr<Schema> newSchema) {
  std::lock_guard lock(schemaMutex_);
  setSchemaLocked(std::move(newSchema));
}

void Collection::setSchemaLocked(std::shared_ptr<Schema> newSchema) {
  newSchema->gen_ = schemaGen_.load();
  auto previous = getSchema();
  if (shard && shard->iw) {
    shard->iw->publishSchema(*newSchema, previous.get(), [&] { persistSchemaLocked(newSchema); });
  } else {
    newSchema->inheritIntroductions(previous.get(), {});
    persistSchemaLocked(newSchema);
  }
}

void Collection::persistSchemaLocked(std::shared_ptr<Schema> newSchema) {
  uint64_t gen = schemaGen_++;
  newSchema->gen_ = gen;

  // Persist the schema source def to the Directory.  The durable file IS the
  // publication point: once it is synced, restart would load this generation,
  // so the in-memory swap below must happen (and the caller must see success)
  // regardless of anything after the sync.  A failure BEFORE the sync removes
  // the staged file so a partial write can never be selected at startup.
  if (shard && shard->dir && !newSchema->sourceDef_.empty()) {
    std::string stored = newSchema->encodeStored();
    std::string fileName = schemaFileName(gen);
    try {
      auto file = shard->dir->createFile(fileName);
      OutputStream out;
      out.setFile(&*file);
      out.write(stored.data(), stored.size());
      out.close();
      shard->dir->finishFile(*file);

      std::vector<std::string> syncFiles = {fileName, "."};
      shard->dir->sync(syncFiles);
    } catch (...) {
      try {
        shard->dir->deleteFile(fileName);
      } catch (...) {
        LOG_WARN("failed to remove staged schema file {} after write failure", fileName);
      }
      throw;
    }

    // indexMutex excludes admission from pre-durable validation through this
    // atomic store. An admission after successful publication sees this schema.
    schema.store(std::move(newSchema));

    // Best-effort cleanup of older generations: the new schema is already
    // durable and published, so a cleanup failure must not fail the request
    // (loadSchema always picks the highest generation anyway).
    try {
      std::vector<Directory::FileInfo> files;
      shard->dir->listFiles(files);
      for (const auto& f : files) {
        if (f.name.starts_with(SCHEMA_PREFIX) && f.name != fileName) {
          shard->dir->deleteFile(f.name);
        }
      }
    } catch (const std::exception& e) {
      LOG_WARN("failed to clean up old schema files: {}", e.what());
    }
    return;
  }

  schema.store(std::move(newSchema));
}


bool Collection::loadSchema() {
  if (!shard || !shard->dir) return false;

  // Find the latest schema file by lexicographic order (sortable naming).
  std::string lastSchemaFile;
  std::vector<Directory::FileInfo> files;
  shard->dir->listFiles(files);
  for (const auto& f : files) {
    if (f.name.starts_with(SCHEMA_PREFIX)) {
      if (f.name > lastSchemaFile) lastSchemaFile = f.name;
    }
  }

  while (!lastSchemaFile.empty()) {
    auto file = shard->dir->openFile(lastSchemaFile, true);
    if (file) {
      InputStream is = file->getInputStream();
      std::span<const char> bytes((const char*)is.ptr(), is.left());
      std::shared_ptr<Schema> newSchema;
      try {
        newSchema = Schema::decodeStored(std::as_bytes(bytes));
      } catch (const std::runtime_error& e) {
        throw std::runtime_error("Failed to parse schema file: " + lastSchemaFile + ": " + e.what());
      }

      // Parse the gen back from the sortable filename suffix
      auto genStr = std::string_view(lastSchemaFile).substr(SCHEMA_PREFIX.size());
      uint64_t gen = Postings::parseSortableString(genStr);

      newSchema->gen_ = gen;
      schemaGen_ = gen + 1;  // next setSchema will use gen+1
      schema.store(std::move(newSchema));
      return true;
    }

    // File was deleted by a concurrent setSchema - rescan for the latest.
    lastSchemaFile.clear();
    files.clear();
    shard->dir->listFiles(files);
    for (const auto& f : files) {
      if (f.name.starts_with(SCHEMA_PREFIX)) {
        if (f.name > lastSchemaFile) lastSchemaFile = f.name;
      }
    }
  }

  return false;
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

std::shared_ptr<Collection> LuxirNode::checkLoaded(std::shared_ptr<Collection> collection) {
  if (collection && !collection->unavailableReason.empty()) {
    throw CollectionUnavailableError(
        "collection '" + collection->name + "' is unavailable: " +
        collection->unavailableReason);
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
      entries.push_back({elem.first, *collection, (*collection)->unavailableReason});
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
    try {
      created = initCollection(collectionName);
      if (schema != nullptr) {
        created->updateSchema(*schema, api::SchemaRequest_::Mode::SET);
      }
      LOG_INFO("Created collection: {}", collectionName);
      return created;
    } catch (...) {
      createFailure = std::current_exception();
      try {
        if (created && created->shard && created->shard->iw) {
          created->shard->iw->close();
        }
        dirFactory->remove(collectionName);
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

std::shared_ptr<Collection> LuxirNode::initCollection(const std::string& name) {
  auto col = std::make_shared<Collection>();
  col->name = name;
  col->shard = std::make_shared<Shard>(*col);
  col->shard->dir = dirFactory->create(name);

  // Set default schema initially (without persisting, gen=0 means not yet persisted)
  auto defaultSchema = Schema::createDefaultSchema();
  defaultSchema->gen_ = 0;
  col->schema.store(std::move(defaultSchema));

  // Pass a schemaProvider that fetches the schema from the Collection
  auto* colPtr = col.get();
  col->shard->iw = std::make_shared<IndexWriter>(*col->shard->dir,
    [colPtr]() { return colPtr->getSchema(); }, &indexRamBudget,
    FilterCacheConfig{.maxBytes = config.queryCacheBytes}, config.index.merge_factor);
  col->shard->iw->perInverterRamBytes = (size_t)config.index.max_inverter_ram_mb * 1024 * 1024;
  col->shard->iw->pressureFlushFloorBytes = (size_t)config.index.pressure_flush_floor_mb * 1024 * 1024;

  // Load the latest persisted schema if one exists.
  col->loadSchema();
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
