#pragma once
#include <functional>
#include <memory>

#include "AtomicMerger.h"

namespace solux {

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
  AtomicMerger<T> merger;
  size_t numSegs;
  std::function<void(std::unique_ptr<T>)> onComplete;

public:
  SegmentMergeDriver(size_t numSegs, std::function<void(std::unique_ptr<T>)> onComplete)
    : numSegs(numSegs), onComplete(std::move(onComplete)) {}

  // Optional custom storage creator (e.g. to pre-wire per-bucket calculators).
  void setCreator(std::function<T*()> creator) { merger.creator = std::move(creator); }

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

  // Empty index (no segments): emit a default (empty) result with no segment
  // contribution.
  void completeEmpty() {
    onComplete(std::unique_ptr<T>(merger.obtain()));
  }
};

}
