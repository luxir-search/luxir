// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#include "RaBitQScorer.h"

#include "luxir/util/luxir_util.h"
#include <faiss/utils/distances.h>

#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <utility>

#if defined(__x86_64__) && defined(__GNUC__) && !defined(__clang__)
#define LUXIR_RABITQ_X86
#include <immintrin.h>
#elif defined(__x86_64__) || defined(__SSE4_1__)
#include <xmmintrin.h>
#elif defined(__aarch64__) && defined(__ARM_NEON)
#define LUXIR_RABITQ_NEON
#include <arm_neon.h>
#endif

namespace luxir {
namespace rabitq {

// Plane layout and entry points for one level.
struct Kernels {
  RaBitQScorer::Level level;
  size_t chunkBytes;
  // The last SIMD chunk may be partial, so there are no scalar tail words.
  bool maskedChunks;
  void (*setList)(State&, const float*);
  Counts (*counts)(const State&, const uint8_t*);
  void (*score)(const State&, const uint8_t*, size_t, const uint16_t*, size_t, float*);
  void (*scoreSelected)(const State&, const uint8_t*, size_t, const uint16_t*, size_t, float*);
};

}  // namespace rabitq
namespace {

using Level = RaBitQScorer::Level;
using rabitq::Counts;
using rabitq::Kernels;
using rabitq::State;

constexpr size_t PREFETCH_ROWS = 6;

#ifndef LUXIR_RABITQ_NEON
namespace generic {

constexpr Level LEVEL = Level::GENERIC;
constexpr size_t CHUNK_BYTES = 0;
constexpr bool MASKED_CHUNKS = false;

// The baseline has only word tails. On x86, residual min/max still uses SSE.
#if defined(__x86_64__) || defined(__SSE4_1__)
inline LUXIR_INLINE size_t minMaxPrefix(const float* values, size_t n, float& minValue, float& maxValue) {
  size_t i = 0;
  if (n >= 4) {
    __m128 lo = _mm_loadu_ps(values), hi = lo;
    for (i = 4; i + 4 <= n; i += 4) {
      __m128 v = _mm_loadu_ps(values + i);
      lo = _mm_min_ps(lo, v);
      hi = _mm_max_ps(hi, v);
    }
    lo = _mm_min_ps(lo, _mm_movehl_ps(lo, lo));
    hi = _mm_max_ps(hi, _mm_movehl_ps(hi, hi));
    minValue = _mm_cvtss_f32(_mm_min_ss(lo, _mm_shuffle_ps(lo, lo, 1)));
    maxValue = _mm_cvtss_f32(_mm_max_ss(hi, _mm_shuffle_ps(hi, hi, 1)));
  }
  return i;
}
#else
inline LUXIR_INLINE size_t minMaxPrefix(const float*, size_t, float&, float&) { return 0; }
#endif

// Word kernels have no SIMD chunks to score.
struct ChunkOps {
  LUXIR_INLINE explicit ChunkOps(size_t) {}

  template <int QB>
  LUXIR_INLINE void score(const uint8_t*, const uint8_t*, size_t, uint64_t& dot, uint64_t& sum) const {
    dot = 0;
    sum = 0;
  }
};

#include "RaBitQScorerBody.inl"

}  // namespace generic
#else
namespace neon {

constexpr Level LEVEL = Level::NEON;
constexpr size_t CHUNK_BYTES = 16;
constexpr bool MASKED_CHUNKS = false;

inline LUXIR_INLINE size_t minMaxPrefix(const float* values, size_t n, float& minValue, float& maxValue) {
  size_t i = 0;
  if (n >= 4) {
    float32x4_t lo = vld1q_f32(values), hi = lo;
    for (i = 4; i + 4 <= n; i += 4) {
      float32x4_t v = vld1q_f32(values + i);
      lo = vminq_f32(lo, v);
      hi = vmaxq_f32(hi, v);
    }
    minValue = vminvq_f32(lo);
    maxValue = vmaxvq_f32(hi);
  }
  return i;
}

// NEON popcount for full 16-byte chunks.
struct ChunkOps {
  LUXIR_INLINE explicit ChunkOps(size_t) {}

  static LUXIR_INLINE uint8x16_t horner(uint8x16_t code, const uint8_t* plane, int count) {
    uint8x16_t acc = vdupq_n_u8(0);
    for (int p = count - 1; p >= 0; p--) {
      uint8x16_t bits = vcntq_u8(vandq_u8(code, vld1q_u8(plane + (size_t)p * CHUNK_BYTES)));
      acc = vaddq_u8(vaddq_u8(acc, acc), bits);
    }
    return acc;
  }
  template <int QB>
  LUXIR_INLINE void score(const uint8_t* code, const uint8_t* plane, size_t chunks, uint64_t& dot, uint64_t& sum) const {
    uint32_t low4 = 0, high4 = 0, bits = 0;
    constexpr int lowPlanes = std::min(QB, 4);
    for (size_t c = 0; c < chunks; c++, plane += QB * CHUNK_BYTES) {
      uint8x16_t row = vld1q_u8(code + c * CHUNK_BYTES);
      low4 += vaddlvq_u8(horner(row, plane, lowPlanes));
      if constexpr (QB > 4) high4 += vaddlvq_u8(horner(row, plane + 4 * CHUNK_BYTES, QB - 4));
      bits += vaddlvq_u8(vcntq_u8(row));
    }
    dot = (uint64_t)low4 + ((uint64_t)high4 << 4);
    sum = bits;
  }
};

#include "RaBitQScorerBody.inl"

}  // namespace neon
#endif

#ifdef LUXIR_RABITQ_X86
// The included definitions inherit this level's target and internal linkage.
#pragma GCC push_options
#pragma GCC target("avx2")
namespace avx2 {

constexpr Level LEVEL = Level::AVX2;
constexpr size_t CHUNK_BYTES = 32;
constexpr bool MASKED_CHUNKS = false;

inline LUXIR_INLINE size_t minMaxPrefix(const float* values, size_t n, float& minValue, float& maxValue) {
  size_t i = 0;
  if (n >= 8) {
    __m256 lo = _mm256_loadu_ps(values), hi = lo;
    for (i = 8; i + 8 <= n; i += 8) {
      __m256 v = _mm256_loadu_ps(values + i);
      lo = _mm256_min_ps(lo, v);
      hi = _mm256_max_ps(hi, v);
    }
    __m128 l = _mm_min_ps(_mm256_castps256_ps128(lo), _mm256_extractf128_ps(lo, 1));
    __m128 h = _mm_max_ps(_mm256_castps256_ps128(hi), _mm256_extractf128_ps(hi, 1));
    l = _mm_min_ps(l, _mm_movehl_ps(l, l));
    h = _mm_max_ps(h, _mm_movehl_ps(h, h));
    minValue = _mm_cvtss_f32(_mm_min_ss(l, _mm_shuffle_ps(l, l, 1)));
    maxValue = _mm_cvtss_f32(_mm_max_ss(h, _mm_shuffle_ps(h, h, 1)));
  }
  return i;
}

// Nibble-table popcount for full 32-byte chunks.
struct ChunkOps {
  // Nibble popcount table, repeated in each 128-bit lane.
  __m256i lut = _mm256_setr_epi8(0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4,
                                 0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4);
  // Selects the low nibble of each byte.
  __m256i low = _mm256_set1_epi8(0x0f);

  LUXIR_INLINE explicit ChunkOps(size_t) {}

  LUXIR_INLINE __m256i popBytes(__m256i x) const {
    __m256i lo = _mm256_shuffle_epi8(lut, _mm256_and_si256(x, low));
    __m256i hi = _mm256_shuffle_epi8(lut, _mm256_and_si256(_mm256_srli_epi16(x, 4), low));
    return _mm256_add_epi8(lo, hi);
  }
  static LUXIR_INLINE uint64_t sum64(__m256i x) {
    __m128i s = _mm_add_epi64(_mm256_castsi256_si128(x), _mm256_extracti128_si256(x, 1));
    return (uint64_t)_mm_cvtsi128_si64(_mm_add_epi64(s, _mm_unpackhi_epi64(s, s)));
  }
  // Weighted counts of the planes in bytes, most significant plane first. Four
  // planes of 8 bits weigh at most 8 * 15, which fits.
  LUXIR_INLINE __m256i horner(__m256i code, const __m256i* plane, int count) const {
    __m256i acc = _mm256_setzero_si256();
    for (int p = count - 1; p >= 0; p--) {
      __m256i bits = popBytes(_mm256_and_si256(code, _mm256_load_si256(plane + p)));
      acc = _mm256_add_epi8(_mm256_add_epi8(acc, acc), bits);
    }
    return _mm256_sad_epu8(acc, _mm256_setzero_si256());
  }
  template <int QB>
  LUXIR_INLINE void score(const uint8_t* code, const uint8_t* planes, size_t chunks, uint64_t& dot, uint64_t& sum) const {
    const __m256i* plane = (const __m256i*)planes;
    __m256i low4 = _mm256_setzero_si256(), high4 = _mm256_setzero_si256(), bits = _mm256_setzero_si256();
    constexpr int lowPlanes = std::min(QB, 4);
    for (size_t c = 0; c < chunks; c++, plane += QB) {
      __m256i row = _mm256_loadu_si256((const __m256i*)(code + c * CHUNK_BYTES));
      low4 = _mm256_add_epi64(low4, horner(row, plane, lowPlanes));
      if constexpr (QB > 4) high4 = _mm256_add_epi64(high4, horner(row, plane + 4, QB - 4));
      bits = _mm256_add_epi64(bits, _mm256_sad_epu8(popBytes(row), _mm256_setzero_si256()));
    }
    dot = sum64(low4);
    if constexpr (QB > 4) dot += sum64(high4) << 4;
    sum = sum64(bits);
  }

  static LUXIR_INLINE void packChunk(const uint8_t*& in, uint8_t*& out, size_t qb, size_t) {
    for (size_t batch = 0; batch < CHUNK_BYTES; batch += 4, in += 32) {
      __m256i q = _mm256_loadu_si256((const __m256i*)in);
      for (size_t p = 0; p < qb; p++) {
        uint32_t mask = (uint32_t)_mm256_movemask_epi8(_mm256_slli_epi16(q, (int)(7 - p)));
        std::memcpy(out + p * CHUNK_BYTES + batch, &mask, sizeof(mask));
      }
    }
    out += qb * CHUNK_BYTES;
  }
};

#include "RaBitQScorerBody.inl"

}  // namespace avx2
#pragma GCC pop_options

#pragma GCC push_options
#pragma GCC target("avx2,avx512f,avx512bw,avx512vpopcntdq")
namespace avx512 {

constexpr Level LEVEL = Level::AVX512;
constexpr size_t CHUNK_BYTES = 64;
constexpr bool MASKED_CHUNKS = true;

using avx2::minMaxPrefix;

// Vector popcount for 64-byte chunks with a masked final load.
struct ChunkOps {
  // Valid row bytes in the final chunk.
  __mmask64 lastMask;

  LUXIR_INLINE explicit ChunkOps(size_t codeBytes)
      : lastMask(codeBytes % CHUNK_BYTES ? ((uint64_t)1 << (codeBytes % CHUNK_BYTES)) - 1 : ~(uint64_t)0) {}

  template <int QB>
  LUXIR_INLINE void score(const uint8_t* code, const uint8_t* planes, size_t chunks, uint64_t& dot, uint64_t& sum) const {
    const __m512i* plane = (const __m512i*)planes;
    __m512i weighted = _mm512_setzero_si512(), bits = _mm512_setzero_si512();
    auto add = [&](__m512i row) LUXIR_INLINE {
      __m512i acc = _mm512_setzero_si512();
      for (int p = QB - 1; p >= 0; p--) {
        __m512i count = _mm512_popcnt_epi64(_mm512_and_si512(row, _mm512_load_si512(plane + p)));
        acc = _mm512_add_epi64(_mm512_add_epi64(acc, acc), count);
      }
      weighted = _mm512_add_epi64(weighted, acc);
      bits = _mm512_add_epi64(bits, _mm512_popcnt_epi64(row));
      plane += QB;
    };
    // Mask only the last chunk, which can run into the factor bytes or past the
    // code: a masked load of every chunk costs 2x per code at 1536 dimensions.
    for (size_t c = 0; c + 1 < chunks; c++) add(_mm512_loadu_si512(code + c * CHUNK_BYTES));
    add(_mm512_maskz_loadu_epi8(lastMask, code + (chunks - 1) * CHUNK_BYTES));
    // One reduction for both: dot in the even lanes, sum in the odd.
    __m512i both = _mm512_add_epi64(_mm512_unpacklo_epi64(weighted, bits), _mm512_unpackhi_epi64(weighted, bits));
    __m256i half = _mm256_add_epi64(_mm512_castsi512_si256(both), _mm512_extracti64x4_epi64(both, 1));
    __m128i pair = _mm_add_epi64(_mm256_castsi256_si128(half), _mm256_extracti128_si256(half, 1));
    dot = (uint64_t)_mm_cvtsi128_si64(pair);
    sum = (uint64_t)_mm_extract_epi64(pair, 1);
  }

  static LUXIR_INLINE void packChunk(const uint8_t*& in, uint8_t*& out, size_t qb, size_t dimsLeft) {
    // Only load groups that contain dimensions. Quantized is padded to 64
    // dimensions, while the planes are padded to a whole 512-dimension chunk.
    for (size_t batch = 0; batch < CHUNK_BYTES && batch * 8 < dimsLeft; batch += 8, in += 64) {
      __m512i q = _mm512_loadu_si512(in);
      for (size_t p = 0; p < qb; p++) {
        uint64_t mask = (uint64_t)_mm512_movepi8_mask(_mm512_slli_epi16(q, (int)(7 - p)));
        std::memcpy(out + p * CHUNK_BYTES + batch, &mask, sizeof(mask));
      }
    }
    out += qb * CHUNK_BYTES;
  }
};

#include "RaBitQScorerBody.inl"

}  // namespace avx512
#pragma GCC pop_options
#endif

constexpr Kernels levels[] = {
#ifdef LUXIR_RABITQ_NEON
  neon::kernels,
#else
  generic::kernels,
#ifdef LUXIR_RABITQ_X86
  avx2::kernels,
  avx512::kernels,
#endif
#endif
};

// A code of at most two 64-bit words scores faster with popcount words than
// with one masked 64-byte chunk and its reduction (Zen 5, 4 query bits: 1.9
// against 2.6 ns per code at 64 dims, 2.3 against 2.5 at 128), so BEST takes
// the first level for those and the last for longer codes.
constexpr size_t WORD_LEVEL_CODE_BYTES = 16;

const Kernels* selectKernels(Level level, size_t codeBytes) {
  auto available = RaBitQScorer::supported();
  if (level == Level::BEST) level = codeBytes <= WORD_LEVEL_CODE_BYTES ? available.front() : available.back();
  else if (std::find(available.begin(), available.end(), level) == available.end())
    throw std::invalid_argument("Unsupported RaBitQ kernel level");
  for (const auto& kernels : levels) {
    if (kernels.level == level) return &kernels;
  }
  std::unreachable();
}

}  // namespace

std::span<const RaBitQScorer::Level> RaBitQScorer::supported() {
#ifdef LUXIR_RABITQ_X86
  // Each level also needs everything the one before it does, so the supported
  // levels are a prefix of this list.
  static constexpr Level ids[] = {Level::GENERIC, Level::AVX2, Level::AVX512};
  static const size_t count = [] {
    __builtin_cpu_init();
    if (!__builtin_cpu_supports("avx2") || !__builtin_cpu_supports("popcnt")) return (size_t)1;
    bool wide = __builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512bw") &&
        __builtin_cpu_supports("avx512vpopcntdq");
    return (size_t)(wide ? 3 : 2);
  }();
  return std::span<const Level>(ids, count);
#else
  return std::span<const Level>(&levels[0].level, 1);
#endif
}

RaBitQScorer::RaBitQScorer(int32_t dims, int32_t queryBits, bool innerProduct, const float* query, Level level)
    : kernels(selectKernels(level, ((size_t)dims + 7) / 8)),
      state{.query = query, .dims = (size_t)dims, .codeBytes = ((size_t)dims + 7) / 8, .queryBits = queryBits,
            .innerProduct = innerProduct, .invSqrtDims = 1.0f / std::sqrt((float)dims),
            .maxCode = (float)((1 << queryBits) - 1), .factors = {}} {
  assert(dims > 0 && queryBits >= 1 && queryBits <= 8);
  size_t chunk = kernels->chunkBytes;
  if (chunk != 0) state.chunks = (state.codeBytes + (kernels->maskedChunks ? chunk - 1 : 0)) / chunk;
  if (!kernels->maskedChunks) state.tailWords = (state.codeBytes - state.chunks * chunk + 7) / 8;
  constexpr size_t ALIGN = 64;
  size_t residualBytes = (state.dims * sizeof(float) + ALIGN - 1) & ~(ALIGN - 1);
  // Whole 64-bit groups, so packPlanes can load past the last dimension.
  size_t quantizedBytes = (state.dims + 7 + ALIGN - 1) & ~(ALIGN - 1);
  size_t planeBytes = ((state.chunks * chunk + state.tailWords * 8) * (size_t)queryBits + ALIGN - 1) & ~(ALIGN - 1);
  storage = std::make_unique_for_overwrite<uint8_t[]>(residualBytes + quantizedBytes + planeBytes + ALIGN);
  uint8_t* base = storage.get() + (ALIGN - (uintptr_t)storage.get() % ALIGN) % ALIGN;
  state.residual = (float*)base;
  state.quantized = base + residualBytes;
  state.planes = state.quantized + quantizedBytes;
  std::memset(state.quantized, 0, quantizedBytes + planeBytes);
  if (innerProduct) state.factors.queryNormSqr = faiss::fvec_norm_L2sqr(query, state.dims);
}

uint8_t RaBitQScorer::planeByte(int32_t plane, size_t index) const {
  size_t qb = (size_t)state.queryBits;
  size_t chunk = kernels->chunkBytes;
  size_t chunkBytes = state.chunks * chunk;
  if (index < chunkBytes)
    return state.planes[(index / chunk) * qb * chunk + (size_t)plane * chunk + index % chunk];
  index -= chunkBytes;
  return state.planes[chunkBytes * qb + (index / 8) * qb * 8 + (size_t)plane * 8 + index % 8];
}

void RaBitQScorer::setList(const float* centroid) {
  kernels->setList(state, centroid);
}

RaBitQScorer::Counts RaBitQScorer::counts(const uint8_t* code) const {
  return kernels->counts(state, code);
}

void RaBitQScorer::score(const uint8_t* codes, size_t codeSize, size_t count, float* out) const {
  kernels->score(state, codes, codeSize, nullptr, count, out);
}

void RaBitQScorer::score(const uint8_t* codes, size_t codeSize, const uint16_t* rows, size_t count, float* out) const {
  kernels->scoreSelected(state, codes, codeSize, rows, count, out);
}

}  // namespace luxir
