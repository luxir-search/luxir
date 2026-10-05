// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/server/ReplicationSource.h"
#include "CommitWaits.h"
#include "LuxirNode.h"
#include "ReplicationFollower.h"
#include "luxir/util/DateTime.h"

namespace luxir {

LuxirNode::LuxirNode(LuxirConfig config, Mode mode)
  : config(std::move(config)) {
  if (following() && this->config.read_only) throw std::invalid_argument("replication.source cannot be combined with read-only");
  // A config assembled in code (tests, embedding) has not been through
  // normalize(), so resolve the RAM sentinels here too - before the writers
  // opened below take a pointer to the budget.
  this->config.resolveRamBudgets();
  auto& replicationConfig = this->config.replication;
  replicationConfig.validate();
  replication = std::make_shared<ReplicationSource>(std::chrono::milliseconds(
      replicationConfig.follower_timeout_ms));
  commitWaits = std::make_shared<CommitWaits>(*replication, following());
  replication->onAcknowledgmentsChanged([weak = std::weak_ptr(commitWaits)](std::string_view name) {
    if (auto waits = weak.lock()) waits->acknowledged(name);
  });
  class Events final : public CollectionEvents {
    std::shared_ptr<ReplicationSource> source;
    std::shared_ptr<CommitWaits> waits;
  public:
    Events(std::shared_ptr<ReplicationSource> source, std::shared_ptr<CommitWaits> waits)
        : source(std::move(source)), waits(std::move(waits)) {}
    void registered(const std::string& name, const std::shared_ptr<Collection>& collection) noexcept override {
      waits->registered(name, collection); source->registered(name, collection);
    }
    void updated(const std::string& name, const Collection& collection) noexcept override {
      waits->updated(name, collection); source->updated(name, collection);
    }
    void removed(const std::string& name) noexcept override {
      waits->removed(name); source->removed(name);
    }
  };
  events = std::make_shared<Events>(replication, commitWaits);
  indexRamBudget.setTotalBytes(this->config.index.max_ram_mb * 1024 * 1024);
  preWarmTimeZoneDatabase();
  collections_ = std::make_unique<Collections>(this->config, *events, indexRamBudget);

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
  if (follower && mode == Mode::SERVE) follower->start();
}

LuxirNode::~LuxirNode() {
  if (follower) follower->stop();
  commitWaits->close();
}

std::shared_ptr<Collection> LuxirNode::checkLoaded(std::shared_ptr<Collection> collection) {
  if (collection) {
    auto reason = collection->getUnavailableReason();
    if (!reason.empty()) throw CollectionUnavailableError(
        "collection '" + collection->getName() + "' is unavailable: " + reason);
  }
  return collection;
}

std::shared_ptr<Collection> LuxirNode::getCollection(std::string_view name) {
  // Resolution is lookup-only on the hot path: an invalid name can never be in
  // the map, so the validation scan runs only on a miss, to tell "you wrote a
  // name that cannot exist" (INVALID_REQUEST) from "no such collection"
  // (NOT_FOUND) the same way every route does.
  if (auto collection = collections_->get(name)) return checkLoaded(std::move(collection));
  Collections::validateName(name);
  throw CollectionNotFoundError("collection '" + std::string(name) + "' does not exist");
}

std::shared_ptr<Collection> LuxirNode::resolveCollection(std::string_view name) {
  return getCollection(name.empty() ? kDefaultCollectionName : name);
}

std::shared_ptr<Collection> LuxirNode::getOrCreateCollection(std::string_view name) {
  if (following()) return getCollection(name);
  return checkLoaded(collections_->getOrCreate(name, config.ingest.auto_create_collection));
}

std::shared_ptr<Collection> LuxirNode::resolveOrCreateCollection(std::string_view name) {
  return getOrCreateCollection(name.empty() ? kDefaultCollectionName : name);
}

std::shared_ptr<Collection> LuxirNode::createCollection(std::string_view name, const api::SchemaDef* schema) {
  if (following()) throw ReadOnlyError("cannot create collections on a follower");
  return collections_->create(name, schema);
}

void LuxirNode::deleteCollection(std::string_view name) {
  if (follower) follower->deleteOrphan(name);
  else collections_->remove(name);
}

} // namespace luxir
