// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/server/ReplicationSource.h"
#include "CommitWaits.h"
#include "LuxirNode.h"
#include "ReplicationFollower.h"
#include "Scrubber.h"
#include "luxir/util/DateTime.h"
#include "luxir/util/log.h"
#include "oneapi/tbb/global_control.h"

namespace luxir {

// One arena slot per thread that enters the task arena from outside: each
// HTTP shard and gRPC thread the configured servers run, plus the embedding
// (main or test) thread.
static int reservedArenaSlots(const LuxirConfig& config) {
  const auto& server = config.server;
  return (server.http.enabled ? server.http.resolveThreads() : 0) + server.grpc.resolveThreads() + 1;
}

// TBB's global pool lends at most max_allowed_parallelism - 1 workers to all
// arenas, and that defaults to the affinity width: one CPU is kept for a
// master thread, as in a default arena.  Arena slots past it stay empty, so a
// node whose workers fill every CPU (the small-node default) would still get
// one fewer.  Raise the pool to the widest node's workers.  It is never
// lowered: each arena caps its own concurrency, and a global_control lowers
// the limit for every arena in the process (the smallest active value wins),
// so a narrow node must not starve a wider one.  The high-water mark is kept
// here rather than read from active_value(), which also reflects controls this
// function does not own.  Leaked on purpose: a static would be destroyed after
// the TBB runtime may have shut down.
static void ensureWorkerPool(int workers) {
  using oneapi::tbb::global_control;
  static std::mutex mutex;
  static global_control* control = nullptr;
  static std::size_t raised = (std::size_t)std::max(1, oneapi::tbb::info::default_concurrency());
  std::lock_guard<std::mutex> lock(mutex);
  std::size_t wanted = (std::size_t)workers + 1;
  if (wanted <= raised) return;
  auto* next = new global_control(global_control::max_allowed_parallelism, wanted);
  delete control;  // the smaller limit holds until here
  control = next;
  raised = wanted;
}

LuxirNode::ArenaEntrants::ArenaEntrants(LuxirNode& node, int n, std::string_view server)
  : node(node), n(n) {
  int total = node.arenaEntrants.fetch_add(n, std::memory_order_relaxed) + n;
  if (total > node.reservedSlots) {
    LOG_WARN("{} with {} threads brings task arena entrants to {}, past the {} slots reserved from "
             "server.http.threads and server.grpc.threads; excess entrants can block behind arena work",
             server, n, total, node.reservedSlots);
  }
}

LuxirNode::LuxirNode(LuxirConfig config, Mode mode, Now clock)
  : config(std::move(config)), clock(std::move(clock)), workers(std::max(1, this->config.server.resolveWorkers())),
    reservedSlots(reservedArenaSlots(this->config)),
    taskArena(workers + reservedSlots, reservedSlots,
              oneapi::tbb::task_arena::priority::normal, oneapi::tbb::task_arena::leave_policy::fast),
    indexArena(workers + 1, 1, oneapi::tbb::task_arena::priority::low) {
  ensureWorkerPool(workers + 1);
  if (following() && this->config.read_only) throw std::invalid_argument("replication.source cannot be combined with read-only");
  // A config assembled in code (tests, embedding) has not been through
  // normalize(), so resolve the RAM sentinels here too - before the writers
  // opened below take a pointer to the budget.
  this->config.resolveRamBudgets();
  auto& replicationConfig = this->config.replication;
  replicationConfig.validate();
  replication = std::make_shared<ReplicationSource>(std::chrono::milliseconds(
      replicationConfig.follower_timeout_ms), this->clock, replicationConfig.max_acknowledgments);
  commitWaits = std::make_shared<CommitWaits>(*replication, following(), this->clock);
  replication->onAcknowledgmentsChanged([weak = std::weak_ptr(commitWaits)](const CollectionId* id) {
    if (auto waits = weak.lock()) waits->acknowledged(id);
  });
  class Events final : public CollectionEvents {
    std::shared_ptr<ReplicationSource> source;
    std::shared_ptr<CommitWaits> waits;
  public:
    Events(std::shared_ptr<ReplicationSource> source, std::shared_ptr<CommitWaits> waits)
        : source(std::move(source)), waits(std::move(waits)) {}
    void registered(const CollectionId& id, const std::shared_ptr<Collection>& collection) noexcept override {
      waits->registered(id, collection); source->registered(id, collection);
    }
    void updated(const CollectionId& id, const Collection& collection) noexcept override {
      waits->updated(id, collection); source->updated(id, collection);
    }
    void removed(const CollectionId& id) noexcept override {
      waits->removed(id); source->removed(id);
    }
  };
  events = std::make_shared<Events>(replication, commitWaits);
  indexRamBudget.setTotalBytes(this->config.index.max_ram_mb * 1024 * 1024);
  preWarmTimeZoneDatabase();
  collections_ = std::make_unique<Collections>(this->config, *events, indexRamBudget, indexArena, metrics);

  if (following()) {
    // The follower first checks that it owns the data directory.
    follower = std::make_unique<ReplicationFollower>(*this);
    follower->seed(collections_->open(Collections::Role::FOLLOWER));
  } else {
    // A follower directory becomes a writer only through offline promotion,
    // which completes for every collection before any writer opens one.
    if (!this->config.read_only && this->config.store.backend == "fs"
        && std::filesystem::exists(std::filesystem::path(this->config.store.data_dir) / "replication.json")) {
      throw std::runtime_error("Follower data directory: run 'luxir promote " + this->config.store.data_dir + "' to make it a writer");
    }
    collections_->open(this->config.read_only ? Collections::Role::READ_ONLY : Collections::Role::WRITER);
  }
  searchEngine = std::make_unique<SearchEngine>(*this);
  scrubber = std::make_unique<Scrubber>(*this);
  if (mode == Mode::SERVE) scrubber->start();
  if (follower && mode == Mode::SERVE) follower->start();
}

LuxirNode::~LuxirNode() {
  scrubber->stop();
  if (follower) follower->stop();
  commitWaits->close();
}

std::shared_ptr<Collection> LuxirNode::checkLoaded(std::shared_ptr<Collection> collection) {
  if (collection) {
    auto reason = collection->getUnavailableReason();
    if (!reason.empty()) throw CollectionUnavailableError(
        "collection '" + collection->getId().label() + "' is unavailable: " + reason);
  }
  return collection;
}

CollectionId LuxirNode::target(std::string_view tenant, std::string_view name) {
  return {std::string(tenant.empty() ? CollectionId::kDefaultTenant : tenant),
          std::string(name.empty() ? kDefaultCollectionName : name)};
}

std::shared_ptr<Collection> LuxirNode::getCollection(const CollectionId& id) {
  // Resolution is lookup-only on the hot path: an invalid name can never be in
  // the map, so the validation scan runs only on a miss, to tell "you wrote a
  // name that cannot exist" (INVALID_REQUEST) from "no such collection"
  // (NOT_FOUND) the same way every route does.
  if (auto collection = collections_->get(id)) return checkLoaded(std::move(collection));
  Collections::validate(id);
  throw CollectionNotFoundError("collection '" + id.label() + "' does not exist");
}

std::shared_ptr<Collection> LuxirNode::getOrCreateCollection(const CollectionId& id) {
  if (following()) return getCollection(id);
  return checkLoaded(collections_->getOrCreate(id, config.ingest.auto_create_collection));
}

std::shared_ptr<Collection> LuxirNode::createCollection(const CollectionId& id, const api::SchemaDef* schema) {
  if (following()) throw ReadOnlyError("cannot create collections on a follower");
  return collections_->create(id, schema);
}

void LuxirNode::deleteCollection(const CollectionId& id) {
  if (follower) follower->deleteOrphan(id);
  else collections_->remove(id);
}

} // namespace luxir
