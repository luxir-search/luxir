#pragma once

#include <cstdint>
#include <cstring>
#include <cassert>
#include <stdexcept>
#include <bit>
#include <emmintrin.h>  // __m128i, for the inline numeric decode loop
#include "solux/util/solux_util.h"

// Integer codecs, backed by FastPFOR's SIMD bit-packing kernels (which have
// native ARM NEON support; the engine migrated off SIMDCompressionAndIntersection
// to FastPFOR for that). The heavy FastPFOR headers are pulled in only by
// Codec.cpp, so this widely-included header stays light. The inline hot paths
// (SoluxSIMDFor::selectWithMeta, decodeWithMeta) need only __m128i plus the one
// forward-declared unpack kernel below, not FastPFOR's headers.

namespace FastPForLib {
// FastPFOR's unaligned SIMD bit-unpack (defined in the fastpfor static lib). Forward-
// declared so the inline decodeWithMeta can call it without pulling in common.h.
void usimdunpack(const __m128i* __restrict__ in, uint32_t* __restrict__ out, uint32_t bit);
}

namespace solux {

class U32Codec {
public:
  virtual ~U32Codec() = default;

  // input size is in ints, output size is size in bytes of the buffer.
  // outSz is updated to reflect how much data was written.
  virtual void encodeBlock(uint32_t* in, uint32_t inSz, char* out, uint32_t &outSz) = 0;

  // inSz is in bytes, outSz is number of ints.
  // outSz is updated to reflect how many ints were written.
  // returns the number of bytes read from the input.
  virtual uint32_t decodeBlock(const char* in, uint32_t inSz, uint32_t* out, uint32_t &outSz) = 0;

  virtual uint32_t select(const char* compressed, uint32_t blockSize, uint32_t index) {
    unused(compressed);
    unused(blockSize);
    unused(index);
    throw std::runtime_error("select not implemented for this codec");
  }
};

// U64 codec can handle both 32 and 64 bit integers.
class U64Codec {
public:
  virtual ~U64Codec() = default;

  // input size is in ints, output size is size in bytes of the buffer.
  // outSz is updated to reflect how much data was written.
  virtual void encodeBlock(uint32_t* in, uint32_t inSz, char* out, uint32_t &outSz) = 0;

  // inSz is in bytes, outSz is number of ints.
  // outSz is updated to reflect how many ints were written.
  // returns the number of bytes read from the input.
  virtual uint32_t decodeBlock(const char* in, uint32_t inSz, uint32_t* out, uint32_t &outSz) = 0;

  // TODO: 64 bit variants
};


/// No compression, just remembers the length and uses memcpy to copy the array
class SimpleCodec : public U32Codec {
public:
  ~SimpleCodec() override = default;

  void encodeBlock(uint32_t* in, uint32_t inSz, char* out, uint32_t &outSz) override {
    storeUnaligned<uint32_t>(out, inSz);
    memcpy(out+sizeof(uint32_t), in, inSz * sizeof(uint32_t));
    outSz = (inSz+1)*sizeof(uint32_t);
  }

  // Hmmm, some codecs may be able to derive the size of the encoded data, and some may not!
  uint32_t decodeBlock(const char* in, uint32_t inSz, uint32_t* out, uint32_t &outSz) override {
    unused(inSz);
    outSz = loadUnaligned<uint32_t>(in);
    assert((outSz+1)*sizeof(uint32_t) <= inSz);
    memcpy(out, in+sizeof(uint32_t), outSz*sizeof(uint32_t));
    return (outSz+1)*sizeof(uint32_t);
  }
};


/// Frame-of-reference / binary-packing numeric codec on FastPFOR's SIMD
/// bit-packing kernels (which have native ARM NEON support).
///
/// Meta-driven: the caller supplies minval and the bit width (both already kept
/// in block metadata), so the *WithMeta path stores no per-block header. Values
/// are bit-packed in 4 interleaved lanes of 32 (the standard FastPFOR layout),
/// which selectWithMeta addresses directly for random access.
///
/// Full 128-value blocks go through the SIMD kernels; the partial tail block is
/// packed compactly with scalar code (same lane layout), so the encoded size
/// stays within the caller's value-count buffer budget and nothing is read or
/// written past the encoded bytes.
class SoluxSIMDFor : public U32Codec {
public:
  ~SoluxSIMDFor() override = default;

  // encodeWithMeta: caller supplies minval + bits (from block metadata); stores no header.
  void encodeWithMeta(uint32_t* in, uint32_t inSz, char* target, uint32_t& outSz, uint32_t minval, uint8_t bits);

  static constexpr uint64_t byteSize(uint32_t count, uint8_t bits) {
    if (bits == 0) return 0;
    uint64_t fullBlocks = count / 128;
    uint32_t tail = count % 128;
    uint64_t bytes = fullBlocks * 4 * bits * sizeof(uint32_t);
    if (tail != 0) {
      uint64_t rows = (tail + 3) / 4;
      bytes += 4 * ((rows * bits + 31) / 32) * sizeof(uint32_t);
    }
    return bytes;
  }

  // decodeWithMeta: inline so the column readers' decode loops fold it in and DCE around it.
  // Production frame-of-reference min is always 0, so a value IS its unpacked delta -- no minval,
  // no unused size arg. The 0/32-bit edge formats and a partial tail are cold (out-of-line).
  inline uint32_t decodeWithMeta(const char* encoded, uint32_t* out, uint32_t outSz, uint8_t bits) {
    const uint32_t* in = (const uint32_t*) encoded;
    uint32_t k = 0;
    for (; k + 128 <= outSz; k += 128) {
      FastPForLib::usimdunpack((const __m128i*) in, out + k, bits);  // bits 0 and 32 handled by the kernel
      in += 4 * bits;
    }
    if (k < outSz) in += decodeTailMeta(in, outSz - k, out + k, bits);
    return (uint32_t) ((const char*) in - encoded);
  }

  // decode a single block (128) or less.
  inline void decodeSingleBlock(const char* encoded, uint32_t* out, uint32_t outSz, uint8_t bits) {
    assert(outSz <= 128);
    if (outSz < 128) [[unlikely]] {
      decodeTailMeta((const uint32_t*)encoded, outSz, out, bits);
    } else {
      FastPForLib::usimdunpack((const __m128i*) encoded, out, bits);
    }
  }

  static uint32_t decodeTailMeta(const uint32_t* in, uint32_t rem, uint32_t* out, uint8_t bits);  // partial tail (<128)

  // U32Codec interface: self-describing variants that store min/max up front
  // (used by codec tests/benchmarks).
  void encodeBlock(uint32_t* in, uint32_t inSz, char* out, uint32_t& outSz) override;
  uint32_t decodeBlock(const char* in, uint32_t inSz, uint32_t* out, uint32_t& outSz) override;
  uint32_t select(const char* compressed, uint32_t nValues, uint32_t index) override;

  // Random access into a meta-driven block. Pure bit-math (no FastPFOR symbols),
  // so it lives inline.
  inline uint32_t selectWithMeta(const char* compressed, uint32_t blockSize, uint32_t index, uint32_t minval, uint8_t bits) {
    unused(blockSize);
    const uint32_t* in = (const uint32_t*) compressed;
    if (bits == 0) {
      return minval;  // all values equal minval, nothing encoded (hand-rolled, so guard the read).
    }
    in += index / 128 * 4 * bits;
    const uint32_t slot = index % 128;
    const uint32_t lane = slot % 4;                /* 4 interleaved lanes */
    const uint32_t bitsinlane = (slot / 4) * bits; /* bits already used in lane */
    const uint32_t firstwordinlane = bitsinlane / 32;
    const uint32_t secondwordinlane = (bitsinlane + bits - 1) / 32;
    const uint32_t firstpart = in[4 * firstwordinlane + lane] >> (bitsinlane % 32);
    const uint32_t mask = (uint32_t)((1ull << bits) - 1);  /* 64-bit shift avoids UB at bits==32 */
    if (firstwordinlane == secondwordinlane) {
      /* easy common case */
      return minval + (firstpart & mask);
    } else {
      /* harder case where we need to combine two words */
      const uint32_t secondpart = in[4 * firstwordinlane + 4 + lane];
      const uint32_t usablebitsinfirstword = 32 - (bitsinlane % 32);
      return minval + ((firstpart | (secondpart << usablebitsinfirstword)) & mask);
    }
  }
};

/// PForDelta on FastPFOR's SIMD bit-packing kernels -- the positions /
/// term-frequencies codec. Encodes exactly one BLOCK_SIZE-value block per call:
/// a uniform bit-width SIMD-packed base plus a patched exception stream. The
/// decode path is hand-rolled with a stack-allocated bit packer (no dynamic
/// allocation).
class SoluxPFOR : public U32Codec {
public:
  ~SoluxPFOR() override = default;
  void encodeBlock(uint32_t* in, uint32_t inSz, char* out, uint32_t& outSz) override;
  uint32_t decodeBlock(const char* in, uint32_t inSz, uint32_t* out, uint32_t& outSz) override;

  // Advance over one encoded block without unpacking it: the 2-3 byte block
  // header fully determines every section's size. Returns the encoded byte
  // length (the same value decodeBlock would return).
  uint32_t skipBlock(const char* in, uint32_t inSz);
};

/// Delta-coded PForDelta -- the documents codec. Applies an adjacent delta over
/// the block before PFor encoding and a prefix sum after decoding. The block's
/// first id is coded as a delta from `base` (the last doc of the previous block,
/// 0 for the first / a standalone block), so cross-block ids stay small instead
/// of every block's first id being a large absolute outlier.
/// NOTE: encodeBlock mutates `in` in place (the delta).
class SoluxPFORd : public U32Codec {
public:
  ~SoluxPFORd() override = default;

  // Base-aware production path: base = last doc id of the previous block (0 for
  // the first block of a term).  encodeBlock mutates `in`.
  void encodeBlock(uint32_t* in, uint32_t inSz, char* out, uint32_t& outSz, uint32_t base);
  uint32_t decodeBlock(const char* in, uint32_t inSz, uint32_t* out, uint32_t& outSz, uint32_t base);

  // U32Codec interface (base == 0): standalone, self-contained blocks, used by
  // the codec round-trip tests / benchmarks.
  void encodeBlock(uint32_t* in, uint32_t inSz, char* out, uint32_t& outSz) override {
    encodeBlock(in, inSz, out, outSz, 0);
  }
  uint32_t decodeBlock(const char* in, uint32_t inSz, uint32_t* out, uint32_t& outSz) override {
    return decodeBlock(in, inSz, out, outSz, 0);
  }
};


// The production codec set. Block-metadata-driven: the numeric codec stores no
// per-block min/max/size of its own.
class IndexCodec {
public:
  using PositionsCodec = SoluxPFOR;
  using DocsCodec = SoluxPFORd;
  using NumericCodec = SoluxSIMDFor;
  using TFreqCodec = PositionsCodec; // same type, but should also share instances for better performance

  static DocsCodec docCodec;
  static PositionsCodec posCodec;
  static TFreqCodec& tfreqCodec;
  static NumericCodec numericCodec;
};

} // end namespace
