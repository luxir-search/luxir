#pragma once

#include <cstdint>

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

  static int64_t predict(const PredictedBlockInfo& info, uint32_t rankInBlock) {
    return info.intercept +
           (((int64_t)rankInBlock * info.scaledSlope) >> SLOPE_SHIFT);
  }
};

} // namespace solux
