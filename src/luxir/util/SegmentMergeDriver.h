// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <cassert>
#include <functional>
#include <memory>
#include <memory_resource>
#include <map>
#include <mutex>
#include <type_traits>
#include <utility>

#include "AtomicMerger.h"

namespace luxir {

// Owns the per-segment merge lifecycle on top of an AtomicMerger: every segment
// contributes its data exactly once, and the segment that completes the set
// hands the fully merged result to onComplete.  The call site can no longer
// forget to release a contribution or to run the completion check -- the class
// of bug where a not-found early-return dropped a segment's merge so the op
// never emitted (and could destroy already-merged data).
//
// Usage:
//   SegmentMergeDriver<T> driver(numSegs,
//       [this](std::unique_ptr<T> merged){ emit(*merged); });
//   // per segment (segnum >= 0):
//   driver.contribute([&](T& data){ ...accumulate this segment into data... });
//   // empty index (segnum == -1, no segments):
//   driver.completeEmpty();
template <Mergeable T>
class SegmentMergeDriver {
  struct OrderedState {
    std::mutex mutex;
    std::pmr::map<size_t, std::unique_ptr<T>> pending;
    std::unique_ptr<T> running;
    size_t next = 0;

    explicit OrderedState(std::pmr::memory_resource* resource)
        : pending(resource) {}
  };

  AtomicMerger<T> merger;
  size_t numSegs;
  std::function<void(std::unique_ptr<T>)> onComplete;
  std::unique_ptr<OrderedState> ordered;

public:
  SegmentMergeDriver(size_t numSegs, std::function<void(std::unique_ptr<T>)> onComplete)
    : merger({}, [](T* data) { delete data; }),
      numSegs(numSegs), onComplete(std::move(onComplete)) {
    if constexpr (std::is_default_constructible_v<T>) setCreator([]() { return new T; });
  }

  // Optional custom storage creator (e.g. to pre-wire per-bucket calculators).
  void setCreator(std::function<T*()> creator) { merger.creator = std::move(creator); }

  // Configure before contributing. The indexed overload folds fresh states
  // in segment order; the ordinary overload retains AtomicMerger reuse.
  void setOrdered(std::pmr::memory_resource* resource = std::pmr::get_default_resource()) {
    assert(ordered == nullptr);
    ordered = std::make_unique<OrderedState>(resource);
  }

  // Contribute one segment: obtain a (possibly reused) accumulator, run
  // accumulate(data), release it, and -- if this was the last segment -- hand
  // the merged result to onComplete.  If accumulate throws, the data is freed
  // without releasing or completing (the request fails up the stack), matching
  // the pre-driver behavior.  A not-found segment simply returns from
  // accumulate without touching data; it still counts as a contribution.
  template <class Accumulate>
  void contribute(Accumulate&& accumulate) {
    std::unique_ptr<T> data(merger.obtain());
    accumulate(*data);
    size_t merged = (size_t) merger.release(data.release());
    if (merged == numSegs) {
      onComplete(std::unique_ptr<T>(merger.obtain()));
    }
  }

  template <class Accumulate>
  void contribute(size_t segnum, Accumulate&& accumulate) {
    if (!ordered) {
      contribute(std::forward<Accumulate>(accumulate));
      return;
    }
    std::unique_ptr<T> data(merger.creator());
    accumulate(*data);
    data->releaseCount = 1;
    std::unique_ptr<T> complete;
    {
      std::lock_guard lock(ordered->mutex);
      assert(segnum < numSegs && segnum >= ordered->next);
      if (segnum != ordered->next) {
        assert(!ordered->pending.contains(segnum));
        ordered->pending.emplace(segnum, std::move(data));
        return;
      }
      while (data) {
        if (!ordered->running) {
          ordered->running = std::move(data);
        } else {
          auto count = ordered->running->releaseCount + data->releaseCount;
          T* merged = T::merge(ordered->running.get(), data.get());
          if (merged == data.get()) data.release();
          if (merged == ordered->running.get()) ordered->running.release();
          ordered->running.reset(merged);
          ordered->running->releaseCount = count;
          data.reset();
        }
        ordered->next++;
        auto next = ordered->pending.begin();
        if (next != ordered->pending.end() && next->first == ordered->next) {
          data = std::move(next->second);
          ordered->pending.erase(next);
        }
      }
      if (ordered->next == numSegs) complete = std::move(ordered->running);
    }
    if (complete) onComplete(std::move(complete));
  }

  // Empty index (no segments): emit a default (empty) result with no segment
  // contribution.
  void completeEmpty() {
    onComplete(std::unique_ptr<T>(merger.obtain()));
  }
};

}
