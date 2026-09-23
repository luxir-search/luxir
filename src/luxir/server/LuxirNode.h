// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include "luxir/util/ApiError.h"
#include "luxir/store/Directory.h"
#include "luxir/store/DirectoryFactory.h"
#include "luxir/index/IndexRamBudget.h"
#include "luxir/index/IndexWriter.h"
#include "oneapi/tbb/task_arena.h"
#include "oneapi/tbb/task_scheduler_observer.h"
#include "luxir/search/SearchEngine.h"
#include "luxir/LuxirConfig.h"
#include "luxir/util/SharedLazyMap.h"
#include "luxir/util/thread.h"

namespace luxir {

/// There should normally be a single LuxirNode instance per process.
/// A single LuxirNode can host many indexes.
/// There still *may* be multiple LuxirNode instances per process, but it's currently more for testing.

class SearchEngine;
class Library;
class Collection;
class LuxirNode;
class ReplicationCatalog;
class ReplicationFollower;

// Resolving a request's collection target failed.  The concrete subclasses fix
// the classification; the base is thrown directly only for internal invariants
// and requires an explicit kind.
class CollectionResolutionError : public ApiError {
public:
  using ApiError::ApiError;
};

// The name cannot denote a collection on any route (INVALID_REQUEST).
class InvalidCollectionNameError : public CollectionResolutionError {
public:
  explicit InvalidCollectionNameError(const std::string& message)
    : CollectionResolutionError(ErrorKind::INVALID_REQUEST, "invalid_collection_name", message) {}
};

class CollectionNotFoundError : public CollectionResolutionError {
public:
  explicit CollectionNotFoundError(const std::string& message)
    : CollectionResolutionError(ErrorKind::NOT_FOUND, "collection_not_found", message) {}
};

// The collection exists but cannot serve: being deleted, a delete failed
// partway, or it failed to load at startup (UNAVAILABLE).
class CollectionUnavailableError : public CollectionResolutionError {
public:
  explicit CollectionUnavailableError(const std::string& message)
    : CollectionResolutionError(ErrorKind::UNAVAILABLE, "collection_unavailable", message) {}
};

class CollectionExistsError : public ApiError {
public:
  explicit CollectionExistsError(const std::string& message)
    : ApiError(ErrorKind::ALREADY_EXISTS, "collection_exists", message) {}
};

class Shard {
  Collection& collection; // hard reference to the collection that owns this shard
  std::shared_ptr<Directory> dir;  // does this need to be shared_ptr?  Perhaps not if we have a shared ptr to a parent object (Shard or Collection?)
  std::unique_ptr<CommitSnapshotRegistry> snapshots;
  std::shared_ptr<IndexWriter> iw; // absent on read-only collections

public:
  explicit Shard(Collection& collection) : collection(collection) {
    unused(this->collection);
    // dir = std::make_shared<RAMDir>();
    // iw = std::make_shared<IndexWriter>(*dir);
  }
  // TODO: we don't want to be in the position of having more than ine IW pointing at the index/dir... this suggests that instead of
  // having the ability for it to come and go, it should be a singleton (which could still be created on demand) that
  // can dump most of it's state for low memory usage?  Then this method would not return a shared_ptr, but a simple reference.
  std::shared_ptr<IndexWriter> getIndexWriter() {
    return iw;
  }

  ~Shard() {
    if (iw) iw->close();
    if (snapshots) snapshots->close();
  }
  ReaderManager& getReaderManager() const { return snapshots->readers; }
  CommitSnapshotRegistry& getSnapshots() const { return *snapshots; }
  std::shared_ptr<IndexWriter> requireIndexWriter() {
    if (!iw) throw ReadOnlyError("collection has no index writer");
    return iw;
  }

  std::shared_ptr<Directory> getDirectory() {
    return dir;
  }

  // TODO: if a shard isn't currently "loaded", should it be removed from the map, or just it's size cut down?

  friend class Collection;
  friend class Library;
  friend class LuxirNode;
  friend class ReplicationFollower;
};

class Schema;

namespace api { struct SchemaDef; }
namespace api::SchemaRequest_ { enum class Mode; }

// A single logical collection of docs which may
// consist of multiple shards.
class Collection {
  std::string name;
  std::string unavailableReason;  // non-empty means resolution rejects the collection
  std::shared_ptr<Shard> shard;
  std::vector<std::shared_ptr<Shard>> shards;
public:
  std::string getUnavailableReason() const;

  std::shared_ptr<Shard> getShard() {
    return shard;
  }

  ReaderManager& getReaderManager() const { return shard->getReaderManager(); }

  // Returns the latest published schema for administration and stats.
  std::shared_ptr<Schema> getSchema() {
    return getReaderManager().getSchema();
  }

  // The schema mutation transaction: applies `def` to the current schema
  // (SET) or replaces it (REPLACE_ALL), persists, swaps, and returns the
  // installed schema.  All external schema mutation (gRPC, HTTP) goes through
  // here; concurrent calls serialize per collection.
  std::shared_ptr<Schema> updateSchema(const luxir::api::SchemaDef& def,
                                       luxir::api::SchemaRequest_::Mode mode);

  // Publishes a copy of this schema over the last committed physical snapshot.
  void setSchema(std::shared_ptr<Schema> newSchema);


  friend class Library;
  friend class LuxirNode;
  friend class ReplicationFollower;
};


/// A library can contain multiple document collections.  It is meant to be the natural
/// implementation of a tenant in a multi-tenant system.
class Library {
public:
  // TODO: schemas (changes from global schemas)
  // TODO: defaults (changes from global)
  // IDEA: for even better multi-tenant support, the ability to arbitrarily nest libraries?  Akin to the ability of a VM to run in a VM.
  //       and in the grpc, rather than distinguish between library and collection, we could just introduce the notion
  //       of a fully qualified index name, represented as an array of string.

private:
  std::string name;
  SharedLazyMap<std::string, Collection> collections;
  friend class LuxirNode;
  friend class ReplicationFollower;
};



class LuxirNode {
  std::shared_ptr<ReplicationCatalog> replication;
  std::unique_ptr<ReplicationFollower> follower;
  friend class ReplicationFollower;
public:
  ReplicationFollower* getFollower() { return follower.get(); }
  bool following() const { return !config.replication.source.empty(); }
  ReplicationCatalog& getReplication() { return *replication; }
  static constexpr std::string_view kDefaultCollectionName = "main";

  struct CollectionEntry {
    std::string name;
    std::shared_ptr<Collection> collection;
    std::string error;
  };

  LuxirNode() : LuxirNode(LuxirConfig{}) {}
  enum class Mode { SERVE, PULL };
  explicit LuxirNode(LuxirConfig config, Mode mode = Mode::SERVE);
  ~LuxirNode();

  const LuxirConfig& getConfig() const { return config; }

  // True when this node serves a data directory it does not own (no write lock).
  // The transports use it to refuse mutating requests up front; the actual
  // guarantee is enforced by ReadOnlyDirectory, not by this flag.
  bool readOnly() const { return config.read_only; }

  // ALTERNATIVE: instead of nested maps, we could also have a single map directly to Collection or Shard objects
  // and represent metadata in the hierarchy.  This choice needs to be informed by the external representation
  // of collections.

  uint64_t storageBytes(std::string_view collection = {}) const { return dirFactory->storageBytes(collection); }

  std::shared_ptr<Collection> getCollection(std::string_view name);
  std::shared_ptr<Collection> getOrCreateCollection(std::string_view name);

  std::shared_ptr<Collection> resolveCollection(std::string_view name);
  std::shared_ptr<Collection> resolveOrCreateCollection(std::string_view name);

  // Snapshot fully-created root collections without waiting for creations in
  // flight. Unavailable tombstones retain their recorded error.
  std::vector<CollectionEntry> collectionEntries();
  std::map<std::string, CommitId> replicationCollections();

  std::shared_ptr<Collection> getCollection(Library* library, std::string_view name);
  std::shared_ptr<Collection> getOrCreateCollection(Library* library, std::string_view name);

  std::shared_ptr<Library> createLibrary(std::string_view name) {
    unused(name);
    return {};
  }

  std::shared_ptr<Collection> createCollection(
      Library* library, std::string_view name, const api::SchemaDef* schema = nullptr);
  void deleteCollection(std::string_view name);

  oneapi::tbb::task_arena& getTaskArena() {
    return taskArena;
  }

  IndexRamBudget& getIndexRamBudget() {
    return indexRamBudget;
  }

  SearchEngine& getSearchEngine() {
    return *searchEngine;
  }


private:

  void createSingletons();
  void observeCollection(const std::string& name, Collection& collection);
  void deleteLocalCollection(std::string_view name);
  std::shared_ptr<Collection> makeCollection(const std::string& name, std::shared_ptr<Directory> directory);
  std::shared_ptr<Collection> initCollection(const std::string& name, std::shared_ptr<Schema> initialSchema = {},
                                             std::shared_ptr<Directory> directory = {});
  // Returns the collection unchanged, or throws if it is unavailable.
  static std::shared_ptr<Collection> checkLoaded(std::shared_ptr<Collection> collection);
  static void validateCollectionName(std::string_view name);

  LuxirConfig config;
  IndexRamBudget indexRamBudget;
  std::unique_ptr<SearchEngine> searchEngine;
  std::shared_ptr<Library> root;
  std::unique_ptr<DirectoryFactory> dirFactory;

  oneapi::tbb::task_arena taskArena;

  // Labels arena worker threads for ps/top/gdb. Masters (transport threads
  // executing in the arena) keep their own names. Declared after taskArena:
  // observation detaches before the arena tears down.
  class ArenaThreadNamer : public oneapi::tbb::task_scheduler_observer {
  public:
    explicit ArenaThreadNamer(oneapi::tbb::task_arena& arena)
      : oneapi::tbb::task_scheduler_observer(arena) {
      observe(true);
    }
    void on_scheduler_entry(bool worker) override {
      if (!worker) return;
      char name[16];
      snprintf(name, sizeof(name), "luxir_tbb_%d",
               oneapi::tbb::this_task_arena::current_thread_index());
      nameThisThread(name);
    }
  };
  ArenaThreadNamer arenaThreadNamer{taskArena};
};

}
