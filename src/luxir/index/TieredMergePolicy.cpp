// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/index/TieredMergePolicy.h"

namespace luxir {

std::vector<MergeSpec> TieredMergePolicy::findMerges(std::span<const MergeSegment> segments) const {
  struct Level {
    int32_t count = 0;
    uint64_t liveBytes = 0;
    int64_t liveDocs = 0;
  };
  std::vector<int32_t> levels;
  levels.reserve(segments.size());
  std::vector<Level> totals;
  for (const auto& segment : segments) {
    if (segment.terminal || segment.liveDocs == 0) {
      levels.push_back(-1);
      continue;
    }
    int32_t level = levelForRows(segment.liveRows());
    levels.push_back(level);
    if ((size_t)level >= totals.size()) totals.resize((size_t)level + 1);
    auto& total = totals[(size_t)level];
    total.count++;
    total.liveBytes += segment.liveBytes();
    total.liveDocs += segment.liveDocs;
  }
  std::vector<MergeSpec> merges;
  for (size_t level = 0; level < totals.size(); level++) {
    const auto& total = totals[level];
    if (total.count < 2) continue;
    if (total.count < mergeFactor && !limits.filled(total.liveBytes, total.liveDocs)) continue;
    auto& spec = merges.emplace_back();
    spec.reserve((size_t)total.count);
    for (size_t i = 0; i < segments.size(); i++) {
      if (levels[i] == (int32_t)level) spec.push_back(segments[i].segId);
    }
  }
  return merges;
}

}  // namespace luxir
