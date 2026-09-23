// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "ReplicationCatalog.h"
#include "LuxirNode.h"
#include "luxir/util/Uuid.h"
#include <glaze/glaze.hpp>
#include "luxir/util/log.h"
#include "luxir/util/Signal.h"

namespace luxir {
namespace {
struct CatalogCollection { std::string commit; std::string state; };
struct CatalogResponse {
  std::string boot;
  std::string cursor;
  std::map<std::string, CatalogCollection> collections;
};
struct InstalledRequest { std::string follower; std::string collection; std::string commit; };

void notify(std::map<uint64_t, std::function<void()>>& watches) {
  if (!watches.empty()) Signal::emit("replicationWatchNotify");
  for (auto& [id, completion] : watches) {
    try { completion(); }
    catch (const std::exception& e) { LOG_WARN("Replication watch completion failed: {}", e.what()); }
    catch (...) { LOG_WARN("Replication watch completion failed"); }
  }
}

uint64_t wallTime() {
  return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
}
void validateFollower(std::string_view follower) {
  if (follower.empty() || follower.size() > 255) throw std::invalid_argument("invalid follower id");
}
void fullTable() {
  throw ApiError(ErrorKind::RESOURCE_EXHAUSTED, "too_many_followers", "live follower table is full");
}
}

ReplicationCatalog::ReplicationCatalog(std::chrono::milliseconds liveness, Now now)
    : boot(newUuid()), liveness(liveness), now(std::move(now)) {}

void ReplicationCatalog::changed(std::string_view name, std::string_view incarnation) noexcept {
  try {
    decltype(watches) ready;
    {
      std::lock_guard lock(mutex);
      revision++;
      if (!name.empty()) for (auto& [id, follower] : followers) {
        auto it = follower.commits.find(name);
        if (it != follower.commits.end() && it->second.incarnation != incarnation) follower.commits.erase(it);
      }
      ready.swap(watches);
    }
    notify(ready);
  } catch (...) { LOG_ERROR("Replication publication notification failed"); }
}

void ReplicationCatalog::remove(const std::string& name) noexcept {
  try {
    decltype(watches) ready;
    {
      std::lock_guard lock(mutex);
      revision++;
      for (auto& [id, follower] : followers) follower.commits.erase(name);
      ready.swap(watches);
    }
    notify(ready);
  } catch (...) { LOG_ERROR("Replication deletion notification failed"); }
}

void ReplicationCatalog::expireLocked() {
  auto time = now();
  std::erase_if(followers, [&](const auto& entry) { return time - entry.second.seen >= liveness; });
}

size_t ReplicationCatalog::rowsLocked() const {
  size_t rows = 0;
  for (const auto& [id, follower] : followers) rows += std::max((size_t)1, follower.commits.size());
  return rows;
}

void ReplicationCatalog::pruneLocked(const std::map<std::string, CommitId>& collections) {
  expireLocked();
  for (auto& [id, follower] : followers) {
    std::erase_if(follower.commits, [&](const auto& entry) {
      auto it = collections.find(entry.first);
      return it == collections.end() || it->second.incarnation != entry.second.incarnation;
    });
  }
}

void ReplicationCatalog::seenLocked(std::string_view follower) {
  if (follower.empty()) return; // anonymous curl discovery is allowed
  validateFollower(follower);
  auto it = followers.find(follower);
  if (it == followers.end()) {
    if (rowsLocked() >= 4096) fullTable();
    it = followers.emplace(std::string(follower), LiveFollower{}).first;
  }
  it->second.seen = now();
  it->second.lastSeen = wallTime();
}

uint64_t ReplicationCatalog::watch(std::string_view cursor, std::string_view follower,
                                 std::function<void()> completion) {
  {
    std::lock_guard lock(mutex);
    expireLocked();
    seenLocked(follower);
    if (cursor == boot + ":" + std::to_string(revision)) {
      auto id = ++nextWatch;
      watches.emplace(id, std::move(completion));
      return id;
    }
  }
  completion();
  return 0;
}

void ReplicationCatalog::cancel(uint64_t watch) {
  if (!watch) return;
  std::lock_guard lock(mutex);
  watches.erase(watch);
}

std::string ReplicationCatalog::catalog(LuxirNode& node) {
  CatalogResponse response;
  {
    // Capture the cursor before walking the live view. Concurrent publication
    // can make the view newer, but the next watch will still observe its revision.
    std::lock_guard lock(mutex);
    response.boot = boot;
    response.cursor = boot + ":" + std::to_string(revision);
  }
  for (const auto& entry : node.collectionEntries()) {
    CatalogCollection value{{}, entry.error.empty() ? "available" : "unavailable"};
    if (auto shard = entry.collection->getShard()) {
      if (auto snapshot = shard->getSnapshots().snapshot()) value.commit = snapshot->id.token();
    }
    response.collections.emplace(entry.name, std::move(value));
  }
  return glz::write_json(response).value();
}

void ReplicationCatalog::installed(LuxirNode& node, std::string_view body) {
  InstalledRequest request;
  if (glz::read_json(request, body) || request.collection.empty()) {
    throw std::invalid_argument("expected follower, collection and commit token");
  }
  validateFollower(request.follower);
  auto id = CommitId::parse(request.commit);
  for (;;) {
    uint64_t observed;
    { std::lock_guard lock(mutex); observed = revision; }
    auto collections = node.replicationCollections();
    std::lock_guard lock(mutex);
    if (observed != revision) continue;
    pruneLocked(collections);
    auto collection = collections.find(request.collection);
    if (collection == collections.end()) throw std::invalid_argument("unknown installed collection");
    if (id.incarnation != collection->second.incarnation || id.index_gen > collection->second.index_gen) {
      throw std::invalid_argument("installed commit does not belong to the current collection");
    }
    auto follower = followers.find(request.follower);
    bool extraRow = follower == followers.end() ||
        (!follower->second.commits.empty() && !follower->second.commits.contains(request.collection));
    if (extraRow && rowsLocked() >= 4096) fullTable();
    seenLocked(request.follower);
    auto& installed = followers.at(request.follower).commits[request.collection];
    if (installed.incarnation != id.incarnation || installed.index_gen < id.index_gen) installed = std::move(id);
    return;
  }
}

std::vector<ReplicationCatalog::Follower> ReplicationCatalog::stats(LuxirNode& node) {
  for (;;) {
    uint64_t observed;
    { std::lock_guard lock(mutex); if (followers.empty()) return {}; observed = revision; }
    auto collections = node.replicationCollections();
    std::lock_guard lock(mutex);
    if (observed != revision) continue;
    pruneLocked(collections);
    std::vector<Follower> result;
    for (const auto& [id, follower] : followers) {
      if (follower.commits.empty()) result.push_back({id, {}, {}, follower.lastSeen, {}});
      for (const auto& [name, commit] : follower.commits) {
        result.push_back({id, name, commit, follower.lastSeen, collections.at(name).index_gen - commit.index_gen});
      }
    }
    return result;
  }
}

void ReplicationCatalog::seen(std::string_view follower) {
  std::lock_guard lock(mutex);
  expireLocked();
  seenLocked(follower);
}

void ReplicationCatalog::expire() {
  std::lock_guard lock(mutex);
  expireLocked();
}

}
