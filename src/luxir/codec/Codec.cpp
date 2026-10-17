// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "Codec.h"

#include <algorithm>
#include <cstring>
#include <immintrin.h>

#include "luxir/reader/Postings.h"

// FastPFOR: only this TU pulls its headers, keeping them out of the widely
// included Codec.h. simdpack/simdunpack and usimd* all use unaligned SIMD
// loads/stores, so the caller's byte buffers need no special alignment.
#include "usimdbitpacking.h"  // usimdpackwithoutmask / usimdunpack
#include "simdbitpacking.h"   // simdpack (with mask) / simdunpack
#include "bitpackinghelpers.h"// fastpackwithoutmask / fastunpack (scalar tails)
#include "util.h"             // gccbits

namespace luxir {

// Single fixed block size (positions/docs codecs always encode one block).
static constexpr uint32_t BLOCK_SIZE = Postings::DOCS_BLOCK_SIZE;  // 128

namespace {

// A stack-resident bit packer for PFor exception streams, with no dynamic
// allocation. Lane k holds the (k+1)-bit exception residuals; write()/read()
// (de)serialize them with FastPFOR's SIMD + scalar bit-packing kernels.
class LuxirBitPacker {
private:
  constexpr static uint32_t SIZE = BLOCK_SIZE;
  uint16_t sizes[32];
  uint32_t data[32][SIZE];

  LuxirBitPacker(const LuxirBitPacker&) = delete;
  LuxirBitPacker& operator=(const LuxirBitPacker&) = delete;

public:
  uint32_t buffer[32];

  LuxirBitPacker() {}

  void directAppend(uint32_t i, uint32_t val) { data[i][sizes[i]++] = val; }
  const uint32_t* get(int i) { return data[i]; }

  void ensureCapacity(int i, uint32_t datatoadd) {
    assert(i >= 0 && i <= 32);
    assert(sizes[i] + datatoadd <= SIZE);
    (void) i;
    (void) datatoadd;
  }

  void clear() {
    for (uint32_t i = 0; i < 32; ++i) sizes[i] = 0;
  }

  // Serialize a single lane's values. Unlike the multi-block FastPFOR original,
  // single-block PFor uses exactly one lane, and its identity (maxb-bestb-1) and
  // count (cexcept) are already in the block header -- so this writes NO bitmap
  // word and NO per-lane size word, just the packed residuals.
  uint32_t* writeLane(uint32_t* out, uint32_t lane) {
    const uint32_t k = lane;
    uint32_t j = 0;
    for (; j + 128 <= sizes[k]; j += 128) {
      FastPForLib::usimdpackwithoutmask(&data[k][j], reinterpret_cast<__m128i*>(out), k + 1);
      out += 4 * (k + 1);
    }
    // scalar fallback for the remainder. The final partial group packs its
    // padding slots unmasked into the last kept word, so zero them or that
    // word carries stale stack bytes; whole over-counted words are backed out
    // below.
    std::fill(&data[k][sizes[k]], &data[k][(sizes[k] + 31u) / 32 * 32], 0u);
    for (; j < sizes[k]; j += 32) {
      FastPForLib::fastpackwithoutmask(&data[k][j], out, k + 1);
      out += k + 1;
    }
    out -= (j - sizes[k]) * (k + 1) / 32;
    return out;
  }

  // Inverse of writeLane: unpack `count` values into `lane`. The caller supplies
  // lane + count from the block header (not read from the stream).
  const uint32_t* readLane(const uint32_t* in, uint32_t lane, uint32_t count) {
    const uint32_t k = lane;
    assert(count <= SIZE);
    sizes[k] = count;
    uint32_t j = 0;
    for (; j + 128 <= count; j += 128) {
      FastPForLib::usimdunpack(reinterpret_cast<const __m128i*>(in), &data[k][j], k + 1);
      in += 4 * (k + 1);
    }
    for (; j + 31 < count; j += 32) {
      FastPForLib::fastunpack(in, &data[k][j], k + 1);
      in += k + 1;
    }
    // final partial group: copy just its words into a scratch buffer so the
    // scalar unpack (which consumes 32 values worth of input) can't over-read.
    uint32_t remaining = count - j;
    memcpy(buffer, in, (remaining * (k + 1) + 31) / 32 * sizeof(uint32_t));
    uint32_t* bpointer = buffer;
    in += ((count + 31) / 32 * 32 - j) / 32 * (k + 1);
    for (; j < count; j += 32) {
      FastPForLib::fastunpack(bpointer, &data[k][j], k + 1);
      bpointer += k + 1;
    }
    in -= (j - count) * (k + 1) / 32;
    return in;
  }
};

// Choose the base bit width (bestb), exception count (bestcexcept) and max bit
// width (maxb) that minimize the encoded size for this block. The cost model is
// in bits and matches the lean single-block layout: a b-bit base over all 128
// values, plus per exception one position byte (overheadofeachexcept) and a
// (maxb-b)-bit residual; the +8 is the single maxb byte, present only when there
// are exceptions (so it is excluded from the cexcept==0 bestcost init). A
// residual of width 1 stores nothing (high bit implicitly 1, reconstructed on
// decode), so its residual term is dropped -- and that keys off the residual
// width maxb-b, NOT bestb (which mutates as the search finds a better base).
void getBestBFromData(const uint32_t* in, uint8_t& bestb, uint8_t& bestcexcept, uint8_t& maxb) {
  constexpr uint32_t overheadofeachexcept = 8;
#if defined(__AVX2__)
  __m256i widths[4];
  const __m256i zero = _mm256_setzero_si256();
  const __m256i one = _mm256_set1_epi32(1);
  const __m256i signBit = _mm256_set1_epi32((int32_t) 0x80000000u);
  const __m256i maxWidth = _mm256_set1_epi32(32);
  const __m256i exponentMask = _mm256_set1_epi32(0xff);
  const __m256i exponentBias = _mm256_set1_epi32(127);

  for (uint32_t block = 0; block < 4; block++) {
    __m256i width32[4];
    for (uint32_t row = 0; row < 4; row++) {
      __m256i values = _mm256_loadu_si256(
          (const __m256i*) (in + block * 32 + row * 8));
      __m256i highBit = _mm256_cmpgt_epi32(zero, values);
      __m256 converted = _mm256_cvtepi32_ps(values);
      __m256i exponent = _mm256_sub_epi32(
          _mm256_and_si256(
              _mm256_srli_epi32(_mm256_castps_si256(converted), 23), exponentMask),
          exponentBias);
      __m256i width = _mm256_add_epi32(exponent, one);

      // Integer-to-float conversion can round 2^k-1 up to 2^k. Correct that
      // edge against the original unsigned value before narrowing the widths.
      __m256i boundary = _mm256_sllv_epi32(one, exponent);
      __m256i roundedUp = _mm256_cmpgt_epi32(
          _mm256_xor_si256(boundary, signBit),
          _mm256_xor_si256(values, signBit));
      width = _mm256_add_epi32(width, roundedUp);
      width = _mm256_min_epi32(width, maxWidth);
      width = _mm256_andnot_si256(_mm256_cmpeq_epi32(values, zero), width);
      width32[row] = _mm256_blendv_epi8(width, maxWidth, highBit);
    }
    widths[block] = _mm256_packus_epi16(
        _mm256_packus_epi32(width32[0], width32[1]),
        _mm256_packus_epi32(width32[2], width32[3]));
  }

  __m256i blockMax = _mm256_max_epu8(
      _mm256_max_epu8(widths[0], widths[1]),
      _mm256_max_epu8(widths[2], widths[3]));
  __m128i widthMax = _mm_max_epu8(
      _mm256_castsi256_si128(blockMax), _mm256_extracti128_si256(blockMax, 1));
  widthMax = _mm_max_epu8(widthMax, _mm_srli_si128(widthMax, 8));
  widthMax = _mm_max_epu8(widthMax, _mm_srli_si128(widthMax, 4));
  widthMax = _mm_max_epu8(widthMax, _mm_srli_si128(widthMax, 2));
  widthMax = _mm_max_epu8(widthMax, _mm_srli_si128(widthMax, 1));
  bestb = (uint8_t) _mm_cvtsi128_si32(widthMax);
  maxb = bestb;
  uint32_t bestcost = bestb * BLOCK_SIZE;
  bestcexcept = 0;
  for (uint32_t b = bestb - 1; b < 32; --b) {
    __m256i threshold = _mm256_set1_epi8((char) b);
    uint32_t cexcept = 0;
    for (uint32_t block = 0; block < 4; block++) {
      cexcept += std::popcount((uint32_t) _mm256_movemask_epi8(
          _mm256_cmpgt_epi8(widths[block], threshold)));
    }
    uint32_t thiscost = cexcept * overheadofeachexcept
        + cexcept * (maxb - b) + b * BLOCK_SIZE + 8;
    if (maxb - b == 1) thiscost -= cexcept;
    if (thiscost < bestcost) {
      bestcost = thiscost;
      bestb = (uint8_t) b;
      bestcexcept = (uint8_t) cexcept;
    }
  }
#else
  uint32_t freqs[33];
  for (uint32_t k = 0; k <= 32; ++k) freqs[k] = 0;
  for (uint32_t k = 0; k < BLOCK_SIZE; ++k) freqs[FastPForLib::gccbits(in[k])]++;
  bestb = 32;
  while (freqs[bestb] == 0) bestb--;
  maxb = bestb;
  uint32_t bestcost = bestb * BLOCK_SIZE;
  uint32_t cexcept = 0;
  bestcexcept = (uint8_t) cexcept;
  for (uint32_t b = bestb - 1; b < 32; --b) {
    cexcept += freqs[b + 1];
    uint32_t thiscost = cexcept * overheadofeachexcept + cexcept * (maxb - b) + b * BLOCK_SIZE + 8;
    if (maxb - b == 1) thiscost -= cexcept;
    if (thiscost < bestcost) {
      bestcost = thiscost;
      bestb = (uint8_t) b;
      bestcexcept = (uint8_t) cexcept;
    }
  }
#endif
}

// Single-block PForDelta encode (NoDelta). Lean layout, no multi-block
// scaffolding:
//   [bestb:1][cexcept:1]  [maxb:1, positions:cexcept B  -- only if cexcept>0]
//   (padded to a word) [SIMD-packed base: 4*bestb words]
//   [residuals: cexcept values @ (maxb-bestb) bits  -- only if cexcept>1 over base]
// Every section's size is derivable from the 2-3 header bytes, so no offset,
// byte-container-size, or bitpacker bitmap/lane-size words are stored. outSz is
// the encoded size in bytes.
void encodeBlockPFor(uint32_t* in, char* outc, uint32_t& outSz) {
  uint8_t bestb, bestcexcept, maxb = 0;
  getBestBFromData(in, bestb, bestcexcept, maxb);

  // Header, written straight into the output.
  uint8_t* bc = reinterpret_cast<uint8_t*>(outc);
  *bc++ = bestb;
  *bc++ = bestcexcept;

  LuxirBitPacker bpacker;
  if (bestcexcept > 0) {
    *bc++ = maxb;
    bpacker.clear();
    bpacker.ensureCapacity(maxb - bestb - 1, bestcexcept);
    const uint32_t maxval = 1U << bestb;
#if defined(__AVX2__)
    const __m256i signBit = _mm256_set1_epi32((int32_t) 0x80000000u);
    const __m256i compareLimit = _mm256_set1_epi32(
        (int32_t) ((maxval - 1) ^ 0x80000000u));
    [[maybe_unused]] uint32_t emitted = 0;
    for (uint32_t k = 0; k < BLOCK_SIZE; k += 8) {
      __m256i values = _mm256_loadu_si256((const __m256i*) (in + k));
      __m256i exceptions = _mm256_cmpgt_epi32(
          _mm256_xor_si256(values, signBit), compareLimit);
      uint32_t mask = (uint32_t) _mm256_movemask_ps(_mm256_castsi256_ps(exceptions));
      while (mask != 0) {
        uint32_t lane = (uint32_t) std::countr_zero(mask);
        uint32_t position = k + lane;
        bpacker.directAppend(maxb - bestb - 1, in[position] >> bestb);
        *bc++ = (uint8_t) position;
        emitted++;
        mask &= mask - 1;
      }
    }
    assert(emitted == bestcexcept);
#else
    for (uint32_t k = 0; k < BLOCK_SIZE; ++k) {
      if (in[k] >= maxval) {
        bpacker.directAppend(maxb - bestb - 1, in[k] >> bestb);
        *bc++ = (uint8_t) k;
      }
    }
#endif
  }
  // Word-align the base so the SIMD/scalar bit-packing kernels stay word-granular
  // (zero the pad bytes for a deterministic encoding).
  while ((reinterpret_cast<char*>(bc) - outc) % sizeof(uint32_t) != 0) *bc++ = 0;
  uint32_t* out = reinterpret_cast<uint32_t*>(bc);

  // SIMD-packed base (masked: exception values keep only their low bestb bits
  // here). bestb==0 packs nothing.
  FastPForLib::simdpack(in, reinterpret_cast<__m128i*>(out), bestb);
  out += 4 * bestb;

  // Exception residuals: one lane of (maxb-bestb)-bit values. Width 1 stores
  // nothing (the high bit is implicitly 1, reconstructed on decode).
  if (bestcexcept > 0 && (uint32_t) (maxb - bestb) > 1) {
    out = bpacker.writeLane(out, maxb - bestb - 1);
  }
  outSz = (uint32_t) (reinterpret_cast<char*>(out) - outc);
}

// Single-block PForDelta decode (hand-rolled, stack bit packer). Returns bytes read.
uint32_t decodeBlockPFor(const char* inc, uint32_t* out) {
  const uint8_t* h = reinterpret_cast<const uint8_t*>(inc);
  const uint8_t bestb = h[0];
  const uint8_t cexcept = h[1];
  uint32_t headerBytes = 2;
  uint8_t maxb = 0;
  const uint8_t* positions = nullptr;
  if (cexcept > 0) {
    maxb = h[2];
    positions = h + 3;
    headerBytes = 3 + cexcept;
  }
  const uint32_t headerWords = (headerBytes + sizeof(uint32_t) - 1) / sizeof(uint32_t);
  const uint32_t* in = reinterpret_cast<const uint32_t*>(inc) + headerWords;

  FastPForLib::simdunpack(reinterpret_cast<const __m128i*>(in), out, bestb);
  in += 4 * bestb;

  if (cexcept > 0) {
    const uint32_t width = (uint32_t) maxb - bestb;
    if (width == 1) {
      for (uint32_t k = 0; k < cexcept; ++k) {
        out[positions[k]] |= (uint32_t) 1 << bestb;
      }
    } else {
      LuxirBitPacker bpacker;
      in = bpacker.readLane(in, maxb - bestb - 1, cexcept);
      const uint32_t* vals = bpacker.get(maxb - bestb - 1);
      for (uint32_t k = 0; k < cexcept; ++k) {
        out[positions[k]] |= vals[k] << bestb;
      }
    }
  }
  return (uint32_t) (reinterpret_cast<const char*>(in) - inc);
}

// Byte length of one encoded PFor block, from its header alone - no unpack.
// Section walk MUST stay in lockstep with decodeBlockPFor / readLane above:
// [header, word-padded][simd base: 4*bestb words][exception lane, width > 1].
uint32_t skipBlockPFor(const char* inc) {
  const uint8_t* h = reinterpret_cast<const uint8_t*>(inc);
  const uint8_t bestb = h[0];
  const uint8_t cexcept = h[1];
  uint32_t headerBytes = 2;
  uint8_t maxb = 0;
  if (cexcept > 0) {
    maxb = h[2];
    headerBytes = 3 + cexcept;
  }
  const uint32_t headerWords = (headerBytes + sizeof(uint32_t) - 1) / sizeof(uint32_t);
  const uint32_t* in = reinterpret_cast<const uint32_t*>(inc) + headerWords;

  in += 4 * bestb;

  if (cexcept > 0 && (uint32_t) (maxb - bestb) > 1) {
    // Mirror LuxirBitPacker::readLane's pointer arithmetic for lane
    // k = maxb - bestb - 1 ((k+1)-bit values, count = cexcept) - INCLUDING
    // its final back-out of the padded partial group's over-counted words,
    // which matches what writeLane actually wrote.
    const uint32_t k = (uint32_t) (maxb - bestb) - 1;
    const uint32_t count = cexcept;
    uint32_t j = 0;
    for (; j + 128 <= count; j += 128) {
      in += 4 * (k + 1);
    }
    for (; j + 31 < count; j += 32) {
      in += k + 1;
    }
    const uint32_t jPadded = (count + 31) / 32 * 32;
    in += (jPadded - j) / 32 * (k + 1);
    in -= (jPadded - count) * (k + 1) / 32;
  }
  return (uint32_t) (reinterpret_cast<const char*>(in) - inc);
}

// Compact 4-lane bit layout for partial (tail) blocks, matching the addressing
// in LuxirSIMDFor::selectWithMeta. Full blocks go through the SIMD kernels; the
// final partial block is packed compactly here (NOT padded to 128) so the
// encoded size never exceeds the caller's value-count-based buffer budget.
// `bits` is in 1..31 (the 0 and 32 cases are handled separately).

uint32_t tailWords(uint32_t len, uint8_t bits) {
  return (uint32_t)(LuxirSIMDFor::byteSize(len, bits) / sizeof(uint32_t));
}

uint32_t packTail(const uint32_t* residuals, uint32_t len, uint8_t bits, uint32_t* out) {
  const uint32_t words = tailWords(len, bits);
  for (uint32_t i = 0; i < words; ++i) out[i] = 0;
  for (uint32_t i = 0; i < len; ++i) {
    const uint32_t lane = i % 4;
    const uint32_t bitsinlane = (i / 4) * bits;
    const uint32_t firstword = bitsinlane / 32;
    const uint32_t off = bitsinlane % 32;
    const uint32_t v = residuals[i];  // already < (1<<bits)
    out[4 * firstword + lane] |= v << off;
    if (off + bits > 32) out[4 * (firstword + 1) + lane] |= v >> (32 - off);
  }
  return words;
}

void unpackTail(const uint32_t* in, uint32_t len, uint8_t bits, uint32_t* out) {
  const uint32_t mask = (uint32_t)((1ull << bits) - 1);  // 64-bit shift avoids UB at bits==32
  for (uint32_t i = 0; i < len; ++i) {
    const uint32_t lane = i % 4;
    const uint32_t bitsinlane = (i / 4) * bits;
    const uint32_t firstword = bitsinlane / 32;
    const uint32_t off = bitsinlane % 32;
    uint32_t v = in[4 * firstword + lane] >> off;
    if (off + bits > 32) v |= in[4 * (firstword + 1) + lane] << (32 - off);
    out[i] = v & mask;
  }
}

// --- T4: transposed-lane docid delta (breaks the D1 serial prefix-sum carry) ---
//
// The docid decode's dominant cost is undoing the doc-gap delta.  A D1 (adjacent)
// delta reconstructs with a serial prefix sum whose vector-to-vector running-count
// broadcast is a loop-carried dependency wider registers cannot break.  T4 instead
// splits the 128-doc block into 4 lanes of 32 (lane j owns docids [32j, 32j+32)) and
// stores the gaps transposed: phys[4*m + j] is row m of lane j.  Decode is then a
// vertical running sum across the 32 rows -- 4 independent lanes, one add per row,
// no cross-lane carry -- plus a 4-wide lane-base offset and an un-transpose back to
// natural order.  The residual values are exactly the D1 adjacent-gap multiset,
// merely permuted, so PForDelta width / exceptions / encoded size are unchanged.
constexpr uint32_t T4_LANES = 4;
constexpr uint32_t T4_ROWS = BLOCK_SIZE / T4_LANES;  // 32

// Encode: natural-order docids `doc` -> transposed gap residuals `phys`.  `base` is
// the previous block's last docid (0 for the first block).  Not perf-critical.
void t4Delta(const uint32_t* doc, uint32_t base, uint32_t* phys) {
  for (uint32_t j = 0; j < T4_LANES; ++j) {
    uint32_t prev = (j == 0) ? base : doc[T4_ROWS * j - 1];
    for (uint32_t m = 0; m < T4_ROWS; ++m) {
      const uint32_t d = doc[T4_ROWS * j + m];
      phys[T4_LANES * m + j] = d - prev;
      prev = d;
    }
  }
}

// Decode: transposed gap residuals in `p` -> natural-order absolute docids in `p`
// (in place; every row is read before any is written back).  `base` as above.
void t4InverseDelta(uint32_t* p, uint32_t base) {
  const __m128i* in = reinterpret_cast<const __m128i*>(p);
  __m128i rows[T4_ROWS];
  __m128i acc = _mm_setzero_si128();
  for (uint32_t m = 0; m < T4_ROWS; ++m) {   // acc.lane j = doc[32j+m] - laneBase[j]
    acc = _mm_add_epi32(acc, _mm_loadu_si128(in + m));
    rows[m] = acc;
  }
  // laneBase[j] = base + sum of lane totals below j; lane totals are the last row.
  uint32_t total[T4_LANES];
  _mm_storeu_si128(reinterpret_cast<__m128i*>(total), rows[T4_ROWS - 1]);
  uint32_t off[T4_LANES];
  off[0] = base;
  for (uint32_t j = 1; j < T4_LANES; ++j) off[j] = off[j - 1] + total[j - 1];
  const __m128i offv = _mm_loadu_si128(reinterpret_cast<const __m128i*>(off));
  // Add the lane base and un-transpose to natural order.  Each iteration adds the
  // base to 4 rows, then a 4x4 lane transpose turns those rows (lane-major) into
  // 4 vectors of consecutive rows per lane -- lane j's rows [mb, mb+4) land
  // contiguously at p[32j + mb], reconstructing ascending docids.
  for (uint32_t mb = 0; mb < T4_ROWS; mb += 4) {
    const __m128i r0 = _mm_add_epi32(rows[mb + 0], offv);
    const __m128i r1 = _mm_add_epi32(rows[mb + 1], offv);
    const __m128i r2 = _mm_add_epi32(rows[mb + 2], offv);
    const __m128i r3 = _mm_add_epi32(rows[mb + 3], offv);
    const __m128i t0 = _mm_unpacklo_epi32(r0, r1);
    const __m128i t1 = _mm_unpackhi_epi32(r0, r1);
    const __m128i t2 = _mm_unpacklo_epi32(r2, r3);
    const __m128i t3 = _mm_unpackhi_epi32(r2, r3);
    __m128i* op = reinterpret_cast<__m128i*>(p + mb);
    _mm_storeu_si128(op + 0 * (T4_ROWS / 4), _mm_unpacklo_epi64(t0, t2));  // lane 0
    _mm_storeu_si128(op + 1 * (T4_ROWS / 4), _mm_unpackhi_epi64(t0, t2));  // lane 1
    _mm_storeu_si128(op + 2 * (T4_ROWS / 4), _mm_unpacklo_epi64(t1, t3));  // lane 2
    _mm_storeu_si128(op + 3 * (T4_ROWS / 4), _mm_unpackhi_epi64(t1, t3));  // lane 3
  }
}

}  // namespace

// --- LuxirSIMDFor (frame-of-reference numeric codec) ---

// Pack inSz values (residuals = value - minval) at a uniform `bits` width.
// Full 128-value blocks use FastPFOR's SIMD packer; the partial tail is packed
// compactly so the encoded size stays within inSz*4 (+ small overhead) -- the
// caller (IntColWriter) sizes its buffer by value count, not block count.
void LuxirSIMDFor::encodeWithMeta(uint32_t* in, uint32_t inSz, char* target, uint32_t& outSz,
                                  uint32_t minval, uint8_t bits) {
  uint32_t* out = (uint32_t*) target;
  if (bits == 0) {  // all values equal minval; nothing to store.
    outSz = 0;
    return;
  }
  // bits 1..32 all go through the SIMD packer (bit==32 is a straight copy in the kernel), storing
  // residuals (value - minval). For production minval==0 this is byte-identical to a raw copy.

  uint32_t tmp[128];
  uint32_t k = 0;
  for (; k + 128 <= inSz; k += 128) {
    const uint32_t* block;
    if (minval == 0) {
      block = in + k;
    } else {
      for (uint32_t i = 0; i < 128; ++i) tmp[i] = in[k + i] - minval;
      block = tmp;
    }
    FastPForLib::usimdpackwithoutmask(block, (__m128i*) out, bits);
    out += 4 * bits;
  }
  if (k < inSz) {  // compact partial tail (not padded to 128)
    const uint32_t rem = inSz - k;
    for (uint32_t i = 0; i < rem; ++i) tmp[i] = in[k + i] - minval;
    out += packTail(tmp, rem, bits, out);
  }
  outSz = (char*) out - target;
  assert(outSz == byteSize(inSz, bits));
}

// Cold partial-tail decode (min==0): unpack the <128 remainder straight into the output buffer
// (no scratch copy, since there's no minval to add). bits==0 (constant block, nothing stored) ->
// zeros, handled here so the hot path needs no branch for it; unpackTail would otherwise read
// past the (empty) block.
uint32_t LuxirSIMDFor::decodeTailMeta(const uint32_t* in, uint32_t rem, uint32_t* out, uint8_t bits) {
  if (bits == 0) {
    memset(out, 0, rem * sizeof(uint32_t));
    return 0;
  }
  unpackTail(in, rem, bits, out);
  return tailWords(rem, bits);
}

void LuxirSIMDFor::encodeBlock(uint32_t* in, uint32_t inSz, char* target, uint32_t& outSz) {
  uint32_t* out = (uint32_t*) target;
  if (inSz == 0) {
    outSz = 0;
    return;
  }
  uint32_t m = in[0], M = in[0];
  for (uint32_t i = 1; i < inSz; ++i) {
    m = std::min(m, in[i]);
    M = std::max(M, in[i]);
  }
  const int b = std::bit_width((uint32_t) (M - m));
  out[0] = m;
  out[1] = M;
  uint32_t innerSz;
  encodeWithMeta(in, inSz, (char*) (out + 2), innerSz, m, (uint8_t) b);
  outSz = innerSz + 2 * sizeof(uint32_t);
}

uint32_t LuxirSIMDFor::decodeBlock(const char* compressed, uint32_t inSz, uint32_t* out, uint32_t& outSz) {
  unused(inSz);
  if (outSz == 0) return 0;
  const uint32_t* in = (const uint32_t*) compressed;
  const uint32_t m = in[0];
  const uint32_t M = in[1];
  const int b = std::bit_width((uint32_t) (M - m));
  const auto readSize = decodeWithMeta((const char*) (in + 2), out, outSz, (uint8_t) b);
  // decodeWithMeta assumes min==0; the self-describing path restores the stored min here
  // (all widths now store residuals). Test/bench only -- not perf sensitive.
  if (m) for (uint32_t i = 0; i < outSz; ++i) out[i] += m;
  return readSize + 2 * sizeof(uint32_t);
}

uint32_t LuxirSIMDFor::select(const char* compressed, uint32_t nValues, uint32_t index) {
  const uint32_t* in = (const uint32_t*) compressed;
  const uint32_t m = in[0];
  const uint32_t M = in[1];
  const int b = std::bit_width((uint32_t) (M - m));
  return selectWithMeta((const char*) (in + 2), nValues, index, m, (uint8_t) b);
}

// --- LuxirPFOR (positions / term-frequencies, no delta) ---

void LuxirPFOR::encodeBlock(uint32_t* in, uint32_t inSz, char* out, uint32_t& outSz) {
  unused(inSz);
  assert(inSz == BLOCK_SIZE);
  encodeBlockPFor(in, out, outSz);
}

uint32_t LuxirPFOR::decodeBlock(const char* in, uint32_t inSz, uint32_t* out, uint32_t& outSz) {
  unused(inSz);
  assert(outSz == BLOCK_SIZE);
  unused(outSz);
  return decodeBlockPFor(in, out);
}

uint32_t LuxirPFOR::skipBlock(const char* in, uint32_t inSz) {
  unused(inSz);
  uint32_t skipped = skipBlockPFor(in);
#ifndef NDEBUG
  // Lockstep tripwire: the header-only walk must agree with the real decoder.
  uint32_t scratch[BLOCK_SIZE];
  assert(skipped == decodeBlockPFor(in, scratch));
#endif
  return skipped;
}

// --- LuxirPFORd (documents, delta) ---

void LuxirPFORd::encodeBlock(uint32_t* in, uint32_t inSz, char* out, uint32_t& outSz, uint32_t base) {
  unused(inSz);
  assert(inSz == BLOCK_SIZE);
  uint32_t phys[BLOCK_SIZE];
  t4Delta(in, base, phys);  // transposed-lane gap residuals (see T4 notes above)
  encodeBlockPFor(phys, out, outSz);
}

uint32_t LuxirPFORd::decodeBlock(const char* in, uint32_t inSz, uint32_t* out, uint32_t& outSz, uint32_t base) {
  unused(inSz);
  assert(outSz == BLOCK_SIZE);
  unused(outSz);
  auto ret = decodeBlockPFor(in, out);
  t4InverseDelta(out, base);  // lane-parallel prefix sum + un-transpose -> ascending docids
  return ret;
}

// --- IndexCodec statics ---

IndexCodec::DocsCodec IndexCodec::docCodec;
IndexCodec::PositionsCodec IndexCodec::posCodec;
IndexCodec::TFreqCodec& IndexCodec::tfreqCodec = IndexCodec::posCodec;
IndexCodec::NumericCodec IndexCodec::numericCodec;

}  // namespace luxir
