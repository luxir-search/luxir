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
#include "Collections.h"
#include "luxir/index/IndexRamBudget.h"
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
class LuxirNode;
class ReplicationSource;
class CommitWaits;
class ReplicationFollower;

class LuxirNode {
public:
  static constexpr std::string_view kDefaultCollectionName = Collections::kDefaultCollectionName;
  using CollectionEntry = Collections::Entry;

  LuxirNode() : LuxirNode(LuxirConfig{}) {}
  enum class Mode { SERVE, PULL };
  explicit LuxirNode(LuxirConfig config, Mode mode = Mode::SERVE);
  ~LuxirNode();

  const LuxirConfig& getConfig() const { return config; }
  ReplicationFollower* getFollower() { return follower.get(); }
  bool following() const { return !config.replication.source.empty(); }
  ReplicationSource& getReplication() { return *replication; }
  CommitWaits& getCommitWaits() { return *commitWaits; }
  Collections& collections() { return *collections_; }

  // True when this node serves a data directory it does not own (no write lock).
  // The transports use it to refuse mutating requests up front; the actual
  // guarantee is enforced by ReadOnlyDirectory, not by this flag.
  bool readOnly() const { return config.read_only; }

  uint64_t storageBytes(std::string_view collection = {}) const { return collections_->storage().storageBytes(collection); }

  // Resolution: an unavailable collection throws CollectionUnavailableError.
  std::shared_ptr<Collection> getCollection(std::string_view name);
  std::shared_ptr<Collection> getOrCreateCollection(std::string_view name);
  // As above, with an empty name selecting the default collection.
  std::shared_ptr<Collection> resolveCollection(std::string_view name);
  std::shared_ptr<Collection> resolveOrCreateCollection(std::string_view name);

  // Snapshot fully-created collections without waiting for creations in
  // flight. Unavailable placeholders retain their recorded error.
  std::vector<CollectionEntry> collectionEntries() { return collections_->entries(); }

  std::shared_ptr<Collection> createCollection(std::string_view name, const api::SchemaDef* schema = nullptr);
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
  // Returns the collection unchanged, or throws if it is unavailable.
  static std::shared_ptr<Collection> checkLoaded(std::shared_ptr<Collection> collection);

  LuxirConfig config;
  IndexRamBudget indexRamBudget;
  std::shared_ptr<ReplicationSource> replication;
  std::shared_ptr<CommitWaits> commitWaits;
  std::shared_ptr<CollectionEvents> events; // ordered feed of the collection map
  std::unique_ptr<Collections> collections_;
  std::unique_ptr<ReplicationFollower> follower;
  std::unique_ptr<SearchEngine> searchEngine;

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
