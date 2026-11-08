// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <string>
#include <string_view>

namespace luxir {

// Bounds in the fitted-term domain. An empty dictionary differs from an
// unknown range and from a dictionary containing the empty string.
struct IdRange {
  std::string min;
  std::string max;
  bool empty = true;

  bool overlaps(std::string_view first, std::string_view last) const {
    return !empty && first <= max && last >= min;
  }
};

} // namespace luxir
