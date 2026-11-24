// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include "ArenaNice.h"
#include "Collections.h"
#include "ControlExecutor.h"
#include "PluginNodeState.h"
#include "luxir/index/IndexRamBudget.h"
#include "oneapi/tbb/task_arena.h"
#include "oneapi/tbb/task_scheduler_observer.h"
#include "luxir/search/SearchEngine.h"
#include "luxir/LuxirConfig.h"
#include "luxir/util/Metrics.h"
#include "luxir/util/SharedLazyMap.h"
#include "luxir/util/thread.h"

namespace luxir {

// A node hosts many collections. A process may run several independent nodes.

class SearchEngine;
class LuxirNode;
class ReplicationSource;
class CommitWaits;
class ReplicationFollower;
class Scrubber;

class LuxirNode {
public:
  // Shared with retained collections so instrumentation outlives their work.
  const std::shared_ptr<Metrics> metrics = std::make_shared<Metrics>();
  static constexpr std::string_view kDefaultCollectionName = Collections::kDefaultCollectionName;
  using CollectionEntry = Collections::Entry;

  using Clock = std::chrono::steady_clock;
  using Now = std::function<Clock::time_point()>;

  LuxirNode() : LuxirNode(LuxirConfig{}) {}
  enum class Mode { SERVE, PULL };
  // `clock` times replication: commit waits, follower liveness and sync retries.
  explicit LuxirNode(LuxirConfig config, Mode mode = Mode::SERVE, Now clock = Clock::now);
  ~LuxirNode();

  const LuxirConfig& getConfig() const { return config; }
  const std::string& nodeId() const { return config.node.id; }
  const std::string& nodeLifetime() const { return lifetime; }
  Mode getMode() const { return mode; }
  // Null for a missing state or a different concrete state type.
  template <class T>
  T* pluginState(std::string_view pluginName) const {
    return dynamic_cast<T*>(pluginStates.find(pluginName));
  }
  Clock::time_point now() const { return clock(); }
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
  uint64_t storageBytes(const CollectionId& id) const { return collections_->storageBytes(id); }

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
  void renameCollection(const CollectionId& from, const CollectionId& to);
  void swapCollections(const CollectionId& a, const CollectionId& b);
  void deleteCollection(std::string_view name) { deleteCollection(CollectionId::of(name)); }
  void detachCollection(const CollectionId& id, std::string_view target);
  void detachCollection(std::string_view name, std::string_view target) { detachCollection(CollectionId::of(name), target); }
  std::shared_ptr<Collection> attachCollection(const CollectionId& id, bool force = false);
  std::shared_ptr<Collection> attachCollection(std::string_view name, bool force = false) { return attachCollection(CollectionId::of(name), force); }

  oneapi::tbb::task_arena& getTaskArena() {
    return taskArena;
  }
  oneapi::tbb::task_arena& getIndexArena() { return indexArena; }
  // Administration, collection lifecycle, schema changes, replication
  // transfers and timers, and detailed stats run off the search arena.
  ControlExecutor& getControlExecutor() { return *control; }

  // The task arena's worker threads (server.workers, resolved).
  int getWorkers() const { return workers; }
  // Workers the indexing arena may use at once (index.threads, resolved).
  int getIndexThreads() const { return indexThreads; }
  // Pool workers currently in each arena.
  int64_t getQueryWorkersActive() const { return arenaThreadNamer.activeWorkers(); }
  int64_t getIndexWorkersActive() const { return indexThreadNamer.activeWorkers(); }

  // Pool workers past the query arena's width.  TBB fills arenas in priority
  // order but never past an arena's own worker cap (market::update_allotment),
  // so whenever background arenas have work they share these workers however
  // busy the query arena is: indexing keeps at least one worker under any
  // search load, and never stalls behind it.  The reverse needs no floor: a
  // query hop finds a worker at once, because indexing is capped below the
  // pool (index.threads), and further workers are recalled from indexing at
  // task boundaries.  The floor is per process: nodes embedded side by side
  // share one, and so does any other background arena.  Holding it as pool
  // width, not a reserved arena slot (slots are seats for entering threads, not
  // workers), is what keeps it out of the query arena's reach.
  static constexpr int kBackgroundFloor = 1;
  // Nice increment applied to workers running indexing work; 0 = off.
  int getIndexNice() const { return indexNiceIncrement; }

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
  std::string lifetime;
  const Mode mode;
  Now clock;

  // Query work.  Workers: server.workers (resolveWorkers(); at least
  // one, since an arena of only reserved slots never serves enqueued tasks).
  // Plus a reserved slot per thread that enters from outside (each configured
  // HTTP shard and the embedding thread; gRPC threads only enqueue), so an
  // execute() from a transport thread always finds a free slot instead of handing its
  // functor to the arena and blocking behind arena backlog.  Reserved slots
  // are seats, not threads: they add no workers.  Workers that run out of
  // work leave at once (leave_policy::fast) instead of yielding for up to a
  // millisecond first: with request-sized fan-outs the linger burned far more
  // CPU than the work.  The arenas are declared ahead of the collections and
  // the search engine so they outlive the work those run in them.
  int workers;
  int indexThreads;
  int controlThreads;
  int reservedSlots;
  std::atomic<int> arenaEntrants{1};  // the embedding thread; see ArenaEntrants
  oneapi::tbb::task_arena taskArena;

  // Labels arena worker threads for ps/top/gdb. Masters (transport threads
  // executing in the arena) keep their own names. Declared after taskArena:
  // observation detaches before the arena tears down. Also pins FAISS's
  // OpenMP to one thread on every thread that enters the arena, masters
  // included: build and query parallelism comes from TBB tasks alone.
  class ArenaThreadNamer : public oneapi::tbb::task_scheduler_observer {
    const char* prefix;
    int reservedSlots;  // workers occupy the slots after these
    std::atomic<int64_t> active{0};  // workers in the arena
    // apply() installs startup flags before node construction. Capture enable
    // for this observer's lifetime so its entries and exits always pair.
    std::unique_ptr<LoadRegistry::ArenaActivity> loadActivity;
  public:
    ArenaThreadNamer(oneapi::tbb::task_arena& arena, const char* prefix, int reservedSlots, bool trackLoad)
      : oneapi::tbb::task_scheduler_observer(arena), prefix(prefix), reservedSlots(reservedSlots) {
      if (trackLoad && LoadRegistry::enabled.load(std::memory_order_relaxed)) {
        loadActivity = std::make_unique<LoadRegistry::ArenaActivity>(LoadRegistry::global());
      }
      observe(true);
    }
    ~ArenaThreadNamer() override { observe(false); }
    int64_t activeWorkers() const { return active.load(std::memory_order_relaxed); }
    void on_scheduler_entry(bool worker) override;
    void on_scheduler_exit(bool worker) override;
  };
  ArenaThreadNamer arenaThreadNamer{taskArena, "luxir_tbb_", reservedSlots, false};

  // Every collection's indexing: update batches, flushes, merges (each
  // writer's update graph runs here; see Collections).  Its own arena at low
  // priority, so TBB lends workers to queries in taskArena first and recalls
  // them from indexing at task boundaries, down to the kBackgroundFloor
  // workers queries cannot take.  index.threads wide, below the pool, so a
  // query hop finds a free worker at once even while indexing holds all of
  // its own.  Reserved seats for threads waiting on a graph: one for the
  // embedding thread (tests), one per control thread (a collection's close).
  oneapi::tbb::task_arena indexArena;
  ArenaThreadNamer indexThreadNamer{indexArena, "luxir_idx_", 1 + controlThreads, true};

  // Nices the indexing arena's workers (index.nice); emplaced only when the
  // probe in ArenaNice::resolve proved the restore is permitted.
  int indexNiceIncrement = 0;
  std::optional<ArenaNice> indexNice;

  IndexRamBudget indexRamBudget;
  std::shared_ptr<ReplicationSource> replication;
  std::shared_ptr<CommitWaits> commitWaits;
  std::shared_ptr<CollectionEvents> events; // ordered feed of the collection map
  std::unique_ptr<Collections> collections_;
  std::unique_ptr<ReplicationFollower> follower;
  std::unique_ptr<Scrubber> scrubber;
  std::unique_ptr<SearchEngine> searchEngine;
  // Drained before tearing down the node state used by its tasks.
  std::unique_ptr<ControlExecutor> control;
  PluginNodeStates pluginStates;
};

}
