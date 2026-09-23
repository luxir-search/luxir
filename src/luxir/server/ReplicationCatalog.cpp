// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "ReplicationCatalog.h"
#include "LuxirNode.h"
#include "luxir/util/Uuid.h"
#include <glaze/glaze.hpp>
#include <charconv>
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
    Pending completed;
    {
      std::lock_guard lock(mutex);
      revision++;
      if (!name.empty()) for (auto& [id, follower] : followers) {
        auto it = follower.commits.find(name);
        if (it != follower.commits.end() && it->second.incarnation != incarnation) follower.commits.erase(it);
      }
      ready.swap(watches);
      if (!waits.empty()) checkLocked(name, Event::CHANGED, completed, false, incarnation);
    }
    notify(ready);
    finish(completed);
  } catch (...) { LOG_ERROR("Replication publication notification failed"); }
}

void ReplicationCatalog::remove(const std::string& name) noexcept {
  try {
    decltype(watches) ready;
    Pending completed;
    {
      std::lock_guard lock(mutex);
      revision++;
      for (auto& [id, follower] : followers) follower.commits.erase(name);
      ready.swap(watches);
      if (!waits.empty()) checkLocked(name, Event::REMOVED, completed);
    }
    notify(ready);
    finish(completed);
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
  Pending completed;
  for (;;) {
    uint64_t observed;
    { std::lock_guard lock(mutex); observed = revision; }
    auto collections = node.replicationCollections();
    std::unique_lock lock(mutex);
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
    if (!waits.empty()) checkLocked(request.collection, Event::CHECK, completed);
    lock.unlock();
    finish(completed);
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
  Pending completed;
  {
    std::lock_guard lock(mutex);
    expireLocked();
    for (auto it = waits.begin(); it != waits.end();) {
      auto name = (it++)->first;
      checkLocked(name, Event::CHECK, completed, true);
    }
  }
  finish(completed);
}

uint32_t ReplicationCatalog::replicaCount(std::string_view value) {
  if (value == "all") return 0;
  uint32_t count = 0;
  auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), count);
  if (value.empty() || error != std::errc() || end != value.data() + value.size())
    throw RequestError("wait_for_replicas must be a nonnegative integer or all");
  return count;
}

ReplicationCatalog::Barrier ReplicationCatalog::barrier(std::string_view wanted, std::string_view collection, const CommitId& commit) {
  Barrier result{wanted == "all", replicaCount(wanted), {}};
  if (result.all) {
    std::lock_guard lock(mutex);
    expireLocked();
    for (const auto& [id, follower] : followers) {
      auto serving = follower.commits.find(collection);
      if (serving != follower.commits.end() && serving->second.incarnation == commit.incarnation) result.members.insert(id);
    }
  }
  return result;
}

api::ReplicaResult ReplicationCatalog::progressLocked(const Barrier& barrier, std::string_view collection, const CommitId& id) {
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
  return {wanted, serving, false};
}

void ReplicationCatalog::finish(Pending& ready) {
  for (auto& wait : ready) wait->complete();
}

void ReplicationCatalog::armLocked() {
  if (!deadlines.empty()) DeadlineScheduler::global().arm(deadline, deadlines.begin()->first.first);
}

void ReplicationCatalog::checkLocked(std::string_view collection, Event event, Pending& ready, bool allOnly, std::string_view incarnation) {
  auto it = waits.find(collection);
  if (it == waits.end()) return;
  std::erase_if(it->second, [&](const auto& wait) {
    if (allOnly && !wait->all) return false;
    auto reason = event;
    if (event == Event::CHANGED && !wait->incarnation.empty() && wait->incarnation != incarnation) reason = Event::RECREATED;
    if (Clock::now() >= wait->deadline) reason = Event::DEADLINE;
    if (!wait->ready(reason)) return false;
    deadlines.erase({wait->deadline, wait->id});
    ready.push_back(wait);
    return true;
  });
  if (it->second.empty()) waits.erase(it);
}

bool ReplicationCatalog::await(std::string collection, std::function<bool(Event)> ready,
                               std::function<void()> complete, uint64_t timeoutMs, std::stop_token stop, bool all, std::string incarnation) {
  auto time = Clock::now();
  auto maximum = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::time_point::max() - time).count();
  auto wait = std::make_shared<Wait>();
  wait->deadline = time + std::chrono::milliseconds((int64_t)std::min(timeoutMs, (uint64_t)maximum));
  wait->incarnation = std::move(incarnation);
  wait->all = all; wait->ready = std::move(ready); wait->complete = std::move(complete);
  {
    std::lock_guard lock(mutex);
    auto event = waitsClosed || stop.stop_requested() ? Event::CANCELLED : timeoutMs ? Event::CHECK : Event::DEADLINE;
    if (wait->ready(event)) return true;
    wait->id = ++nextWait;
    waits[collection].push_back(wait);
    deadlines.emplace(std::pair{wait->deadline, wait->id}, collection);
    armLocked();
  }
  wait->cancellation.emplace(stop, [this, collection, id = wait->id] { cancelWait(collection, id); });
  Signal::emit("replicationWaitParked");
  return false;
}

void ReplicationCatalog::awaitBarrier(LuxirNode& node, std::string collection, CommitId id, std::string_view wanted, uint64_t timeoutMs,
                                      std::function<void(api::ReplicaResult, Event)> complete, std::stop_token stop) {
  auto captured = barrier(wanted, collection, id);
  auto result = std::make_shared<api::ReplicaResult>();
  auto event = std::make_shared<Event>(Event::CHECK);
  auto ready = [this, &node, captured, collection, id, result, event](Event cause) {
    if (cause != Event::CANCELLED) try {
      auto current = node.resolveCollection(collection)->getShard()->getSnapshots().snapshot();
      if (current && current->id.incarnation != id.incarnation) cause = Event::RECREATED;
    } catch (const std::exception&) { cause = Event::REMOVED; }
    *result = progressLocked(captured, collection, id);
    *event = cause;
    bool terminal = cause == Event::RECREATED || cause == Event::REMOVED || cause == Event::CANCELLED || cause == Event::DEADLINE;
    result->timed_out = cause == Event::DEADLINE && result->serving < result->wanted;
    return terminal || result->serving >= result->wanted;
  };
  auto delivery = [complete = std::move(complete), result, event] { complete(*result, *event); };
  if (await(std::move(collection), std::move(ready), delivery, timeoutMs, stop, captured.all, id.incarnation)) delivery();
}

void ReplicationCatalog::cancelWait(std::string_view collection, uint64_t id) {
  Pending completed;
  {
    std::lock_guard lock(mutex);
    auto it = waits.find(collection);
    if (it == waits.end()) return;
    std::erase_if(it->second, [&](const auto& wait) {
      if (wait->id != id) return false;
      wait->ready(Event::CANCELLED);
      deadlines.erase({wait->deadline, wait->id});
      completed.push_back(wait); return true;
    });
    if (it->second.empty()) waits.erase(it);
  }
  finish(completed);
}

void ReplicationCatalog::tick() {
  Pending completed;
  {
    std::lock_guard lock(mutex);
    expireLocked();
    while (!deadlines.empty() && deadlines.begin()->first.first <= Clock::now()) {
      auto name = deadlines.begin()->second;
      checkLocked(name, Event::CHECK, completed);
    }
    armLocked();
  }
  finish(completed);
}

void ReplicationCatalog::closeWaits() {
  Pending completed;
  {
    std::lock_guard lock(mutex);
    waitsClosed = true;
    for (auto& [name, pending] : waits) for (auto& wait : pending) {
      wait->ready(Event::CANCELLED); completed.push_back(std::move(wait));
    }
    waits.clear();
    deadlines.clear();
  }
  DeadlineScheduler::global().detach(deadline);
  finish(completed);
}

ReplicationCatalog::~ReplicationCatalog() { closeWaits(); }

}
