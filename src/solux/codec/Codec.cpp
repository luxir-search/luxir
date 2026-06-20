#include "Codec.h"

#include <algorithm>
#include <cstring>

#include "solux/reader/Postings.h"

// FastPFOR: only this TU pulls its headers, keeping them out of the widely
// included Codec.h. simdpack/simdunpack and usimd* all use unaligned SIMD
// loads/stores, so the caller's byte buffers need no special alignment.
#include "usimdbitpacking.h"  // usimdpackwithoutmask / usimdunpack
#include "simdbitpacking.h"   // simdpack (with mask) / simdunpack
#include "bitpackinghelpers.h"// fastpackwithoutmask / fastunpack (scalar tails)
#include "util.h"             // gccbits
#include "deltautil.h"        // Delta::fastDelta / fastinverseDelta2

namespace solux {

// Single fixed block size (positions/docs codecs always encode one block).
static constexpr uint32_t BLOCK_SIZE = Postings::DOCS_BLOCK_SIZE;  // 128

namespace {

// A stack-resident bit packer for PFor exception streams, with no dynamic
// allocation. Lane k holds the (k+1)-bit exception residuals; write()/read()
// (de)serialize them with FastPFOR's SIMD + scalar bit-packing kernels.
class SoluxBitPacker {
private:
  constexpr static uint32_t SIZE = BLOCK_SIZE;
  uint16_t sizes[32];
  uint32_t data[32][SIZE];

  SoluxBitPacker(const SoluxBitPacker&) = delete;
  SoluxBitPacker& operator=(const SoluxBitPacker&) = delete;

public:
  uint32_t buffer[32];

  SoluxBitPacker() {}

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

  uint32_t* write(uint32_t* out) {
    uint32_t bitmap = 0;
    for (uint32_t k = 1; k < 32; ++k)
      if (sizes[k] != 0) bitmap |= (1U << k);
    *(out++) = bitmap;

    for (uint32_t k = 1; k < 32; ++k) {
      if (sizes[k] != 0) {
        *out = sizes[k];
        out++;
        uint32_t j = 0;
        for (; j + 128 <= sizes[k]; j += 128) {
          FastPForLib::usimdpackwithoutmask(&data[k][j], reinterpret_cast<__m128i*>(out), k + 1);
          out += 4 * (k + 1);
        }
        // scalar fallback for the remainder
        for (; j < sizes[k]; j += 32) {
          FastPForLib::fastpackwithoutmask(&data[k][j], out, k + 1);
          out += k + 1;
        }
        out -= (j - sizes[k]) * (k + 1) / 32;
      }
    }
    return out;
  }

  const uint32_t* read(const uint32_t* in) {
    clear();
    const uint32_t bitmap = *(in++);

    for (uint32_t k = 1; k < 32; ++k) {
      if ((bitmap & (1U << k)) != 0) {
        sizes[k] = *in++;
        assert(sizes[k] <= SIZE);
        uint32_t j = 0;
        for (; j + 128 <= sizes[k]; j += 128) {
          FastPForLib::usimdunpack(reinterpret_cast<const __m128i*>(in), &data[k][j], k + 1);
          in += 4 * (k + 1);
        }
        for (; j + 31 < sizes[k]; j += 32) {
          FastPForLib::fastunpack(in, &data[k][j], k + 1);
          in += k + 1;
        }
        uint32_t remaining = sizes[k] - j;
        memcpy(buffer, in, (remaining * (k + 1) + 31) / 32 * sizeof(uint32_t));
        uint32_t* bpointer = buffer;
        in += ((sizes[k] + 31) / 32 * 32 - j) / 32 * (k + 1);
        for (; j < sizes[k]; j += 32) {
          FastPForLib::fastunpack(bpointer, &data[k][j], k + 1);
          bpointer += k + 1;
        }
        in -= (j - sizes[k]) * (k + 1) / 32;
      }
    }
    return in;
  }
};

// Port of SIMDFastPFor::getBestBFromData: choose the base bit width (bestb),
// the exception count (bestcexcept) and the max bit width (maxb) that minimize
// the encoded size for this block.
void getBestBFromData(const uint32_t* in, uint8_t& bestb, uint8_t& bestcexcept, uint8_t& maxb) {
  constexpr uint32_t overheadofeachexcept = 8;
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
    if (bestb - b == 1) thiscost -= cexcept;
    if (thiscost < bestcost) {
      bestcost = thiscost;
      bestb = (uint8_t) b;
      bestcexcept = (uint8_t) cexcept;
    }
  }
}

// Single-block PForDelta encode (NoDelta). Writes the header word, the
// SIMD-packed base, the exception byte-container and the bit-packed exception
// residuals. outSz is set to the encoded size in bytes.
void encodeBlockPFor(uint32_t* in, char* outc, uint32_t& outSz) {
  SoluxBitPacker bpacker;
  uint32_t* out = (uint32_t*) outc;
  uint32_t* const initout = out;
  uint32_t* const headerout = out++;
  bpacker.clear();
  uint8_t bytescontainer[3 + BLOCK_SIZE];
  uint8_t* bc = bytescontainer;

  uint8_t bestb, bestcexcept, maxb;
  getBestBFromData(in, bestb, bestcexcept, maxb);
  *bc++ = bestb;
  *bc++ = bestcexcept;
  if (bestcexcept > 0) {
    *bc++ = maxb;
    bpacker.ensureCapacity(maxb - bestb - 1, bestcexcept);
    const uint32_t maxval = 1U << bestb;
    for (uint32_t k = 0; k < BLOCK_SIZE; ++k) {
      if (in[k] >= maxval) {
        bpacker.directAppend(maxb - bestb - 1, in[k] >> bestb);
        *bc++ = (uint8_t) k;
      }
    }
  }
  for (uint32_t k = 0; k < BLOCK_SIZE; k += 128) {
    FastPForLib::simdpack(in + k, reinterpret_cast<__m128i*>(out), bestb);
    out += 4 * bestb;
  }
  headerout[0] = (uint32_t) (out - headerout);
  const uint32_t bytescontainersize = (uint32_t) (bc - bytescontainer);
  *(out++) = bytescontainersize;
  memcpy(out, bytescontainer, bytescontainersize);
  out += (bytescontainersize + sizeof(uint32_t) - 1) / sizeof(uint32_t);
  uint32_t* const lastout = bpacker.write(out);
  outSz = (uint32_t) ((lastout - initout) * sizeof(uint32_t));
}

// Single-block PForDelta decode (hand-rolled, stack bit packer). Returns bytes read.
uint32_t decodeBlockPFor(const char* inc, uint32_t* out) {
  SoluxBitPacker bpacker;
  uint32_t* in = (uint32_t*) inc;
  const uint32_t* const initin = in;
  const uint32_t* const headerin = in++;
  const uint32_t wheremeta = headerin[0];
  const uint32_t* inexcept = headerin + wheremeta;
  const uint32_t bytesize = *inexcept++;
  const uint8_t* bytep = reinterpret_cast<const uint8_t*>(inexcept);

  inexcept += (bytesize + sizeof(uint32_t) - 1) / sizeof(uint32_t);
  inexcept = bpacker.read(inexcept);
  const uint32_t lengthWords = (uint32_t) (inexcept - initin);

  const uint8_t b = *bytep++;
  const uint8_t cexcept = *bytep++;
  for (uint32_t k = 0; k < BLOCK_SIZE; k += 128) {
    FastPForLib::simdunpack(reinterpret_cast<const __m128i*>(in), out + k, b);
    in += 4 * b;
  }
  if (cexcept > 0) {
    const uint8_t maxbits = *bytep++;
    if (maxbits - b == 1) {
      for (uint32_t k = 0; k < cexcept; ++k) {
        const uint8_t pos = *(bytep++);
        out[pos] |= (uint32_t) 1 << b;
      }
    } else {
      const uint32_t* vals = bpacker.get(maxbits - b - 1);
      for (uint32_t k = 0; k < cexcept; ++k) {
        const uint8_t pos = *(bytep++);
        out[pos] |= vals[k] << b;
      }
    }
  }
  return lengthWords * sizeof(uint32_t);
}

// Compact 4-lane bit layout for partial (tail) blocks, matching the addressing
// in SoluxSIMDFor::selectWithMeta. Full blocks go through the SIMD kernels; the
// final partial block is packed compactly here (NOT padded to 128) so the
// encoded size never exceeds the caller's value-count-based buffer budget.
// `bits` is in 1..31 (the 0 and 32 cases are handled separately).

uint32_t tailWords(uint32_t len, uint8_t bits) {
  const uint32_t rows = (len + 3) / 4;
  return 4 * ((rows * bits + 31) / 32);
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
  const uint32_t mask = (1u << bits) - 1;
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

}  // namespace

// --- SoluxSIMDFor (frame-of-reference numeric codec) ---

// Pack inSz values (residuals = value - minval) at a uniform `bits` width.
// Full 128-value blocks use FastPFOR's SIMD packer; the partial tail is packed
// compactly so the encoded size stays within inSz*4 (+ small overhead) -- the
// caller (IntColWriter) sizes its buffer by value count, not block count.
void SoluxSIMDFor::encodeWithMeta(uint32_t* in, uint32_t inSz, char* target, uint32_t& outSz,
                                  uint32_t minval, uint8_t bits) {
  uint32_t* out = (uint32_t*) target;
  if (bits == 0) {  // all values equal minval; nothing to store.
    outSz = 0;
    return;
  }
  if (bits == 32) {  // raw passthrough -- selectWithMeta returns the raw word.
    memcpy(out, in, inSz * sizeof(uint32_t));
    outSz = inSz * sizeof(uint32_t);
    return;
  }

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
}

uint32_t SoluxSIMDFor::decodeWithMeta(const char* encoded, uint32_t inSz, uint32_t* out, uint32_t& outSz,
                                      uint32_t minval, uint8_t bits) {
  unused(inSz);
  const uint32_t* in = (const uint32_t*) encoded;
  if (bits == 0) {
    for (uint32_t i = 0; i < outSz; ++i) out[i] = minval;
    return 0;
  }
  if (bits == 32) {
    memcpy(out, in, outSz * sizeof(uint32_t));
    return outSz * sizeof(uint32_t);
  }

  uint32_t tmp[128];
  uint32_t k = 0;
  for (; k + 128 <= outSz; k += 128) {
    FastPForLib::usimdunpack((const __m128i*) in, out + k, bits);
    if (minval) for (uint32_t i = 0; i < 128; ++i) out[k + i] += minval;
    in += 4 * bits;
  }
  if (k < outSz) {  // compact partial tail (matches packTail)
    const uint32_t rem = outSz - k;
    unpackTail(in, rem, bits, tmp);
    for (uint32_t i = 0; i < rem; ++i) out[k + i] = tmp[i] + minval;
    in += tailWords(rem, bits);
  }
  return (char*) in - encoded;
}

void SoluxSIMDFor::encodeBlock(uint32_t* in, uint32_t inSz, char* target, uint32_t& outSz) {
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

uint32_t SoluxSIMDFor::decodeBlock(const char* compressed, uint32_t inSz, uint32_t* out, uint32_t& outSz) {
  if (outSz == 0) return 0;
  const uint32_t* in = (const uint32_t*) compressed;
  const uint32_t m = in[0];
  const uint32_t M = in[1];
  const int b = std::bit_width((uint32_t) (M - m));
  const auto readSize = decodeWithMeta((const char*) (in + 2), inSz - 2 * sizeof(uint32_t), out, outSz, m, (uint8_t) b);
  return readSize + 2 * sizeof(uint32_t);
}

uint32_t SoluxSIMDFor::select(const char* compressed, uint32_t nValues, uint32_t index) {
  const uint32_t* in = (const uint32_t*) compressed;
  const uint32_t m = in[0];
  const uint32_t M = in[1];
  const int b = std::bit_width((uint32_t) (M - m));
  return selectWithMeta((const char*) (in + 2), nValues, index, m, (uint8_t) b);
}

// --- SoluxPFOR (positions / term-frequencies, no delta) ---

void SoluxPFOR::encodeBlock(uint32_t* in, uint32_t inSz, char* out, uint32_t& outSz) {
  unused(inSz);
  assert(inSz == BLOCK_SIZE);
  encodeBlockPFor(in, out, outSz);
}

uint32_t SoluxPFOR::decodeBlock(const char* in, uint32_t inSz, uint32_t* out, uint32_t& outSz) {
  unused(inSz);
  assert(outSz == BLOCK_SIZE);
  unused(outSz);
  return decodeBlockPFor(in, out);
}

// --- SoluxPFORd (documents, delta) ---

void SoluxPFORd::encodeBlock(uint32_t* in, uint32_t inSz, char* out, uint32_t& outSz) {
  unused(inSz);
  assert(inSz == BLOCK_SIZE);
  FastPForLib::Delta::fastDelta(in, BLOCK_SIZE);  // adjacent delta, in place
  encodeBlockPFor(in, out, outSz);
}

uint32_t SoluxPFORd::decodeBlock(const char* in, uint32_t inSz, uint32_t* out, uint32_t& outSz) {
  unused(inSz);
  assert(outSz == BLOCK_SIZE);
  unused(outSz);
  auto ret = decodeBlockPFor(in, out);
  FastPForLib::Delta::fastinverseDelta2(out, BLOCK_SIZE);
  return ret;
}

// --- IndexCodec statics ---

IndexCodec::DocsCodec IndexCodec::docCodec;
IndexCodec::PositionsCodec IndexCodec::posCodec;
IndexCodec::TFreqCodec& IndexCodec::tfreqCodec = IndexCodec::posCodec;
IndexCodec::NumericCodec IndexCodec::numericCodec;

}  // namespace solux
