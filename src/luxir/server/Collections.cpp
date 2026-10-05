// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "Collections.h"
#include "Promotion.h"
#include "luxir/api/luxir_types.hpp"
#include "luxir/schema/Schema.h"
#include "luxir/store/CheckedDirFactory.h"
#include "luxir/store/ReadOnlyDirectory.h"
#include "luxir/util/Signal.h"
#include "luxir/util/Uuid.h"

namespace luxir {

static constexpr std::string_view DELETING_REASON = "being deleted";

std::shared_ptr<Collection> Collection::placeholder(std::string name, std::string reason) {
  auto collection = std::make_shared<Collection>();
  collection->name = std::move(name);
  collection->unavailableReason = std::move(reason);
  return collection;
}

std::string Collection::getUnavailableReason() const {
  if (!unavailableReason.empty()) return unavailableReason;
  if (shard && shard->iw) {
    if (auto failure = shard->iw->getFailureReason()) return *failure;
  }
  return {};
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

void Collections::validateName(std::string_view name) {
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

Collections::Collections(const LuxirConfig& config, CollectionEvents& events, IndexRamBudget& indexRamBudget)
    : config(config), events(events), indexRamBudget(indexRamBudget) {
  if (config.store.backend == "fs") {
    factory = std::make_unique<FSDirFactory>(config.store.data_dir, config.read_only);
  } else {
    if (config.store.ram_limit_mb > UINT64_MAX / (1024 * 1024)) throw std::invalid_argument("store.ram_limit_mb is too large");
    factory = std::make_unique<RAMDirFactory>(config.store.ram_limit_mb * 1024 * 1024, [this] { return reclaimStorage(); });
  }
  if (config.store.checked_dir.sync != "off") {
    auto mode = config.store.checked_dir.sync == "throw" ? CheckedDirMode::THROW : CheckedDirMode::WARN;
    factory = std::make_unique<CheckedDirFactory>(std::move(factory), mode);
  }
  // Outermost, so a mutation is refused before any wrapper does bookkeeping for it.
  if (config.read_only) factory = std::make_unique<ReadOnlyDirFactory>(std::move(factory));
}

Collections::~Collections() = default;

Collections::Transition::Transition(Collections& owner, std::string name) : owner(owner), name(std::move(name)) {
  std::lock_guard lock(owner.slotsMutex);
  auto& slot = owner.slots[this->name];
  if (slot.busy) throw CollectionUnavailableError("collection '" + this->name + "' is changing");
  slot.busy = true;
}

Collections::Transition::~Transition() {
  std::lock_guard lock(owner.slotsMutex);
  auto it = owner.slots.find(name);
  it->second.busy = false;
  if (it->second.leased.empty() && it->second.retained.empty()) owner.slots.erase(it);
}

void Collections::release(const std::string& name, const std::string& incarnation) noexcept {
  std::lock_guard lock(slotsMutex);
  auto it = slots.find(name);
  if (it == slots.end()) return;
  if (auto leased = it->second.leased.find(incarnation); leased != it->second.leased.end()) it->second.leased.erase(leased);
  if (!it->second.busy && it->second.leased.empty() && it->second.retained.empty()) slots.erase(it);
}

void Collections::retire(const std::string& name, const std::string& selected) noexcept {
  try {
    auto storage = factory->collection(name);
    for (const auto& incarnation : storage.incarnations()) {
      if (incarnation == selected) continue;
      {
        std::lock_guard lock(slotsMutex);
        auto it = slots.find(name);
        if (it != slots.end() && (it->second.leased.contains(incarnation) || it->second.retained.contains(incarnation))) continue;
      }
      try { storage.removeIncarnation(incarnation); }
      catch (const std::exception& e) { LOG_WARN("Incarnation cleanup '{}/{}' failed: {}", name, incarnation, e.what()); }
    }
  } catch (const std::exception& e) { LOG_WARN("Incarnation cleanup '{}' failed: {}", name, e.what()); }
}

bool Collections::reclaimStorage() {
  std::shared_ptr<Collection> oldest;
  auto time = CommitSnapshotRegistry::Clock::time_point::max();
  for (const auto& entry : entries()) if (auto shard = entry.collection->getShard()) {
    auto created = shard->getSnapshots().oldestReclaimableReservation();
    if (created && *created < time) { time = *created; oldest = entry.collection; }
  }
  if (!oldest) return false;
  oldest->getShard()->getSnapshots().reclaimOldestReservation();
  return true; // A concurrent drop also warrants retrying the allocation.
}

std::vector<Collections::Entry> Collections::entries() {
  std::vector<Entry> result;
  using Pointer = SharedLazyMap<std::string, Collection>::Pointer;
  map.dataMap.cvisit_all([&](const auto& elem) {
    if (auto* collection = std::get_if<Pointer>(&elem.second)) {
      result.push_back({elem.first, *collection, (*collection)->getUnavailableReason()});
    }
  });
  std::sort(result.begin(), result.end(), [](const Entry& a, const Entry& b) { return a.name < b.name; });
  return result;
}

std::shared_ptr<Collection> Collections::makeCollection(const std::string& name, std::shared_ptr<Directory> directory) {
  auto col = std::make_shared<Collection>();
  col->name = name;
  col->shard = std::make_shared<Shard>(*col);
  col->shard->dir = std::move(directory);
  col->shard->snapshots = std::make_unique<CommitSnapshotRegistry>(*col->shard->dir,
      FilterCacheConfig{.maxBytes = config.queryCacheBytes});
  const auto& policy = config.replication;
  col->shard->snapshots->setPolicy({std::chrono::milliseconds(policy.pin_idle_timeout_ms), policy.pin_retained_bytes});
  return col;
}

void Collections::observe(const std::string& name, Collection& collection) {
  // The registry belongs to this collection, so the hook never outlives it.
  collection.getShard()->getSnapshots().onChange = [&events = events, name, &collection]() noexcept {
    events.updated(name, collection);
  };
}

std::shared_ptr<Collection> Collections::initWriter(const std::string& name, std::shared_ptr<Schema> initialSchema) {
  auto storage = factory->collection(name);
  auto selected = CollectionStorage::current(*factory->container(name, true));
  bool creating = !selected;
  if (creating) {
    if (storage.hasFiles() || !storage.incarnations().empty()) throw std::runtime_error("Collection files exist without CURRENT");
    selected = newUuid();
  } else {
    auto manifest = Manifest::load(*storage.open(*selected));
    if (manifest.bytes && manifest.generation != manifest.highestGeneration) {
      auto recovered = copySnapshot(storage, *selected, manifest);
      LOG_ERROR("Recovered collection '{}' from snapshot {} below {}; using new incarnation {}",
                name, manifest.generation, manifest.highestGeneration, recovered);
      storage.select(recovered);
      selected = std::move(recovered);
    }
  }
  auto col = makeCollection(name, creating ? storage.create(*selected) : storage.open(*selected));
  if (!creating && !Manifest::load(*col->shard->dir).bytes) throw std::runtime_error("CURRENT selects an empty index directory");
  col->shard->iw = std::make_shared<IndexWriter>(*col->shard->snapshots,
    std::move(initialSchema), &indexRamBudget,
    config.index.merge_factor, creating ? *selected : std::string{});
  col->shard->iw->perInverterRamBytes = (size_t)config.index.max_inverter_ram_mb * 1024 * 1024;
  col->shard->iw->pressureFlushFloorBytes = (size_t)config.index.pressure_flush_floor_mb * 1024 * 1024;
  if (col->shard->snapshots->snapshot()->id.incarnation != *selected) throw std::runtime_error("Snapshot incarnation does not match CURRENT");
  if (creating) storage.select(*selected);
  retire(name, *selected);
  observe(name, *col);
  Signal::emit("collectionInitialized", col.get());
  return col;
}

// Read-only and follower collections serve their selected local snapshot.
std::shared_ptr<Collection> Collections::openCollection(const std::string& name, Role role,
                                                        const std::optional<std::string>& selected) {
  if (role == Role::WRITER) return initWriter(name);
  if (!selected) throw ReadOnlyError("Collection has no CURRENT");
  auto col = makeCollection(name, factory->collection(name).open(*selected));
  col->shard->snapshots->openLocalSnapshot();
  if (col->shard->snapshots->snapshot()->id.incarnation != *selected) throw std::runtime_error("Snapshot incarnation does not match CURRENT");
  observe(name, *col);
  return col;
}

std::vector<Collections::Opened> Collections::open(Role role) {
  std::vector<Opened> rows;
  auto names = factory->collections();
  bool createDefault = names.empty() && role != Role::FOLLOWER;
  if (createDefault) names.emplace_back(kDefaultCollectionName);
  for (const auto& name : names) {
    Opened row{name, nullptr, std::nullopt, {}};
    try {
      validateName(name);
      auto storage = factory->collection(name);
      if (!createDefault) {
        row.incarnation = storage.current();
        if (row.incarnation && !isUuid(*row.incarnation)) throw std::runtime_error("CURRENT selects an invalid incarnation");
        if (!row.incarnation && role == Role::FOLLOWER) {
          if (storage.hasFiles()) throw std::runtime_error("Collection has files without CURRENT");
          for (const auto& incarnation : storage.incarnations()) {
            if (!isUuid(incarnation)) throw std::runtime_error("Collection has an invalid incarnation directory");
          }
          rows.push_back(std::move(row)); // completed candidate files survive a restarted download
          continue;
        }
        if (!row.incarnation && role == Role::WRITER && !storage.hasFiles() && std::ranges::all_of(storage.incarnations(), isUuid)) {
          storage.remove();
          LOG_INFO("Removed unselected collection: {}", name);
          continue;
        }
      }
      auto collection = openCollection(name, role, row.incarnation);
      if (!map.insert(name, collection)) throw std::logic_error("collection opened twice");
      row.collection = std::move(collection);
      events.registered(name, row.collection);
      LOG_INFO("Loaded collection: {}", name);
    } catch (const std::exception& e) {
      // Keep the node up and the data: the name resolves to a clear error
      // instead of "does not exist", and cannot be silently re-created over it.
      LOG_ERROR("Failed to load collection '{}': {}", name, e.what());
      row.error = "failed to load: " + std::string(e.what());
      row.collection = unavailable(name, row.error);
      // Whatever CURRENT selects (or failed to say) is kept until an
      // installation replaces the copy.
      if (role == Role::FOLLOWER) try {
        auto incarnations = factory->collection(name).incarnations();
        std::lock_guard lock(slotsMutex);
        slots[name].retained.insert(incarnations.begin(), incarnations.end());
      } catch (const std::exception& listing) { LOG_WARN("Cannot list incarnations of '{}': {}", name, listing.what()); }
    }
    rows.push_back(std::move(row));
  }
  return rows;
}

std::shared_ptr<Collection> Collections::unavailable(const std::string& name, std::string reason) {
  auto placeholder = Collection::placeholder(name, std::move(reason));
  if (!map.insert(name, placeholder)) return map.get(name);
  events.registered(name, placeholder);
  return placeholder;
}

std::shared_ptr<Collection> Collections::getOrCreate(std::string_view name, bool autoCreate) {
  std::string key(name);
  validateName(key);
  if (auto existing = map.get(key)) return existing;
  if (!autoCreate) throw CollectionNotFoundError("collection '" + key + "' does not exist");
  bool created = false;
  auto collection = map.getOrCreate(key, [&] {
    auto col = initWriter(key);
    created = true;
    LOG_INFO("Created collection: {}", key);
    return col;
  });
  if (created) events.registered(key, collection);
  return collection;
}

std::shared_ptr<Collection> Collections::create(std::string_view name, const api::SchemaDef* schema) {
  std::string key(name);
  validateName(key);
  bool createdHere = false;
  std::exception_ptr createFailure;
  auto collection = map.getOrCreate(key, [&]() {
    createdHere = true;
    std::shared_ptr<Collection> created;
    bool stored = false;
    try {
      auto initialSchema = Schema::createDefaultSchema();
      if (schema) initialSchema = Schema::fromProto(*schema, initialSchema.get());
      factory->collection(key).createExclusive();
      stored = true;
      created = initWriter(key, std::move(initialSchema));
      LOG_INFO("Created collection: {}", key);
      return created;
    } catch (...) {
      createFailure = std::current_exception();
      try {
        if (created && created->shard && created->shard->iw) created->shard->iw->close();
        if (stored) factory->collection(key).remove();
      } catch (const std::exception& e) {
        return Collection::placeholder(key, "create failed and cleanup failed: " + std::string(e.what()));
      } catch (...) {
        return Collection::placeholder(key, "create failed and cleanup failed: unknown non-standard exception");
      }
      std::rethrow_exception(createFailure);
    }
  });
  if (createdHere) events.registered(key, collection);
  if (createFailure) std::rethrow_exception(createFailure);
  if (!createdHere) throw CollectionExistsError("collection '" + key + "' already exists");
  return collection;
}

void Collections::remove(std::string_view name, bool mustExist) {
  // Empty means the request never named a collection - a malformed request,
  // not a missing collection.
  if (name.empty()) throw InvalidCollectionNameError("collection name is empty");
  // Otherwise lookup-only, no name validation: every map key is
  // filesystem-safe (from a validated create or a startup directory listing),
  // and a tombstoned legacy-named collection must stay deletable.
  std::string key(name);
  auto storage = factory->collection(key);
  Transition transition(*this, key);
  {
    std::lock_guard lock(slotsMutex);
    if (!slots[key].leased.empty()) throw CollectionUnavailableError("collection '" + key + "' has a staged replacement");
  }
  auto collection = map.get(key);
  if (!collection) {
    if (mustExist) {
      validateName(key);
      throw CollectionNotFoundError("collection '" + key + "' does not exist");
    }
    storage.remove();
    return;
  }
  // The placeholder answers resolution while the transition holds the name.
  auto deleting = Collection::placeholder(key, std::string(DELETING_REASON));
  if (!map.replace(key, collection, deleting)) {
    throw CollectionUnavailableError("collection '" + key + "' changed while deletion started");
  }
  events.registered(key, deleting);
  auto fail = [&](std::string reason) {
    auto failed = Collection::placeholder(key, "delete failed: " + reason);
    if (map.replace(key, deleting, failed)) events.registered(key, failed);
  };
  try {
    if (auto shard = collection->getShard()) {
      if (shard->iw) shard->iw->close();
      shard->snapshots->close();
    }
    storage.remove();
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
    throw std::runtime_error("collection '" + key + "' tombstone disappeared during deletion");
  }
  events.removed(key);
}

Collections::Candidate Collections::stage(std::string_view name, std::string_view incarnation) {
  std::string key(name);
  {
    std::lock_guard lock(slotsMutex);
    slots[key].leased.insert(std::string(incarnation));
  }
  auto lease = std::unique_ptr<Lease>(new Lease{*this, key, std::string(incarnation)});
  auto collection = makeCollection(key, factory->collection(key).create(incarnation));
  return Candidate(key, std::string(incarnation), std::move(collection), std::move(lease));
}

Collections::Prepared Collections::prepare(Candidate& candidate, std::shared_ptr<const CommitSnapshot> snapshot) {
  if (snapshot->id.incarnation != candidate.incarnation_) throw std::logic_error("snapshot of another incarnation");
  auto& registry = candidate.collection_->getShard()->getSnapshots();
  // An interrupted activation may already have committed this snapshot.
  auto committed = registry.snapshot();
  if (!committed || committed->id != snapshot->id) {
    auto opened = registry.readers.prepare(*snapshot);
    registry.commit(std::move(snapshot), std::move(opened));
  }
  return Prepared(std::move(candidate));
}

std::shared_ptr<Collection> Collections::install(Prepared prepared, const std::shared_ptr<Collection>& expected) {
  auto& candidate = prepared.candidate;
  const auto& name = candidate.name_;
  const auto& incarnation = candidate.incarnation_;
  auto collection = candidate.collection_;
  auto storage = factory->collection(name);
  Transition transition(*this, name);
  if (map.get(name) != expected) throw std::runtime_error("collection changed during installation");
  // Allocate what registration needs before the commit point.
  observe(name, *collection);
  std::shared_ptr<Collection> reserved;
  if (!expected) {
    reserved = Collection::placeholder(name, "being installed");
    if (!map.insert(name, reserved)) throw std::runtime_error("collection changed during installation");
  }
  // The commit point. Selection is rewritten and synced even if CURRENT
  // already names the candidate: an earlier attempt may have renamed it into
  // place without making it durable. On failure nothing is retired.
  try { storage.select(incarnation); }
  catch (...) {
    if (reserved) map.erase(name, reserved);
    throw;
  }
  // The transition owns the name, so the entry is still the expected one.
  map.replace(name, reserved ? reserved : expected, collection);
  events.registered(name, collection);
  if (auto shard = expected ? expected->getShard() : nullptr) shard->snapshots->detach();
  {
    std::lock_guard lock(slotsMutex);
    slots[name].retained.clear();
  }
  candidate.lease.reset();
  retire(name, incarnation);
  return collection;
}

void Collections::retainSelected(const std::string& name) noexcept {
  try {
    Transition transition(*this, name);
    if (auto selected = factory->collection(name).current()) retire(name, *selected);
  } catch (const std::exception& e) { LOG_WARN("Incarnation cleanup '{}' skipped: {}", name, e.what()); }
}

void Collections::discard(Candidate& candidate) noexcept {
  if (!candidate.collection_) return;
  candidate.collection_->shard->snapshots->close();
  candidate.collection_.reset();
  candidate.lease.reset();
  try {
    Transition transition(*this, candidate.name_);
    auto storage = factory->collection(candidate.name_);
    // An unreadable CURRENT keeps the directory: it may be the selected one.
    if (storage.current() == candidate.incarnation_) return;
    {
      std::lock_guard lock(slotsMutex);
      auto it = slots.find(candidate.name_);
      if (it != slots.end() && (it->second.leased.contains(candidate.incarnation_) || it->second.retained.contains(candidate.incarnation_))) return;
    }
    storage.removeIncarnation(candidate.incarnation_);
  } catch (const std::exception& e) {
    LOG_WARN("Candidate cleanup '{}/{}' skipped: {}", candidate.name_, candidate.incarnation_, e.what());
  }
}

}
