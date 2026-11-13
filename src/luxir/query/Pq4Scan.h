// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// SIMD fast-scan of block-packed 4-bit IVF+PQ lists (ENGINE_IVFPQ4FS), inner
// product / cosine only. FAISS supplies the kernels (pq4_pack_LUT,
// accumulate_to_mem); thresholds, selectors and results stay with the caller.
//
// Residual IP: score(x) = <q, c> + sum_m <q_m, r_m[code_m]>. The second term is
// a per-query lookup table shared by every list; <q, c> is the coarse score the
// caller already holds per list. The table is negated into distance form,
// shifted to a per-sub-quantizer minimum and scaled to uint8 so the kernel's
// uint16 sums cannot overflow:
//   score ~= coarse - sum_m lo_m - acc / scale

#pragma once

#include <faiss/MetricType.h>
#include <faiss/impl/ProductQuantizer.h>
#include <faiss/impl/fast_scan/fast_scan.h>
#include <faiss/utils/AlignedTable.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <vector>

#include "luxir/reader/VectorAuxReader.h"

namespace luxir {

/// One query's lookup table for one overlay's codebooks. Immutable after
/// construction, so the parallel scan tasks of a segment share it.
class Pq4QueryTable {
  size_t nsq, codeSize;
  faiss::AlignedTable<uint8_t> lut;  // pq4_pack_LUT output; the kernels need 32-alignment
  float scaleFactor = 1.0f;
  float loSum = 0.0f;

public:
  static constexpr uint32_t NONE = 0x10000;  // cutoff(): no sum can beat the threshold

  Pq4QueryTable(const faiss::ProductQuantizer& pq, const float* query)
      : nsq(VectorAuxListsFooter::packedNsq(pq.M)), codeSize(nsq / 2), lut(nsq * 16) {
    size_t m = pq.M;
    std::vector<float> ip(m * 16);
    pq.compute_inner_prod_table(query, ip.data());
    std::vector<float> lo(m);
    float maxRange = 0, sumRange = 0;
    for (size_t i = 0; i < m; i++) {
      float* row = ip.data() + i * 16;
      for (size_t k = 0; k < 16; k++) row[k] = -row[k];
      auto [mn, mx] = std::minmax_element(row, row + 16);
      lo[i] = *mn;
      maxRange = std::max(maxRange, *mx - *mn);
      sumRange += *mx - *mn;
      loSum += *mn;
    }
    // Entries <= 255, and the rounded sum stays under 65535: the scale leaves
    // M / 2 of headroom for rounding up. A zero or denormal range has no usable
    // scale (the quotient is 0/0 or +inf); every entry is then effectively zero.
    float scale = sumRange > 0 ? std::min(255.0f / maxRange, (65535.0f - (float)m) / sumRange) : 1.0f;
    if (std::isfinite(scale) && scale > 0) scaleFactor = scale;
    std::vector<uint8_t> src(nsq * 16, 0);
    for (size_t i = 0; i < m; i++) {
      for (size_t k = 0; k < 16; k++) {
        // NaN fails the first test (a NaN query): casting it would be UB.
        float entry = std::nearbyint((ip[i * 16 + k] - lo[i]) * scaleFactor);
        src[i * 16 + k] = entry >= 0 ? (uint8_t)std::min(entry, 255.0f) : 0;
      }
    }
    faiss::pq4_pack_LUT(1, (int)nsq, src.data(), lut.get());
  }

  float scale() const noexcept { return scaleFactor; }
  size_t blockBytes() const noexcept { return VectorAuxListsFooter::PACK_BLOCK * codeSize; }
  // The list's score upper bound: the estimate of a uint16 sum of zero.
  float listBase(float coarse) const noexcept { return coarse - loSum; }
  float estimate(float base, uint16_t acc) const noexcept { return base - (float)acc / scaleFactor; }

  /// Largest uint16 sum whose estimate can still beat threshold, or NONE. One
  /// step wider than the rounded quotient: the float compare at the handler
  /// makes the exact call, and the prefilter must never disagree with it.
  uint32_t cutoff(float base, float threshold) const noexcept {
    float x = (base - threshold) * scaleFactor;
    if (!(x >= -1.0f)) return NONE;
    if (x >= 65534.0f) return 65535;
    return (uint32_t)(std::ceil(std::max(x, 0.0f)) + 1);
  }

  /// Kernel: sums for count2 codes (a multiple of the block size).
  void accumulate(const uint8_t* blocks, size_t count2, uint16_t* acc) const {
    faiss::accumulate_to_mem(1, count2, (int)nsq, blocks, lut.get(), acc);
  }
};

/// One scan task's scratch and work counters. Scans a list in chunks of CHUNK
/// codes. Before each chunk it refreshes the handler's threshold, so the shared
/// floor from other segments and tasks prunes pq4 like every other codec.
///
/// Handler needs: a float `threshold` (estimates must beat it),
/// updateThreshold(), and add_result(estimate, id).
class Pq4ListScan {
  static constexpr size_t CHUNK = 256;  // 8 blocks
  static_assert(CHUNK % VectorAuxListsFooter::PACK_BLOCK == 0);
  const Pq4QueryTable& table;
  alignas(64) uint16_t acc[CHUNK];
  uint16_t eligible[CHUNK];

public:
  int64_t passes = 0;       // codes the probe admitted, kernel run or not
  int64_t kernelCodes = 0;  // codes through the kernel, padding lanes included

  explicit Pq4ListScan(const Pq4QueryTable& table) : table(table) {}

  template <class Probe, class Handler>
  void scanList(const uint8_t* blocks, size_t count, const faiss::idx_t* ids, float coarse,
                Probe probe, Handler& handler) {
    constexpr bool filtered = !std::is_same_v<Probe, std::nullptr_t>;
    constexpr size_t BLOCK = VectorAuxListsFooter::PACK_BLOCK;
    float base = table.listBase(coarse);
    for (size_t begin = 0; begin < count; begin += CHUNK) {
      size_t n = std::min(CHUNK, count - begin);
      handler.updateThreshold();
      size_t admitted = n;
      if constexpr (filtered) {
        // Branchless: pass rates near one half must not cost a misprediction
        // per code. A chunk with no eligible code skips the kernel.
        admitted = 0;
        for (size_t i = 0; i < n; i++) {
          eligible[admitted] = (uint16_t)i;
          admitted += probe(ids[begin + i]) ? 1 : 0;
        }
        if (admitted == 0) continue;
      }
      passes += (int64_t)admitted;
      float threshold = handler.threshold;
      uint32_t cut = table.cutoff(base, threshold);
      if (cut == Pq4QueryTable::NONE) continue;  // Selector passes above still count.
      size_t n2 = (n + BLOCK - 1) / BLOCK * BLOCK;
      table.accumulate(blocks + begin / BLOCK * table.blockBytes(), n2, acc);
      kernelCodes += (int64_t)n2;
      auto offer = [&](size_t i) LUXIR_INLINE {
        if (acc[i] > cut) return;
        float estimate = table.estimate(base, acc[i]);
        if (!(estimate > handler.threshold)) return;
        handler.add_result(estimate, ids[begin + i]);
        if (handler.threshold != threshold) {
          threshold = handler.threshold;
          cut = table.cutoff(base, threshold);
        }
      };
      if constexpr (filtered) {
        for (size_t j = 0; j < admitted; j++) offer(eligible[j]);
      } else {
        // Few codes survive once the pool is full: test a block's minimum.
        for (size_t b = 0; b < n; b += BLOCK) {
          uint16_t least = acc[b];
          for (size_t i = b + 1; i < b + BLOCK; i++) least = std::min(least, acc[i]);
          if (least > cut) continue;
          for (size_t i = b, end = std::min(n, b + BLOCK); i < end; i++) offer(i);
        }
      }
    }
  }
};

}  // namespace luxir
