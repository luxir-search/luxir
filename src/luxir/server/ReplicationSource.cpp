// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "ReplicationSource.h"
#include "luxir/util/Uuid.h"
#include "luxir/api/build.h"
#include "luxir/util/log.h"
#include "luxir/util/Signal.h"

namespace luxir {
namespace {
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

ReplicationSource::ReplicationSource(std::chrono::milliseconds liveness, Now now)
    : boot(newUuid()), liveness(liveness), now(std::move(now)) {}

void ReplicationSource::pruneLocked(const std::string& name) {
  auto current = collections.find(name);
  for (auto& [id, follower] : followers) {
    auto it = follower.commits.find(name);
    if (it != follower.commits.end() && (current == collections.end() || !current->second.commit
        || current->second.commit->incarnation != it->second.incarnation)) follower.commits.erase(it);
  }
}

void ReplicationSource::registered(const std::string& name, const std::shared_ptr<Collection>& collection) noexcept {
  try {
    decltype(watches) ready;
    {
      std::lock_guard lock(mutex);
      auto& state = collections[name];
      if (state.collection.lock() != collection) state = {collection, {}, {}};
      state.refresh();
      pruneLocked(name);
      revision++;
      ready.swap(watches);
    }
    notify(ready);
  } catch (...) { LOG_ERROR("Replication registration notification failed"); }
}

void ReplicationSource::updated(const std::string& name, const Collection& collection) noexcept {
  try {
    decltype(watches) ready;
    {
      std::lock_guard lock(mutex);
      auto it = collections.find(name);
      if (it == collections.end() || it->second.collection.lock().get() != &collection) return;
      it->second.refresh();
      pruneLocked(name);
      revision++;
      ready.swap(watches);
    }
    notify(ready);
  } catch (...) { LOG_ERROR("Replication publication notification failed"); }
}

void ReplicationSource::removed(const std::string& name) noexcept {
  try {
    decltype(watches) ready;
    {
      std::lock_guard lock(mutex);
      collections.erase(name);
      pruneLocked(name);
      revision++;
      ready.swap(watches);
    }
    notify(ready);
  } catch (...) { LOG_ERROR("Replication deletion notification failed"); }
}

void ReplicationSource::expireLocked() {
  auto time = now();
  std::erase_if(followers, [&](const auto& entry) { return time - entry.second.seen >= liveness; });
}

size_t ReplicationSource::rowsLocked() const {
  size_t rows = 0;
  for (const auto& [id, follower] : followers) rows += std::max((size_t)1, follower.commits.size());
  return rows;
}

void ReplicationSource::seenLocked(std::string_view follower) {
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

uint64_t ReplicationSource::watch(std::string_view cursor, std::string_view follower,
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

void ReplicationSource::cancel(uint64_t watch) {
  if (!watch) return;
  std::lock_guard lock(mutex);
  watches.erase(watch);
}

api::ReplicationCatalog ReplicationSource::catalog(std::pmr::memory_resource& arena) {
  std::lock_guard lock(mutex);
  api::ReplicationCatalog response;
  response.boot = api::build::arenaStr(arena, boot);
  response.cursor = api::build::arenaStr(arena, boot + ":" + std::to_string(revision));
  auto* entries = api::build::allocArray(response.collections, collections.size(), arena);
  size_t i = 0;
  for (const auto& [name, state] : collections) {
    auto& entry = entries[i++];
    entry.first = api::build::arenaStr(arena, name);
    entry.second.available = state.error.empty();
    if (state.commit) entry.second.commit = api::build::arenaStr(arena, state.commit->token());
  }
  return response;
}

void ReplicationSource::installed(const api::ReplicationInstalled& request) {
  validateFollower(request.follower);
  auto id = CommitId::parse(request.commit);
  {
    std::lock_guard lock(mutex);
    expireLocked();
    auto collection = collections.find(request.collection);
    if (collection == collections.end() || !collection->second.commit) throw std::invalid_argument("unknown installed collection");
    const auto& current = *collection->second.commit;
    if (id.incarnation != current.incarnation || id.index_gen > current.index_gen)
      throw std::invalid_argument("installed commit does not belong to the current collection");
    auto follower = followers.find(request.follower);
    bool extraRow = follower == followers.end() ||
        (!follower->second.commits.empty() && !follower->second.commits.contains(request.collection));
    if (extraRow && rowsLocked() >= 4096) fullTable();
    seenLocked(request.follower);
    auto& installed = followers.find(request.follower)->second.commits[std::string(request.collection)];
    if (installed.incarnation != id.incarnation || installed.index_gen < id.index_gen) installed = std::move(id);
  }
  if (acknowledgmentsChanged) acknowledgmentsChanged(request.collection);
}

std::vector<ReplicationSource::Follower> ReplicationSource::status() {
  std::lock_guard lock(mutex);
  expireLocked();
  std::vector<Follower> result;
  for (const auto& [id, follower] : followers) {
    if (follower.commits.empty()) result.push_back({id, {}, {}, follower.lastSeen, {}});
    for (const auto& [name, commit] : follower.commits)
      result.push_back({id, name, commit, follower.lastSeen, collections.at(name).commit->index_gen - commit.index_gen});
  }
  return result;
}

void ReplicationSource::seen(std::string_view follower) {
  std::lock_guard lock(mutex);
  expireLocked();
  seenLocked(follower);
}

std::optional<ReplicationSource::Clock::time_point> ReplicationSource::nextExpiry() {
  std::lock_guard lock(mutex);
  std::optional<Clock::time_point> next;
  for (const auto& [id, follower] : followers) {
    auto due = follower.seen + liveness;
    if (!next || due < *next) next = due;
  }
  return next;
}

void ReplicationSource::expire() {
  { std::lock_guard lock(mutex); expireLocked(); }
  if (acknowledgmentsChanged) acknowledgmentsChanged({});
}

ReplicationSource::Barrier ReplicationSource::capture(const api::ReplicaRequirement& requirement,
                                                       std::string_view collection, const CommitId& commit) {
  if (requirement.kind.index() == 0) throw RequestError("wait_for_replicas requires count or all");
  Barrier result{std::holds_alternative<api::AllReplicas>(requirement.kind), 0, {}};
  if (!result.all) result.wanted = std::get<uint32_t>(requirement.kind);
  else {
    std::lock_guard lock(mutex);
    expireLocked();
    for (const auto& [id, follower] : followers) {
      auto serving = follower.commits.find(collection);
      if (serving != follower.commits.end() && serving->second.incarnation == commit.incarnation) result.members.insert(id);
    }
  }
  return result;
}

api::ReplicaResult ReplicationSource::progress(const Barrier& barrier, std::string_view collection, const CommitId& id) {
  std::lock_guard lock(mutex);
  expireLocked();
  uint32_t wanted = barrier.all ? 0 : barrier.wanted, serving = 0;
  for (const auto& [name, follower] : followers) {
    if (barrier.all) {
      auto member = barrier.members.find(name);
      if (member == barrier.members.end()) continue;
      wanted++;
    }
    auto commit = follower.commits.find(collection);
    if (commit != follower.commits.end() && commit->second.incarnation == id.incarnation
        && commit->second.index_gen >= id.index_gen) serving++;
  }
  return {wanted, serving, api::ReplicaResult::Outcome::SATISFIED};
}


}
