#pragma once

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <type_traits>

namespace solux {

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
    PredictedBlockInfo info;
    uint64_t count = 0;
  };

  static int64_t predict(int64_t intercept, int64_t scaledSlope,
                         uint64_t rank, uint8_t slopeShift) {
    assert(slopeShift < 64);
    __int128 value = (__int128)intercept +
        (((__int128)rank * scaledSlope) >> slopeShift);
    assert(value >= std::numeric_limits<int64_t>::min() &&
           value <= std::numeric_limits<int64_t>::max());
    return (int64_t)value;
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
    assert(count > 0 && slopeShift < 64);
    if (count == 1) return 0;
    __int128 denominator = count - 1;
    __int128 numerator = ((__int128)last - first) *
                         ((__int128)1 << slopeShift);
    // Round to nearest so fixed-point drift is bounded by count/2^(shift+1).
    numerator += numerator >= 0 ? denominator / 2 : -denominator / 2;
    __int128 scaledSlope = numerator / denominator;
    if (scaledSlope < std::numeric_limits<int64_t>::min() ||
        scaledSlope > std::numeric_limits<int64_t>::max()) {
      return std::nullopt;
    }
    return (int64_t)scaledSlope;
  }

  static std::optional<PredictedBlockInfo> finishPlan(
      int64_t intercept, int64_t scaledSlope,
      __int128 minError, __int128 maxError) {
    __int128 loweredIntercept = (__int128)intercept + minError;
    __int128 errorRange = maxError - minError;
    if (loweredIntercept < std::numeric_limits<int64_t>::min() ||
        loweredIntercept > std::numeric_limits<int64_t>::max() ||
        errorRange < 0 || errorRange > std::numeric_limits<uint64_t>::max()) {
      return std::nullopt;
    }

    PredictedBlockInfo info{};
    info.intercept = (int64_t)loweredIntercept;
    info.scaledSlope = scaledSlope;
    info.bits = (uint8_t)std::bit_width((uint64_t)errorRange);
    if (info.bits > 57) return std::nullopt;
    return info;
  }

  template <class T>
  static std::optional<PredictedBlockInfo> evaluate(
      std::span<const T> values, int64_t intercept, int64_t scaledSlope,
      uint8_t slopeShift) {
    static_assert(std::is_integral_v<T> && std::is_unsigned_v<T>);
    assert(!values.empty() && slopeShift < 64);
    __int128 minError = std::numeric_limits<__int128>::max();
    __int128 maxError = std::numeric_limits<__int128>::min();
    for (uint64_t i = 0; i < values.size(); i++) {
      assert((__int128)values[i] <= std::numeric_limits<int64_t>::max());
      __int128 error = (__int128)values[i] -
          predict(intercept, scaledSlope, i, slopeShift);
      minError = std::min(minError, error);
      maxError = std::max(maxError, error);
    }
    return finishPlan(intercept, scaledSlope, minError, maxError);
  }

  template <class T>
  static std::optional<PredictedBlockInfo> planLinearFit(
      std::span<const T> values, uint8_t slopeShift) {
    static_assert(std::is_integral_v<T> && std::is_unsigned_v<T>);
    assert(!values.empty());
    assert((__int128)values.front() <= std::numeric_limits<int64_t>::max());
    assert((__int128)values.back() <= std::numeric_limits<int64_t>::max());
    auto scaledSlope = endpointSlope((int64_t)values.front(),
                                     (int64_t)values.back(), values.size(),
                                     slopeShift);
    if (!scaledSlope) return std::nullopt;
    return evaluate(values, (int64_t)values.front(), *scaledSlope, slopeShift);
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

} // namespace solux
