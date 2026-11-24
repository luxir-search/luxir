// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <vector>

#include "NumColumnFormat.h"

namespace luxir {

// Write-side choice among the block kinds (NumBlockKind) for one block of a
// general column that opted in (NumColumnFormat::BlockCodecs). It starts
// from the FOR plan NumColumnFormat::planBlock made and switches to another
// kind only when that kind's estimated size is strictly smaller. Zones are
// left alone: they bound the stored values, whatever the kind.
//
// Payload layouts, all bit-packing through LinearPack:
//   DELTA        frameCount(n) anchors packed at anchorBits() as
//                anchor - gcd, then n slots at bits(): 0 at a frame's first
//                rank, else value - previous - baseBits (mod 2^64)
//   DICT         dictSize() int64 entries ascending, then n codes at bits()
//   ALP_*        n slots at bits(), n - baseBits (0 for an exception), then
//                exceptions() uint16 ranks ascending and their int64 values
//   FOR_PATCHED  n slots at bits(), (value - baseBits) / gcd (0 for an
//                exception), then the exceptions as for ALP
class NumBlockPlanner {
public:
  using ValueDomain = NumColumnFormat::ValueDomain;
  static constexpr uint32_t BULK_SIZE = NumColumnFormat::BULK_SIZE;
  static constexpr uint8_t MAX_PACKED_BITS = NumColumnFormat::MAX_PACKED_BITS;

  // The other kinds are only weighed for blocks of at least one decode
  // frame: a smaller (tail) block cannot save enough to matter, and tiny
  // columns keep the plain layout.
  static constexpr uint32_t MIN_CODEC_BLOCK = BULK_SIZE;
  // DICT codes stay at most 8 bits, and the dictionary at most 2 KB.
  static constexpr uint32_t MAX_DICT_SIZE = 256;
  // ALP picks its exponent pair on this many evenly spaced values, then
  // encodes the whole block with the winner.
  static constexpr uint32_t ALP_SAMPLE = 64;
  // An exception costs its uint16 rank and its int64 value.
  static constexpr uint64_t EXCEPTION_BITS = 16 + 64;
  // FOR_PATCHED patches at most this many values of a block, the least and
  // greatest ones.
  static constexpr uint32_t MAX_PATCHES = 128;
  static constexpr uint64_t DELTA_MIN_SAVED_BITS = 2;

  // A non-FOR plan's payload, and scratch reused across blocks.
  struct Payload {
    std::vector<uint64_t> anchors;   // DELTA, less the least anchor
    std::vector<int64_t> dict;       // DICT entries
    std::vector<uint64_t> slots;
    std::vector<uint16_t> positions;
    std::vector<int64_t> exceptions;
    std::vector<int64_t> sorted;
    std::vector<double> numbers;
    std::vector<uint8_t> scalable;
    std::vector<char> bytes;         // the encoded block, for the writer
  };

  static uint64_t frameCount(uint64_t count) {
    return (count + BULK_SIZE - 1) / BULK_SIZE;
  }

  static void plan(std::span<const int64_t> values,
                   NumColumnFormat::BlockPlan& plan, ValueDomain domain,
                   Payload& payload) {
    uint64_t n = values.size();
    if (n < MIN_CODEC_BLOCK) return;
    assert(plan.kind == NumBlockKind::FOR);
    uint64_t best = n * (plan.raw() ? NumColumnFormat::RAW_BITS : plan.bits);
    NumBlockKind choice = NumBlockKind::FOR;
    auto consider = [&](std::optional<uint64_t> cost, NumBlockKind kind) {
      if (cost && *cost < best) {
        best = *cost;
        choice = kind;
      }
    };

    // A DELTA point read sums up to a frame's prefix (up to 127 adds after
    // a 128-slot unpack), and field sorts point-read every collected doc, so
    // DELTA has to save at least DELTA_MIN_SAVED_BITS per value over FOR.
    DeltaPlan delta;
    if (auto cost = planDelta(values, delta);
        cost && *cost + n * DELTA_MIN_SAVED_BITS <= best) {
      consider(cost, NumBlockKind::DELTA);
    }
    uint8_t dictBits = 0;
    consider(planDict(values, plan, payload, dictBits), NumBlockKind::DICT);
    PatchPlan patch;
    consider(planPatched(values, plan, payload, patch),
             NumBlockKind::FOR_PATCHED);
    // ALP encodes into the payload as it plans, so it goes last.
    std::optional<AlpPlan> alp;
    if (domain != ValueDomain::INT) {
      NumBlockKind kind = domain == ValueDomain::FLOAT
          ? NumBlockKind::ALP_FLOAT : NumBlockKind::ALP_DOUBLE;
      alp = planAlp(values, kind, payload, best);
      if (alp) consider(alp->cost, kind);
    }
    if (choice == NumBlockKind::FOR) return;

    payload.slots.resize(n);
    if (!alp || choice != alp->kind) {
      payload.positions.clear();
      payload.exceptions.clear();
    }
    plan.kind = choice;
    plan.info.gcd = 1;
    plan.info.scaledSlope = 0;
    switch (choice) {
    case NumBlockKind::FOR:
      break;
    case NumBlockKind::DELTA:
      encodeDelta(values, delta, plan, payload);
      break;
    case NumBlockKind::DICT:
      plan.bits = dictBits;
      plan.info.baseBits = 0;
      plan.info.gcd = payload.dict.size();
      for (size_t i = 0; i < n; i++) {
        payload.slots[i] = (uint64_t)(std::lower_bound(payload.dict.begin(),
                                                       payload.dict.end(),
                                                       values[i])
                                      - payload.dict.begin());
      }
      break;
    case NumBlockKind::FOR_PATCHED:
      encodePatched(values, patch, plan, payload);
      break;
    case NumBlockKind::ALP_DOUBLE:
    case NumBlockKind::ALP_FLOAT:
      // planAlp already left the slots and exceptions in the payload.
      plan.bits = alp->bits;
      plan.info.baseBits = (uint64_t)alp->minN;
      plan.info.gcd = (uint64_t)alp->e | ((uint64_t)alp->f << 8);
      plan.info.scaledSlope = (int64_t)payload.positions.size();
      break;
    }
    assert(std::ranges::all_of(payload.slots, [&](uint64_t slot) {
      return std::bit_width(slot) <= plan.bits;
    }));
  }

private:
  struct DeltaPlan {
    __int128 minDelta = 0;
    int64_t minAnchor = 0;
    uint8_t bits = 0;
    uint8_t anchorBits = 0;
  };

  // DELTA: each 128-value frame stores its first value as an anchor, and
  // every later value as its delta from the previous one, minus the block's
  // least in-frame delta. Frames keep decodeFrame independent of earlier
  // frames, and the point read to at most one frame's prefix sum. Deltas
  // are taken in 128-bit arithmetic for the width and reconstruct mod 2^64.
  // The anchors are FOR-packed off the least one.
  static std::optional<uint64_t> planDelta(std::span<const int64_t> values,
                                           DeltaPlan& delta) {
    __int128 lo = 0;
    __int128 hi = 0;
    bool any = false;
    int64_t minAnchor = values[0];
    int64_t maxAnchor = values[0];
    for (size_t i = 1; i < values.size(); i++) {
      if (i % BULK_SIZE == 0) {
        minAnchor = std::min(minAnchor, values[i]);
        maxAnchor = std::max(maxAnchor, values[i]);
        continue;
      }
      __int128 d = (__int128)values[i] - (__int128)values[i - 1];
      if (!any || d < lo) lo = d;
      if (!any || d > hi) hi = d;
      any = true;
    }
    if (!any) return std::nullopt;
    auto range = (unsigned __int128)(hi - lo);
    uint64_t anchorRange = (uint64_t)maxAnchor - (uint64_t)minAnchor;
    if (range >= ((unsigned __int128)1 << MAX_PACKED_BITS)
        || std::bit_width(anchorRange) > MAX_PACKED_BITS) {
      return std::nullopt;
    }
    delta.minDelta = lo;
    delta.minAnchor = minAnchor;
    delta.bits = (uint8_t)std::bit_width((uint64_t)range);
    delta.anchorBits = (uint8_t)std::bit_width(anchorRange);
    return values.size() * delta.bits
        + frameCount(values.size()) * delta.anchorBits;
  }

  static void encodeDelta(std::span<const int64_t> values,
                          const DeltaPlan& delta,
                          NumColumnFormat::BlockPlan& plan, Payload& payload) {
    plan.bits = delta.bits;
    plan.info.baseBits = (uint64_t)(int64_t)delta.minDelta;
    plan.info.gcd = (uint64_t)delta.minAnchor;
    plan.info.scaledSlope = delta.anchorBits;
    payload.anchors.clear();
    for (size_t i = 0; i < values.size(); i++) {
      if (i % BULK_SIZE == 0) {
        payload.anchors.push_back((uint64_t)values[i]
                                  - (uint64_t)delta.minAnchor);
        payload.slots[i] = 0;
      } else {
        payload.slots[i] = (uint64_t)((__int128)values[i]
                                      - (__int128)values[i - 1]
                                      - delta.minDelta);
      }
    }
  }

  // DICT: the block's distinct values ascending, so codes keep value
  // order. Bails out past MAX_DICT_SIZE with an open-addressed table at
  // most half full.
  static std::optional<uint64_t> planDict(
      std::span<const int64_t> values, const NumColumnFormat::BlockPlan& plan,
      Payload& payload, uint8_t& bits) {
    if (!plan.raw() && plan.bits <= 1) return std::nullopt;
    constexpr uint32_t SLOTS = 2 * MAX_DICT_SIZE;
    constexpr int SHIFT = 64 - std::countr_zero(SLOTS);
    int64_t table[SLOTS];
    bool used[SLOTS] = {};
    std::vector<int64_t>& dict = payload.dict;
    dict.clear();
    for (int64_t value : values) {
      uint32_t slot =
          (uint32_t)(((uint64_t)value * 0x9E3779B97F4A7C15ULL) >> SHIFT);
      while (used[slot] && table[slot] != value) {
        slot = (slot + 1) & (SLOTS - 1);
      }
      if (used[slot]) continue;
      if (dict.size() == MAX_DICT_SIZE) return std::nullopt;
      used[slot] = true;
      table[slot] = value;
      dict.push_back(value);
    }
    if (dict.size() < 2) return std::nullopt;
    std::sort(dict.begin(), dict.end());
    bits = (uint8_t)std::bit_width(dict.size() - 1);
    return values.size() * bits + dict.size() * 64;
  }

  struct PatchPlan {
    int64_t lo = 0;
    int64_t hi = 0;
    uint64_t gcd = 1;
    uint8_t bits = 0;
  };

  // FOR_PATCHED: the constant FOR frame over [lo, hi], with every value
  // outside it patched. A single outlier otherwise sets the width for the
  // whole block. The frame keeps the block's gcd (every value is congruent
  // to the block minimum, so to lo too) and is chosen exactly, not on a
  // sample: for c = 0..MAX_PATCHES patches split as the i least and c - i
  // greatest values, the width of [sorted[i], sorted[n - 1 - (c - i)]],
  // at n * width + c * EXCEPTION_BITS bits. Only the MAX_PATCHES + 1 least
  // and greatest values need ordering, so selection keeps this linear.
  static std::optional<uint64_t> planPatched(
      std::span<const int64_t> values, const NumColumnFormat::BlockPlan& plan,
      Payload& payload, PatchPlan& patch) {
    uint64_t n = values.size();
    uint32_t maxPatches = (uint32_t)std::min<uint64_t>(MAX_PATCHES, n / 8);
    // A RAW block is weighed too: outliers can be what makes its range
    // exceed MAX_PACKED_BITS.
    if (!plan.raw() && plan.bits <= 1) return std::nullopt;
    uint64_t gcd = plan.info.gcd;
    assert(gcd != 0);
    std::vector<int64_t>& s = payload.sorted;
    s.assign(values.begin(), values.end());
    size_t edge = maxPatches + 1;
    std::nth_element(s.begin(), s.begin() + (ptrdiff_t)(edge - 1), s.end());
    std::sort(s.begin(), s.begin() + (ptrdiff_t)edge);
    std::nth_element(s.begin() + (ptrdiff_t)edge,
                     s.end() - (ptrdiff_t)edge, s.end());
    std::sort(s.end() - (ptrdiff_t)edge, s.end());

    std::optional<uint64_t> best;
    for (uint32_t c = 1; c <= maxPatches; c++) {
      uint64_t patchCost = (uint64_t)c * EXCEPTION_BITS;
      if (best && patchCost >= *best) break;
      for (uint32_t i = 0; i <= c; i++) {
        int64_t lo = s[i];
        int64_t hi = s[n - 1 - (c - i)];
        uint64_t range = ((uint64_t)hi - (uint64_t)lo) / gcd;
        int width = std::bit_width(range);
        if (width > MAX_PACKED_BITS) continue;
        uint64_t cost = n * (uint64_t)width + patchCost;
        if (!best || cost < *best) {
          best = cost;
          patch = {lo, hi, gcd, (uint8_t)width};
        }
      }
    }
    return best;
  }

  static void encodePatched(std::span<const int64_t> values,
                            const PatchPlan& patch,
                            NumColumnFormat::BlockPlan& plan,
                            Payload& payload) {
    uint64_t gcd = patch.gcd;
    plan.bits = patch.bits;
    plan.info.baseBits = (uint64_t)patch.lo;
    for (size_t i = 0; i < values.size(); i++) {
      int64_t v = values[i];
      if (v < patch.lo || v > patch.hi) {
        payload.slots[i] = 0;
        payload.positions.push_back((uint16_t)i);
        payload.exceptions.push_back(v);
        continue;
      }
      payload.slots[i] = ((uint64_t)v - (uint64_t)patch.lo) / gcd;
    }
    assert(!payload.positions.empty());
    plan.info.gcd = gcd;
    plan.info.scaledSlope = (int64_t)payload.positions.size();
  }

  // The number a stored value stands for; false when it has none ALP can
  // scale (an out-of-range FLOAT pattern).
  static bool alpValue(int64_t stored, NumBlockKind kind, double& value) {
    if (kind == NumBlockKind::ALP_DOUBLE) {
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

  // n for `value` at (e, f), or false if the value is an exception there:
  // the decode must reproduce the stored bits exactly (the round-trip
  // check that makes ALP lossless). The range test also rejects NaN and
  // the infinities.
  static bool alpEncode(double value, int64_t stored, uint8_t e, uint8_t f,
                        NumBlockKind kind, int64_t& n) {
    double scaled = value * NumColumnFormat::ALP_POW10[e]
        / NumColumnFormat::ALP_POW10[f];
    if (!(std::fabs(scaled) <= NumColumnFormat::ALP_MAX_SCALED)) return false;
    n = (int64_t)std::round(scaled);
    return NumColumnFormat::alpDecode(n, e, f, kind) == stored;
  }

  struct AlpPlan {
    NumBlockKind kind = NumBlockKind::ALP_DOUBLE;
    uint8_t e = 0;
    uint8_t f = 0;
    uint8_t bits = 0;
    int64_t minN = 0;
    uint64_t cost = 0;
  };

  // Chooses (e, f) on a sample by estimated size, then, when that beats
  // `budget`, encodes the block: the payload receives each value's slot (0
  // for exceptions) and the exceptions' ranks and stored values.
  static std::optional<AlpPlan> planAlp(std::span<const int64_t> values,
                                        NumBlockKind kind, Payload& payload,
                                        uint64_t budget) {
    uint8_t maxExponent = kind == NumBlockKind::ALP_FLOAT
        ? NumColumnFormat::ALP_MAX_EXPONENT_FLOAT
        : NumColumnFormat::ALP_MAX_EXPONENT_DOUBLE;
    size_t n = values.size();
    payload.numbers.resize(n);
    payload.scalable.resize(n);
    for (size_t i = 0; i < n; i++) {
      payload.scalable[i] = alpValue(values[i], kind, payload.numbers[i]);
    }

    // Sizes one (e, f) over every stride-th value.
    auto measure = [&](uint8_t e, uint8_t f, size_t stride, int64_t& lo,
                       uint64_t& exceptions) {
      int64_t hi = 0;
      bool any = false;
      exceptions = 0;
      for (size_t i = 0; i < n; i += stride) {
        int64_t scaled;
        if (!payload.scalable[i]
            || !alpEncode(payload.numbers[i], values[i], e, f, kind,
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
    best.kind = kind;
    bool found = false;
    for (uint8_t e = 0; e <= maxExponent; e++) {
      for (uint8_t f = 0; f <= e; f++) {
        int64_t lo = 0;
        uint64_t exceptions = 0;
        uint8_t bits = measure(e, f, stride, lo, exceptions);
        if (exceptions == sampled) continue;
        uint64_t cost = sampled * bits + exceptions * EXCEPTION_BITS;
        if (!found || cost < best.cost) {
          best.e = e;
          best.f = f;
          best.cost = cost;
          found = true;
        }
      }
    }
    if (!found) return std::nullopt;

    uint64_t exceptions = 0;
    best.bits = measure(best.e, best.f, 1, best.minN, exceptions);
    if (best.bits > MAX_PACKED_BITS) return std::nullopt;
    best.cost = n * best.bits + exceptions * EXCEPTION_BITS;
    if (best.cost >= budget) return best;

    payload.slots.resize(n);
    payload.positions.clear();
    payload.exceptions.clear();
    for (size_t i = 0; i < n; i++) {
      int64_t scaled;
      if (!payload.scalable[i]
          || !alpEncode(payload.numbers[i], values[i], best.e, best.f, kind,
                        scaled)) {
        payload.slots[i] = 0;
        payload.positions.push_back((uint16_t)i);
        payload.exceptions.push_back(values[i]);
        continue;
      }
      payload.slots[i] = (uint64_t)scaled - (uint64_t)best.minN;
    }
    assert(payload.positions.size() == exceptions);
    return best;
  }
};

} // namespace luxir
