// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "RemoteSearchGate.h"

#include <algorithm>

#include "luxir/util/Signal.h"

namespace luxir {

void RemoteSearchGate::admit(std::stop_token stop, std::function<void(Slot)> start, std::function<void()> cancelled) {
  auto waiter = std::make_shared<Waiter>();
  {
    std::lock_guard lock(mutex);
    if (running < slots && queue.empty()) {
      running++;
      admitted++;
      waiter.reset();
    } else {
      waiter->id = ++nextId;
      waiter->queuedAt = Clock::now();
      waiter->start = std::move(start);
      waiter->cancelled = std::move(cancelled);
      queue.push_back(waiter);
    }
  }
  if (!waiter) {
    start(Slot(this));
    return;
  }
  Signal::emit("remoteSearchQueued");
  // Registered outside the lock: a stop already requested runs cancel() right
  // here. The releasing thread never touches `cancellation`, and whichever
  // thread drops the last reference destroys it.
  if (stop.stop_possible()) waiter->cancellation.emplace(std::move(stop), [this, id = waiter->id] { cancel(id); });
}

void RemoteSearchGate::release() noexcept {
  std::shared_ptr<Waiter> next;
  {
    std::lock_guard lock(mutex);
    if (queue.empty()) {
      running--;
      return;
    }
    // The slot passes to the oldest waiter: running stays as it is.
    next = std::move(queue.front());
    queue.pop_front();
    admitted++;
    waited++;
    waitNanos += (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - next->queuedAt).count();
  }
  next->start(Slot(this));
}

void RemoteSearchGate::cancel(uint64_t id) noexcept {
  std::shared_ptr<Waiter> waiter;
  {
    std::lock_guard lock(mutex);
    auto it = std::find_if(queue.begin(), queue.end(), [id](const auto& w) { return w->id == id; });
    if (it == queue.end()) return;  // already started
    waiter = std::move(*it);
    queue.erase(it);
    cancelledCount++;
  }
  waiter->cancelled();
}

RemoteSearchGate::Snapshot RemoteSearchGate::snapshot() {
  std::lock_guard lock(mutex);
  return {slots, running, (uint64_t)queue.size(), admitted, waited, cancelledCount, waitNanos};
}

}  // namespace luxir
