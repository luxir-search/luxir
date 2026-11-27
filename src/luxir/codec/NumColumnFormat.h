// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <expected>
#include <functional>
#include <limits>
#include <numeric>
#include <optional>
#include <span>
#include <stdexcept>
#include <vector>

#include "LinearFit.h"
#include "Codec.h"
#include "luxir/util/encoding.h"
#include "luxir/util/NumericUtils.h"
#include "luxir/util/luxir_util.h"

namespace luxir {

// Block encodings, persisted in the top 4 bits of the descriptor's first
// word: append only.
//   FOR          frame of reference: base + gcd * (slopeTerm(rank) +
//                residual), or RAW (bits() == RAW_BITS) plain int64s
//   DELTA        FOR-packed anchors, one per 128-value frame, then the
//                bit-packed in-frame deltas
//   DICT         a sorted per-block dictionary, then bit-packed codes
//   ALP_DOUBLE   decimal-scaled doubles (ALP), then patched exceptions
//   ALP_FLOAT    the same for floats
//   FOR_PATCHED  constant FOR over all but a few outliers, which are patched
//                exceptions
// Every kind decodes to the column's int64 values (sortable bits for
// FLOAT/DOUBLE), so consumers above the decode layer never see the kind.
// Only general field value columns write the kinds other than FOR
// (NumColumnFormat::BlockCodecs).
enum class NumBlockKind : uint8_t {
  FOR = 0,
  DELTA = 1,
  DICT = 2,
  ALP_DOUBLE = 3,
  ALP_FLOAT = 4,
  FOR_PATCHED = 5,
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
  // anything can reach. Kind above width keeps "a FOR block with a packed
  // payload", the hot case, one compare of the top 12 bits (forPacked).
  //
  // A general block can use gcd and scaledSlope together - a sorted
  // day-granularity date column is a ramp over values sharing a divisor - so
  // the two are separate fields. Monotonic columns pin gcd to 1 and
  // NumColumnT<false> never reads it.
  //
  // The other kinds reuse the three value fields:
  //   DELTA        baseBits = the least in-frame delta (mod 2^64), gcd =
  //                the least anchor, scaledSlope = the anchors' width
  //   DICT         gcd = dictionary size
  //   ALP_*        baseBits = the least scaled integer, gcd = exponent |
  //                factor << 8, scaledSlope = exception count
  //   FOR_PATCHED  baseBits and gcd as a constant FOR block, scaledSlope =
  //                exception count
  // so every reader of scaledSlope as a slope, and of baseBits/gcd as a FOR
  // frame, must first test the kind (forPacked, linearFit,
  // residualComparable all do).
  uint64_t offsetAndBits = 0;
  uint64_t baseBits = 0;
  uint64_t gcd = 1;
  int64_t scaledSlope = 0;

  static constexpr uint32_t OFFSET_BITS = 52;
  static constexpr uint32_t KIND_SHIFT = OFFSET_BITS + 8;
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
    return (NumBlockKind)(offsetAndBits >> KIND_SHIFT);
  }

  // A FOR block with a bit-packed payload: not RAW, not another kind.
  bool forPacked() const {
    return (offsetAndBits >> OFFSET_BITS) <= MAX_PACKED_BITS;
  }

  // A FOR block of plain int64s.
  bool raw() const {
    return (offsetAndBits >> OFFSET_BITS) == RAW_BITS;
  }

  bool linearFit() const {
    return forPacked() && scaledSlope != 0;
  }

  // Packed values are (value - min) / gcd, directly comparable with bounds
  // transformed into that residual domain.
  bool residualComparable() const {
    return forPacked() && bits() <= 32 && scaledSlope == 0;
  }

  // Packed slots ordered like the values they decode to, at most 32 bits:
  // range queries compare them against bounds mapped into the slot domain
  // (NumColumnT::slotBounds), and check exceptions() apart. DELTA slots
  // are not ordered, and a sloped or wide FOR block is not either.
  bool slotComparable() const {
    if (residualComparable()) return true;
    NumBlockKind k = kind();
    return bits() <= 32
        && (k == NumBlockKind::DICT || k == NumBlockKind::FOR_PATCHED
            || k == NumBlockKind::ALP_DOUBLE || k == NumBlockKind::ALP_FLOAT);
  }

  // Patched kinds: values stored apart from the packed slots, whose own
  // slot reads as 0.
  bool patched() const {
    NumBlockKind k = kind();
    return k == NumBlockKind::FOR_PATCHED || k == NumBlockKind::ALP_DOUBLE
        || k == NumBlockKind::ALP_FLOAT;
  }

  uint64_t exceptions() const {
    return patched() ? (uint64_t)scaledSlope : 0;
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

  uint8_t anchorBits() const {
    assert(kind() == NumBlockKind::DELTA);
    return (uint8_t)scaledSlope;
  }

  void setPayload(uint64_t payloadOffset, uint8_t bits,
                  NumBlockKind kind = NumBlockKind::FOR) {
    assert(payloadOffset <= OFFSET_MASK);
    assert(kind == NumBlockKind::FOR || bits <= MAX_PACKED_BITS);
    offsetAndBits = payloadOffset | ((uint64_t)bits << OFFSET_BITS)
        | ((uint64_t)kind << KIND_SHIFT);
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

  static int64_t blockCount(int64_t numValues) {
    return numValues / BLOCK_SIZE + (numValues % BLOCK_SIZE != 0);
  }

  static bool validBlockBytes(uint64_t bytes, int64_t numValues) noexcept {
    // Raw blocks use eight bytes per value plus alignment padding.
    return bytes <= (__uint128_t)numValues * sizeof(int64_t)
        + (__uint128_t)blockCount(numValues) * (MAX_ALIGN - 1);
  }

  // Absolute start of a numeric column's index, after any alignment padding.
  static uint64_t indexOffset(uint64_t blocksEnd) noexcept {
    return alignUp(blocksEnd, MAX_ALIGN);
  }

  // Length of the padded values extent before the monotonic metadata suffix.
  static std::optional<uint64_t> monotonicSplit(uint64_t offset, uint64_t bytes, int64_t numValues) noexcept {
    if (numValues < 0 || bytes > UINT64_MAX - offset) return {};
    uint64_t metadataBytes = (uint64_t)blockCount(numValues) * sizeof(NumBlockInfo);
    if (metadataBytes > bytes) return {};
    auto valuesBytes = bytes - metadataBytes;
    // The metadata start is aligned, including for a zero-value index.
    if ((offset + valuesBytes) % MAX_ALIGN || !validBlockBytes(valuesBytes, numValues)) return {};
    return valuesBytes;
  }

  static int64_t leafOffset(int64_t numBlocks) {
    return numBlocks * (int64_t)(sizeof(NumBlockInfo) + sizeof(NumBlockZone));
  }

  struct RankBounds {
    // Ranks outside [checkBegin, checkEnd) cannot match; those in
    // [insideBegin, insideEnd) all match. Ranks are relative to the block.
    uint32_t checkBegin, checkEnd;
    uint32_t insideBegin, insideEnd;
  };

  struct ResidualBounds {
    // Inclusive; lower > upper represents an empty interval.
    uint32_t lower;
    uint32_t upper;

    static constexpr ResidualBounds empty() { return {1, 0}; }
  };

  // How a column's int64 values read as numbers, for the ALP kinds: FLOAT
  // and DOUBLE columns hold sortable bits (NumericUtils.h), FLOAT ones
  // sign-extended from int32.
  enum class ValueDomain : uint8_t { INT, FLOAT, DOUBLE };

  // Which block kinds a writer weighs beyond FOR (NumBlockPlanner). Off by
  // default: internal point-read columns (monotonic sidecars, an OrdMap's
  // global columns) stay FOR, and field value columns opt in.
  struct BlockCodecs {
    bool enabled = false;
    ValueDomain domain = ValueDomain::INT;
  };

  struct BlockPlan {
    NumBlockInfo info;
    NumBlockZone zone;
    // Exact per-512-value bounds, computed in the same extrema pass before
    // the RAW/packed choice - RAW blocks carry ordinary leaf zones.
    NumBlockZone leafZones[LEAVES_PER_BLOCK];
    uint32_t leafCount = 0;
    LinearFit::Plan fit;
    uint64_t count = 0;
    // Held outside info until the writer knows the payload offset; the
    // three share a word on disk.
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
    auto linear = LinearFit::planBestLinearFit(q, SLOPE_SHIFT);
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

  // Leaf visit orders. A field sort over the column visits leaves best bound
  // first, and that bound is a pure function of the leaf zones and the sort
  // direction - never of the query - so a reader sorts once per column
  // (LeafMeta) instead of every query rederiving the order. `out`
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

  // Value sketch, persisted after the leaf zones: points (rank,
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
  // Columns under this many values do not persist their sketch, nor do
  // BOOLEAN ones (two values): a reader derives one from the leaf zones
  // (zoneSketch), which costs nothing on disk and is near exact for so few
  // leaves. The persisted count reads SKETCH_FROM_ZONES for them, and 0 for
  // a column that has no sketch at all.
  static constexpr int64_t SKETCH_MIN_VALUES = 65536;
  static constexpr uint32_t SKETCH_FROM_ZONES = UINT32_MAX;

  // Whether a column keeps a value sketch, and where it comes from.
  enum class SketchMode : uint8_t { NONE, FROM_ZONES, PERSIST };

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

  // A value sketch from leaf zones alone, for a column that persists none
  // (SKETCH_FROM_ZONES): each leaf holds a value at either end of its zone
  // and the rest spread evenly between (all of them at one value where the
  // zone is one). The column's expected
  // count below a value is then piecewise linear between the zones' ends,
  // and those ends become the points, each at the expected count below
  // it. Coarser than a persisted sketch, but monotone with the exact
  // extremes, which is all ValueSketch needs.
  static std::vector<SketchPoint> zoneSketch(std::span<const NumBlockZone> leaves,
                                             int64_t population) {
    std::vector<SketchPoint> points;
    if (population == 0) return points;
    struct Event {
      int64_t at;
      double slope;  // change in values per unit of value from here on
      double mass;   // values that sit exactly here
    };
    std::vector<Event> events;
    events.reserve(2 * leaves.size());
    int64_t leafCount = (int64_t)leaves.size();
    for (int64_t l = 0; l < leafCount; l++) {
      NumBlockZone zone = leaves[(size_t)l];
      double n = (double)std::min<int64_t>(LEAF_ZONE_SIZE,
                                           population - l * LEAF_ZONE_SIZE);
      if (zone.min == zone.max) {
        events.push_back({zone.min, 0, n});
        continue;
      }
      // The zone's ends are values of the leaf: one at each, the rest
      // spread between, which keeps the sketch's extreme ranks near exact.
      double density = (n - 2) / (double)((uint64_t)zone.max - (uint64_t)zone.min);
      events.push_back({zone.min, density, 1});
      events.push_back({zone.max, -density, 1});
    }
    std::sort(events.begin(), events.end(),
              [](const Event& a, const Event& b) { return a.at < b.at; });
    double below = 0;
    double slope = 0;
    for (size_t i = 0; i < events.size();) {
      int64_t at = events[i].at;
      if (i > 0) below += slope * (double)((uint64_t)at - (uint64_t)events[i - 1].at);
      int64_t rank = std::clamp<int64_t>((int64_t)std::llround(below), 0,
                                         population - 1);
      if (points.empty() || rank > points.back().rank) points.push_back({rank, at});
      double mass = 0;
      for (; i < events.size() && events[i].at == at; i++) {
        slope += events[i].slope;
        mass += events[i].mass;
      }
      // Values sitting here hold every rank they cover.
      int64_t last = std::clamp<int64_t>((int64_t)std::llround(below + mass - 1), 0,
                                         population - 1);
      if (mass >= 2 && last > points.back().rank) points.push_back({last, at});
      below += mass;
    }
    // The last point holds the maximum at the last rank.
    int64_t max = events.back().at;
    if (points.back().rank == population - 1) points.back().value = max;
    else points.push_back({population - 1, max});
    return points;
  }

  // What a reader derives from a column's leaf zones, held by whoever keeps
  // the view (the PostingsReader field cache): the two leaf visit orders
  // (leafVisitOrder), ascending-min then descending-max, and the value
  // sketch of a column that left it to the reader (zoneSketch).
  struct LeafDerived {
    std::vector<uint32_t> orders;
    std::vector<SketchPoint> points;

    size_t bytes() const {
      return sizeof(*this) + orders.capacity() * sizeof(uint32_t)
          + points.capacity() * sizeof(SketchPoint);
    }
  };

  // Read view of a column's leaf metadata as NumColumnWriter::finish lays
  // it out after the block zones: the leaf zones, then the value sketch.
  // The leaf visit orders are not persisted: they are a pure function of
  // the zones, which derive() sorts once into storage its caller keeps (the
  // PostingsReader field cache does, per numeric column). A view without
  // them answers summarizeRange by scanning the zones instead and has no
  // visit order; one without a derived zone sketch reports none. Copyable
  // and trivially destructible, so a planner can keep one per segment;
  // valid while the column's bytes and the derived storage are.
  class LeafMeta {
    const char* zones = nullptr;  // NumBlockZones
    int64_t count = 0;
    int64_t values = 0;
    ValueSketch sketch;
    const LeafDerived* derived = nullptr;
    bool sketchFromZones = false;

    int64_t orderAt(int64_t order, int64_t rank) const {
      assert(rank >= 0 && rank < count);
      // A reader built without the field cache has no orders; a caller
      // must use one that derived them (hasVisitOrders).
      if (derived == nullptr) throw std::logic_error("leaf visit orders were not derived");
      return derived->orders[(size_t)(order * count + rank)];
    }

    // Leaves whose zone satisfies `keep`, by scanning.
    template <class Keep>
    int64_t countLeaves(Keep keep) const {
      int64_t n = 0;
      for (int64_t leaf = 0; leaf < count; leaf++) n += keep(leafZone(leaf));
      return n;
    }

  public:
    static int64_t sketchOffset(int64_t leafCount) {
      return leafCount * (int64_t)sizeof(NumBlockZone);
    }

    static int64_t sketchPointsBytes(uint32_t points) {
      return points * (int64_t)sizeof(SketchPoint);
    }

    static int64_t leafCount(int64_t numValues) {
      return numValues / LEAF_ZONE_SIZE + (numValues % LEAF_ZONE_SIZE != 0);
    }

    static uint64_t minimumIndexBytes(int64_t numValues) {
      // A nonempty index ends with a uint32 sketch count and its points.
      return leafOffset(blockCount(numValues)) + sketchOffset(leafCount(numValues))
          + (numValues ? sizeof(uint32_t) : 0);
    }

    static bool validIndexSize(uint64_t bytes, int64_t numValues) {
      auto minimum = minimumIndexBytes(numValues);
      if (bytes < minimum) return false;
      auto sketchBytes = bytes - minimum;
      return sketchBytes % sizeof(SketchPoint) == 0 && sketchBytes / sizeof(SketchPoint) <= (uint64_t)numValues;
    }

    // Populations of value ranks, not documents. The OUTSIDE population is
    // exact at leaf granularity; bounded intervals can leave INSIDE uncertain.
    struct RangeSummary {
      int64_t outside = 0;
      int64_t insideLower = 0;
      int64_t insideUpper = 0;
    };

    LeafMeta() = default;

    // The index contains block metadata, block zones and leaf metadata.
    static std::expected<LeafMeta, const char*> decode(std::span<const char> index, int64_t numValues);

    // Sorts the visit orders and derives any zone sketch into `into`, which
    // must outlive this view and its copies.
    void derive(LeafDerived& into) {
      std::vector<NumBlockZone> copy((size_t)count);
      if (count > 0) memcpy(copy.data(), zones, (size_t)count * sizeof(NumBlockZone));
      into.orders.resize((size_t)(2 * count));
      std::span<uint32_t> all(into.orders);
      leafVisitOrder(copy, false, all.first((size_t)count));
      leafVisitOrder(copy, true, all.last((size_t)count));
      if (sketchFromZones) {
        into.points = zoneSketch(copy, values);
        sketch = ValueSketch((const char*)into.points.data(),
                             (int64_t)into.points.size(), values);
      }
      derived = &into;
    }

    bool hasVisitOrders() const { return derived != nullptr || count == 0; }

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

    RangeSummary summarizeRange(int64_t lo, int64_t hi) const {
      assert(lo <= hi);
      if (count == 0) return {};
      // Independent boundary searches, not forward traversal of leaf ranks.
      auto prefixCount = [&](auto predicate) {
        int64_t begin = 0, end = count;
        while (begin < end) {
          int64_t mid = begin + (end - begin) / 2;
          if (predicate(mid)) begin = mid + 1;
          else end = mid;
        }
        return begin;
      };
      NumBlockZone tail = leafZone(count - 1);
      int64_t padding = count * LEAF_ZONE_SIZE - values;
      auto population = [&](int64_t leaves, bool includesTail) {
        return leaves * LEAF_ZONE_SIZE - (includesTail ? padding : 0);
      };
      // Leaves with max < x, min > x, max <= x and min >= x: suffixes of
      // the visit orders, or a scan without them.
      auto maxBelow = [&](int64_t x) {
        if (!derived) return countLeaves([&](NumBlockZone z) { return z.max < x; });
        return count - prefixCount([&](int64_t rank) {
          return leafZone(leafByMaxDescending(rank)).max >= x;
        });
      };
      auto minAbove = [&](int64_t x) {
        if (!derived) return countLeaves([&](NumBlockZone z) { return z.min > x; });
        return count - prefixCount([&](int64_t rank) {
          return leafZone(leafByMinAscending(rank)).min <= x;
        });
      };
      auto maxAtMost = [&](int64_t x) {
        if (!derived) return countLeaves([&](NumBlockZone z) { return z.max <= x; });
        return count - prefixCount([&](int64_t rank) {
          return leafZone(leafByMaxDescending(rank)).max > x;
        });
      };
      auto minAtLeast = [&](int64_t x) {
        if (!derived) return countLeaves([&](NumBlockZone z) { return z.min >= x; });
        return count - prefixCount([&](int64_t rank) {
          return leafZone(leafByMinAscending(rank)).min < x;
        });
      };
      int64_t below = population(maxBelow(lo), tail.max < lo);
      int64_t above = population(minAbove(hi), tail.min > hi);
      int64_t maxInside = population(maxAtMost(hi), tail.max <= hi);
      int64_t minInside = population(minAtLeast(lo), tail.min >= lo);
      return {below + above,
          std::max<int64_t>(0, maxInside + minInside - values),
          std::min(maxInside - below, minInside - above)};
    }

    // Empty for a column without values or written without a sketch, and
    // for one that left it to the reader until derive().
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

  // Values lie in [L(rank), L(rank) + gcd * (2^bits - 1)] with L monotonic.
  // Partitioning those envelopes gives conservative check and inside runs.
  // Recover the signed intercept from rank zero: planBlock chooses a linear
  // fit only when it saves a bit, so gcd * residual0 fits uint64 and equals
  // (value0 - baseBits) mod 2^64 even when the persisted base wraps.
  static std::optional<RankBounds> linearRankBounds(
      const NumBlockInfo& block, int64_t value0, uint32_t count,
      int64_t lo, int64_t hi) {
    assert(count > 0 && count <= BLOCK_SIZE);
    // The envelope math reads bits/gcd/scaledSlope as a FOR fit; the other
    // kinds give those fields other meanings.
    if (!block.linearFit()) return std::nullopt;
    assert(block.kind() == NumBlockKind::FOR);
    if (hi < lo) return RankBounds{0, 0, 0, 0};
    uint64_t gcd = block.gcd;
    assert(gcd != 0);
    __int128 spread = (__int128)gcd * (__int128)((1ULL << block.bits()) - 1);
    if (spread > (__int128)UINT64_MAX) return std::nullopt;
    uint64_t scaledResidual0 = (uint64_t)value0 - block.baseBits;
    assert(scaledResidual0 % gcd == 0);
    __int128 intercept = (__int128)value0 - (__int128)scaledResidual0;
    int64_t slope = block.scaledSlope;
    auto lower = [&](uint32_t rank) {
      return intercept + (__int128)gcd * (__int128)slopeTerm(rank, slope);
    };
    auto upper = [&](uint32_t rank) { return lower(rank) + spread; };
    // First rank where pred stops holding; pred must hold on a prefix.
    auto partition = [count](auto pred) {
      uint32_t first = 0;
      uint32_t last = count;
      while (first < last) {
        uint32_t mid = first + (last - first) / 2;
        if (pred(mid)) first = mid + 1;
        else last = mid;
      }
      return first;
    };
    RankBounds bounds;
    if (slope > 0) {
      bounds.checkBegin = partition([&](uint32_t r) { return upper(r) < lo; });
      bounds.checkEnd = partition([&](uint32_t r) { return lower(r) <= hi; });
      bounds.insideBegin = partition([&](uint32_t r) { return lower(r) < lo; });
      bounds.insideEnd = partition([&](uint32_t r) { return upper(r) <= hi; });
    } else {
      bounds.checkBegin = partition([&](uint32_t r) { return lower(r) > hi; });
      bounds.checkEnd = partition([&](uint32_t r) { return upper(r) >= lo; });
      bounds.insideBegin = partition([&](uint32_t r) { return upper(r) > hi; });
      bounds.insideEnd = partition([&](uint32_t r) { return lower(r) >= lo; });
    }
    bounds.checkEnd = std::max(bounds.checkBegin, bounds.checkEnd);
    bounds.insideBegin = std::clamp(
        bounds.insideBegin, bounds.checkBegin, bounds.checkEnd);
    bounds.insideEnd = std::clamp(
        bounds.insideEnd, bounds.insideBegin, bounds.checkEnd);
    return bounds;
  }

  // Constant FOR blocks reconstruct as signed base + gcd * residual. Unsigned
  // distances cover the full INT64 span; quotient+remainder gives ceil without
  // overflow. Clip before narrowing so empty lattice intervals stay empty.
  static ResidualBounds residualBounds(
      const NumBlockInfo& block, int64_t lo, int64_t hi) {
    assert(block.residualComparable() && block.kind() == NumBlockKind::FOR);
    return latticeBounds(block.baseBits, block.gcd, block.bits(), lo, hi);
  }

  // The slots s < 2^bits with base + gcd * s in [lo, hi], for a block that
  // reconstructs that way: constant FOR, and FOR_PATCHED off its
  // exceptions.
  static ResidualBounds latticeBounds(uint64_t baseBits, uint64_t gcd,
                                      uint8_t bits, int64_t lo, int64_t hi) {
    assert(gcd != 0 && bits <= 32);
    int64_t base = (int64_t)baseBits;
    if (hi < lo || hi < base) return ResidualBounds::empty();
    uint64_t lower = 0;
    if (lo > base) {
      uint64_t delta = (uint64_t)lo - (uint64_t)base;
      lower = delta / gcd + (delta % gcd != 0);
    }
    uint64_t upper = std::min<uint64_t>(
        ((uint64_t)hi - (uint64_t)base) / gcd, (1ULL << bits) - 1);
    if (lower > upper) return ResidualBounds::empty();
    return {(uint32_t)lower, (uint32_t)upper};
  }

  // ALP (Afroozeh, Kuffo, Boncz, SIGMOD 2024): a decimal-origin value v is
  // stored as the integer n = round(v * 10^e / 10^f) and read back as
  // n * 10^f / 10^e. The writer keeps n only when that read reproduces the
  // stored bits exactly, through this same function, so the encoding is
  // lossless by construction; any other value is an exception. ALP itself
  // multiplies by an inexact 10^-e; dividing by the exact 10^e instead
  // costs a division per value but makes the read the correctly rounded
  // quotient, which is what a parser produced for a decimal with at most
  // e - f fraction digits, so such values never become exceptions. Floats
  // decode through double and round once more, which is innocuous for a
  // quotient (53 >= 2 * 24 + 2 bits). The result is nondecreasing in n
  // (each step rounds monotonically), which is what lets range queries map
  // bounds to an interval of n. Never NaN, so the sortable flip needs no
  // NaN canonicalization and the loop stays branch free.
  static constexpr uint8_t ALP_MAX_EXPONENT_DOUBLE = 18;
  static constexpr uint8_t ALP_MAX_EXPONENT_FLOAT = 10;
  static constexpr double ALP_POW10[] = {
      1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9,
      1e10, 1e11, 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18};

  // Encoded integers stay within +-ALP_MAX_SCALED: inside the double
  // mantissa, so round() is exact, and inside the range where alpNumber
  // converts exactly.
  static constexpr double ALP_MAX_SCALED = 1125899906842624.0;  // 2^50

  // (double)n for |n| <= 2^51 without the int64 conversion, which AVX2 has
  // no instruction for: n added to the bits of 1.5 * 2^52 is the double
  // 1.5 * 2^52 + n exactly (unit ulp there), so subtracting 1.5 * 2^52
  // leaves n. This is what lets the frame decode loop vectorize.
  static LUXIR_INLINE double alpNumber(int64_t n) {
    constexpr double MAGIC = 6755399441055744.0;  // 1.5 * 2^52
    return std::bit_cast<double>(std::bit_cast<int64_t>(MAGIC) + n) - MAGIC;
  }

  static LUXIR_INLINE int64_t alpDecodeDouble(double n, double factor,
                                              double exponent) {
    double value = n * factor / exponent;
    return sortableDoubleBits(std::bit_cast<int64_t>(value));
  }

  static LUXIR_INLINE int64_t alpDecodeFloat(double n, double factor,
                                             double exponent) {
    float value = (float)(n * factor / exponent);
    return (int64_t)sortableFloatBits(std::bit_cast<int32_t>(value));
  }

  // The value of n for every n, encoded or not: the slot bound search
  // probes integers past the encoded range, where alpNumber would not be
  // exact. On encoded integers it agrees with the frame decode bit for bit,
  // both converting n exactly.
  static int64_t alpDecode(int64_t n, uint8_t e, uint8_t f, NumBlockKind kind) {
    assert(kind == NumBlockKind::ALP_DOUBLE || kind == NumBlockKind::ALP_FLOAT);
    return kind == NumBlockKind::ALP_FLOAT
        ? alpDecodeFloat((double)n, ALP_POW10[f], ALP_POW10[e])
        : alpDecodeDouble((double)n, ALP_POW10[f], ALP_POW10[e]);
  }
};

inline std::expected<NumColumnFormat::LeafMeta, const char*> NumColumnFormat::LeafMeta::decode(
    std::span<const char> index, int64_t numValues) {
  LeafMeta result;
  result.count = leafCount(numValues);
  result.values = numValues;
  if (result.count == 0) return result;
  auto leafStart = (uint64_t)leafOffset(blockCount(numValues));
  auto sketchStart = leafStart + sketchOffset(result.count);
  if (sketchStart > index.size() || index.size() - sketchStart < sizeof(uint32_t)) {
    return std::unexpected("truncated numeric index");
  }
  uint32_t points;
  memcpy(&points, index.data() + sketchStart, sizeof(points));
  if (points == SKETCH_FROM_ZONES) {
    result.sketchFromZones = true;
    points = 0;
  }
  if ((uint64_t)points > (uint64_t)numValues
      || index.size() - sketchStart - sizeof(points) != (uint64_t)sketchPointsBytes(points)) {
    return std::unexpected("invalid numeric sketch size");
  }
  result.zones = index.data() + leafStart;
  if (points) result.sketch = ValueSketch(index.data() + sketchStart + sizeof(points), points, numValues);
  return result;
}

} // namespace luxir
