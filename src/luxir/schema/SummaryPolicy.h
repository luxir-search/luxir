// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <vector>

namespace luxir {

// Per-field summary settings, resolved from the schema (FieldSummaryDef).
struct SummaryPolicy {
  enum class Terms : uint8_t { AUTO, SET, FILTER, NONE };
  static constexpr uint32_t DEFAULT_SET_MAX = 256;
  static constexpr uint32_t MAX_SET_MAX = 4096;
  static constexpr uint64_t DEFAULT_FILTER_BYTES = 1u << 20;
  static constexpr uint64_t MIN_FILTER_BYTES = 4096;
  static constexpr uint32_t MAX_PREFIX_LEN = 32;

  Terms terms = Terms::AUTO;
  uint32_t setMax = DEFAULT_SET_MAX;
  uint64_t filterMaxBytes = DEFAULT_FILTER_BYTES;
  std::vector<uint8_t> prefixes;  // sorted, unique, 1..MAX_PREFIX_LEN
  bool bounds = true;

  bool collectsTerms() const noexcept { return terms != Terms::NONE; }
  bool operator==(const SummaryPolicy&) const = default;
};

} // namespace luxir
