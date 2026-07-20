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
  static constexpr uint8_t SLOPE_SHIFT = 14;

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
    uint32_t count = 0;
  };

  static int64_t predict(const PredictedBlockInfo& info, uint32_t rankInBlock) {
    __int128 value = (__int128)info.intercept +
        (((__int128)rankInBlock * info.scaledSlope) >> SLOPE_SHIFT);
    assert(value >= std::numeric_limits<int64_t>::min() &&
           value <= std::numeric_limits<int64_t>::max());
    return (int64_t)value;
  }

  static uint64_t residual(const PredictedBlockInfo& info,
                           uint32_t rankInBlock, uint64_t value) {
    __int128 result = (__int128)value - predict(info, rankInBlock);
    assert(result >= 0 && result <= std::numeric_limits<uint64_t>::max());
    return (uint64_t)result;
  }

  template <class T>
  static PredictedBlockPlan planBlock(std::span<const T> values) {
    static_assert(std::is_integral_v<T> && std::is_unsigned_v<T>);
    assert(!values.empty() && values.size() <= BLOCK_SIZE);
    for (T value : values) {
      assert((__int128)value <= std::numeric_limits<int64_t>::max());
    }

    auto evaluate = [&](int64_t intercept, int64_t scaledSlope)
        -> std::optional<PredictedBlockInfo> {
      __int128 minError = std::numeric_limits<__int128>::max();
      __int128 maxError = std::numeric_limits<__int128>::min();
      for (uint32_t i = 0; i < values.size(); i++) {
        __int128 predicted = (__int128)intercept +
            (((__int128)i * scaledSlope) >> SLOPE_SHIFT);
        __int128 error = (__int128)values[i] - predicted;
        minError = std::min(minError, error);
        maxError = std::max(maxError, error);
      }

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
    };

    auto constant = evaluate((int64_t)values.front(), 0);
    assert(constant);

    std::optional<PredictedBlockInfo> linear;
    if (values.size() > 1) {
      __int128 scaledSlope =
          ((__int128)values.back() - (__int128)values.front()) *
          ((__int128)1 << SLOPE_SHIFT) / (int64_t)(values.size() - 1);
      if (scaledSlope >= std::numeric_limits<int64_t>::min() &&
          scaledSlope <= std::numeric_limits<int64_t>::max()) {
        linear = evaluate((int64_t)values.front(), (int64_t)scaledSlope);
      }
    }

    PredictedBlockPlan plan;
    plan.info = linear && linear->bits < constant->bits ? *linear : *constant;
    plan.count = (uint32_t)values.size();
    return plan;
  }
};

} // namespace solux
