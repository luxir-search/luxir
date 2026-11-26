// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// Shared RaBitQ kernel body. Included inside each level's namespace and
// target region, which supply the chunk layout, ChunkOps and minMaxPrefix.

inline LUXIR_INLINE uint64_t load64(const uint8_t* p) {
  uint64_t v;
  std::memcpy(&v, p, sizeof(v));
  return v;
}

// Query values are finite. SIMD min/max may choose a different signed zero,
// which only changes the sign of a zero c2 and leaves estimates unchanged.
inline LUXIR_INLINE void minMax(const float* values, size_t n, float& low, float& high) {
  low = high = values[0];
  for (size_t i = minMaxPrefix(values, n, low, high); i < n; i++) {
    low = values[i] < low ? values[i] : low;
    high = high < values[i] ? values[i] : high;
  }
}

// Scorer definition: round (residual - low) / delta half up and clamp to the
// query-code range. Constant residuals map to zero (0 * inf becomes NaN, then
// the first comparison selects zero). The half test has no rounding of its own.
// Auto-vectorized under the enclosing level's target.
LUXIR_NOINLINE uint32_t quantize(const float* LUXIR_RESTRICT residual, size_t n, float low, float scale,
                                  float maxCode, uint8_t* LUXIR_RESTRICT out) {
  uint32_t sum = 0;
  for (size_t i = 0; i < n; i++) {
    float x = (residual[i] - low) * scale;
    x = x >= 0 ? x : 0;
    x = x < maxCode ? x : maxCode;
    int32_t truncated = (int32_t)x;
    int32_t code = truncated + (x - (float)truncated >= 0.5f ? 1 : 0);
    out[i] = (uint8_t)code;
    sum += (uint32_t)code;
  }
  return sum;
}

// Transposes 8 bytes and 8 bit planes: on return byte p holds bit p of each
// input byte, the input's first byte in the lowest bit.
inline LUXIR_INLINE uint64_t transposeBits(uint64_t x) {
  uint64_t t = (x ^ (x >> 7)) & 0x00AA00AA00AA00AAULL;
  x ^= t ^ (t << 7);
  t = (x ^ (x >> 14)) & 0x0000CCCC0000CCCCULL;
  x ^= t ^ (t << 14);
  t = (x ^ (x >> 28)) & 0x00000000F0F0F0F0ULL;
  return x ^ t ^ (t << 28);
}

// FAISS packs one bit at a time; the word path here takes eight dimensions
// at a time and vector packers take 32 or 64.
template <typename Ops>
void packPlanes(const State& state) {
  const uint8_t* in = state.quantized;
  uint8_t* out = state.planes;
  size_t qb = (size_t)state.queryBits;
  auto packUnit = [&](size_t unit, size_t dimsLeft) {
    for (size_t byte = 0; byte < unit; byte++, in += 8) {
      if (byte * 8 >= dimsLeft) {
        for (size_t p = 0; p < qb; p++) out[p * unit + byte] = 0;
        continue;
      }
      uint64_t x = transposeBits(load64(in));
      for (size_t p = 0; p < qb; p++, x >>= 8) out[p * unit + byte] = (uint8_t)x;
    }
    out += qb * unit;
  };
  size_t left = state.dims;
  if constexpr (CHUNK_BYTES != 0) {
    for (size_t c = 0; c < state.chunks; c++, left = left > CHUNK_BYTES * 8 ? left - CHUNK_BYTES * 8 : 0) {
      if constexpr (requires { Ops::packChunk(in, out, qb, left); }) {
        Ops::packChunk(in, out, qb, left);
      } else {
        packUnit(CHUNK_BYTES, left);
      }
    }
  }
  for (size_t w = 0; w < state.tailWords; w++, left = left > 64 ? left - 64 : 0) packUnit(8, left);
}

void setList(State& state, const float* centroid) {
  const size_t dims = state.dims;
  auto& factors = state.factors;
  factors.queryToCentroidSqr = faiss::fvec_L2sqr(state.query, centroid, dims);
  state.gError = std::sqrt(factors.queryToCentroidSqr);
  float* LUXIR_RESTRICT r = state.residual;
  const float* LUXIR_RESTRICT q = state.query;
  const float* LUXIR_RESTRICT c = centroid;
  for (size_t i = 0; i < dims; i++) r[i] = q[i] - c[i];
  float low, high;
  minMax(r, dims, low, high);
  float delta = (high - low) / state.maxCode;
  uint32_t sum = quantize(r, dims, low, 1.0f / delta, state.maxCode, state.quantized);
  packPlanes<ChunkOps>(state);
  factors.c1 = 2 * delta * state.invSqrtDims;
  factors.c2 = 2 * low * state.invSqrtDims;
  factors.c34 = state.invSqrtDims * (delta * (float)sum + (float)dims * low);
}

template <int QB>
inline LUXIR_INLINE Counts countCode(const State& state, const uint8_t* code, const ChunkOps& ops) {
  size_t codeBytes = state.codeBytes, chunks = state.chunks, tailWords = state.tailWords;
  const uint8_t* planes = state.planes;
  uint64_t dot = 0, sum = 0;
  if constexpr (CHUNK_BYTES != 0) {
    if (chunks != 0) ops.score<QB>(code, planes, chunks, dot, sum);
  }
  if constexpr (!MASKED_CHUNKS) {
    // Only the last word can overlap the factor bytes. Planes are zero there.
    const uint64_t lastMask = codeBytes % 8 == 0 ? ~(uint64_t)0 : ((uint64_t)1 << (codeBytes % 8 * 8)) - 1;
    const uint8_t* plane = planes + chunks * CHUNK_BYTES * QB;
    for (size_t w = 0; w < tailWords; w++, plane += QB * sizeof(uint64_t)) {
      uint64_t row = load64(code + chunks * CHUNK_BYTES + w * sizeof(uint64_t));
      for (int p = 0; p < QB; p++)
        dot += (uint64_t)std::popcount(row & load64(plane + (size_t)p * sizeof(uint64_t))) << p;
      sum += (uint64_t)std::popcount(w + 1 == tailWords ? row & lastMask : row);
    }
  }
  return {dot, sum};
}

Counts counts(const State& state, const uint8_t* code) {
  const ChunkOps ops(state.codeBytes);
  switch (state.queryBits) {
    case 1: return countCode<1>(state, code, ops);
    case 2: return countCode<2>(state, code, ops);
    case 3: return countCode<3>(state, code, ops);
    case 4: return countCode<4>(state, code, ops);
    case 5: return countCode<5>(state, code, ops);
    case 6: return countCode<6>(state, code, ops);
    case 7: return countCode<7>(state, code, ops);
    default: return countCode<8>(state, code, ops);
  }
}

template <int QB, bool IP, bool Selected>
LUXIR_NOINLINE void scoreBlock(const State& state, const uint8_t* codes, size_t codeSize, const uint16_t* rows,
                               size_t count, float* LUXIR_RESTRICT out) {
  const size_t binary = state.codeBytes;
  const ChunkOps ops(binary);
  const auto& factors = state.factors;
  const float c1 = factors.c1, c2 = factors.c2, c34 = factors.c34;
  const float queryToCentroid = factors.queryToCentroidSqr, queryNorm = factors.queryNormSqr;
  for (size_t i = 0; i < count; i++) {
    const uint8_t* code = codes + (Selected ? (size_t)rows[i] : i) * codeSize;
    // Filtered rows skip unevenly, out of the hardware prefetcher's reach.
    if constexpr (Selected) {
      if (i + PREFETCH_ROWS < count) {
        const uint8_t* next = codes + (size_t)rows[i + PREFETCH_ROWS] * codeSize;
        __builtin_prefetch(next);
        __builtin_prefetch(next + codeSize - 1);
      }
    }
    auto [dot, sum] = countCode<QB>(state, code, ops);
    float orMinusC, dpMultiplier;
    std::memcpy(&orMinusC, code + binary, sizeof(float));
    std::memcpy(&dpMultiplier, code + binary + sizeof(float), sizeof(float));
    // Through int64: unsigned 64-bit to float is slow without AVX-512.
    float finalDot = c1 * (float)(int64_t)dot + c2 * (float)(int64_t)sum - c34;
    float preDist = orMinusC + queryToCentroid - 2 * dpMultiplier * finalDot;
    out[i] = IP ? -0.5f * (preDist - queryNorm) : preDist;
  }
}

template <bool Selected>
void dispatch(const State& state, const uint8_t* codes, size_t codeSize, const uint16_t* rows,
              size_t count, float* out) {
  auto run = [&]<int QB>() {
    if (state.innerProduct) scoreBlock<QB, true, Selected>(state, codes, codeSize, rows, count, out);
    else scoreBlock<QB, false, Selected>(state, codes, codeSize, rows, count, out);
  };
  switch (state.queryBits) {
    case 1: run.template operator()<1>(); break;
    case 2: run.template operator()<2>(); break;
    case 3: run.template operator()<3>(); break;
    case 4: run.template operator()<4>(); break;
    case 5: run.template operator()<5>(); break;
    case 6: run.template operator()<6>(); break;
    case 7: run.template operator()<7>(); break;
    default: run.template operator()<8>(); break;
  }
}

constexpr Kernels kernels = {LEVEL, CHUNK_BYTES, MASKED_CHUNKS, setList, counts, dispatch<false>, dispatch<true>};
