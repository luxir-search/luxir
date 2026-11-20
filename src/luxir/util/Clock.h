// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <chrono>
#include <cstdint>

namespace luxir {

inline int64_t currentEpochMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
}

inline uint64_t nanosSince(std::chrono::steady_clock::time_point start,
                           std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now()) {
  auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
  return elapsed > 0 ? (uint64_t)elapsed : 0;
}

inline uint64_t microsSince(std::chrono::steady_clock::time_point start,
                            std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now()) {
  return nanosSince(start, end) / 1000;
}

} // namespace luxir
