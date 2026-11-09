// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace luxir {

// What a merge policy may learn about a segment's values without reading its
// documents. Answers cover every document the segment holds, deleted ones
// included, so they bound its live documents conservatively. Answers may
// read segment metadata on first use (the writer keeps it for later calls).
class MergeSegmentSummary {
public:
  struct NumericBounds {
    int64_t min, max;
  };

  virtual ~MergeSegmentSummary() = default;

  // Inclusive bounds of the numeric column of the physical field `field`, in
  // the column's order-preserving int64 encoding (the value itself for INT
  // and DATE fields). nullopt when the segment has no column for it.
  virtual std::optional<NumericBounds> numericBounds(std::string_view field) const = 0;
};

// One live segment as a merge policy sees it.
struct MergeSegment {
  uint64_t segId = 0;
  int32_t maxDoc = 0;        // documents written, deleted ones included
  int32_t liveDocs = 0;      // documents not deleted
  uint64_t bytes = 0;        // size of the segment's data files
  uint64_t minVersion = 0;   // update versions of its documents, which follow ingest order
  uint64_t maxVersion = 0;
  uint64_t firstCommitTime = 0;  // 0 until the segment is first committed
  const MergeSegmentSummary* summary = nullptr;  // never null during a policy call
};

// Ids of the segments to merge into one.
using MergeSpec = std::vector<uint64_t>;

// Decides which segments of a collection merge. The writer asks whenever no
// merge is in flight: after a segment flush it runs the first merge that
// includes the new segment, and after a merge (or when merges resume after a
// forced one) the first merge returned. It asks again when the merge it
// scheduled starts, so segments flushed in between can join. Scheduling,
// concurrency, the RAM budget and merge admission (the size limits a merged
// segment must respect, which may drop segments from a merge) stay with the
// writer. Specs naming unknown or empty segments are trimmed, and a spec left
// with fewer than two segments is not run.
//
// Calls are serialized per collection and made under the writer's index
// lock: keep them to in-memory work beyond the summary reads. A policy is
// built per collection from its schema's merge_policy (MergePolicyRegistry)
// and rebuilt when that changes.
class MergePolicy {
public:
  virtual ~MergePolicy() = default;

  // Merges to run now, most urgent first; empty when none is due.
  // `segments` are the collection's live segments in segId order.
  virtual std::vector<MergeSpec> findMerges(std::span<const MergeSegment> segments) const = 0;

  // The tier of a segment, reported in segment stats (merge_level); 0 for a
  // policy without tiers.
  virtual int32_t level(const MergeSegment& segment) const {
    (void)segment;
    return 0;
  }
};

}  // namespace luxir
