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
class Scrubber;

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
  // Checksum scrub of published snapshots; runs periodically only when
  // store.scrub-interval-s is set, but scrub() works on demand regardless.
  Scrubber& getScrubber() { return *scrubber; }
  bool following() const { return !config.replication.source.empty(); }
  ReplicationSource& getReplication() { return *replication; }
  CommitWaits& getCommitWaits() { return *commitWaits; }
  Collections& collections() { return *collections_; }

  // True when this node serves a data directory it does not own (no write lock).
  // The transports use it to refuse mutating requests up front; the actual
  // guarantee is enforced by ReadOnlyDirectory, not by this flag.
  bool readOnly() const { return config.read_only; }

  uint64_t storageBytes() const { return collections_->storage().storageBytes(); }
  uint64_t storageBytes(const CollectionId& id) const { return collections_->storage().collectionBytes(id); }

  // A request's target: an empty tenant is the default tenant (the request
  // scope supplies it), and an empty name the default collection.
  static CollectionId target(std::string_view tenant, std::string_view name);
  // An administration target: the tenant defaults, the name never does.
  static CollectionId adminTarget(std::string_view tenant, std::string_view name) {
    return {std::string(tenant.empty() ? CollectionId::kDefaultTenant : tenant), std::string(name)};
  }

  // Resolution: an unavailable collection throws CollectionUnavailableError.
  // Requests resolve a complete identity built at ingress with target(); the
  // name-only overloads address the default tenant for embedding and tests.
  std::shared_ptr<Collection> getCollection(const CollectionId& id);
  std::shared_ptr<Collection> getCollection(std::string_view name) { return getCollection(CollectionId::of(name)); }
  std::shared_ptr<Collection> getOrCreateCollection(const CollectionId& id);
  std::shared_ptr<Collection> getOrCreateCollection(std::string_view name) { return getOrCreateCollection(CollectionId::of(name)); }

  // Snapshot fully-created collections without waiting for creations in
  // flight. Unavailable placeholders retain their recorded error.
  std::vector<CollectionEntry> collectionEntries() { return collections_->entries(); }
  std::vector<CollectionEntry> collectionEntries(std::string_view tenant) { return collections_->entries(tenant); }
  std::vector<std::string> tenants() { return collections_->tenants(); }

  std::shared_ptr<Collection> createCollection(const CollectionId& id, const api::SchemaDef* schema = nullptr);
  std::shared_ptr<Collection> createCollection(std::string_view name, const api::SchemaDef* schema = nullptr) {
    return createCollection(CollectionId::of(name), schema);
  }
  void deleteCollection(const CollectionId& id);
  void deleteCollection(std::string_view name) { deleteCollection(CollectionId::of(name)); }

  oneapi::tbb::task_arena& getTaskArena() {
    return taskArena;
  }

  // Runs f on the calling thread inside the task arena, in one of the slots
  // reserved for threads entering from outside.  Tasks f spawns are stealable
  // by the arena's workers, and f's waits run only tasks spawned within f
  // (isolation): a transport thread never picks up a merge or another
  // request's work while its connections wait.
  template <typename F>
  void executeIsolated(const F& f) {
    taskArena.execute([&] { oneapi::tbb::this_task_arena::isolate(f); });
  }

  // Counts a transport server's threads against the task arena's reserved
  // slots for the holder's lifetime.  Warns when entrants outnumber the slots
  // (servers run with more threads than the config the node was built from):
  // the excess take worker slots, or when none is free hand their work to the
  // arena and block behind its backlog.
  class ArenaEntrants {
    LuxirNode& node;
    int n;
  public:
    ArenaEntrants(LuxirNode& node, int n, std::string_view server);
    ~ArenaEntrants() { node.arenaEntrants.fetch_sub(n, std::memory_order_relaxed); }
    ArenaEntrants(const ArenaEntrants&) = delete;
    ArenaEntrants& operator=(const ArenaEntrants&) = delete;
  };

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
  std::unique_ptr<Scrubber> scrubber;
  std::unique_ptr<SearchEngine> searchEngine;

  // Query and index work.  Workers: the affinity width less one, as a default
  // arena has, but at least one slot even on a single CPU: an arena of only
  // reserved slots never serves enqueued tasks.  Plus a reserved slot per
  // thread that enters from outside (each configured HTTP shard and gRPC
  // thread, and the embedding thread), so an execute() from a transport thread
  // always finds a free slot instead of handing its functor to the arena and
  // blocking behind arena backlog.  Reserved slots are seats, not threads:
  // they add no workers.  Workers that run out of work leave at once
  // (leave_policy::fast) instead of yielding for up to a millisecond first:
  // with request-sized fan-outs the linger burned far more CPU than the work.
  int reservedSlots;
  std::atomic<int> arenaEntrants{1};  // the embedding thread; see ArenaEntrants
  oneapi::tbb::task_arena taskArena;

  // Labels arena worker threads for ps/top/gdb. Masters (transport threads
  // executing in the arena) keep their own names. Declared after taskArena:
  // observation detaches before the arena tears down.
  class ArenaThreadNamer : public oneapi::tbb::task_scheduler_observer {
    int reservedSlots;  // workers occupy the slots after these
  public:
    ArenaThreadNamer(oneapi::tbb::task_arena& arena, int reservedSlots)
      : oneapi::tbb::task_scheduler_observer(arena), reservedSlots(reservedSlots) {
      observe(true);
    }
    void on_scheduler_entry(bool worker) override {
      if (!worker) return;
      char name[16];
      snprintf(name, sizeof(name), "luxir_tbb_%d",
               oneapi::tbb::this_task_arena::current_thread_index() - reservedSlots);
      nameThisThread(name);
    }
  };
  ArenaThreadNamer arenaThreadNamer{taskArena, reservedSlots};
};

}
