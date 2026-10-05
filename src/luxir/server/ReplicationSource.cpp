// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "ReplicationSource.h"
#include "Collections.h"
#include <charconv>
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

ReplicationSource::ReplicationSource(std::chrono::milliseconds liveness, Now now, uint64_t maxAcknowledgments)
    : boot(newUuid()), maxAcknowledgments(maxAcknowledgments), liveness(liveness), now(std::move(now)) {}

ReplicationSource::Watch ReplicationSource::takeWatchLocked(uint64_t id) {
  auto entry = watches.extract(id);
  auto& watch = entry.mapped();
  if (watch.tenants.empty()) allWatches.erase(id);
  else for (const auto& tenant : watch.tenants) {
    auto it = tenantWatches.find(tenant);
    it->second.watches.erase(id);
    if (!it->second.revision && it->second.watches.empty()) tenantWatches.erase(it);
  }
  return std::move(watch);
}

std::map<uint64_t, std::function<void()>> ReplicationSource::changedLocked(const std::string& tenant) {
  auto& state = tenantWatches[tenant];
  state.revision = ++revision;
  std::map<uint64_t, std::function<void()>> ready;
  while (!allWatches.empty()) {
    auto id = *allWatches.begin();
    ready.emplace(id, takeWatchLocked(id).completion);
  }
  while (!state.watches.empty()) {
    auto id = *state.watches.begin();
    ready.emplace(id, takeWatchLocked(id).completion);
  }
  return ready;
}

void ReplicationSource::pruneLocked(const CollectionId& name) {
  auto current = collections.find(name);
  for (auto& [id, follower] : followers) {
    auto it = follower.commits.find(name);
    if (it != follower.commits.end() && (current == collections.end() || !current->second.commit
        || current->second.commit->incarnation != it->second.commit.incarnation)) {
      follower.commits.erase(it);
      acknowledgments--;
    }
  }
}

void ReplicationSource::registered(const CollectionId& name, const std::shared_ptr<Collection>& collection) noexcept {
  try {
    std::map<uint64_t, std::function<void()>> ready;
    {
      std::lock_guard lock(mutex);
      auto& state = collections[name];
      if (state.collection.lock() != collection) state = {collection, {}, 0, {}};
      state.refresh();
      pruneLocked(name);
      ready = changedLocked(name.tenant);
    }
    notify(ready);
  } catch (...) { LOG_ERROR("Replication registration notification failed"); }
}

void ReplicationSource::updated(const CollectionId& name, const Collection& collection) noexcept {
  try {
    std::map<uint64_t, std::function<void()>> ready;
    {
      std::lock_guard lock(mutex);
      auto it = collections.find(name);
      if (it == collections.end() || it->second.collection.lock().get() != &collection) return;
      it->second.refresh();
      pruneLocked(name);
      ready = changedLocked(name.tenant);
    }
    notify(ready);
  } catch (...) { LOG_ERROR("Replication publication notification failed"); }
}

void ReplicationSource::removed(const CollectionId& name) noexcept {
  try {
    std::map<uint64_t, std::function<void()>> ready;
    {
      std::lock_guard lock(mutex);
      collections.erase(name);
      pruneLocked(name);
      ready = changedLocked(name.tenant);
    }
    notify(ready);
  } catch (...) { LOG_ERROR("Replication deletion notification failed"); }
}

void ReplicationSource::expireLocked() {
  auto time = now();
  std::erase_if(followers, [&](const auto& entry) {
    if (time - entry.second.seen < liveness) return false;
    acknowledgments -= entry.second.commits.size();
    return true;
  });
}

void ReplicationSource::seenLocked(std::string_view follower) {
  if (follower.empty()) return; // anonymous curl discovery is allowed
  validateFollower(follower);
  auto it = followers.find(follower);
  if (it == followers.end()) {
    if (followers.size() >= 4096) fullTable();
    it = followers.emplace(std::string(follower), LiveFollower{}).first;
  }
  it->second.seen = now();
  it->second.lastSeen = wallTime();
}

uint64_t ReplicationSource::watch(std::string_view cursor, std::string_view follower,
                                 std::function<void()> completion, std::span<const std::string> tenants) {
  std::set<std::string> subscription;
  for (const auto& tenant : tenants) {
    Collections::validateName(tenant, "tenant");
    subscription.insert(tenant);
  }
  uint64_t parked = 0;
  bool membershipChanged = false;
  {
    std::lock_guard lock(mutex);
    expireLocked();
    seenLocked(follower);
    if (!follower.empty()) {
      auto& live = followers.find(follower)->second;
      membershipChanged = live.tenants != subscription;
      live.tenants = subscription;
      acknowledgments -= std::erase_if(live.commits, [&](const auto& entry) { return !live.includes(entry.first); });
    }
    uint64_t relevant = revision;
    if (!subscription.empty()) {
      relevant = 0;
      for (const auto& tenant : subscription) {
        auto it = tenantWatches.find(tenant);
        if (it != tenantWatches.end()) relevant = std::max(relevant, it->second.revision);
      }
    }
    auto prefix = boot + ":";
    uint64_t since = 0;
    bool current = false;
    if (cursor.starts_with(prefix)) {
      auto number = cursor.substr(prefix.size());
      auto [end, error] = std::from_chars(number.data(), number.data() + number.size(), since);
      current = error == std::errc() && end == number.data() + number.size() && since >= relevant && since <= revision;
    }
    if (current) {
      parked = ++nextWatch;
      watches.emplace(parked, Watch{subscription, std::move(completion)});
      if (subscription.empty()) allWatches.insert(parked);
      else for (const auto& tenant : subscription) tenantWatches[tenant].watches.insert(parked);
    }
  }
  if (membershipChanged && acknowledgmentsChanged) acknowledgmentsChanged(nullptr);
  if (!parked) completion();
  return parked;
}

void ReplicationSource::cancel(uint64_t watch) {
  if (!watch) return;
  std::lock_guard lock(mutex);
  if (watches.contains(watch)) takeWatchLocked(watch);
}

api::ReplicationCatalog ReplicationSource::catalog(std::pmr::memory_resource& arena, std::span<const std::string> tenants) {
  std::lock_guard lock(mutex);
  api::ReplicationCatalog response;
  response.boot = api::build::arenaStr(arena, boot);
  response.cursor = api::build::arenaStr(arena, boot + ":" + std::to_string(revision));
  auto wanted = [&](const CollectionId& id) { return tenants.empty() || std::ranges::find(tenants, id.tenant) != tenants.end(); };
  auto* entries = api::build::allocArray(response.collections,
      (size_t)std::ranges::count_if(collections, [&](const auto& entry) { return wanted(entry.first); }), arena);
  size_t i = 0;
  for (const auto& [id, state] : collections) {
    if (!wanted(id)) continue;
    auto& entry = entries[i++];
    entry.tenant = api::build::arenaStr(arena, id.tenant);
    entry.collection = api::build::arenaStr(arena, id.name);
    entry.available = state.error.empty();
    if (state.commit) {
      entry.commit = api::build::arenaStr(arena, state.commit->token());
      entry.manifest_xxh3 = state.digest;
    }
  }
  return response;
}

void ReplicationSource::installed(const api::ReplicationInstalled& request) {
  validateFollower(request.follower);
  auto id = CommitId::parse(request.commit);
  if (request.tenant.empty() || request.collection.empty()) throw std::invalid_argument("expected follower, tenant, collection and commit");
  CollectionId target(std::string(request.tenant), std::string(request.collection));
  {
    std::lock_guard lock(mutex);
    expireLocked();
    auto collection = collections.find(target);
    if (collection == collections.end() || !collection->second.commit) throw std::invalid_argument("unknown installed collection");
    const auto& current = *collection->second.commit;
    if (id.incarnation != current.incarnation || id.index_gen > current.index_gen)
      throw std::invalid_argument("installed commit does not belong to the current collection");
    auto follower = followers.find(request.follower);
    if (follower != followers.end() && !follower->second.includes(target))
      throw std::invalid_argument("installed collection is outside follower subscription");
    bool extraRow = follower == followers.end() || !follower->second.commits.contains(target);
    if (extraRow && acknowledgments >= maxAcknowledgments)
      throw ApiError(ErrorKind::RESOURCE_EXHAUSTED, "too_many_acknowledgments", "acknowledgment row budget is full");
    seenLocked(request.follower);
    auto [entry, added] = followers.find(request.follower)->second.commits.try_emplace(target);
    if (added) {
      acknowledgments++;
      entry->second.membership = ++nextMembership;
    }
    auto& installed = entry->second.commit;
    if (installed.incarnation != id.incarnation || installed.index_gen < id.index_gen) installed = std::move(id);
  }
  if (acknowledgmentsChanged) acknowledgmentsChanged(&target);
}

std::vector<ReplicationSource::Follower> ReplicationSource::status() {
  std::lock_guard lock(mutex);
  expireLocked();
  std::vector<Follower> result;
  for (const auto& [id, follower] : followers) {
    if (follower.commits.empty()) result.push_back({id, {}, {}, follower.lastSeen, {}});
    for (const auto& [name, commit] : follower.commits)
      result.push_back({id, name, commit.commit, follower.lastSeen, collections.at(name).commit->index_gen - commit.commit.index_gen});
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
  if (acknowledgmentsChanged) acknowledgmentsChanged(nullptr);
}

ReplicationSource::Barrier ReplicationSource::capture(const api::ReplicaRequirement& requirement,
                                                       const CollectionId& collection, const CommitId& commit) {
  if (requirement.kind.index() == 0) throw RequestError("wait_for_replicas requires count or all");
  Barrier result{std::holds_alternative<api::AllReplicas>(requirement.kind), 0, {}};
  if (!result.all) result.wanted = std::get<uint32_t>(requirement.kind);
  else {
    std::lock_guard lock(mutex);
    expireLocked();
    for (const auto& [id, follower] : followers) {
      auto serving = follower.commits.find(collection);
      if (follower.includes(collection) && serving != follower.commits.end()
          && serving->second.commit.incarnation == commit.incarnation) result.members.emplace(id, serving->second.membership);
    }
  }
  return result;
}

api::ReplicaResult ReplicationSource::progress(const Barrier& barrier, const CollectionId& collection, const CommitId& id) {
  std::lock_guard lock(mutex);
  expireLocked();
  uint32_t wanted = barrier.all ? 0 : barrier.wanted, serving = 0;
  for (const auto& [name, follower] : followers) {
    if (!follower.includes(collection)) continue;
    auto commit = follower.commits.find(collection);
    if (barrier.all) {
      auto member = barrier.members.find(name);
      if (member == barrier.members.end() || commit == follower.commits.end()
          || member->second != commit->second.membership) continue;
      wanted++;
    }
    if (commit != follower.commits.end() && commit->second.commit.incarnation == id.incarnation
        && commit->second.commit.index_gen >= id.index_gen) serving++;
  }
  return {wanted, serving, api::ReplicaResult::Outcome::SATISFIED};
}


}
