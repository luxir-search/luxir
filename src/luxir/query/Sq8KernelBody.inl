// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// The sq8 kernel loop, included by Sq8Kernel.cpp into each level's namespace
// inside that level's GCC target region, so every function here is compiled
// for the level.  The namespace supplies Vec, Mask, W and the lane operations
// (broadcast, add, sub, fmadd, loadQuery, loadCodes, tailMask, loadTailQuery,
// loadTailCodes, tailFmadd, horizontalSum); Op and ACC come from the file.
template <Op OP>
inline LUXIR_INLINE void step(Vec& sum, Vec& norm, Vec q, Vec x) {
  if constexpr (OP == Op::L2) {
    Vec d = sub(q, x);
    sum = fmadd(d, d, sum);
  } else {
    sum = fmadd(q, x, sum);
    if constexpr (OP == Op::DOT_NORM) norm = fmadd(x, x, norm);
  }
}

template <Op OP>
inline LUXIR_INLINE void fullStep(Vec& sum, Vec& norm, const float* q, const uint8_t* c, Vec offset, Vec scale) {
  step<OP>(sum, norm, loadQuery(q), fmadd(scale, loadCodes(c), offset));
}

inline LUXIR_INLINE float reduce(const Vec* a) {
  return horizontalSum(add(add(a[0], a[1]), add(a[2], a[3])));
}

template <Op OP>
inline LUXIR_INLINE float kernel(const float* q, const uint8_t* c, float offset, float scale,
                                 int32_t n, float* normSq) {
  Vec vo = broadcast(offset);
  Vec vs = broadcast(scale);
  Vec sum[ACC] = {};
  Vec norm[ACC] = {};
  int32_t i = 0;
  for (; n - i >= ACC * W; i += ACC * W) {
    #pragma GCC unroll 4
    for (int32_t j = 0; j < ACC; j++) {
      fullStep<OP>(sum[j], norm[j], q + i + j * W, c + i + j * W, vo, vs);
    }
  }
  for (; n - i >= W; i += W) fullStep<OP>(sum[0], norm[0], q + i, c + i, vo, vs);
  if (i < n) {
    int32_t tail = n - i;
    Mask mask = tailMask(tail);
    // Clear decoded lanes as well as query lanes so padding contributes zero
    // even when the row offset is nonzero. Use a separate chain from full steps.
    Vec x = tailFmadd(vs, loadTailCodes(c + i, tail), vo, mask);
    step<OP>(sum[1], norm[1], loadTailQuery(q + i, mask), x);
  }
  if constexpr (OP == Op::DOT_NORM) *normSq = reduce(norm);
  return reduce(sum);
}

template <Op OP>
float score(const float* q, const uint8_t* codes, float offset, float scale, int32_t n) {
  return kernel<OP>(q, codes, offset, scale, n, nullptr);
}

float dotNorm(const float* q, const uint8_t* codes, float offset, float scale, int32_t n, float& normSq) {
  return kernel<Op::DOT_NORM>(q, codes, offset, scale, n, &normSq);
}
