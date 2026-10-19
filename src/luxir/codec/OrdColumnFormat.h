// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <type_traits>

#include "LinearFit.h"
#include "LinearPack.h"

namespace luxir {

struct OrdColumnFormat {
  static constexpr uint32_t BLOCK_SIZE = 4096;
  static constexpr uint32_t BULK_SIZE = 128;
  static constexpr uint8_t BLOCK_SLOPE_SHIFT = 14;
  static constexpr uint8_t SINGLE_FIT_SLOPE_SHIFT = 31;

  struct PredictedBlockInfo {
    uint64_t payloadOffset;
    int64_t intercept;
    int64_t scaledSlope;
    uint8_t bits;
    uint8_t padding[7] = {};
  };
  static_assert(sizeof(PredictedBlockInfo) == 32);

  struct PredictedBlockPlan {
    PredictedBlockInfo info{};
    uint64_t count = 0;
  };

  // A run-length encoded block: the start (rank in the block) of every run
  // but the first, which starts at 0, at startBits each, then every run's
  // value minus base at valueBits each. A rank's run is the count of stored
  // starts at or below it, found by binary search.
  struct RunBlockInfo {
    uint64_t payloadOffset;
    uint32_t base;
    uint16_t runs;
    uint8_t startBits;
    uint8_t valueBits;
  };
  static_assert(sizeof(RunBlockInfo) == 16);

  static uint64_t runStartsByteSize(const RunBlockInfo& info) {
    return LinearPack::packedByteSize(info.runs - 1u, info.startBits);
  }

  static uint64_t runPayloadByteSize(const RunBlockInfo& info) {
    return runStartsByteSize(info)
        + LinearPack::packedByteSize(info.runs, info.valueBits);
  }

  template <class T>
  static RunBlockInfo planRunBlock(std::span<const T> values) {
    static_assert(std::is_integral_v<T> && std::is_unsigned_v<T>);
    assert(!values.empty() && values.size() <= BLOCK_SIZE);
    T low = values[0];
    T high = values[0];
    uint32_t runs = 1;
    uint32_t lastStart = 0;
    for (size_t i = 1; i < values.size(); i++) {
      low = std::min(low, values[i]);
      high = std::max(high, values[i]);
      if (values[i] != values[i - 1]) {
        runs++;
        lastStart = (uint32_t)i;
      }
    }
    assert(high <= std::numeric_limits<uint32_t>::max());
    RunBlockInfo info{};
    info.base = (uint32_t)low;
    info.runs = (uint16_t)runs;
    info.startBits = (uint8_t)std::bit_width(lastStart);
    info.valueBits = (uint8_t)std::bit_width((uint64_t)(high - low));
    return info;
  }

  static int64_t predict(int64_t intercept, int64_t scaledSlope,
                         uint64_t rank, uint8_t slopeShift) {
    return LinearFit::predict(intercept, scaledSlope, rank, slopeShift);
  }

  static int64_t predict(const PredictedBlockInfo& info, uint64_t rank,
                         uint8_t slopeShift = BLOCK_SLOPE_SHIFT) {
    return predict(info.intercept, info.scaledSlope, rank, slopeShift);
  }

  static uint64_t residual(const PredictedBlockInfo& info,
                           uint64_t rank, uint64_t value,
                           uint8_t slopeShift = BLOCK_SLOPE_SHIFT) {
    __int128 result = (__int128)value - predict(info, rank, slopeShift);
    assert(result >= 0 && result <= std::numeric_limits<uint64_t>::max());
    return (uint64_t)result;
  }

  static std::optional<int64_t> endpointSlope(int64_t first, int64_t last,
                                               uint64_t count,
                                               uint8_t slopeShift) {
    return LinearFit::endpointSlope(first, last, count, slopeShift);
  }

  static std::optional<PredictedBlockInfo> finishPlan(
      int64_t intercept, int64_t scaledSlope,
      __int128 minError, __int128 maxError) {
    auto fit = LinearFit::finishPlan(intercept, scaledSlope, minError, maxError);
    if (!fit) return std::nullopt;
    PredictedBlockInfo info{};
    info.intercept = fit->intercept;
    info.scaledSlope = fit->scaledSlope;
    info.bits = fit->bits;
    return info;
  }

  template <class T>
  static std::optional<PredictedBlockInfo> evaluate(
      std::span<const T> values, int64_t intercept, int64_t scaledSlope,
      uint8_t slopeShift) {
    auto fit = LinearFit::evaluate(values, intercept, scaledSlope, slopeShift);
    if (!fit) return std::nullopt;
    PredictedBlockInfo info{};
    info.intercept = fit->intercept;
    info.scaledSlope = fit->scaledSlope;
    info.bits = fit->bits;
    return info;
  }

  template <class T>
  static std::optional<PredictedBlockInfo> planLinearFit(
      std::span<const T> values, uint8_t slopeShift) {
    auto fit = LinearFit::planLinearFit(values, slopeShift);
    if (!fit) return std::nullopt;
    PredictedBlockInfo info{};
    info.intercept = fit->intercept;
    info.scaledSlope = fit->scaledSlope;
    info.bits = fit->bits;
    return info;
  }

  template <class T>
  static PredictedBlockPlan planBlock(std::span<const T> values) {
    static_assert(std::is_integral_v<T> && std::is_unsigned_v<T>);
    assert(!values.empty() && values.size() <= BLOCK_SIZE);
    auto constant = evaluate(values, (int64_t)values.front(), 0,
                             BLOCK_SLOPE_SHIFT);
    assert(constant);
    auto linear = planLinearFit(values, BLOCK_SLOPE_SHIFT);

    PredictedBlockPlan plan;
    plan.info = linear && linear->bits < constant->bits ? *linear : *constant;
    plan.count = values.size();
    return plan;
  }
};

} // namespace luxir
