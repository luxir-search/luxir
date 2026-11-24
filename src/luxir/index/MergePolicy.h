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

  // Inclusive bounds of the numeric column of the root-level physical field
  // `field`, in the column's order-preserving int64 encoding (the value
  // itself for INT and DATE fields). nullopt when the segment has no column
  // for it; fields of nested child rows are not answered.
  virtual std::optional<NumericBounds> numericBounds(std::string_view field) const = 0;
};

// One live segment as a merge policy sees it.
struct MergeSegment {
  uint64_t segId = 0;
  int32_t maxDoc = 0;        // documents written, deleted ones included
  int32_t liveDocs = 0;      // documents not deleted
  int32_t rows = 0;          // largest physical row space (nested child rows count), at least maxDoc
  // Size of the data files a merge rewrites: the segment's files without its
  // live docs file. Overlays (summary, id filter, vector indexes) are not
  // counted.
  uint64_t bytes = 0;
  uint64_t minVersion = 0;   // update versions of its documents, which follow ingest order
  uint64_t maxVersion = 0;
  uint64_t firstCommitTime = 0;  // 0 until the segment is first committed
  // Too large to merge again under the collection's MergeLimits: its live
  // size passes half of a cap (MergeLimits::terminal). The writer admits it
  // into a merge only while the output stays within the caps, so a policy
  // that never selects it loses nothing; its deletes are reclaimed by the
  // writer's delete-reclaim merges.
  bool terminal = false;
  const MergeSegmentSummary* summary = nullptr;  // never null during a policy call

  // The live share of bytes and rows: deleted documents drop out of a merge.
  uint64_t liveBytes() const { return maxDoc > 0 ? (uint64_t)((double)bytes * liveDocs / maxDoc) : 0; }
  int64_t liveRows() const { return maxDoc > 0 ? (int64_t)rows * liveDocs / maxDoc : 0; }
};

// The size limits a collection's merges respect (MergePolicyDef's
// max_merged_segment_bytes, max_merged_segment_docs, deletes_pct_allowed).
// The writer enforces the caps when it admits a merge's sources, for every
// policy; a policy may read them to plan merges that fill a segment.
struct MergeLimits {
  static constexpr uint64_t GiB = 1024ULL * 1024 * 1024;
  // Defaults by collection kind; an explicit MergePolicyDef field wins.
  static constexpr uint64_t DEFAULT_MAX_BYTES = 5 * GiB;
  static constexpr int64_t DEFAULT_MAX_DOCS = 50'000'000;
  // A collection with a vector field: about 8M 768-dim float vectors
  // (~25 GiB of column), so the doc cap binds and per-segment IVF lists stay
  // within the default nlist clamp.
  static constexpr uint64_t VECTOR_MAX_BYTES = 32 * GiB;
  static constexpr int64_t VECTOR_MAX_DOCS = 8'000'000;
  // A REMOTE directory: a cold query's first touch fetches roughly the
  // segment, so segments stay small.
  static constexpr uint64_t REMOTE_MAX_BYTES = 1 * GiB;
  static constexpr int64_t REMOTE_MAX_DOCS = 10'000'000;
  static constexpr int32_t DEFAULT_DELETES_PCT_ALLOWED = 20;

  uint64_t maxMergedSegmentBytes = DEFAULT_MAX_BYTES;  // 0 = unlimited
  int64_t maxMergedSegmentDocs = DEFAULT_MAX_DOCS;     // 0 = unlimited
  // A segment the policy no longer merges is rewritten once more than this
  // percent of its documents are deleted; 100 never rewrites.
  int32_t deletesPctAllowed = DEFAULT_DELETES_PCT_ALLOWED;

  static MergeLimits unlimited() { return {0, 0, 100}; }

  // Whether a merge output of `liveBytes` and `liveDocs` stays within both caps.
  bool fits(uint64_t liveBytes, int64_t liveDocs) const {
    return (maxMergedSegmentBytes == 0 || liveBytes <= maxMergedSegmentBytes)
        && (maxMergedSegmentDocs == 0 || liveDocs <= maxMergedSegmentDocs);
  }
  // Over half of either cap: any further merge of it would pass that cap
  // unless its partners are smaller than what it already holds.
  bool terminal(uint64_t liveBytes, int64_t liveDocs) const {
    return (maxMergedSegmentBytes > 0 && liveBytes > maxMergedSegmentBytes / 2)
        || (maxMergedSegmentDocs > 0 && liveDocs > maxMergedSegmentDocs / 2);
  }
  // Whether segments holding `liveBytes` and `liveDocs` together reach a cap,
  // so merging them in segId order builds a terminal segment.
  bool filled(uint64_t liveBytes, int64_t liveDocs) const {
    return (maxMergedSegmentBytes > 0 && liveBytes >= maxMergedSegmentBytes)
        || (maxMergedSegmentDocs > 0 && liveDocs >= maxMergedSegmentDocs);
  }
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
// with fewer than two segments is not run. Admission takes a spec's segments
// in order and stops at the first that would carry the output past the
// collection's MergeLimits, so a spec is best given in segId order.
//
// Calls are serialized per collection and made under the writer's index
// lock: keep them to in-memory work beyond the summary reads. A policy is
// built per collection from its schema's merge_policy (MergePolicyRegistry)
// and rebuilt when that changes.
class MergePolicy {
public:
  virtual ~MergePolicy() = default;

  // Merges to run now, most urgent first; empty when none is due.
  // `segments` are all of the collection's live segments in segId order,
  // terminal ones included (MergeSegment::terminal).
  virtual std::vector<MergeSpec> findMerges(std::span<const MergeSegment> segments) const = 0;

  // The tier of a segment, reported in segment stats (merge_level); 0 for a
  // policy without tiers.
  virtual int32_t level(const MergeSegment& segment) const {
    (void)segment;
    return 0;
  }
};

}  // namespace luxir
