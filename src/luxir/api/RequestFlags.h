// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "luxir/api/luxir_types.hpp"
#include "luxir/util/ApiError.h"
#include "luxir/util/DateTime.h"

namespace luxir {

struct RequestFlags {
  enum class DateMode : uint8_t { NATIVE = 0, ISO = 1, MILLIS = 2, NANOS = 3 };
  struct Override {
    uint64_t flags = 0;
    uint64_t flagsSet = 0;
  };

  static constexpr uint64_t DATE_MASK =
      (uint64_t{1} << (int)api::RequestFlagBit::DATE_MODE_LOW_BIT)
      | (uint64_t{1} << (int)api::RequestFlagBit::DATE_MODE_HIGH_BIT);
  static constexpr uint64_t INT64_STR = uint64_t{1} << 2; // reserved, not accepted yet
  static constexpr std::array<std::string_view, 4> DATE_NAMES{
      "date_native", "date_iso", "date_ms", "date_ns"};

  static std::string_view validationError(uint64_t flags, uint64_t flagsSet) {
    if ((flags | flagsSet) & INT64_STR) return "int64_str is not supported";
    if ((flags | flagsSet) & ~(DATE_MASK | INT64_STR)) return "unknown request flag bits";
    uint64_t dateSet = flagsSet & DATE_MASK;
    if (dateSet != 0 && dateSet != DATE_MASK) {
      return "flags_set must cover both date-mode bits or neither";
    }
    return {};
  }

  static uint64_t resolve(uint64_t inherited, uint64_t flags, uint64_t flagsSet) {
    if (auto error = validationError(flags, flagsSet); !error.empty()) {
      throw RequestError(std::string(error));
    }
    return (inherited & ~flagsSet) | (flags & flagsSet);
  }

  static Override fromNames(std::span<const std::string_view> names) {
    Override out;
    for (auto name : names) {
      if (name == "int64_str") throw RequestError("int64_str is not supported");
      size_t mode = 0;
      for (; mode < DATE_NAMES.size(); mode++) if (DATE_NAMES[mode] == name) break;
      if (mode == DATE_NAMES.size()) {
        throw RequestError("unknown request flag '" + std::string(name) + "'");
      }
      if (out.flagsSet & DATE_MASK) {
        throw RequestError("duplicate or conflicting date flags");
      }
      out.flags = mode;
      out.flagsSet = DATE_MASK;
    }
    return out;
  }

  static std::vector<std::string_view> names(uint64_t flags, uint64_t flagsSet) {
    std::vector<std::string_view> out;
    if (flagsSet & DATE_MASK) out.push_back(dateName(flags));
    return out;
  }

  static DateMode dateMode(uint64_t flags) { return (DateMode)(flags & DATE_MASK); }
  static std::string_view dateName(uint64_t flags) { return DATE_NAMES[flags & DATE_MASK]; }

  static std::optional<DateUnit> dateInputUnit(uint64_t flags) {
    switch (dateMode(flags)) {
      case DateMode::MILLIS: return DateUnit::MILLIS;
      case DateMode::NANOS: return DateUnit::NANOS;
      case DateMode::NATIVE:
      case DateMode::ISO: return std::nullopt;
    }
    return std::nullopt;
  }
};

} // namespace luxir
