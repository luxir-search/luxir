// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "SearchAdmission.h"

#include <algorithm>

#include "luxir/util/Signal.h"
#include "luxir/util/log.h"

namespace luxir {

namespace {

SearchAdmission::Limits normalized(SearchAdmission::Limits limits) {
  limits.maxRunning = std::max<uint32_t>(1, limits.maxRunning);
  limits.maxRunningPerCollection = std::clamp<uint32_t>(limits.maxRunningPerCollection, 1, limits.maxRunning);
  limits.maxRunningRemote = std::clamp<uint32_t>(limits.maxRunningRemote, 1, limits.maxRunning);
  return limits;
}

}  // namespace

SearchAdmission::SearchAdmission(Limits limits) : limits(normalized(limits)) {}

SearchAdmission::~SearchAdmission() {
  DeadlineScheduler::global().detach(timer);
  std::vector<std::shared_ptr<Waiter>> searches;
  std::deque<std::shared_ptr<Waiter>> resumesLeft;
  {
    std::lock_guard lock(mutex);
    closing = true;
    for (Lane* lane : ring) {
      for (auto& waiter : lane->queue) {
        waiter->inQueue = false;
        searches.push_back(std::move(waiter));
      }
      lane->queue.clear();
    }
    ring.clear();
    queued = 0;
    resumesLeft.swap(resumeQueue);
  }
  for (auto& waiter : searches) refuse(waiter, Refusal::SHUTDOWN);
  // A stranded resume would leave its stream open forever: let it run (or
  // observe its cancelled transport) without a slot.
  for (auto& waiter : resumesLeft) {
    try {
      waiter->start(Slot());
    } catch (...) {
      LOG_ERROR("search resume failed at shutdown");
    }
  }
}

SearchAdmission::Lane& SearchAdmission::laneLocked(const std::string& collection) {
  auto& lane = lanes[collection];
  if (!lane) {
    lane = std::make_unique<Lane>();
    lane->collection = collection;
  }
  return *lane;
}

void SearchAdmission::dropLaneIfIdleLocked(Lane* lane) {
  if (lane == nullptr || lane->running != 0 || !lane->queue.empty() || lane->inRing) return;
  auto it = lanes.find(lane->collection);
  if (it != lanes.end() && it->second.get() == lane) lanes.erase(it);
}

bool SearchAdmission::runnableLocked(const Lane& lane, bool remote) const {
  return running < limits.maxRunning && lane.running < limits.maxRunningPerCollection
      && (!remote || runningRemote < limits.maxRunningRemote);
}

SearchAdmission::Slot SearchAdmission::claimLocked(Lane* lane, bool remote, bool wasQueued) {
  running++;
  if (lane) lane->running++;
  if (remote) runningRemote++;
  return Slot(this, lane, remote, wasQueued);
}

void SearchAdmission::publishLocked() {
  runningNow.store(running, std::memory_order_relaxed);
  queuedNow.store(queued, std::memory_order_relaxed);
}

void SearchAdmission::unqueueLocked(const std::shared_ptr<Waiter>& waiter) {
  Lane* lane = waiter->lane;
  auto it = std::find(lane->queue.begin(), lane->queue.end(), waiter);
  if (it != lane->queue.end()) lane->queue.erase(it);
  waiter->inQueue = false;
  queued--;
  if (lane->queue.empty() && lane->inRing) {
    auto at = (size_t)(std::find(ring.begin(), ring.end(), lane) - ring.begin());
    ring.erase(ring.begin() + (std::ptrdiff_t)at);
    if (at < cursor) cursor--;
    lane->inRing = false;
  }
  dropLaneIfIdleLocked(lane);
}

void SearchAdmission::armLocked() {
  if (limits.maxQueueWait.count() <= 0) return;
  auto due = Clock::time_point::max();
  // Each lane's FIFO is ordered by deadline: its front is the earliest.
  for (Lane* lane : ring) due = std::min(due, lane->queue.front()->deadline);
  if (due != Clock::time_point::max()) DeadlineScheduler::global().arm(timer, due);
}

void SearchAdmission::startWaitersLocked(Started& started) {
  // Resumed production first: it is already under way and never refused.
  // Each entry is placed in `started` before anything is taken off a queue, so
  // an allocation failure loses no waiter and leaks no slot.
  while (running < limits.maxRunning && !resumeQueue.empty()) {
    started.emplace_back(resumeQueue.front(), Slot());
    resumeQueue.pop_front();
    started.back().first->inQueue = false;
    resumes++;
    resumesWaited++;
    started.back().second = claimLocked(nullptr, false, true);
  }
  // Then one search per collection per turn, from where the last turn ended. A
  // collection at its own cap (or a remote one at the remote cap) keeps its
  // place and passes the turn on.
  auto now = Clock::now();
  size_t passed = 0;
  while (!ring.empty() && passed < ring.size() && running < limits.maxRunning) {
    if (cursor >= ring.size()) cursor = 0;
    Lane* lane = ring[cursor];
    if (!runnableLocked(*lane, lane->queue.front()->remote)) {
      passed++;
      cursor++;
      continue;
    }
    started.emplace_back(lane->queue.front(), Slot());
    lane->queue.pop_front();
    auto& waiter = *started.back().first;
    waiter.inQueue = false;
    queued--;
    admitted++;
    waited++;
    waitNanos += (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(now - waiter.queuedAt).count();
    started.back().second = claimLocked(lane, waiter.remote, true);
    passed = 0;
    if (lane->queue.empty()) {
      ring.erase(ring.begin() + (std::ptrdiff_t)cursor);  // the cursor now names the next lane
      lane->inRing = false;
    } else {
      cursor++;
    }
  }
}

void SearchAdmission::start(Started& started) noexcept {
  for (auto& [waiter, slot] : started) {
    try {
      waiter->start(std::move(slot));
    } catch (const std::exception& e) {
      LOG_ERROR("search admission start failed: {}", e.what());
    } catch (...) {
      LOG_ERROR("search admission start failed");
    }
  }
}

void SearchAdmission::refuse(const std::shared_ptr<Waiter>& waiter, Refusal why) noexcept {
  try {
    waiter->refuse(why);
  } catch (const std::exception& e) {
    LOG_ERROR("search admission refusal failed: {}", e.what());
  } catch (...) {
    LOG_ERROR("search admission refusal failed");
  }
}

SearchAdmission::Slot SearchAdmission::tryAcquire(const std::string& collection, bool remote) {
  std::lock_guard lock(mutex);
  if (closing || queued != 0 || !resumeQueue.empty()) return {};
  Lane& lane = laneLocked(collection);
  if (!runnableLocked(lane, remote)) {
    dropLaneIfIdleLocked(&lane);
    return {};
  }
  admitted++;
  auto slot = claimLocked(&lane, remote, false);
  publishLocked();
  return slot;
}

void SearchAdmission::admit(const std::string& collection, bool remote, std::stop_token stop,
                            std::function<void(Slot)> startFn, std::function<void(Refusal)> refuseFn) {
  auto waiter = std::make_shared<Waiter>();
  waiter->start = std::move(startFn);
  waiter->refuse = std::move(refuseFn);
  Started now;
  now.reserve(1);  // never allocate while a claimed slot would release under the lock
  std::optional<Refusal> refused;
  {
    std::lock_guard lock(mutex);
    Lane& lane = laneLocked(collection);
    if (closing) {
      refused = Refusal::SHUTDOWN;
    } else if (lane.queue.empty() && resumeQueue.empty() && runnableLocked(lane, remote)) {
      admitted++;
      now.emplace_back(waiter, claimLocked(&lane, remote, false));
    } else if (queued >= limits.maxQueued) {
      rejectedFull++;
      refused = Refusal::QUEUE_FULL;
    } else {
      waiter->lane = &lane;
      waiter->remote = remote;
      waiter->inQueue = true;
      waiter->queuedAt = Clock::now();
      if (limits.maxQueueWait.count() > 0) waiter->deadline = waiter->queuedAt + limits.maxQueueWait;
      lane.queue.push_back(waiter);
      if (!lane.inRing) {
        try {
          ring.push_back(&lane);
        } catch (...) {
          lane.queue.pop_back();
          dropLaneIfIdleLocked(&lane);
          throw;
        }
        lane.inRing = true;
      }
      queued++;
      armLocked();
    }
    if (refused) dropLaneIfIdleLocked(&lane);
    publishLocked();
  }
  if (!now.empty()) {
    start(now);
    return;
  }
  if (refused) {
    refuse(waiter, *refused);
    return;
  }
  Signal::emit("searchAdmissionQueued");
  // Registered outside the lock: a stop already requested runs cancel() right
  // here. The starting thread never touches `cancellation`, and whichever
  // thread drops the last reference destroys it.
  if (stop.stop_possible()) {
    waiter->cancellation.emplace(std::move(stop), [this, weak = std::weak_ptr<Waiter>(waiter)] { cancel(weak); });
  }
}

void SearchAdmission::resume(std::function<void(Slot)> startFn) {
  auto waiter = std::make_shared<Waiter>();
  waiter->start = std::move(startFn);
  Started now;
  now.reserve(1);
  {
    std::lock_guard lock(mutex);
    if (closing) {
      now.emplace_back(waiter, Slot());
    } else if (running < limits.maxRunning && resumeQueue.empty()) {
      resumes++;
      now.emplace_back(waiter, claimLocked(nullptr, false, false));
    } else {
      waiter->inQueue = true;
      resumeQueue.push_back(std::move(waiter));
    }
    publishLocked();
  }
  start(now);
}

void SearchAdmission::release(Lane* lane, bool remote) noexcept {
  Started next;
  {
    std::lock_guard lock(mutex);
    running--;
    if (lane) lane->running--;
    if (remote) runningRemote--;
    try {
      startWaitersLocked(next);
    } catch (...) {
      // Only the Started vector allocates: the waiters it did not take keep
      // their places for the next release.
      LOG_ERROR("search admission could not start the next search");
    }
    dropLaneIfIdleLocked(lane);
    publishLocked();
  }
  start(next);
}

void SearchAdmission::cancel(const std::weak_ptr<Waiter>& weak) noexcept {
  auto waiter = weak.lock();
  if (!waiter) return;
  {
    std::lock_guard lock(mutex);
    if (!waiter->inQueue) return;  // already started, refused or expired
    unqueueLocked(waiter);
    cancelledCount++;
    publishLocked();
  }
  refuse(waiter, Refusal::CANCELLED);
}

void SearchAdmission::expire() noexcept {
  std::vector<std::shared_ptr<Waiter>> expired;
  {
    std::lock_guard lock(mutex);
    auto now = Clock::now();
    try {
      std::vector<Lane*> queuedLanes(ring.begin(), ring.end());
      for (Lane* lane : queuedLanes) {
        // unqueueLocked drops the lane with its last waiter: stop there.
        while (lane->queue.front()->deadline <= now) {
          auto waiter = lane->queue.front();
          bool last = lane->queue.size() == 1;
          unqueueLocked(waiter);
          rejectedTimeout++;
          expired.push_back(std::move(waiter));
          if (last) break;
        }
      }
    } catch (...) {
      LOG_ERROR("search admission could not expire queued searches");
    }
    armLocked();
    publishLocked();
  }
  for (auto& waiter : expired) refuse(waiter, Refusal::QUEUE_TIMEOUT);
}

bool SearchAdmission::crowded(uint32_t width) const {
  if (queuedNow.load(std::memory_order_relaxed) != 0) return true;
  uint64_t others = runningNow.load(std::memory_order_relaxed);
  others = others > 0 ? others - 1 : 0;
  return others + std::max<uint32_t>(1, width) > limits.maxRunning;
}

SearchAdmission::Snapshot SearchAdmission::snapshot() {
  Snapshot s;
  std::lock_guard lock(mutex);
  s.limits = limits;
  s.running = running;
  s.runningRemote = runningRemote;
  s.queued = queued;
  s.resumesQueued = resumeQueue.size();
  s.admitted = admitted;
  s.waited = waited;
  s.waitNanos = waitNanos;
  s.rejectedFull = rejectedFull;
  s.rejectedTimeout = rejectedTimeout;
  s.cancelled = cancelledCount;
  s.resumes = resumes;
  s.resumesWaited = resumesWaited;
  s.downgraded = downgradedCount.load(std::memory_order_relaxed);
  for (const auto& [name, lane] : lanes) {
    if (lane->running == 0 && lane->queue.empty()) continue;
    s.collections.push_back({name, lane->running, (uint64_t)lane->queue.size()});
  }
  std::sort(s.collections.begin(), s.collections.end(),
            [](const auto& a, const auto& b) { return a.collection < b.collection; });
  return s;
}

}  // namespace luxir
