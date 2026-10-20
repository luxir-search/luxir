// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iterator>
#include <limits>
#include <numeric>
#include <optional>
#include <span>
#include <vector>

#include "LinearFit.h"
#include "luxir/util/NumericUtils.h"

namespace luxir {

// Block encodings. Persisted in the descriptor: append only.
//   FOR        frame of reference: min + gcd * (slopeTerm(rank) + residual),
//              or RAW (bits() == RAW_BITS) plain int64s
//   DELTA      per-128-value frame anchors, then bit-packed in-frame deltas
//   DICT       a sorted per-block dictionary, then bit-packed codes
//   ALP_DOUBLE decimal-scaled doubles (ALP), then patched exceptions
//   ALP_FLOAT  the same for floats
// Every kind decodes to the column's int64 values (sortable bits for
// FLOAT/DOUBLE), so consumers above the decode layer never see the kind.
enum class NumBlockKind : uint8_t {
  FOR = 0,
  DELTA = 1,
  DICT = 2,
  ALP_DOUBLE = 3,
  ALP_FLOAT = 4,
};

// Naturally aligned, and read in place off the mapping: every writer 8-aligns
// the block-meta array (NumColumnWriter::finish), and every reader asserts it
// on construction.  Anything that relocates a finished column has to preserve
// that - OrdMap appends columns into one buffer and pads to 8 for exactly this
// reason.  Do not make this LUXIR_UNALIGNED to paper over a new relocator;
// pad the relocator instead, or the assert will point at it.
struct NumBlockInfo {
  // Payload offset in the low 52 bits, packed width in the next 8, block
  // kind in the top 4. One load yields all three, and it lands the
  // descriptor on 32 bytes: two per cache line, never straddling one,
  // indexed by a shift. 2^52 bytes of payload per column is not a limit
  // anything can reach. Kind above width makes "a FOR block with a packed
  // payload", the hot case, one compare of the top 12 bits (forPacked).
  //
  // A general block can use gcd and scaledSlope together - a sorted
  // day-granularity date column is a ramp over values sharing a divisor - so
  // the two are separate fields. Monotonic columns pin gcd to 1 and
  // NumColumnT<false> never reads it.
  //
  // The other kinds reuse the three value fields:
  //   DELTA  baseBits = the minimum in-frame delta (mod 2^64)
  //   DICT   gcd = dictionary size
  //   ALP_*  baseBits = the minimum scaled integer, gcd = exponent |
  //          factor << 8, scaledSlope = exception count
  // Their scaledSlope is otherwise 0 and their gcd otherwise 1.
  uint64_t offsetAndBits = 0;
  uint64_t baseBits = 0;
  uint64_t gcd = 1;
  int64_t scaledSlope = 0;

  static constexpr uint32_t OFFSET_BITS = 52;
  static constexpr uint64_t OFFSET_MASK = (1ULL << OFFSET_BITS) - 1;
  static constexpr uint8_t MAX_PACKED_BITS = 57;
  static constexpr uint8_t RAW_BITS = 64;

  uint64_t payloadOffset() const {
    return offsetAndBits & OFFSET_MASK;
  }

  uint8_t bits() const {
    return (uint8_t)(offsetAndBits >> OFFSET_BITS);
  }

  NumBlockKind kind() const {
    return (NumBlockKind)(offsetAndBits >> (OFFSET_BITS + 8));
  }

  // A FOR block with a bit-packed payload (not RAW, not another kind).
  bool forPacked() const {
    return (offsetAndBits >> OFFSET_BITS) <= MAX_PACKED_BITS;
  }

  bool raw() const {
    return (offsetAndBits >> OFFSET_BITS) == RAW_BITS;
  }

  // Whether the packed values are FOR residuals (value - min) / gcd, which
  // range queries compare against bounds moved into the residual domain
  // instead of decoding.
  bool residualComparable() const {
    return forPacked() && bits() <= 32 && scaledSlope == 0;
  }

  uint64_t dictSize() const {
    assert(kind() == NumBlockKind::DICT);
    return gcd;
  }

  uint8_t alpExponent() const {
    return (uint8_t)gcd;
  }

  uint8_t alpFactor() const {
    return (uint8_t)(gcd >> 8);
  }

  uint64_t alpExceptions() const {
    return (uint64_t)scaledSlope;
  }

  void setPayload(uint64_t payloadOffset, uint8_t bits,
                  NumBlockKind kind = NumBlockKind::FOR) {
    assert(payloadOffset <= OFFSET_MASK);
    assert(kind == NumBlockKind::FOR || bits <= MAX_PACKED_BITS);
    offsetAndBits = payloadOffset | ((uint64_t)bits << OFFSET_BITS)
        | ((uint64_t)kind << (OFFSET_BITS + 8));
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
  // Leaf zone granularity: exact min/max bounds are also kept per 512
  // values, one level under the whole-block zones, so bound consumers can
  // prune at ~1/8 block spans (the irreducible gather under block bounds is
  // ~k * granularity docs). Leaves nest in blocks and decode frames, and
  // stay word-aligned for masked gathers.
  static constexpr uint32_t LEAF_ZONE_SIZE = 512;
  static constexpr uint32_t LEAVES_PER_BLOCK = BLOCK_SIZE / LEAF_ZONE_SIZE;
  static_assert(BLOCK_SIZE % LEAF_ZONE_SIZE == 0);
  static_assert(LEAF_ZONE_SIZE % BULK_SIZE == 0);
  static_assert(LEAF_ZONE_SIZE % 64 == 0);

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
  static constexpr uint8_t MAX_PACKED_BITS = NumBlockInfo::MAX_PACKED_BITS;
  static constexpr uint8_t RAW_BITS = NumBlockInfo::RAW_BITS;
  static constexpr uint8_t MIN_LINEAR_BITS_SAVED = 1;

  struct BlockPlan {
    NumBlockInfo info;
    NumBlockZone zone;
    // Exact per-512-value bounds, computed in the same extrema pass before
    // the RAW/packed choice - RAW blocks carry ordinary leaf zones.
    NumBlockZone leafZones[LEAVES_PER_BLOCK];
    uint32_t leafCount = 0;
    LinearFit::Plan fit;
    uint64_t count = 0;
    // Held outside info until the writer knows the payload offset; the three
    // share a word on disk.
    uint8_t bits = 0;
    NumBlockKind kind = NumBlockKind::FOR;

    bool raw() const {
      return kind == NumBlockKind::FOR && bits > MAX_PACKED_BITS;
    }
  };

  // useGcd == false skips the common-divisor scan and pins gcd to 1. Monotonic
  // columns pass false: the slope already carries any regular step, so gcd only
  // buys bits when the residuals themselves share a divisor - rare - and it
  // costs a load and a multiply on every point read of the hottest columns in
  // the engine (endValueRank and endOffset, read twice per doc).
  // leafZones == false skips the per-leaf extrema pass for writers that never
  // persist zones (monotonic sidecars), whose finish() drops them anyway.
  template<class Values>
  static BlockPlan planBlock(const Values& values, std::vector<uint64_t>& quotients,
                            bool useGcd = true, bool leafZones = true) {
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
    if (leafZones) {
      for (size_t leafStart = 0; leafStart < values.size();
           leafStart += LEAF_ZONE_SIZE) {
        size_t leafEnd = std::min(leafStart + LEAF_ZONE_SIZE, values.size());
        auto [leafMinIt, leafMaxIt] = std::minmax_element(
            values.begin() + (ptrdiff_t)leafStart,
            values.begin() + (ptrdiff_t)leafEnd);
        plan.leafZones[plan.leafCount++] = {*leafMinIt, *leafMaxIt};
      }
    }

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

  // How a column's int64 values read as numbers, for the ALP kinds: FLOAT
  // and DOUBLE columns hold sortable bits (NumericUtils.h), FLOAT ones
  // sign-extended from int32.
  enum class ValueDomain : uint8_t { INT, FLOAT, DOUBLE };

  // The non-FOR kinds are only weighed for blocks of at least one decode
  // frame: a smaller (tail) block cannot save enough to matter, and tiny
  // columns keep the plain layout.
  static constexpr uint32_t MIN_CODEC_BLOCK = BULK_SIZE;
  // DICT codes stay at most 8 bits, and the dictionary at most 2 KB.
  static constexpr uint32_t MAX_DICT_SIZE = 256;
  // ALP picks its exponent pair on this many evenly spaced values, then
  // encodes the whole block with the winner.
  static constexpr uint32_t ALP_SAMPLE = 64;
  static constexpr uint8_t ALP_MAX_EXPONENT_DOUBLE = 18;
  static constexpr uint8_t ALP_MAX_EXPONENT_FLOAT = 10;
  // An exception costs its uint16 rank and its int64 value.
  static constexpr uint64_t ALP_EXCEPTION_BITS = 16 + 64;
  // Scaled values stay inside the double mantissa, so round() is exact and
  // the integers' FOR range fits MAX_PACKED_BITS.
  static constexpr double ALP_MAX_SCALED = 4503599627370496.0;  // 2^52

  static constexpr double ALP_POW10[] = {
      1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9,
      1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18};

  // ALP (Afroozeh, Kuffo, Boncz, SIGMOD 2024): a decimal-origin value v is
  // stored as the integer n = round(v * 10^e / 10^f) and read back as
  // n * 10^f / 10^e. The writer keeps n only when that read reproduces the
  // stored bits exactly, through this same function, so the encoding is
  // lossless by construction; any other value is an exception. ALP itself
  // multiplies by an inexact 10^-e; dividing by the exact 10^e instead costs
  // a division per value but makes the read the correctly rounded quotient,
  // which is exactly what a parser produced for a decimal with at most e - f
  // fraction digits, so such values never become exceptions. Floats decode
  // through double and round once more, which is innocuous for a quotient
  // (53 >= 2 * 24 + 2 bits).
  static int64_t alpDecode(int64_t n, uint8_t e, uint8_t f,
                           NumBlockKind kind) {
    double value = (double)n * ALP_POW10[f] / ALP_POW10[e];
    return kind == NumBlockKind::ALP_FLOAT
        ? (int64_t)floatToSortableInt32((float)value)
        : doubleToSortableInt64(value);
  }

  // The number a stored value stands for; false when it has none ALP can
  // scale (an out-of-range FLOAT pattern).
  static bool alpValue(int64_t stored, ValueDomain domain, double& value) {
    assert(domain != ValueDomain::INT);
    if (domain == ValueDomain::DOUBLE) {
      value = sortableInt64ToDouble(stored);
      return true;
    }
    if (stored < std::numeric_limits<int32_t>::min()
        || stored > std::numeric_limits<int32_t>::max()) {
      return false;
    }
    value = (double)sortableInt32ToFloat((int32_t)stored);
    return true;
  }

  // n for `value` at (e, f), or false if the value is an exception there.
  // The range test also rejects NaN and the infinities.
  static bool alpEncode(double value, int64_t stored, uint8_t e, uint8_t f,
                        NumBlockKind kind, int64_t& n) {
    double scaled = value * ALP_POW10[e] / ALP_POW10[f];
    if (!(std::fabs(scaled) <= ALP_MAX_SCALED)) return false;
    n = (int64_t)std::round(scaled);
    return alpDecode(n, e, f, kind) == stored;
  }

  // Payload of a non-FOR plan, reused across blocks: `prefix` (DELTA frame
  // anchors or the DICT entries), then `slots` bit-packed at plan.bits, then
  // for ALP the exception ranks and their stored values.
  struct CodecScratch {
    std::vector<int64_t> prefix;
    std::vector<uint64_t> slots;
    std::vector<uint16_t> positions;
    std::vector<int64_t> exceptions;
    std::vector<int64_t> dict;
    std::vector<double> numbers;
    std::vector<uint8_t> scalable;
  };

  // DELTA: each 128-value frame stores its first value as an anchor, and
  // every later value as its delta from the previous one, minus the
  // block's least in-frame delta. Frames keep decodeFrame independent of
  // earlier frames, and the point read to at most one frame's prefix sum.
  // Deltas are taken in 128-bit arithmetic for the width, and reconstruct
  // mod 2^64.
  static std::optional<__int128> deltaMin(std::span<const int64_t> values,
                                          uint8_t& bits) {
    __int128 lo = 0;
    __int128 hi = 0;
    bool any = false;
    for (size_t i = 1; i < values.size(); i++) {
      if (i % BULK_SIZE == 0) continue;
      __int128 delta = (__int128)values[i] - (__int128)values[i - 1];
      if (!any || delta < lo) lo = delta;
      if (!any || delta > hi) hi = delta;
      any = true;
    }
    if (!any) return std::nullopt;
    auto range = (unsigned __int128)(hi - lo);
    if (range >= ((unsigned __int128)1 << MAX_PACKED_BITS)) {
      return std::nullopt;
    }
    bits = (uint8_t)std::bit_width((uint64_t)range);
    return lo;
  }

  static uint64_t frameCount(uint64_t count) {
    return (count + BULK_SIZE - 1) / BULK_SIZE;
  }

  // DICT: the block's distinct values ascending, so codes keep value
  // order. Bails out past MAX_DICT_SIZE with an open-addressed table at
  // most half full.
  static bool collectDistinct(std::span<const int64_t> values,
                              std::vector<int64_t>& dict) {
    constexpr uint32_t SLOTS = 2 * MAX_DICT_SIZE;
    constexpr int SHIFT = 64 - std::countr_zero(SLOTS);
    int64_t table[SLOTS];
    bool used[SLOTS] = {};
    dict.clear();
    for (int64_t value : values) {
      uint32_t slot =
          (uint32_t)(((uint64_t)value * 0x9E3779B97F4A7C15ULL) >> SHIFT);
      while (used[slot] && table[slot] != value) {
        slot = (slot + 1) & (SLOTS - 1);
      }
      if (used[slot]) continue;
      if (dict.size() == MAX_DICT_SIZE) return false;
      used[slot] = true;
      table[slot] = value;
      dict.push_back(value);
    }
    std::sort(dict.begin(), dict.end());
    return true;
  }

  struct AlpPlan {
    uint8_t e = 0;
    uint8_t f = 0;
    uint8_t bits = 0;
    int64_t minN = 0;
    uint64_t cost = 0;
  };

  // Chooses (e, f) on a sample by estimated size, then encodes the block:
  // scratch.slots receives each value's n (0 for exceptions) and the
  // exception lists their ranks and stored values.
  static std::optional<AlpPlan> planAlp(std::span<const int64_t> values,
                                        ValueDomain domain,
                                        CodecScratch& scratch) {
    NumBlockKind kind = domain == ValueDomain::FLOAT
        ? NumBlockKind::ALP_FLOAT : NumBlockKind::ALP_DOUBLE;
    uint8_t maxExponent = domain == ValueDomain::FLOAT
        ? ALP_MAX_EXPONENT_FLOAT : ALP_MAX_EXPONENT_DOUBLE;
    size_t n = values.size();
    scratch.numbers.resize(n);
    scratch.scalable.resize(n);
    for (size_t i = 0; i < n; i++) {
      scratch.scalable[i] = alpValue(values[i], domain, scratch.numbers[i]);
    }

    // Sizes one (e, f) over every stride-th value.
    auto measure = [&](uint8_t e, uint8_t f, size_t stride, int64_t& lo,
                       uint64_t& exceptions) {
      int64_t hi = 0;
      bool any = false;
      exceptions = 0;
      for (size_t i = 0; i < n; i += stride) {
        int64_t scaled;
        if (!scratch.scalable[i]
            || !alpEncode(scratch.numbers[i], values[i], e, f, kind,
                          scaled)) {
          exceptions++;
          continue;
        }
        if (!any || scaled < lo) lo = scaled;
        if (!any || scaled > hi) hi = scaled;
        any = true;
      }
      if (!any) lo = 0;
      return any ? (uint8_t)std::bit_width((uint64_t)hi - (uint64_t)lo)
                 : (uint8_t)0;
    };

    size_t stride = std::max<size_t>(1, n / ALP_SAMPLE);
    uint64_t sampled = (n + stride - 1) / stride;
    AlpPlan best;
    bool found = false;
    for (uint8_t e = 0; e <= maxExponent; e++) {
      for (uint8_t f = 0; f <= e; f++) {
        int64_t lo = 0;
        uint64_t exceptions = 0;
        uint8_t bits = measure(e, f, stride, lo, exceptions);
        if (exceptions == sampled) continue;
        uint64_t cost = sampled * bits + exceptions * ALP_EXCEPTION_BITS;
        if (!found || cost < best.cost) {
          best = {e, f, bits, lo, cost};
          found = true;
        }
      }
    }
    if (!found) return std::nullopt;

    uint64_t exceptions = 0;
    best.bits = measure(best.e, best.f, 1, best.minN, exceptions);
    best.cost = n * best.bits + exceptions * ALP_EXCEPTION_BITS;

    scratch.slots.resize(n);
    scratch.positions.clear();
    scratch.exceptions.clear();
    for (size_t i = 0; i < n; i++) {
      int64_t scaled;
      if (!scratch.scalable[i]
          || !alpEncode(scratch.numbers[i], values[i], best.e, best.f, kind,
                        scaled)) {
        scratch.slots[i] = 0;
        scratch.positions.push_back((uint16_t)i);
        scratch.exceptions.push_back(values[i]);
        continue;
      }
      scratch.slots[i] = (uint64_t)scaled - (uint64_t)best.minN;
    }
    assert(scratch.positions.size() == exceptions);
    return best;
  }

  // Switches a FOR plan to the smallest other kind, when one is strictly
  // smaller, and fills scratch with its payload. Zones are left alone: they
  // bound the stored values, whatever the kind. ALP is weighed only for
  // FLOAT/DOUBLE domains.
  static void planCodec(std::span<const int64_t> values, BlockPlan& plan,
                        ValueDomain domain, CodecScratch& scratch) {
    uint64_t n = values.size();
    if (n < MIN_CODEC_BLOCK) return;
    uint64_t best = n * (plan.raw() ? RAW_BITS : plan.bits);
    NumBlockKind choice = NumBlockKind::FOR;

    uint8_t deltaBits = 0;
    auto minDelta = deltaMin(values, deltaBits);
    if (minDelta) {
      uint64_t cost = n * deltaBits + frameCount(n) * 64;
      if (cost < best) {
        best = cost;
        choice = NumBlockKind::DELTA;
      }
    }

    uint8_t dictBits = 0;
    if (plan.bits > 1 && collectDistinct(values, scratch.dict)
        && scratch.dict.size() > 1) {
      dictBits = (uint8_t)std::bit_width(scratch.dict.size() - 1);
      uint64_t cost = n * dictBits + scratch.dict.size() * 64;
      if (cost < best) {
        best = cost;
        choice = NumBlockKind::DICT;
      }
    }

    std::optional<AlpPlan> alp;
    if (domain != ValueDomain::INT) {
      alp = planAlp(values, domain, scratch);
      if (alp && alp->cost < best) {
        best = alp->cost;
        choice = domain == ValueDomain::FLOAT ? NumBlockKind::ALP_FLOAT
                                              : NumBlockKind::ALP_DOUBLE;
      }
    }

    if (choice == NumBlockKind::FOR) return;
    scratch.prefix.clear();
    if (choice != NumBlockKind::ALP_DOUBLE
        && choice != NumBlockKind::ALP_FLOAT) {
      scratch.positions.clear();
      scratch.exceptions.clear();
      scratch.slots.resize(n);
    }
    plan.kind = choice;
    plan.info.gcd = 1;
    plan.info.scaledSlope = 0;
    switch (choice) {
    case NumBlockKind::FOR:
      break;
    case NumBlockKind::DELTA:
      plan.bits = deltaBits;
      plan.info.baseBits = (uint64_t)*minDelta;
      for (size_t i = 0; i < n; i++) {
        if (i % BULK_SIZE == 0) {
          scratch.prefix.push_back(values[i]);
          scratch.slots[i] = 0;
        } else {
          scratch.slots[i] = (uint64_t)((__int128)values[i]
                                        - (__int128)values[i - 1]
                                        - *minDelta);
        }
      }
      break;
    case NumBlockKind::DICT:
      plan.bits = dictBits;
      plan.info.baseBits = 0;
      plan.info.gcd = scratch.dict.size();
      scratch.prefix = scratch.dict;
      for (size_t i = 0; i < n; i++) {
        scratch.slots[i] = (uint64_t)(std::lower_bound(scratch.dict.begin(),
                                                       scratch.dict.end(),
                                                       values[i])
                                      - scratch.dict.begin());
      }
      break;
    case NumBlockKind::ALP_DOUBLE:
    case NumBlockKind::ALP_FLOAT:
      // planAlp already left the slots and exceptions in scratch.
      plan.bits = alp->bits;
      plan.info.baseBits = (uint64_t)alp->minN;
      plan.info.gcd = (uint64_t)alp->e | ((uint64_t)alp->f << 8);
      plan.info.scaledSlope = (int64_t)scratch.positions.size();
      break;
    }
    assert(std::ranges::all_of(scratch.slots, [&](uint64_t slot) {
      return std::bit_width(slot) <= plan.bits;
    }));
  }

  // Leaf visit orders, persisted after the leaf zones. A field sort over the
  // column visits leaves best bound first, and that bound is a pure function
  // of the leaf zones and the sort direction - never of the query - so the
  // writer sorts once instead of every query rederiving the order. `out`
  // receives the leaf ids ascending by (zone.min, leaf), the ascending-sort
  // order, or with `byMaxDescending`, descending by zone.max with ties by
  // ascending leaf: ascending ~max, the descending transformed key. The leaf
  // id tie-break is doc order for dense columns.
  static void leafVisitOrder(std::span<const NumBlockZone> leafZones,
                             bool byMaxDescending, std::span<uint32_t> out) {
    assert(out.size() == leafZones.size());
    assert(leafZones.size() <= std::numeric_limits<uint32_t>::max());
    std::iota(out.begin(), out.end(), 0u);
    if (byMaxDescending) {
      std::sort(out.begin(), out.end(), [&](uint32_t a, uint32_t b) {
        int64_t ka = leafZones[a].max;
        int64_t kb = leafZones[b].max;
        return ka != kb ? ka > kb : a < b;
      });
    } else {
      std::sort(out.begin(), out.end(), [&](uint32_t a, uint32_t b) {
        int64_t ka = leafZones[a].min;
        int64_t kb = leafZones[b].min;
        return ka != kb ? ka < kb : a < b;
      });
    }
  }

  // Value sketch, persisted after the leaf visit orders: points (rank,
  // value) of the column's values in sorted order, ranks strictly
  // increasing from 0 (the minimum) to numValues - 1 (the maximum). A field
  // sort reads it to estimate where its k-th best key falls (ValueSketch),
  // which sits in a tail of the distribution whenever k is small next to
  // the domain, so the tails are exact: every rank within SKETCH_DENSE of
  // either end, then ranks spaced at most 1/32 of their distance from that
  // end, through SKETCH_TAIL. Past the tails a systematic sample - the
  // values at every stride-th position in column order, the stride doubling
  // to keep between SKETCH_SAMPLES and 2 * SKETCH_SAMPLES - 1 of them - places
  // a point per sampled value at its expected rank. A 5M-value column keeps
  // about 2,700 points (43 KB). The tails cost the writer amortized O(1) per
  // value (candidate buffers trimmed by selection) and 512 KB while writing,
  // so only the columns field-sort planning reads persist one
  // (NumColumnWriter); the rest write an empty sketch.
  static constexpr int64_t SKETCH_DENSE = 64;
  static constexpr int64_t SKETCH_TAIL = 16384;
  static constexpr int64_t SKETCH_SAMPLES = 1024;

  struct SketchPoint {
    int64_t rank;
    int64_t value;
  };
  static_assert(sizeof(SketchPoint) == 16);

  // The ranks a tail keeps, in increasing distance from its end: every one
  // below SKETCH_DENSE, then steps of the largest power of two at most 1/32
  // of the distance, then the tail's last rank.
  static std::vector<int64_t> tailRanks(int64_t depth) {
    static_assert(SKETCH_DENSE == 64, "the step reaches 2 at rank 64");
    std::vector<int64_t> ranks;
    for (int64_t r = 0; r < depth;
         r += std::max<int64_t>(1, (int64_t)std::bit_floor((uint64_t)r / 32))) {
      ranks.push_back(r);
    }
    if (!ranks.empty() && ranks.back() != depth - 1) {
      ranks.push_back(depth - 1);
    }
    return ranks;
  }

  // The `depth` least values of a stream under `before`: candidates are
  // appended while they could still rank within depth, and selection trims
  // them back to depth whenever they reach twice that.
  template <typename Before>
  class TailBuffer {
    std::vector<int64_t> candidates;
    int64_t depth;
    int64_t threshold = 0;
    bool trimmed = false;

    void trim() {
      std::nth_element(candidates.begin(),
                       candidates.begin() + (ptrdiff_t)(depth - 1),
                       candidates.end(), Before());
      candidates.resize((size_t)depth);
      threshold = candidates.back();
      trimmed = true;
    }

  public:
    explicit TailBuffer(int64_t depth) : depth(depth) {}

    void add(int64_t value) {
      if (trimmed && Before()(threshold, value)) return;
      candidates.push_back(value);
      if ((int64_t)candidates.size() == 2 * depth) {
        // The selected depth-th value bounds every later candidate.
        trim();
      }
    }

    // Least first, at most depth of them (consumes the buffer).
    std::vector<int64_t> finish() {
      if ((int64_t)candidates.size() > depth) trim();
      std::sort(candidates.begin(), candidates.end(), Before());
      return std::move(candidates);
    }
  };

  class ValueSketcher {
    TailBuffer<std::less<int64_t>> low{SKETCH_TAIL};
    TailBuffer<std::greater<int64_t>> high{SKETCH_TAIL};
    std::vector<int64_t> sample;
    int64_t stride = 1;
    // Offset of the next sampled position from the start of the next add().
    int64_t next = 0;
    int64_t count = 0;

    void addSample(std::span<const int64_t> values) {
      int64_t i = next;
      for (; i < (int64_t)values.size(); i += stride) {
        sample.push_back(values[(size_t)i]);
        if ((int64_t)sample.size() == 2 * SKETCH_SAMPLES) {
          // Keep the even entries: the positions that are multiples of the
          // doubled stride. The next sampled position, (2 * SKETCH_SAMPLES)
          // * stride, is one of them, so it is reached with the old stride.
          for (int64_t j = 0; j < SKETCH_SAMPLES; j++) {
            sample[(size_t)j] = sample[(size_t)(2 * j)];
          }
          sample.resize((size_t)SKETCH_SAMPLES);
          i += stride;
          stride *= 2;
          i -= stride;
        }
      }
      next = i - (int64_t)values.size();
    }

  public:
    // Continues the value stream with `values`.
    void add(std::span<const int64_t> values) {
      for (int64_t value : values) {
        low.add(value);
        high.add(value);
      }
      addSample(values);
      count += (int64_t)values.size();
    }

    // The sketch's points, by rank (consumes the sketcher).
    std::vector<SketchPoint> finish() {
      std::vector<int64_t> least = low.finish();
      std::vector<int64_t> greatest = high.finish();
      std::vector<SketchPoint> points;
      if (count == 0) return points;
      // Each tail's ranks, from its own end; a column under twice the tail
      // depth has the two overlap, and either buffer holds the value there.
      int64_t lowDepth = (int64_t)least.size();
      int64_t highStart = count - (int64_t)greatest.size();
      std::vector<int64_t> ranks = tailRanks(lowDepth);
      for (int64_t r : tailRanks((int64_t)greatest.size())) {
        ranks.push_back(count - 1 - r);
      }
      std::sort(ranks.begin(), ranks.end());
      ranks.erase(std::unique(ranks.begin(), ranks.end()), ranks.end());
      for (int64_t rank : ranks) {
        points.push_back({rank, rank < lowDepth
                                    ? least[(size_t)rank]
                                    : greatest[(size_t)(count - 1 - rank)]});
      }
      // The bulk between the tails, from the sample at its expected ranks.
      if (highStart > lowDepth) {
        std::sort(sample.begin(), sample.end());
        int64_t m = (int64_t)sample.size();
        std::vector<SketchPoint> bulk;
        for (int64_t j = 0; j < m; j++) {
          int64_t rank =
              (int64_t)(((double)j + 0.5) * (double)count / (double)m);
          int64_t value = sample[(size_t)j];
          if (rank < lowDepth || rank >= highStart || value < least.back()
              || value > greatest.back()
              || (!bulk.empty() && rank <= bulk.back().rank)) {
            continue;
          }
          bulk.push_back({rank, value});
        }
        auto at = std::lower_bound(
            points.begin(), points.end(), highStart,
            [](const SketchPoint& p, int64_t rank) { return p.rank < rank; });
        points.insert(at, bulk.begin(), bulk.end());
      }
      return points;
    }
  };

  // Read view of a persisted value sketch over `population` values: the
  // nondecreasing value-at-rank polyline through its points. Between two
  // points the unseen values spread evenly (equal points make a run of
  // ties), and points at adjacent ranks count exactly. Ranks are
  // fractional, so countBelow and valueAt are inverses up to rounding to
  // whole values.
  class ValueSketch {
    const char* points = nullptr;  // `count` SketchPoints, unaligned
    int64_t count = 0;
    int64_t population = 0;

    static double span(int64_t lo, int64_t hi) {
      return (double)((uint64_t)hi - (uint64_t)lo);
    }

  public:
    ValueSketch() = default;
    ValueSketch(const char* points, int64_t count, int64_t population)
        : points(points), count(count), population(population) {
      assert(count > 0 && count <= population);
      assert(point(0).rank == 0 && point(count - 1).rank == population - 1);
    }

    bool empty() const { return count == 0; }
    int64_t size() const { return count; }
    int64_t values() const { return population; }

    SketchPoint point(int64_t i) const {
      assert(i >= 0 && i < count);
      SketchPoint p;
      memcpy(&p, points + i * (int64_t)sizeof(p), sizeof(p));
      return p;
    }

    // Expected number of values strictly below v.
    double countBelow(int64_t v) const {
      assert(!empty());
      if (v <= point(0).value) return 0;
      // The first point at or above v.
      int64_t lo = 0;
      int64_t hi = count;
      while (lo < hi) {
        int64_t mid = lo + (hi - lo) / 2;
        if (point(mid).value < v) lo = mid + 1;
        else hi = mid;
      }
      if (lo == 0) return 0;
      if (lo == count) return (double)population;
      SketchPoint a = point(lo - 1);
      SketchPoint b = point(lo);
      // Ranks through a hold values below v, rank b's does not.
      double below = (double)a.rank
          + (double)(b.rank - a.rank) * span(a.value, v)
              / span(a.value, b.value);
      return std::clamp(below, (double)a.rank + 1, (double)b.rank);
    }

    // The value at fractional rank r, clamped to [0, population - 1].
    int64_t valueAt(double r) const {
      assert(!empty());
      if (r <= 0) return point(0).value;
      if (r >= (double)(population - 1)) return point(count - 1).value;
      // The last point at or below rank r.
      int64_t lo = 0;
      int64_t hi = count - 1;
      while (lo < hi) {
        int64_t mid = lo + (hi - lo + 1) / 2;
        if ((double)point(mid).rank <= r) lo = mid;
        else hi = mid - 1;
      }
      SketchPoint a = point(lo);
      SketchPoint b = point(lo + 1);
      // Adjacent ranks hold no value between them.
      if (b.rank == a.rank + 1) return r < (double)b.rank ? a.value : b.value;
      double offset = (r - (double)a.rank) / (double)(b.rank - a.rank)
          * span(a.value, b.value);
      uint64_t step = std::min<uint64_t>(
          (uint64_t)offset, (uint64_t)b.value - (uint64_t)a.value);
      return (int64_t)((uint64_t)a.value + step);
    }
  };

  // Read view of a column's leaf metadata as NumColumnWriter::finish lays
  // it out after the block zones: the leaf zones, the two leaf visit orders
  // (leafVisitOrder), and the value sketch. Copyable, so a planner can keep
  // one per segment; valid while the column's bytes are.
  class LeafMeta {
    const char* zones = nullptr;  // NumBlockZones, then the uint32 orders
    int64_t count = 0;
    ValueSketch sketch;

    int64_t orderAt(int64_t order, int64_t rank) const {
      assert(rank >= 0 && rank < count);
      uint32_t leaf;
      memcpy(&leaf,
             zones + count * (int64_t)sizeof(NumBlockZone)
                 + (order * count + rank) * (int64_t)sizeof(leaf),
             sizeof(leaf));
      return leaf;
    }

  public:
    LeafMeta() = default;

    // `leafZones` points at the first leaf zone of a column of numValues
    // values; `end` receives the first byte past the sketch.
    LeafMeta(const char* leafZones, int64_t numValues, const char*& end)
        : zones(leafZones),
          count((numValues + LEAF_ZONE_SIZE - 1) / LEAF_ZONE_SIZE) {
      end = zones + count * (int64_t)(sizeof(NumBlockZone)
                                      + 2 * sizeof(uint32_t));
      if (count == 0) return;
      uint32_t points;
      memcpy(&points, end, sizeof(points));
      if (points > 0) {
        sketch = ValueSketch(end + sizeof(points), points, numValues);
      }
      end += sizeof(points) + points * (int64_t)sizeof(SketchPoint);
    }

    int64_t numLeafZones() const { return count; }

    NumBlockZone leafZone(int64_t leaf) const {
      assert(leaf >= 0 && leaf < count);
      NumBlockZone zone;
      memcpy(&zone, zones + leaf * (int64_t)sizeof(zone), sizeof(zone));
      return zone;
    }

    // The leaf at `rank` ascending by (zone.min, leaf), and descending by
    // zone.max with ties by ascending leaf.
    int64_t leafByMinAscending(int64_t rank) const { return orderAt(0, rank); }
    int64_t leafByMaxDescending(int64_t rank) const {
      return orderAt(1, rank);
    }

    // Empty for a column without values or written without a sketch.
    const ValueSketch& valueSketch() const { return sketch; }
  };

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

} // namespace luxir
