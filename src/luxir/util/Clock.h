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

// Request timing (the responses' took_us). steady_clock is CLOCK_MONOTONIC, a
// vDSO read on Linux: no syscall, no lock, no allocation.
using RequestClock = std::chrono::steady_clock;

inline uint64_t elapsedMicros(RequestClock::time_point start) {
  auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(RequestClock::now() - start).count();
  return elapsed > 0 ? (uint64_t)elapsed : 0;
}

} // namespace luxir
