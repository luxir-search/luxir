// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "Collections.h"
#include "Promotion.h"
#include "luxir/api/luxir_types.hpp"
#include "luxir/schema/Schema.h"
#include "luxir/store/CheckedDirFactory.h"
#include "luxir/store/CorruptionMarker.h"
#include "luxir/store/FileVerifier.h"
#include "luxir/store/ReadOnlyDirectory.h"
#include "luxir/store/StorageRegistry.h"
#include "luxir/util/Signal.h"
#include "luxir/util/Uuid.h"

namespace luxir {

static constexpr std::string_view DELETING_REASON = "being deleted";

std::shared_ptr<Collection> Collection::placeholder(CollectionId id, std::string reason) {
  auto collection = std::make_shared<Collection>();
  collection->id = std::move(id);
  collection->fail(reason);
  return collection;
}

void Collection::fail(const std::string& reason) {
  {
    std::lock_guard lock(failureMutex);
    if (failed.load(std::memory_order_relaxed)) return;
    failure = reason;
    failed.store(true, std::memory_order_release);
  }
  if (shard && shard->iw) shard->iw->fail(reason);
}

std::string Collection::getUnavailableReason() const {
  if (isFailed()) {
    std::lock_guard lock(failureMutex);
    return failure;
  }
  if (shard && shard->iw) {
    if (auto failure = shard->iw->getFailureReason()) return *failure;
  }
  return {};
}

std::shared_ptr<const CommitSnapshot> Collection::updateSchema(const luxir::api::SchemaDef& def,
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

void Collections::validateName(std::string_view name, std::string_view kind) {
  constexpr std::size_t kMaxCollectionNameBytes = 255;
  std::string what(kind);
  if (name.empty()) {
    throw InvalidCollectionNameError(what + " name is empty");
  }
  if (name.size() > kMaxCollectionNameBytes) {
    throw InvalidCollectionNameError(what + " '" + std::string(name) + "' exceeds maximum length");
  }
  if (name[0] == '_') {
    throw InvalidCollectionNameError(what + " '" + std::string(name) + "' is reserved");
  }
  // Lowercase names keep tenant paths portable to case-insensitive filesystems.
  if (name[0] < 'a' || name[0] > 'z') {
    throw InvalidCollectionNameError(what + " '" + std::string(name) +
                                     "' must start with a lowercase letter");
  }
  for (char c : name) {
    if ((c < 'a' || c > 'z') && (c < '0' || c > '9') && c != '_') {
      throw InvalidCollectionNameError(what + " '" + std::string(name) +
          "' may only contain lowercase letters, digits, and underscores");
    }
  }
}

void Collections::validate(const CollectionId& id) {
  validateName(id.tenant, "tenant");
  validateName(id.name);
}

Collections::Collections(const LuxirConfig& config, CollectionEvents& events, IndexRamBudget& indexRamBudget,
                         oneapi::tbb::task_arena& arena, std::shared_ptr<Metrics> metrics)
    : metrics(std::move(metrics)), config(config), events(events), indexRamBudget(indexRamBudget), arena(arena),
      verifyOnOpen(config.store.verify_on_open == "full"), cacheDefaultBytes(config.queryCacheBytes) {
  if (!verifyOnOpen && config.store.verify_on_open != "none") throw std::invalid_argument("store.verify-on-open must be none or full");
  // Backend, then plugin layers (innermost first), then the core wrappers below.
  const auto& storage = StorageRegistry::global();
  // Before any maker runs, so a mistyped component fails before a backend takes its lock.
  for (const auto& [component, params] : config.store.options) {
    if (component == config.store.backend || std::ranges::find(config.store.layers, component) != config.store.layers.end()) continue;
    std::string configured = config.store.backend;
    for (const auto& layer : config.store.layers) configured += ", " + layer;
    throw ParamError("store options for unconfigured component '" + component + "'; configured components: " + configured);
  }
  StorageOptions options(config.store);
  std::vector<ParamReader> readers;
  readers.reserve(1 + config.store.layers.size());
  readers.push_back(options.reader(config.store.backend, "store backend '" + config.store.backend + "'"));
  factory = storage.makeBackend(config.store.backend, {config, readers.back(), [this] { return reclaimStorage(); }});
  for (const auto& layer : config.store.layers) {
    readers.push_back(options.reader(layer, "store layer '" + layer + "'"));
    factory = storage.makeLayer(layer, std::move(factory), {config, readers.back(), [this] { return reclaimStorage(); }});
  }
  for (const auto& reader : readers) reader.rejectUnknown();
  if (config.store.checked_dir.sync != "off") {
    auto mode = config.store.checked_dir.sync == "throw" ? CheckedDirMode::THROW : CheckedDirMode::WARN;
    factory = std::make_unique<CheckedDirFactory>(std::move(factory), mode);
  }
  // Outermost, so a mutation is refused before any wrapper does bookkeeping for it.
  if (config.read_only) factory = std::make_unique<ReadOnlyDirFactory>(std::move(factory));
  TenantCatalog::initialize(*factory, !config.read_only);
  LOG_INFO("storage: {}", factory->describe());
}

Collections::~Collections() = default;

TenantCatalog& Collections::catalog(std::string_view tenant) {
  std::lock_guard lock(catalogsMutex);
  auto it = catalogs.find(tenant);
  if (it == catalogs.end()) it = catalogs.emplace(std::string(tenant), std::make_unique<TenantCatalog>(*factory, tenant)).first;
  return *it->second;
}

Collections::Transition::Transition(Collections& owner, CollectionId name) : owner(owner), name(std::move(name)) {
  {
    std::lock_guard lock(owner.slotsMutex);
    auto& slot = owner.slots[this->name];
    if (slot.busy) throw CollectionUnavailableError("collection '" + this->name.label() + "' is changing");
    slot.busy = true;
  }
  catalogLock = owner.catalog(this->name.tenant).transition();
}

Collections::Transition::~Transition() {
  std::lock_guard lock(owner.slotsMutex);
  auto it = owner.slots.find(name);
  it->second.busy = false;
  if (it->second.leased.empty() && it->second.retained.empty()) owner.slots.erase(it);
}

void Collections::release(const CollectionId& name, const std::string& incarnation) noexcept {
  std::lock_guard lock(slotsMutex);
  auto it = slots.find(name);
  if (it == slots.end()) return;
  if (auto leased = it->second.leased.find(incarnation); leased != it->second.leased.end()) it->second.leased.erase(leased);
  if (!it->second.busy && it->second.leased.empty() && it->second.retained.empty()) slots.erase(it);
}

void Collections::retire(const CollectionId& name, const std::string& selected) noexcept {
  try {
    auto& storage = catalog(name.tenant);
    for (const auto& incarnation : storage.incarnations(name.name)) {
      if (incarnation == selected) continue;
      {
        std::lock_guard lock(slotsMutex);
        auto it = slots.find(name);
        if (it != slots.end() && (it->second.leased.contains(incarnation) || it->second.retained.contains(incarnation))) continue;
      }
      try { storage.removeIncarnation(incarnation); }
      catch (const std::exception& e) { LOG_WARN("Incarnation cleanup '{}/{}' failed: {}", name.label(), incarnation, e.what()); }
    }
  } catch (const std::exception& e) { LOG_WARN("Incarnation cleanup '{}' failed: {}", name.label(), e.what()); }
}

bool Collections::reclaimStorage() {
  std::shared_ptr<Collection> oldest;
  auto time = CommitSnapshotRegistry::Clock::time_point::max();
  for (const auto& entry : entries()) if (auto shard = entry.collection->getShard()) {
    auto created = shard->getSnapshots().oldestReclaimablePin();
    if (created && *created < time) { time = *created; oldest = entry.collection; }
  }
  if (!oldest) return false;
  oldest->getShard()->getSnapshots().reclaimOldestPin();
  return true; // A concurrent drop also warrants retrying the allocation.
}

size_t Collections::cacheBytes(const CollectionId& id) const {
  auto it = cacheExceptions.find(id);
  return it == cacheExceptions.end() ? cacheDefaultBytes : it->second;
}

void Collections::applyCachePolicy(const CollectionId& id, const std::shared_ptr<Collection>& collection) {
  if (auto shard = collection->getShard()) {
    if (auto cache = shard->getReaderManager().filterCache) cache->resize(cacheBytes(id));
  }
}

void Collections::listCollection(const CollectionId& id, const std::shared_ptr<Collection>& collection) {
  std::lock_guard lock(listingMutex);
  listing[id.tenant].insert_or_assign(id.name, collection);
}

void Collections::registered(const CollectionId& id, const std::shared_ptr<Collection>& collection) {
  {
    std::lock_guard lock(cachePolicyMutex);
    applyCachePolicy(id, collection);
    listCollection(id, collection);
  }
  events.registered(id, collection, revision(id.tenant));
}

void Collections::removed(const CollectionId& id) {
  {
    std::lock_guard policyLock(cachePolicyMutex);
    cacheExceptions.erase(id);
    std::lock_guard lock(listingMutex);
    if (auto tenant = listing.find(id.tenant); tenant != listing.end()) {
      tenant->second.erase(id.name);
      if (tenant->second.empty()) listing.erase(tenant);
    }
  }
  events.removed(id, revision(id.tenant));
}

std::vector<Collections::Entry> Collections::setCacheBytes(
    std::string_view tenant, std::string_view collection, size_t bytes) {
  std::vector<Entry> selected;
  {
    std::lock_guard policyLock(cachePolicyMutex);
    {
      std::lock_guard lock(listingMutex);
      if (!collection.empty()) {
        CollectionId id{std::string(tenant.empty() ? CollectionId::kDefaultTenant : tenant),
                        std::string(collection)};
        if (auto t = listing.find(id.tenant); t != listing.end()) {
          if (auto c = t->second.find(id.name); c != t->second.end()) {
            selected.push_back({id, c->second, {}});
          }
        }
        if (selected.empty()) {
          validate(id);
          throw CollectionNotFoundError("collection '" + id.label() + "' does not exist");
        }
      } else {
        for (const auto& [t, collections] : listing) {
          if (!tenant.empty() && t != tenant) continue;
          for (const auto& [name, current] : collections) selected.push_back({{t, name}, current, {}});
        }
      }
    }
    if (tenant.empty() && collection.empty()) {
      cacheDefaultBytes = bytes;
      cacheExceptions.clear();
    } else {
      for (const auto& entry : selected) {
        if (bytes == cacheDefaultBytes) cacheExceptions.erase(entry.id);
        else cacheExceptions.insert_or_assign(entry.id, bytes);
      }
    }
    for (const auto& entry : selected) applyCachePolicy(entry.id, entry.collection);
  }
  for (auto& entry : selected) entry.error = entry.collection->getUnavailableReason();
  return selected;
}

std::vector<Collections::Entry> Collections::entries() {
  std::vector<Entry> result;
  {
    std::lock_guard lock(listingMutex);
    for (const auto& [tenant, collections] : listing)
      for (const auto& [name, collection] : collections) result.push_back({{tenant, name}, collection, {}});
  }
  for (auto& entry : result) entry.error = entry.collection->getUnavailableReason();
  return result;
}

std::vector<Collections::Entry> Collections::entries(std::string_view tenant) {
  std::vector<Entry> result;
  {
    std::lock_guard lock(listingMutex);
    if (auto it = listing.find(tenant); it != listing.end())
      for (const auto& [name, collection] : it->second) result.push_back({{it->first, name}, collection, {}});
  }
  for (auto& entry : result) entry.error = entry.collection->getUnavailableReason();
  return result;
}

std::vector<std::string> Collections::tenants() {
  std::lock_guard lock(listingMutex);
  std::vector<std::string> result;
  result.reserve(listing.size());
  for (const auto& entry : listing) result.push_back(entry.first);
  return result;
}

std::shared_ptr<Collection> Collections::makeCollection(const CollectionId& name, std::shared_ptr<Directory> directory) {
  auto col = std::make_shared<Collection>();
  col->id = name;
  col->shard = std::make_shared<Shard>(*col);
  col->shard->dir = std::move(directory);
  size_t bytes;
  {
    std::lock_guard lock(cachePolicyMutex);
    bytes = cacheBytes(name);
  }
  col->shard->snapshots = std::make_unique<CommitSnapshotRegistry>(*col->shard->dir,
      FilterCacheConfig{.maxBytes = bytes}, CommitSnapshotRegistry::Clock::now, schemas, name.tenant);
  col->shard->getReaderManager().metrics = metrics;
  const auto& policy = config.replication;
  col->shard->snapshots->setPolicy({std::chrono::milliseconds(policy.pin_idle_timeout_ms), policy.pin_retained_bytes});
  return col;
}

void Collections::observe(const CollectionId& name, Collection& collection) {
  // The registry belongs to this collection, so the hook never outlives it.
  unused(name);
  collection.getShard()->getSnapshots().onChange = [&events = events, &collection]() noexcept {
    events.updated(collection.getId(), collection);
  };
}

std::string Collections::recoverWriter(const CollectionId& name, TenantCatalog& storage, const std::string& selected) {
  auto manifest = Manifest::load(*storage.directory(selected));
  if (!manifest.bytes || manifest.generation == manifest.highestGeneration) return selected;
  auto recovered = copySnapshot(storage, name.name, selected, manifest);
  LOG_ERROR("Recovered collection '{}' from snapshot {} below {}; using new incarnation {}",
            name.label(), manifest.generation, manifest.highestGeneration, recovered);
  storage.activate(name.name, recovered);
  return recovered;
}

// Clears suspect-file markers once their files verified: best effort, and
// never on a read-only directory.
static void clearMarkers(const CollectionId& name, Directory& dir) {
  try { CorruptionMarker::clear(dir); }
  catch (const ReadOnlyError&) {}
  catch (const std::exception& e) { LOG_WARN("Cannot clear corruption markers of '{}': {}", name.label(), e.what()); }
}

std::shared_ptr<Collection> Collections::initWriter(const CollectionId& name, std::shared_ptr<Schema> initialSchema, bool startup, std::optional<std::string> reserved) {
  auto& storage = catalog(name.tenant);
  auto selected = storage.current(name.name);
  bool creating = !selected;
  if (creating) {
    selected = reserved ? std::move(reserved) : std::optional(storage.reserve(name.name, {}, true));
  } else {
    selected = recoverWriter(name, storage, *selected);
  }
  auto col = makeCollection(name, storage.directory(*selected));
  if (!creating) {
    auto manifest = Manifest::load(*col->shard->dir);
    if (!manifest.bytes) throw std::runtime_error("catalog selects an empty index directory");
    if (startup) arena.execute([&] { verifyFiles(*col->shard->dir, Manifest::files(manifest.bytes), verifyOnOpen); });
  }
  col->shard->iw = std::make_shared<IndexWriter>(*col->shard->snapshots,
    std::move(initialSchema), &indexRamBudget,
    config.index.merge_factor, creating ? *selected : std::string{}, &arena);
  col->shard->iw->metrics = metrics;
  col->shard->iw->collectionMetrics = col->metrics;
  col->shard->iw->perInverterRamBytes = (size_t)config.index.max_inverter_ram_mb * 1024 * 1024;
  col->shard->iw->pressureFlushFloorBytes = (size_t)config.index.pressure_flush_floor_mb * 1024 * 1024;
  if (col->shard->snapshots->snapshot()->id.incarnation != *selected) throw std::runtime_error("Snapshot incarnation does not match catalog selection");
  if (creating) storage.activate(name.name, *selected);
  retire(name, *selected);
  observe(name, *col);
  Signal::emit("collectionInitialized", col.get());
  return col;
}

// Read-only and follower collections serve their selected local snapshot.
std::shared_ptr<Collection> Collections::openCollection(const CollectionId& name, Role role,
                                                        const std::optional<std::string>& selected) {
  std::shared_ptr<Collection> col;
  if (role == Role::WRITER) {
    col = initWriter(name, {}, true);
  } else {
    if (!selected) throw ReadOnlyError("Collection has no catalog selection");
    col = makeCollection(name, catalog(name.tenant).directory(*selected));
    arena.execute([&] { col->shard->snapshots->openLocalSnapshot(verifyOnOpen); });
    if (col->shard->snapshots->snapshot()->id.incarnation != *selected) throw std::runtime_error("Snapshot incarnation does not match catalog selection");
    observe(name, *col);
  }
  // Whatever a marker named was verified before this point.
  clearMarkers(name, *col->shard->dir);
  return col;
}

std::vector<Collections::Opened> Collections::open(Role role) {
  std::vector<Opened> rows;
  if (role != Role::READ_ONLY) TenantCatalog::recover(*factory, role == Role::FOLLOWER);
  auto names = TenantCatalog::collections(*factory);
  bool createDefault = names.empty() && role != Role::FOLLOWER;
  if (createDefault) names.push_back(CollectionId::of(kDefaultCollectionName));
  // Keep the node up and the data: the name resolves to a clear error instead
  // of "does not exist", and cannot be silently re-created over it.
  auto failLoad = [&](Opened& row, const std::exception& e) {
    LOG_ERROR("Failed to load collection '{}': {}", row.id.label(), e.what());
    row.error = "failed to load: " + std::string(e.what());
    row.collection = unavailable(row.id, row.error);
    // Whatever the catalog selects (or failed to say) is kept until an
    // installation replaces the copy.
    if (role == Role::FOLLOWER) try {
      auto incarnations = catalog(row.id.tenant).incarnations(row.id.name);
      std::lock_guard lock(slotsMutex);
      slots[row.id].retained.insert(incarnations.begin(), incarnations.end());
    } catch (const std::exception& listing) { LOG_WARN("Cannot list incarnations of '{}': {}", row.id.label(), listing.what()); }
  };
  for (const auto& name : names) {
    Opened row{name, nullptr, std::nullopt, {}};
    try {
      validate(name);
      auto& storage = catalog(name.tenant);
      if (!createDefault) {
        row.incarnation = storage.current(name.name);
        if (row.incarnation && !CommitId::validIncarnation(*row.incarnation)) throw std::runtime_error("catalog selects an invalid incarnation");
        if (!row.incarnation && role == Role::FOLLOWER) {
          rows.push_back(std::move(row)); // reserved files survive a restarted download
          continue;
        }
      }
      auto collection = openCollection(row.id, role, row.incarnation);
      {
        std::lock_guard lock(cachePolicyMutex);
        applyCachePolicy(row.id, collection);
        if (!map.insert(row.id, collection)) throw std::logic_error("collection opened twice");
        listCollection(row.id, collection);
      }
      row.collection = std::move(collection);
      events.registered(row.id, row.collection, revision(row.id.tenant));
      LOG_INFO("Loaded collection: {}", row.id.label());
    } catch (const std::exception& e) {
      failLoad(row, e);
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

std::shared_ptr<Collection> Collections::unavailable(const CollectionId& name, std::string reason) {
  auto placeholder = Collection::placeholder(name, std::move(reason));
  if (!map.insert(name, placeholder)) return map.get(name);
  registered(name, placeholder);
  return placeholder;
}

void Collections::fail(const CollectionId& name, const std::shared_ptr<Collection>& collection, const std::string& reason,
                       std::span<const std::string> suspects) {
  unused(name);
  if (auto shard = collection->getShard(); shard && !suspects.empty()) {
    try { CorruptionMarker::write(*shard->dir, suspects, reason); }
    catch (const ReadOnlyError&) {}
    catch (const std::exception& e) { LOG_WARN("Cannot record suspect files of '{}': {}", collection->getId().label(), e.what()); }
  }
  collection->fail(reason);
  events.updated(collection->getId(), *collection);
}

void Collections::unload(const CollectionId& name, const std::shared_ptr<Collection>& expected, std::string reason) {
  Transition transition(*this, name);
  auto shard = expected->getShard();
  auto snapshot = shard ? shard->getSnapshots().snapshot() : nullptr;
  auto placeholder = Collection::placeholder(name, std::move(reason));
  if (!map.replace(name, expected, placeholder)) throw std::runtime_error("collection '" + name.label() + "' changed before unloading");
  if (snapshot) {
    std::lock_guard lock(slotsMutex);
    slots[name].retained.insert(snapshot->id.incarnation);
  }
  registered(name, placeholder);
  if (shard) shard->snapshots->detach();
}

std::shared_ptr<Collection> Collections::getOrCreate(const CollectionId& key, bool autoCreate) {
  validate(key);
  if (auto existing = map.get(key)) return existing;
  if (!autoCreate) throw CollectionNotFoundError("collection '" + key.label() + "' does not exist");
  // Concurrent first writers wait on the map's creation slot, which lets them
  // help run work rather than block. The builder owns the name's transition and
  // announces the collection before the map publishes it.
  auto collection = map.getOrCreate(key, [&] {
    Transition transition(*this, key);
    auto col = initWriter(key);
    LOG_INFO("Created collection: {}", key.label());
    registered(key, col);
    return col;
  });
  // A removal can follow the creation before this caller re-reads the entry.
  if (!collection) throw CollectionUnavailableError("collection '" + key.label() + "' changed during creation");
  return collection;
}

std::shared_ptr<Collection> Collections::create(const CollectionId& key, const api::SchemaDef* schema) {
  validate(key);
  bool createdHere = false;
  std::shared_ptr<Collection> created;
  std::exception_ptr createFailure;
  map.getOrCreate(key, [&]() {
    createdHere = true;
    Transition transition(*this, key);
    bool stored = false;
    try {
      auto initialSchema = Schema::createDefaultSchema();
      if (schema) initialSchema = Schema::fromProto(*schema, initialSchema.get());
      auto reserved = catalog(key.tenant).reserve(key.name, {}, true);
      stored = true;
      created = initWriter(key, std::move(initialSchema), false, std::move(reserved));
      LOG_INFO("Created collection: {}", key.label());
      registered(key, created);
      return created;
    } catch (...) {
      createFailure = std::current_exception();
      std::shared_ptr<Collection> failed;
      try {
        if (created && created->shard && created->shard->iw) created->shard->iw->close();
        if (stored) catalog(key.tenant).remove(key.name);
      } catch (const std::exception& e) {
        failed = Collection::placeholder(key, "create failed and cleanup failed: " + std::string(e.what()));
      } catch (...) {
        failed = Collection::placeholder(key, "create failed and cleanup failed: unknown non-standard exception");
      }
      if (!failed) std::rethrow_exception(createFailure);
      registered(key, failed);
      return failed;
    }
  });
  if (createFailure) std::rethrow_exception(createFailure);
  if (!createdHere) throw CollectionExistsError("collection '" + key.label() + "' already exists");
  // The collection this call built, even if a removal has since replaced it.
  return created;
}

void Collections::remove(const CollectionId& key, bool mustExist) {
  // Empty means the request never named a collection - a malformed request,
  // not a missing collection.
  if (key.name.empty()) throw InvalidCollectionNameError("collection name is empty");
  // Otherwise lookup-only: every map key came from a validated create or catalog.
  auto& storage = catalog(key.tenant);
  Transition transition(*this, key);
  {
    std::lock_guard lock(slotsMutex);
    if (!slots[key].leased.empty()) throw CollectionUnavailableError("collection '" + key.label() + "' has a staged replacement");
  }
  auto collection = map.get(key);
  if (!collection) {
    if (mustExist) {
      validate(key);
      throw CollectionNotFoundError("collection '" + key.label() + "' does not exist");
    }
    storage.remove(key.name);
    return;
  }
  // The placeholder answers resolution while the transition holds the name.
  auto deleting = Collection::placeholder(key, std::string(DELETING_REASON));
  if (!map.replace(key, collection, deleting)) {
    throw CollectionUnavailableError("collection '" + key.label() + "' changed while deletion started");
  }
  registered(key, deleting);
  auto fail = [&](std::string reason) {
    auto failed = Collection::placeholder(key, "delete failed: " + reason);
    if (map.replace(key, deleting, failed)) registered(key, failed);
  };
  try {
    if (auto shard = collection->getShard()) {
      if (shard->iw) shard->iw->close();
      shard->snapshots->close();
    }
    storage.remove(key.name);
  } catch (const std::exception& e) {
    fail(e.what());
    throw;
  } catch (...) {
    fail("unknown non-standard exception");
    throw;
  }
  {
    std::lock_guard lock(slotsMutex);
    slots[key].retained.clear();
  }
  if (!map.erase(key, deleting)) {
    throw std::runtime_error("collection '" + key.label() + "' tombstone disappeared during deletion");
  }
  removed(key);
}

void Collections::detach(const CollectionId& key) {
  validate(key);
  Transition transition(*this, key);
  {
    std::lock_guard lock(slotsMutex);
    if (!slots[key].leased.empty()) throw CollectionUnavailableError("collection '" + key.label() + "' has a staged replacement");
  }
  auto collection = map.get(key);
  if (!collection) throw CollectionNotFoundError("collection '" + key.label() + "' does not exist");
  auto detaching = Collection::placeholder(key, "being detached");
  if (!map.replace(key, collection, detaching)) {
    throw CollectionUnavailableError("collection '" + key.label() + "' changed while detaching started");
  }
  registered(key, detaching);
  // The writer may be closed already: the collection is unavailable here
  // until a restart opens it again, and its storage is untouched.
  auto fail = [&](std::string reason) {
    auto failed = Collection::placeholder(key, "detach failed: " + reason);
    if (map.replace(key, detaching, failed)) registered(key, failed);
  };
  try {
    if (auto shard = collection->getShard()) {
      if (shard->iw) {
        shard->iw->commit();
        shard->iw->close();
      }
      shard->snapshots->close();
    }
  } catch (const std::exception& e) {
    fail(e.what());
    throw;
  } catch (...) {
    fail("unknown non-standard exception");
    throw;
  }
  {
    std::lock_guard lock(slotsMutex);
    slots[key].retained.clear();
  }
  if (!map.erase(key, detaching)) {
    throw std::runtime_error("collection '" + key.label() + "' placeholder disappeared while detaching");
  }
  removed(key);
  LOG_INFO("Detached collection: {}", key.label());
}

std::shared_ptr<Collection> Collections::attach(const CollectionId& key) {
  validate(key);
  Transition transition(*this, key);
  if (map.get(key)) throw CollectionExistsError("collection '" + key.label() + "' already exists");
  auto selected = catalog(key.tenant).current(key.name);
  if (!selected) throw CollectionNotFoundError("collection '" + key.label() + "' has no active incarnation in storage");
  if (!CommitId::validIncarnation(*selected)) throw std::runtime_error("catalog selects an invalid incarnation");
  auto collection = openCollection(key, Role::WRITER, selected);
  {
    std::lock_guard lock(cachePolicyMutex);
    applyCachePolicy(key, collection);
    if (!map.insert(key, collection)) {
      collection->getShard()->iw->close();
      throw CollectionExistsError("collection '" + key.label() + "' already exists");
    }
    listCollection(key, collection);
  }
  events.registered(key, collection, revision(key.tenant));
  LOG_INFO("Attached collection: {}", key.label());
  return collection;
}

uint64_t Collections::revision(std::string_view tenant) {
  try { return catalog(tenant).read().rev; }
  catch (...) { return 0; } // An unreadable catalog still announces an unavailable collection.
}

uint64_t Collections::storageBytes(const CollectionId& id) {
  auto collection = get(id);
  return collection ? storageBytes(*collection) : 0;
}

uint64_t Collections::storageBytes(const Collection& collection) {
  auto shard = collection.getShard();
  auto snapshot = shard ? shard->getSnapshots().snapshot() : nullptr;
  return snapshot ? catalog(collection.getId().tenant).bytes(snapshot->id.incarnation) : 0;
}

void Collections::publishMapping(std::string_view tenant,
    const std::map<std::string, std::shared_ptr<Collection>>& next, const std::function<uint64_t()>& publish) {
  struct Change { CollectionId key; std::shared_ptr<Collection> expected, replacement; };
  std::vector<Change> changes;
  std::vector<CollectionEvents::Binding> bindings;
  std::vector<std::pair<CollectionId, std::shared_ptr<Collection>>> reserved;
  auto rollback = scope_guard([&] { for (const auto& [id, collection] : reserved) map.erase(id, collection); });
  for (const auto& entry : entries(tenant)) {
    auto it = next.find(entry.id.name);
    auto replacement = it == next.end() ? nullptr : it->second;
    if (replacement != entry.collection) changes.push_back({entry.id, entry.collection, replacement});
  }
  for (const auto& [name, collection] : next) {
    CollectionId id{std::string(tenant), name};
    if (map.get(id)) continue;
    auto placeholder = Collection::placeholder(id, "being installed");
    if (!map.insert(id, placeholder)) throw CollectionUnavailableError("collection is changing");
    reserved.emplace_back(id, placeholder);
    changes.push_back({id, placeholder, collection});
  }
  for (const auto& change : changes) bindings.push_back({change.key, change.replacement});
  // Prepare the listing before publication; lookups continue to see the old map.
  auto nextListing = next;
  auto rev = publish();
  Signal::emit("collectionMappingPublished", this);
  {
    std::lock_guard policyLock(cachePolicyMutex);
    std::lock_guard lock(listingMutex);
    for (const auto& [name, collection] : next) {
      CollectionId id{std::string(tenant), name};
      collection->setId(id);
      applyCachePolicy(id, collection);
      if (std::ranges::any_of(changes, [&](const auto& change) { return change.key == id; })) Signal::emit("collectionPublishing", collection.get());
    }
    // Transitions own all affected names. Each lookup sees its old or new
    // binding; two names may briefly resolve to the same collection during a swap.
    // Install destinations before removing sources, including for a rename.
    for (const auto& change : changes) if (change.replacement) {
      if (!map.replace(change.key, change.expected, change.replacement)) throw std::logic_error("collection mapping changed during publication");
    }
    for (const auto& change : changes) if (!change.replacement) {
      if (!map.erase(change.key, change.expected)) throw std::logic_error("collection mapping changed during publication");
    }
    listing[std::string(tenant)].swap(nextListing);
    if (listing[std::string(tenant)].empty()) listing.erase(std::string(tenant));
  }
  events.remapped(tenant, rev, bindings);
  for (const auto& change : changes) if (auto shard = change.expected->getShard()) {
    bool reused = std::ranges::any_of(next, [&](const auto& e) { return e.second == change.expected; });
    if (!reused) shard->snapshots->detach();
  }
}

void Collections::changeNames(const CollectionId& from, const CollectionId& to, bool exchange) {
  validate(from); validate(to);
  if (from.tenant != to.tenant || from == to) throw InvalidCollectionNameError("names must differ and belong to one tenant");
  Transition first(*this, std::min(from, to)), second(*this, std::max(from, to));
  {
    std::lock_guard lock(slotsMutex);
    if (!slots[from].leased.empty() || !slots[to].leased.empty()) throw CollectionUnavailableError("collection has a staged replacement");
  }
  auto source = map.get(from), target = map.get(to);
  if (!source || (exchange && !target)) throw CollectionNotFoundError("collection does not exist");
  if (!exchange && target) throw CollectionExistsError("destination collection already exists");
  if (!source->getUnavailableReason().empty() || (target && !target->getUnavailableReason().empty()))
    throw CollectionUnavailableError("collection is unavailable");
  std::map<std::string, std::shared_ptr<Collection>> next;
  for (const auto& entry : entries(from.tenant)) next.emplace(entry.id.name, entry.collection);
  next[to.name] = source;
  if (exchange) next[from.name] = target;
  else next.erase(from.name);
  publishMapping(from.tenant, next, [&] {
    auto& storage = catalog(from.tenant);
    return exchange ? storage.swap(from.name, to.name) : storage.rename(from.name, to.name);
  });
}

void Collections::applyMapping(std::string_view tenant, const std::map<std::string, std::string>& names,
                               const std::map<std::string, std::string>& desired,
                               std::vector<Prepared> prepared, std::span<const std::string> retained) {
  auto lock = catalog(tenant).transition();
  std::set<CollectionId> keys;
  for (const auto& entry : entries(tenant)) keys.insert(entry.id);
  for (const auto& [name, id] : names) keys.emplace(std::string(tenant), name);
  std::vector<std::unique_ptr<Transition>> transitions;
  for (const auto& key : keys) transitions.push_back(std::make_unique<Transition>(*this, key));
  std::map<std::string, std::shared_ptr<Collection>> byId, next;
  auto previous = catalog(tenant).read();
  for (const auto& entry : entries(tenant)) {
    if (auto shard = entry.collection->getShard()) {
      if (auto snapshot = shard->snapshots->snapshot()) byId.emplace(snapshot->id.incarnation, entry.collection);
    } else {
      auto selected = std::ranges::find_if(previous.entries, [&](const auto& e) { return e.name == entry.id.name && e.state == "active"; });
      if (selected != previous.entries.end()) byId.emplace(selected->id, entry.collection);
    }
  }
  for (auto& item : prepared) {
    auto& candidate = item.candidate;
    observe(candidate.id_, *candidate.collection_);
    byId.insert_or_assign(candidate.incarnation_, candidate.collection_);
  }
  std::map<std::string, std::string> selected;
  for (const auto& [name, id] : names) {
    if (id.empty()) {
      auto old = get({std::string(tenant), name});
      next.emplace(name, old && !old->getShard() ? old : Collection::placeholder({std::string(tenant), name}, "waiting for replicated data"));
    } else {
      auto it = byId.find(id);
      if (it == byId.end()) throw std::runtime_error("mapping incarnation is not ready");
      next.emplace(name, it->second);
      selected.emplace(name, id);
    }
  }
  publishMapping(tenant, next, [&] { return catalog(tenant).apply(selected, desired); });
  for (auto& item : prepared) {
    item.candidate.lease.reset();
    try { item.candidate.collection_->getShard()->getSnapshots().sweepOrphans(); }
    catch (const std::exception& e) { LOG_WARN("Follower file retirement failed: {}", e.what()); }
  }
  {
    std::lock_guard lock(slotsMutex);
    for (const auto& key : keys) slots[key].retained.clear();
  }
  for (const auto& entry : catalog(tenant).read().entries) if (entry.state == "retiring" && std::ranges::find(retained, entry.id) == retained.end()) {
    try { catalog(tenant).removeIncarnation(entry.id); }
    catch (const std::exception& e) { LOG_WARN("Follower retirement failed: {}", e.what()); }
  }
}

Collections::Candidate Collections::stage(const CollectionId& key, std::string_view incarnation) {
  auto lease = std::unique_ptr<Lease>(new Lease{*this, key, std::string(incarnation)});
  {
    // Admission and the lease are one step: no removal or retirement of this
    // name is in progress, and none can start without seeing the lease.
    std::lock_guard lock(slotsMutex);
    auto& slot = slots[key];
    if (slot.busy) throw CollectionUnavailableError("collection '" + key.label() + "' is changing");
    slot.leased.insert(lease->incarnation);
    lease->active = true;
  }
  auto& tenant = catalog(key.tenant);
  tenant.reserve(key.name, incarnation);
  auto collection = makeCollection(key, tenant.directory(incarnation));
  return Candidate(key, std::string(incarnation), std::move(collection), std::move(lease));
}

Collections::Prepared Collections::prepare(Candidate& candidate, std::shared_ptr<const CommitSnapshot> snapshot) {
  if (snapshot->id.incarnation != candidate.incarnation_) throw std::logic_error("snapshot of another incarnation");
  auto& registry = candidate.collection_->getShard()->getSnapshots();
  // An interrupted activation may already have committed this snapshot.
  auto committed = registry.snapshot();
  if (!committed || committed->id != snapshot->id) {
    snapshot = registry.resolve(snapshot->bytes);
    auto opened = registry.readers.prepare(*snapshot);
    registry.commit(std::move(snapshot), std::move(opened));
  }
  // The caller verified every file the snapshot lists.
  clearMarkers(candidate.id_, registry.dir);
  return Prepared(std::move(candidate));
}

std::shared_ptr<Collection> Collections::install(Prepared prepared, const std::shared_ptr<Collection>& expected) {
  auto& candidate = prepared.candidate;
  const auto& name = candidate.id_;
  auto collection = candidate.collection_;
  auto& storage = catalog(name.tenant);
  Transition transition(*this, name);
  if (map.get(name) != expected) throw std::runtime_error("collection changed during installation");
  observe(name, *collection);
  std::map<std::string, std::shared_ptr<Collection>> next;
  for (const auto& entry : entries(name.tenant)) next.emplace(entry.id.name, entry.collection);
  next[name.name] = collection;
  publishMapping(name.tenant, next, [&] { storage.activate(name.name, candidate.incarnation_); return storage.read().rev; });
  {
    std::lock_guard lock(slotsMutex);
    slots[name].retained.clear();
  }
  candidate.lease.reset();
  retire(name, candidate.incarnation_);
  return collection;
}

void Collections::discard(Candidate& candidate) noexcept {
  if (!candidate.collection_) return;
  candidate.collection_->shard->snapshots->close();
  candidate.collection_.reset();
  candidate.lease.reset();
  try {
    Transition transition(*this, candidate.id_);
    auto& storage = catalog(candidate.id_.tenant);
    // An unreadable catalog selection keeps the directory: it may be the selected one.
    if (storage.current(candidate.id_.name) == candidate.incarnation_) return;
    {
      std::lock_guard lock(slotsMutex);
      auto it = slots.find(candidate.id_);
      if (it != slots.end() && (it->second.leased.contains(candidate.incarnation_) || it->second.retained.contains(candidate.incarnation_))) return;
    }
    storage.removeIncarnation(candidate.incarnation_);
  } catch (const std::exception& e) {
    LOG_WARN("Candidate cleanup '{}/{}' skipped: {}", candidate.id_.label(), candidate.incarnation_, e.what());
  }
}

}
