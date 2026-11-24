// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace luxir {

// Raw membership of one root in its validation context.
class SegmentMask {
  std::vector<uint64_t> excluded;
  size_t segments;
  size_t excludedCount = 0;

public:
  // Populated during planning only for explain or profiling.
  std::vector<std::string> reasons;

  explicit SegmentMask(size_t segments) : excluded((segments + 63) / 64), segments(segments) {}

  bool test(size_t ord) const noexcept {
    assert(ord < segments);
    return !(excluded[ord / 64] & (1ULL << (ord % 64)));
  }
  void exclude(size_t ord) noexcept {
    assert(test(ord));
    excluded[ord / 64] |= 1ULL << (ord % 64);
    ++excludedCount;
  }

  bool prunesAny() const noexcept { return excludedCount != 0; }
  bool none() const noexcept { return segments != 0 && excludedCount == segments; }
};

} // namespace luxir
