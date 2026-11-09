// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <string_view>

#include "luxir/index/MergePolicy.h"

namespace luxir {

// The default policy ("tiered"): a segment's level is the number of times
// merge_factor divides into its maxDoc (factor 10: 0-9 docs is level 0,
// 10-99 level 1, ...), and once merge_factor segments share a level, all of
// that level's segments merge into one, lowest level first.
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
// TODO: deletes are not considered. The level comes from maxDoc and a merge
// needs merge_factor segments at one level, so a mostly-deleted segment keeps
// its level and its dead docs are reclaimed only once that level fills;
// overwrite-heavy loads grow the index until then.
class TieredMergePolicy : public MergePolicy {
public:
  static constexpr std::string_view NAME = "tiered";
  static constexpr int32_t DEFAULT_MERGE_FACTOR = 10;

  const int32_t mergeFactor;

  explicit TieredMergePolicy(int32_t mergeFactor = DEFAULT_MERGE_FACTOR) : mergeFactor(std::max(2, mergeFactor)) {}

  // Integer arithmetic: a floating-point log ratio rounds exact powers of the
  // merge factor (e.g. 100 docs for factor 10) down a level.
  int32_t levelForMaxDoc(int32_t maxDoc) const {
    int32_t level = 0;
    for (int64_t bound = mergeFactor; maxDoc >= bound; bound *= mergeFactor) {
      level++;
    }
    return level;
  }

  int32_t level(const MergeSegment& segment) const override { return levelForMaxDoc(segment.maxDoc); }

  std::vector<MergeSpec> findMerges(std::span<const MergeSegment> segments) const override;
};

}  // namespace luxir
