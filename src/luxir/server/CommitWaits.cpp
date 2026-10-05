// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#include "CommitWaits.h"
#include "luxir/util/Signal.h"

namespace luxir {
CommitWaits::Clock::time_point CommitWaits::deadlineAfter(uint64_t timeoutMs) const {
  auto time = now();
  auto maximum = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::time_point::max() - time).count();
  return time + std::chrono::milliseconds((int64_t)std::min(timeoutMs, (uint64_t)maximum));
}

bool CommitWaits::evaluate(Wait& wait, bool cancelled, bool removed) {
  auto it = collections.find(wait.name);
  const auto* current = it == collections.end() ? nullptr : &it->second;
  bool mismatch = current && current->commit && current->commit->incarnation != wait.commit.incarnation;
  bool expired = now() >= wait.deadline;
  if (auto* local = std::get_if<Local>(&wait.kind)) {
    auto fail = [&](ErrorKind kind, const char* code, const char* message) {
      local->result.error = ErrorInfo{kind, code, message};
      return true;
    };
    if (cancelled || closed || (removed && !following))
      return fail(ErrorKind::UNAVAILABLE, "stale_replica", "search floor wait cancelled or collection deleted");
    if (mismatch && (!following || expired))
      return fail(ErrorKind::FAILED_PRECONDITION, "commit_incarnation_mismatch", "min_commit belongs to a different collection incarnation");
    // A follower's removal or replacement is transient; wait it out.
    if (current && !current->error.empty() && !following)
      return fail(ErrorKind::UNAVAILABLE, "stale_replica", current->error.c_str());
    if (current && current->commit && !mismatch && current->commit->index_gen >= wait.commit.index_gen) {
      local->result.collection = current->collection.lock();
      if (!local->result.collection) return fail(ErrorKind::UNAVAILABLE, "stale_replica", "collection no longer available");
      return true;
    }
    if (expired) return fail(ErrorKind::UNAVAILABLE, "stale_replica", "min_commit was not available before min_commit_timeout_ms");
    return false;
  }
  auto& replicas = std::get<Replicas>(wait.kind);
  replicas.result = source.progress(replicas.barrier, wait.name, wait.commit);
  using Outcome = api::ReplicaResult::Outcome;
  if (cancelled || closed || !current || !current->commit || mismatch) replicas.result.outcome = Outcome::CANCELLED;
  else if (replicas.result.serving >= replicas.result.wanted) replicas.result.outcome = Outcome::SATISFIED;
  else if (expired) replicas.result.outcome = Outcome::TIMED_OUT;
  else return false;
  return true;
}

void CommitWaits::finish(Pending& ready) {
  for (auto& wait : ready) try {
    if (auto* local = std::get_if<Local>(&wait->kind)) local->complete(std::move(local->result));
    else {
      auto& replicas = std::get<Replicas>(wait->kind);
      Signal::emit("replicaWaitCompleted", &replicas.result);
      replicas.complete(replicas.result);
    }
  } catch (const std::exception& e) { LOG_WARN("Commit wait completion failed: {}", e.what()); }
    catch (...) { LOG_WARN("Commit wait completion failed"); }
}

void CommitWaits::armLocked() {
  if (deadlines.empty()) return;
  // Convert the injected clock's remaining duration to the scheduler clock.
  // A test clock may be anywhere on its timeline; never arm a real past deadline
  // repeatedly while that clock is stopped.
  auto next = deadlines.begin()->first.first;
  if (allWaits) if (auto expiry = source.nextExpiry()) next = std::min(next, *expiry);
  auto delay = next - now();
  auto time = Clock::now();
  auto due = delay <= Clock::duration::zero() ? time : time + std::min(delay, Clock::time_point::max() - time);
  DeadlineScheduler::global().arm(timer, due);
}

void CommitWaits::retireLocked(const Wait& wait) {
  deadlines.erase({wait.deadline, wait.id});
  if (auto replicas = std::get_if<Replicas>(&wait.kind); replicas && replicas->barrier.all) allWaits--;
}

void CommitWaits::checkLocked(std::string_view name, Pending& ready, bool removed) {
  auto it = waits.find(name);
  if (it == waits.end()) return;
  std::erase_if(it->second, [&](const auto& wait) {
    if (!evaluate(*wait, false, removed)) return false;
    retireLocked(*wait);
    ready.push_back(wait);
    return true;
  });
  if (it->second.empty()) waits.erase(it);
}

void CommitWaits::admit(std::shared_ptr<Wait> wait, std::stop_token stop) {
  Pending ready;
  {
    std::lock_guard lock(mutex);
    if (evaluate(*wait, stop.stop_requested())) ready.push_back(wait);
    else {
      wait->id = ++nextWait;
      waits[wait->name].push_back(wait);
      if (auto replicas = std::get_if<Replicas>(&wait->kind); replicas && replicas->barrier.all) allWaits++;
      deadlines.emplace(std::pair{wait->deadline, wait->id}, wait->name);
      armLocked();
    }
  }
  if (!ready.empty()) { finish(ready); return; }
  wait->cancellation.emplace(stop, [this, name = wait->name, id = wait->id] { cancel(name, id); });
  Signal::emit("replicationWaitParked");
}

void CommitWaits::awaitCommit(std::string name, CommitId floor, Clock::time_point deadline,
                              std::stop_token stop, std::function<void(CommitResult)> complete) {
  auto wait = std::make_shared<Wait>();
  wait->name = std::move(name); wait->commit = std::move(floor); wait->deadline = deadline;
  wait->kind = Local{std::move(complete), {}};
  admit(std::move(wait), stop);
}

CommitWaits::ReplicaWait CommitWaits::prepareReplicas(std::string name, CommitId commit,
    const api::ReplicaRequirement& requirement, Clock::time_point deadline) {
  auto captured = source.capture(requirement, name, commit);
  return {std::move(name), std::move(commit), deadline, std::move(captured)};
}

void CommitWaits::awaitReplicas(std::string name, CommitId commit, const api::ReplicaRequirement& requirement,
    Clock::time_point deadline, std::stop_token stop, std::function<void(api::ReplicaResult)> complete) {
  awaitReplicas(prepareReplicas(std::move(name), std::move(commit), requirement, deadline), stop, std::move(complete));
}

void CommitWaits::awaitReplicas(ReplicaWait spec, std::stop_token stop, std::function<void(api::ReplicaResult)> complete) {
  auto wait = std::make_shared<Wait>();
  wait->kind = Replicas{std::move(spec.barrier), std::move(complete), {}};
  wait->name = std::move(spec.name); wait->commit = std::move(spec.commit); wait->deadline = spec.deadline;
  admit(std::move(wait), stop);
}

void CommitWaits::registered(const std::string& name, const std::shared_ptr<Collection>& collection) noexcept {
  try {
    Pending ready;
    {
      std::lock_guard lock(mutex);
      auto& state = collections[name];
      if (state.collection.lock() != collection) state = {collection, {}, {}};
      state.refresh();
      checkLocked(name, ready);
    }
    finish(ready);
  } catch (...) { LOG_ERROR("Commit wait registration notification failed"); }
}

void CommitWaits::updated(const std::string& name, const Collection& collection) noexcept {
  try {
    Pending ready;
    {
      std::lock_guard lock(mutex);
      auto it = collections.find(name);
      if (it == collections.end() || it->second.collection.lock().get() != &collection) return;
      it->second.refresh();
      checkLocked(name, ready);
    }
    finish(ready);
  } catch (...) { LOG_ERROR("Commit wait publication notification failed"); }
}

void CommitWaits::removed(const std::string& name) noexcept {
  try {
    Pending ready;
    {
      std::lock_guard lock(mutex);
      collections.erase(name);
      checkLocked(name, ready, true);
    }
    finish(ready);
  } catch (...) { LOG_ERROR("Commit wait deletion notification failed"); }
}

void CommitWaits::cancel(std::string_view name, uint64_t id) {
  Pending ready;
  {
    std::lock_guard lock(mutex);
    auto it = waits.find(name);
    if (it == waits.end()) return;
    std::erase_if(it->second, [&](const auto& wait) {
      if (wait->id != id) return false;
      evaluate(*wait, true);
      retireLocked(*wait);
      ready.push_back(wait);
      return true;
    });
    if (it->second.empty()) waits.erase(it);
  }
  finish(ready);
}

void CommitWaits::acknowledged(std::string_view name) {
  if (name.empty()) { poll(); return; }
  Pending ready;
  {
    std::lock_guard lock(mutex);
    checkLocked(name, ready);
  }
  finish(ready);
}

void CommitWaits::poll() {
  Pending ready;
  {
    std::lock_guard lock(mutex);
    for (auto it = waits.begin(); it != waits.end();) {
      auto name = (it++)->first;
      checkLocked(name, ready);
    }
    armLocked();
  }
  finish(ready);
}

void CommitWaits::close() {
  Pending ready;
  {
    std::lock_guard lock(mutex);
    closed = true;
    for (auto& [name, pending] : waits) for (auto& wait : pending) {
      evaluate(*wait, true); ready.push_back(wait);
    }
    waits.clear(); deadlines.clear(); allWaits = 0;
  }
  DeadlineScheduler::global().detach(timer);
  finish(ready);
}
CommitWaits::~CommitWaits() { close(); }
}
