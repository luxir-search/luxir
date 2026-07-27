#pragma once

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstdint>
#include <numeric>
#include <span>
#include <vector>

#include "LinearFit.h"

namespace solux {

struct NumBlockInfo {
  uint64_t payloadOffset = 0;
  uint64_t baseBits = 0;
  uint64_t gcd = 1;
  int64_t scaledSlope = 0;
  uint8_t bits = 0;
  uint8_t padding[7] = {};
};
static_assert(sizeof(NumBlockInfo) == 40);
// Readers locate the zone array at metaOff + nBlocks * sizeof(NumBlockInfo),
// while the writer aligns it to 8. Both agree only while the descriptor is a
// multiple of 8; otherwise the writer inserts padding the reader never skips.
static_assert(sizeof(NumBlockInfo) % 8 == 0);

struct NumBlockZone {
  int64_t min = 0;
  int64_t max = 0;
};
static_assert(sizeof(NumBlockZone) == 16);

struct NumColumnFormat {
  static constexpr uint32_t BLOCK_SIZE = 4096;
  static constexpr uint32_t BULK_SIZE = 128;
  static constexpr uint8_t SLOPE_SHIFT = 14;
  static constexpr uint8_t MAX_PACKED_BITS = 57;
  static constexpr uint8_t RAW_BITS = 64;
  static constexpr uint8_t MIN_LINEAR_BITS_SAVED = 1;

  struct BlockPlan {
    NumBlockInfo info;
    NumBlockZone zone;
    LinearFit::Plan fit;
    uint64_t count = 0;

    bool raw() const {
      return info.bits > MAX_PACKED_BITS;
    }
  };

  static BlockPlan planBlock(std::span<const int64_t> values,
                             std::vector<uint64_t>& quotients) {
    assert(!values.empty() && values.size() <= BLOCK_SIZE);

    auto [minIt, maxIt] = std::minmax_element(values.begin(), values.end());
    int64_t min = *minIt;
    int64_t max = *maxIt;

    uint64_t gcd = 0;
    for (int64_t value : values) {
      if (gcd == 1) break;
      gcd = std::gcd(gcd, (uint64_t)value - (uint64_t)min);
    }
    if (gcd == 0) gcd = 1;

    BlockPlan plan;
    plan.info.gcd = gcd;
    plan.zone = {min, max};
    plan.count = values.size();

    uint64_t range = ((uint64_t)max - (uint64_t)min) / gcd;
    if (std::bit_width(range) > MAX_PACKED_BITS) {
      plan.info.bits = RAW_BITS;
      return plan;
    }

    quotients.resize(values.size());
    for (size_t i = 0; i < values.size(); i++) {
      quotients[i] = ((uint64_t)values[i] - (uint64_t)min) / gcd;
      assert(quotients[i] <= range);
    }
    std::span<const uint64_t> q(quotients.data(), quotients.size());

    auto constant = LinearFit::evaluate(q, 0, 0, SLOPE_SHIFT);
    assert(constant);
    auto linear = LinearFit::planLinearFit(q, SLOPE_SHIFT);
    plan.fit = linear &&
        constant->bits >= linear->bits + MIN_LINEAR_BITS_SAVED
        ? *linear : *constant;

    plan.info.baseBits = (uint64_t)min +
        gcd * (uint64_t)plan.fit.intercept;
    plan.info.scaledSlope = plan.fit.scaledSlope;
    plan.info.bits = plan.fit.bits;
    // Constant blocks must reconstruct as min + gcd * residual: the residual is
    // then exactly (value - min) / gcd, which is what lets range queries
    // compare packed residuals against transformed bounds instead of decoding.
    // Holds because q contains 0 at the block minimum, so a zero-slope fit
    // lowers its intercept to 0. Asserted because the range query's
    // residual-domain path silently returns wrong matches if it ever stops
    // holding.
    assert(plan.info.scaledSlope != 0 || plan.info.baseBits == (uint64_t)min);
    return plan;
  }

  static uint64_t residual(const BlockPlan& plan, uint64_t rank,
                           uint64_t quotient) {
    assert(!plan.raw());
    return LinearFit::residual(plan.fit, rank, quotient, SLOPE_SHIFT);
  }

  // Equivalent to (rank * scaledSlope) >> SLOPE_SHIFT, without overflowing
  // int64 and without putting __int128 on the read path.
  static int64_t slopeTerm(uint64_t rank, int64_t scaledSlope) {
    constexpr uint64_t mask = (1ULL << SLOPE_SHIFT) - 1;
    int64_t whole = scaledSlope >> SLOPE_SHIFT;
    uint64_t fraction = (uint64_t)scaledSlope & mask;
    return (int64_t)rank * whole +
        (int64_t)((rank * fraction) >> SLOPE_SHIFT);
  }
};

} // namespace solux
