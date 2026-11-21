// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <string_view>

#include "luxir/index/MergePolicy.h"

namespace luxir {

// The default policy ("tiered"): a segment's level is the number of times
// merge_factor divides into its live rows (MergeSegment::liveRows, its largest
// row space prorated by live docs; factor 10: 0-9 rows is level 0, 10-99
// level 1, ...), and once merge_factor segments share a level, all of that
// level's segments merge into one, lowest level first. A level also merges,
// with fewer segments, once their live size together reaches a cap of the
// collection's MergeLimits: the writer admits the segId-ordered prefix that
// fits, which is then terminal, so near-terminal segments do not wait for
// merge_factor peers. Terminal and empty segments take no part.
//
// TODO: Hmmm, a high merge floor can lead to some O^N2 behavior: see
// https://issues.apache.org/jira/browse/LUCENE-10574. Perhaps an alternative
// would be to remove the floor and then kick off merges like normal, *but*
// when a merge happens at tier 3, sweep up all the smaller segments as well.
// I'm not sure this really makes sense though since it would only
// occasionally fix the "many small segments" problem, and it is the indexing
// pattern that is causing the issue. Perhaps this should be fixed by the user
// through an API that requests a more aggressive merge to sweep up small
// segments.
//
// Deletes lower a segment's level, so a mostly-deleted segment rejoins
// smaller segments' merges. Deletes in terminal segments are reclaimed by the
// writer (IndexWriter::_selectReclaimMergeLocked).
class TieredMergePolicy : public MergePolicy {
public:
  static constexpr std::string_view NAME = "tiered";
  static constexpr int32_t DEFAULT_MERGE_FACTOR = 10;

  const int32_t mergeFactor;
  const MergeLimits limits;

  explicit TieredMergePolicy(int32_t mergeFactor = DEFAULT_MERGE_FACTOR, const MergeLimits& limits = {})
    : mergeFactor(std::max(2, mergeFactor)), limits(limits) {}

  // Integer arithmetic: a floating-point log ratio rounds exact powers of the
  // merge factor (e.g. 100 rows for factor 10) down a level.
  int32_t levelForRows(int64_t rows) const {
    int32_t level = 0;
    for (int64_t bound = mergeFactor; rows >= bound; bound *= mergeFactor) {
      level++;
    }
    return level;
  }

  int32_t level(const MergeSegment& segment) const override { return levelForRows(segment.liveRows()); }

  std::vector<MergeSpec> findMerges(std::span<const MergeSegment> segments) const override;
};

}  // namespace luxir
