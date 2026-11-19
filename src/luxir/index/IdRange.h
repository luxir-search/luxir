// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <string>
#include <string_view>

namespace luxir {

// Bounds in the fitted-term domain. An empty dictionary differs from an
// unknown range and from a dictionary containing the empty string. A range
// taken from a segment summary may be wider than the dictionary (truncated
// bounds), and its maximum may be unbounded.
struct IdRange {
  std::string min;
  std::string max;
  bool empty = true;
  bool maxUnbounded = false;

  bool overlaps(std::string_view first, std::string_view last) const {
    return !empty && (maxUnbounded || first <= max) && last >= min;
  }
};

} // namespace luxir
