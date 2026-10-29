// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// SIMD fast-scan of block-packed 4-bit IVF+PQ lists (ENGINE_IVFPQ4FS), inner
// product / cosine only.  FAISS supplies the kernels (pq4_pack_LUT,
// accumulate_to_mem); thresholding, selectors and the result heaps stay with
// the caller.
//
// Residual IP: score(x) = <q, c> + sum_m <q_m, r_m[code_m]>.  The second term
// is a per-query lookup table shared by every list; <q, c> is the coarse
// score the caller already holds per list.  The table is negated into
// distance form, shifted to a per-sub-quantizer minimum and scaled to uint8
// so the kernel's uint16 sums cannot overflow:
//   score ~= coarse - sum_m lo_m - acc / scale

#pragma once

#include <faiss/IndexIVFPQ.h>
#include <faiss/impl/IDSelector.h>
#include <faiss/impl/ProductQuantizer.h>
#include <faiss/impl/fast_scan/fast_scan.h>
#include <faiss/utils/AlignedTable.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "luxir/reader/VectorAuxReader.h"

namespace luxir {

class Pq4Scanner {
  size_t nsq;
  faiss::AlignedTable<uint8_t> lut;  // pq4_pack_LUT output, nsq * 16 bytes; kernels assert 32-alignment
  float scale = 1.0f;
  float loSum = 0.0f;
  std::vector<uint16_t> acc;      // per-list kernel output scratch

public:
  Pq4Scanner(const faiss::ProductQuantizer& pq, const float* query)
    : nsq(VectorAuxListsFooter::packedNsq(pq.M)), lut(nsq * 16) {
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
    // M / 2 of headroom for rounding up.
    if (sumRange > 0) {
      scale = std::min(255.0f / maxRange, (65535.0f - (float)m) / sumRange);
    }
    std::vector<uint8_t> src(nsq * 16, 0);
    for (size_t i = 0; i < m; i++) {
      for (size_t k = 0; k < 16; k++) {
        float q = std::nearbyint((ip[i * 16 + k] - lo[i]) * scale);
        src[i * 16 + k] = (uint8_t)std::clamp(q, 0.0f, 255.0f);
      }
    }
    faiss::pq4_pack_LUT(1, (int)nsq, src.data(), lut.get());
  }

  /// Scan one block-packed list of n codes.  coarse is the query's score
  /// against the list centroid.  sink.threshold() is the score a hit must
  /// beat (-inf when unbounded); sink.add(score, id) offers a hit.  Hits the
  /// selector rejects are skipped before the sink sees them.
  template <class Sink>
  void scanList(const uint8_t* blocks, size_t n, const faiss::idx_t* ids, float coarse,
                const faiss::IDSelector* sel, Sink& sink) {
    if (n == 0) return;
    size_t n2 = (n + VectorAuxListsFooter::PACK_BLOCK - 1)
      / VectorAuxListsFooter::PACK_BLOCK * VectorAuxListsFooter::PACK_BLOCK;
    float base = coarse - loSum;
    uint32_t cut = cutoff(base, sink.threshold());
    if (cut == NONE) return;
    acc.resize(n2);
    faiss::accumulate_to_mem(1, n2, (int)nsq, blocks, lut.get(), acc.data());
    for (size_t i = 0; i < n; i++) {
      if (acc[i] > cut) continue;
      faiss::idx_t id = ids[i];
      if (sel != nullptr && !sel->is_member(id)) continue;
      if (sink.add(base - (float)acc[i] / scale, id)) {
        cut = cutoff(base, sink.threshold());
        if (cut == NONE) return;
      }
    }
  }

private:
  static constexpr uint32_t NONE = 0x10000;

  // Largest uint16 sum whose estimated score can still beat thr (inclusive:
  // the estimate is quantized, the sink makes the exact call), or NONE.
  uint32_t cutoff(float base, float thr) const {
    float x = (base - thr) * scale;
    if (!(x >= 0)) return NONE;
    if (x >= 65535.0f) return 65535;
    return (uint32_t)std::ceil(x);
  }
};

}  // namespace luxir
