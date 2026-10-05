// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/server/ReplicationCatalog.h"
#include "LuxirNode.h"
#include "Promotion.h"
#include "ReplicationFollower.h"
#include "ReplicationState.h"
#include "luxir/util/DateTime.h"

namespace luxir {

LuxirNode::LuxirNode(LuxirConfig config, Mode mode)
  : config(std::move(config)) {
  if (this->config.promote && !this->config.replication.source.empty()) throw std::invalid_argument("--promote cannot be combined with --replicate-from");
  if (this->config.promote && this->config.store.backend == "ram") throw std::invalid_argument("--promote requires the FS backend");
  if (following() && this->config.read_only) throw std::invalid_argument("replication.source cannot be combined with read-only");
  // A config assembled in code (tests, embedding) has not been through
  // normalize(), so resolve the RAM sentinels here too - before the writers
  // opened below take a pointer to the budget.
  this->config.resolveRamBudgets();
  auto& replicationConfig = this->config.replication;
  replicationConfig.validate();
  replication = std::make_shared<ReplicationCatalog>(std::chrono::milliseconds(
      replicationConfig.follower_timeout_ms));
  events = replication;
  indexRamBudget.setTotalBytes(this->config.index.max_ram_mb * 1024 * 1024);
  preWarmTimeZoneDatabase();
  collections_ = std::make_unique<Collections>(this->config, *events, indexRamBudget);

  if (following()) {
    // The follower first checks that it owns the data directory.
    follower = std::make_unique<ReplicationFollower>(*this);
    follower->seed(collections_->open(Collections::Role::FOLLOWER));
  } else {
    std::map<std::string, std::string> promotionErrors;
    // Promotion is a storage operation before any writer opens the replicas.
    // Hard links retain immutable data without copying it. CURRENT still selects
    // the old complete snapshot until the new root and directory are durable.
    if (!this->config.read_only && this->config.store.backend == "fs") {
      FSDirectory metadata(this->config.store.data_dir);
      if (auto binding = metadata.openFile("replication.json")) {
        if (!this->config.promote) throw std::runtime_error("Follower-bound data directory: use --promote to start a writer");
        auto state = ReplicationState::read(*binding);
        promotionErrors = promote(collections_->storage(), metadata, state);
        if (promotionErrors.empty()) {
          LOG_INFO("promoted from follower of {}", state.source);
        } else {
          LOG_WARN("Promotion from follower of {} incomplete; restart with --promote to retry failed collections", state.source);
        }
      } else if (this->config.promote) {
        LOG_INFO("No follower binding; nothing to promote");
      }
    }
    collections_->open(this->config.read_only ? Collections::Role::READ_ONLY : Collections::Role::WRITER, promotionErrors);
  }
  searchEngine = std::make_unique<SearchEngine>(*this);
  if (follower && mode == Mode::SERVE) follower->start();
}

LuxirNode::~LuxirNode() {
  if (follower) follower->stop();
  replication->closeWaits();
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

std::map<std::string, CommitId> LuxirNode::replicationCollections() {
  std::map<std::string, CommitId> result;
  for (const auto& entry : collectionEntries()) {
    if (auto shard = entry.collection->getShard()) {
      if (auto snapshot = shard->getSnapshots().snapshot()) result.emplace(entry.name, snapshot->id);
    }
  }
  return result;
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
