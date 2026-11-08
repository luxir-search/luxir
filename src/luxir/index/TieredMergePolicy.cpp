// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/index/TieredMergePolicy.h"

namespace luxir {

std::vector<MergeSpec> TieredMergePolicy::findMerges(std::span<const MergeSegment> segments) const {
  std::vector<int32_t> levels;
  levels.reserve(segments.size());
  std::vector<int32_t> counts;
  for (const auto& segment : segments) {
    int32_t level = levelForMaxDoc(segment.maxDoc);
    levels.push_back(level);
    if ((size_t)level >= counts.size()) counts.resize((size_t)level + 1);
    counts[(size_t)level]++;
  }
  std::vector<MergeSpec> merges;
  for (size_t level = 0; level < counts.size(); level++) {
    if (counts[level] < mergeFactor) continue;
    auto& spec = merges.emplace_back();
    spec.reserve((size_t)counts[level]);
    for (size_t i = 0; i < segments.size(); i++) {
      if ((size_t)levels[i] == level) spec.push_back(segments[i].segId);
    }
  }
  return merges;
}

}  // namespace luxir
