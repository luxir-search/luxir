// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <memory>
#include <string>
#include <vector>
#include "luxir/util/ApiError.h"
#include "luxir/store/Directory.h"
#include "luxir/index/IndexWriter.h"
#include "luxir/store/CollectionId.h"

namespace luxir {

class Collection;
class Collections;

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
  friend class Collections;
};

class Schema;

namespace api { struct SchemaDef; }
namespace api::SchemaRequest_ { enum class Mode; }

// A single logical collection of docs which may
// consist of multiple shards.
class Collection {
  CollectionId id;
  std::string unavailableReason;  // non-empty means resolution rejects the collection
  std::shared_ptr<Shard> shard;
  std::vector<std::shared_ptr<Shard>> shards;
public:
  const CollectionId& getId() const { return id; }
  std::string getUnavailableReason() const;
  // An unavailable entry: a failed load, delete or create, or a deletion in
  // progress. Resolution rejects it with `reason`.
  static std::shared_ptr<Collection> placeholder(CollectionId id, std::string reason);

  std::shared_ptr<Shard> getShard() const {
    return shard;
  }

  ReaderManager& getReaderManager() const { return shard->getReaderManager(); }

  // Returns the latest published schema for administration and stats.
  std::shared_ptr<Schema> getSchema() {
    return getReaderManager().getSchema();
  }

  // The schema mutation transaction: applies `def` to the current schema
  // (SET) or replaces it (REPLACE_ALL), persists, swaps, and returns the
  // published snapshot. All external schema mutation (gRPC, HTTP) goes through
  // here; concurrent calls serialize per collection.
  std::shared_ptr<const CommitSnapshot> updateSchema(const luxir::api::SchemaDef& def,
                                       luxir::api::SchemaRequest_::Mode mode);

  // Publishes a copy of this schema over the last committed physical snapshot.
  void setSchema(std::shared_ptr<Schema> newSchema);


  friend class Collections;
};

}
