// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/server/ReplicationSource.h"
#include "CommitWaits.h"
#include "LuxirNode.h"
#include "ReplicationFollower.h"
#include "Scrubber.h"
#include "luxir/plugin/Plugin.h"
#include "luxir/util/DateTime.h"
#include "luxir/util/Uuid.h"
#include "luxir/util/log.h"
#include "oneapi/tbb/global_control.h"

#include <cerrno>
#include <climits>
#include <cstring>
#include <thread>
#include <omp.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/resource.h>
#include <sys/syscall.h>
#endif

namespace luxir {

void PluginNodeStates::create(LuxirNode& node, const PluginRegistry& plugins) {
  if (!states.empty()) throw std::logic_error("plugin node states have already been created");
  states.reserve(plugins.plugins().size());
  try {
    for (const auto& plugin : plugins.plugins()) {
      states.emplace_back(plugin->info().name, nullptr);
      states.back().second = plugin->createNodeState(node);
    }
  } catch (...) {
    clear();
    throw;
  }
}

void PluginNodeStates::clear() noexcept {
  while (!states.empty()) {
    states.back().second.reset();
    states.pop_back();
  }
}

PluginNodeState* PluginNodeStates::find(std::string_view name) const {
  for (const auto& [key, state] : states) {
    if (key == name) return state.get();
  }
  return nullptr;
}

// One arena slot per thread that enters the task arena from outside: each
// HTTP shard the configured server runs, plus the embedding (main or test)
// thread.  gRPC threads never enter: they enqueue their work.
static int reservedArenaSlots(const LuxirConfig& config) {
  const auto& server = config.server;
  return (server.http.enabled ? server.http.resolveThreads() : 0) + 1;
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
             "server.http.threads; excess entrants can block behind arena work",
             server, n, total, node.reservedSlots);
  }
}

void LuxirNode::ArenaThreadNamer::on_scheduler_entry(bool worker) {
  omp_set_num_threads(1);
  if (!worker) return;
  char name[16];
  snprintf(name, sizeof(name), "%s%d", prefix,
           oneapi::tbb::this_task_arena::current_thread_index() - reservedSlots);
  nameThisThread(name);
}

#if defined(__linux__)
static id_t threadId() { return (id_t)syscall(SYS_gettid); }

// A thread's nice value, or INT_MIN when it cannot be read (getpriority can
// legitimately return -1).
static int threadNice() {
  errno = 0;
  int nice = getpriority(PRIO_PROCESS, threadId());
  return errno != 0 ? INT_MIN : nice;
}

static void restoreNice(id_t thread, int nice) {
  if (setpriority(PRIO_PROCESS, thread, nice) == 0 || errno == ESRCH) return;
  static std::atomic<bool> warned{false};
  if (!warned.exchange(true)) {
    LOG_WARN("Restoring a worker's nice after indexing work failed ({}); it keeps the indexing priority",
             strerror(errno));
  }
}
#endif

void LuxirNode::ArenaNice::on_scheduler_entry(bool worker) {
#if defined(__linux__)
  if (!worker) return;
  int base = threadNice();
  if (base == INT_MIN) return;
  std::lock_guard lock(mutex);
  if (setpriority(PRIO_PROCESS, threadId(), std::min(19, base + increment)) == 0) niced[(long)threadId()] = base;
#else
  unused(worker);
#endif
}

void LuxirNode::ArenaNice::on_scheduler_exit(bool worker) {
#if defined(__linux__)
  if (!worker) return;
  std::lock_guard lock(mutex);
  auto it = niced.find((long)threadId());
  if (it == niced.end()) return;
  restoreNice(threadId(), it->second);
  niced.erase(it);
#else
  unused(worker);
#endif
}

LuxirNode::ArenaNice::~ArenaNice() {
  observe(false);  // returns once no callback is running
#if defined(__linux__)
  std::lock_guard lock(mutex);
  for (auto [thread, nice] : niced) restoreNice((id_t)thread, nice);
#endif
}

// The nice increment for the indexing arena's workers: index.nice, or the auto
// default, provided a thread can take it and return to its base afterwards.
// Lowering priority is always allowed; raising it back needs CAP_SYS_NICE or
// a high enough RLIMIT_NICE.  Probed on a scratch thread, so no pool worker
// is ever left niced.
static int resolveIndexNice(int configured) {
  if (configured == 0) return 0;
  int increment = configured < 0 ? IndexConfig::AUTO_NICE : configured;
#if defined(__linux__)
  bool restored = false;
  std::thread([&] {
    int base = threadNice();
    if (base == INT_MIN || setpriority(PRIO_PROCESS, threadId(), std::min(19, base + increment)) != 0) return;
    restored = setpriority(PRIO_PROCESS, threadId(), base) == 0;
  }).join();
  if (restored) return increment;
  if (configured > 0) {
    LOG_WARN("indexing.nice={} ignored: restoring a worker's priority needs CAP_SYS_NICE or RLIMIT_NICE >= {}",
             configured, 20 - threadNice());
  } else {
    LOG_DEBUG("indexing.nice auto: off, restoring a worker's priority is not permitted");
  }
#else
  if (configured > 0) LOG_WARN("indexing.nice={} ignored: per-thread nice is Linux only", configured);
#endif
  return 0;
}

// host:port of the listener, so a restarted node keeps its identity (and a
// follower its acknowledgment rows). Without a fixed port, a per-process id.
static std::string defaultNodeId(const ServerConfig& server, bool& random) {
  int port = server.http.enabled ? server.http.port : server.grpc.port;
  char host[256] = {};
  if (port <= 0 || ::gethostname(host, sizeof(host) - 1) != 0 || !*host) {
    random = true;
    return newUuid();
  }
  return std::string(host) + ":" + std::to_string(port);
}

LuxirNode::LuxirNode(LuxirConfig config, Mode mode, Now clock)
  : config(std::move(config)), mode(mode), clock(std::move(clock)), workers(std::max(1, this->config.server.resolveWorkers())),
    indexThreads(this->config.resolveIndexThreads()),
    reservedSlots(reservedArenaSlots(this->config)),
    taskArena(workers + reservedSlots, reservedSlots,
              oneapi::tbb::task_arena::priority::normal, oneapi::tbb::task_arena::leave_policy::fast),
    indexArena(indexThreads + 1, 1, oneapi::tbb::task_arena::priority::low) {
  if (this->config.node.id.empty()) this->config.node.id = defaultNodeId(this->config.server, this->config.node.random);
  this->config.node.validate();
  if (!this->config.read_only) lifetime = newUuid();
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
    void registered(const CollectionId& id, const std::shared_ptr<Collection>& collection, uint64_t revision) noexcept override {
      waits->registered(id, collection, revision); source->registered(id, collection, revision);
    }
    void remapped(std::string_view tenant, uint64_t revision, std::span<const Binding> changes) noexcept override {
      source->remapped(tenant, revision, changes); waits->remapped(tenant, revision, changes);
    }
    void updated(const CollectionId& id, const Collection& collection) noexcept override {
      waits->updated(id, collection); source->updated(id, collection);
    }
    void removed(const CollectionId& id, uint64_t revision) noexcept override {
      waits->removed(id, revision); source->removed(id, revision);
    }
  };
  events = std::make_shared<Events>(replication, commitWaits);
  indexRamBudget.setTotalBytes(this->config.index.max_ram_mb * 1024 * 1024);
  preWarmTimeZoneDatabase();
  // Before the writers open: their first tasks already get the priority.
  indexNiceIncrement = resolveIndexNice(this->config.index.nice);
  if (indexNiceIncrement) indexNice.emplace(indexArena, indexNiceIncrement);
  collections_ = std::make_unique<Collections>(this->config, lifetime, *events, indexRamBudget, indexArena, metrics);

  if (following()) {
    // The follower first checks that it owns the data directory.
    follower = std::make_unique<ReplicationFollower>(*this);
    follower->seed(collections_->open(Collections::Role::FOLLOWER));
  } else {
    // A follower directory becomes a writer only through offline promotion,
    // which completes for every collection before any writer opens one.
    // Only durable node metadata can outlive a follower process.
    auto nodeDir = collections_->storage().directory(StorageRoot::NODE_PRIVATE, "");
    if (!this->config.read_only && nodeDir->openFile("replication.json")) {
      throw std::runtime_error("Follower data directory (" + collections_->storage().describe()
                               + "): run 'luxir promote' on it to make it a writer");
    }
    collections_->open(this->config.read_only ? Collections::Role::READ_ONLY : Collections::Role::WRITER);
  }
  searchEngine = std::make_unique<SearchEngine>(*this);
  scrubber = std::make_unique<Scrubber>(*this);
  if (mode == Mode::SERVE) scrubber->start();
  if (follower && mode == Mode::SERVE) follower->start();
  pluginStates.create(*this, PluginRegistry::global());
}

LuxirNode::~LuxirNode() {
  pluginStates.clear();
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

void LuxirNode::renameCollection(const CollectionId& from, const CollectionId& to) {
  if (following() || config.read_only) throw ReadOnlyError("cannot rename collections on a read-only node or follower");
  collections_->rename(from, to);
}
void LuxirNode::swapCollections(const CollectionId& a, const CollectionId& b) {
  if (following() || config.read_only) throw ReadOnlyError("cannot swap collections on a read-only node or follower");
  collections_->swap(a, b);
}

void LuxirNode::deleteCollection(const CollectionId& id) {
  if (follower) follower->deleteOrphan(id);
  else collections_->remove(id);
}

void LuxirNode::detachCollection(const CollectionId& id, std::string_view target) {
  collections_->detach(id, target);
}

std::shared_ptr<Collection> LuxirNode::attachCollection(const CollectionId& id, bool force) {
  return collections_->attach(id, force);
}

} // namespace luxir
