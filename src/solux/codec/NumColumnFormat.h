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

// Naturally aligned, and read in place off the mapping: every writer 8-aligns
// the block-meta array (NumColumnWriter::finish), and every reader asserts it
// on construction.  Anything that relocates a finished column has to preserve
// that - OrdMap appends columns into one buffer and pads to 8 for exactly this
// reason.  Do not make this SOLUX_UNALIGNED to paper over a new relocator;
// pad the relocator instead, or the assert will point at it.
struct NumBlockInfo {
  // Payload offset in the low 56 bits, packed width in the top 8. One load
  // yields both, and it lands the descriptor on 32 bytes: two per cache line,
  // never straddling one, indexed by a shift. 2^56 bytes of payload per column
  // is not a limit anything can reach.
  //
  // A general block can use gcd and scaledSlope together - a sorted
  // day-granularity date column is a ramp over values sharing a divisor - so
  // the two are separate fields. Monotonic columns pin gcd to 1 and
  // NumColumnT<false> never reads it.
  uint64_t offsetAndBits = 0;
  uint64_t baseBits = 0;
  uint64_t gcd = 1;
  int64_t scaledSlope = 0;

  static constexpr uint64_t OFFSET_MASK = (1ULL << 56) - 1;

  uint64_t payloadOffset() const {
    return offsetAndBits & OFFSET_MASK;
  }

  uint8_t bits() const {
    return (uint8_t)(offsetAndBits >> 56);
  }

  void setPayload(uint64_t payloadOffset, uint8_t bits) {
    assert(payloadOffset <= OFFSET_MASK);
    offsetAndBits = payloadOffset | ((uint64_t)bits << 56);
  }
};
static_assert(sizeof(NumBlockInfo) == 32);
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

  // Fixed-point slope: scaledSlope = round(slope * 2^SLOPE_SHIFT), so the
  // rounding error is under 2^-(SLOPE_SHIFT+1) per rank and the drift across a
  // whole block is under BLOCK_SIZE * 2^-(SLOPE_SHIFT+1). Residuals are
  // integers, so drift below 1 widens their range by at most one and usually
  // not at all - which puts the point of diminishing returns at
  // log2(BLOCK_SIZE)+1. We take one bit past it (scale = 4 * BLOCK_SIZE, drift
  // under 1/8) and no more: extra bits buy nothing measurable and cost
  // headroom, because endpointSlope has to hold slope * 2^SLOPE_SHIFT in an
  // int64 and falls back to the constant fit when it cannot. Derived from
  // BLOCK_SIZE so retuning the block size cannot silently break the bound.
  static constexpr uint8_t SLOPE_SHIFT =
      (uint8_t)(std::bit_width(BLOCK_SIZE) + 1);
  static_assert(BLOCK_SIZE <= (1u << SLOPE_SHIFT), "block drift must stay < 1");
  static constexpr uint8_t MAX_PACKED_BITS = 57;
  static constexpr uint8_t RAW_BITS = 64;
  static constexpr uint8_t MIN_LINEAR_BITS_SAVED = 1;

  struct BlockPlan {
    NumBlockInfo info;
    NumBlockZone zone;
    LinearFit::Plan fit;
    uint64_t count = 0;
    // Held outside info until the writer knows the payload offset; the two
    // share a word on disk.
    uint8_t bits = 0;

    bool raw() const {
      return bits > MAX_PACKED_BITS;
    }
  };

  // useGcd == false skips the common-divisor scan and pins gcd to 1. Monotonic
  // columns pass false: the slope already carries any regular step, so gcd only
  // buys bits when the residuals themselves share a divisor - rare - and it
  // costs a load and a multiply on every point read of the hottest columns in
  // the engine (endValueRank and endOffset, read twice per doc).
  static BlockPlan planBlock(std::span<const int64_t> values,
                             std::vector<uint64_t>& quotients,
                             bool useGcd = true) {
    assert(!values.empty() && values.size() <= BLOCK_SIZE);

    auto [minIt, maxIt] = std::minmax_element(values.begin(), values.end());
    int64_t min = *minIt;
    int64_t max = *maxIt;

    uint64_t gcd = 0;
    if (useGcd) {
      for (int64_t value : values) {
        if (gcd == 1) break;
        gcd = std::gcd(gcd, (uint64_t)value - (uint64_t)min);
      }
    }
    if (gcd == 0) gcd = 1;

    BlockPlan plan;
    plan.info.gcd = gcd;
    plan.zone = {min, max};
    plan.count = values.size();

    uint64_t range = ((uint64_t)max - (uint64_t)min) / gcd;
    if (std::bit_width(range) > MAX_PACKED_BITS) {
      plan.bits = RAW_BITS;
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
    plan.bits = plan.fit.bits;
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

  // rank * scaledSlope overflows int64 for a wide block (rank < 2^12 against a
  // slope that can reach ~2^58), but the shifted result always fits. A 64x64
  // multiply already produces 128 bits in a register pair, so taking the wide
  // product costs the shift-combine and nothing else - cheaper than splitting
  // the slope into whole and fractional parts, which needs two multiplies.
  // This is also verbatim what LinearFit::predict computes on the write side,
  // so the two cannot drift.
  static int64_t slopeTerm(uint64_t rank, int64_t scaledSlope) {
    return (int64_t)(((__int128)(int64_t)rank * scaledSlope) >> SLOPE_SHIFT);
  }
};

} // namespace solux
