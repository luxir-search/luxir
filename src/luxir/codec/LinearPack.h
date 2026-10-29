// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

#include "luxir/store/OutputStream.h"
#include "luxir/util/luxir_util.h"

namespace luxir {

class LinearPack {
public:
  // A 16-byte SIMD load on the last packed group (bulk decode, bits 11-25) can
  // read up to 10 bytes past the final value at 11-12 bits; select's u64 load
  // needs only 7. Pad to the larger so both stay in bounds at a file tail.
  static constexpr uint8_t TAIL_PAD = 10;

  static constexpr uint64_t packedByteSize(uint64_t count, uint8_t bits) {
    assert(bits <= 57);
    return (count * bits + 7) >> 3;
  }

  static constexpr uint64_t byteSize(uint64_t count, uint8_t bits) {
    return packedByteSize(count, bits) + TAIL_PAD;
  }

  static constexpr uint32_t mask32(uint8_t bits) {
    assert(bits <= 32);
    return bits == 0 ? 0 : (uint32_t)((1ull << bits) - 1);
  }

  static constexpr uint64_t mask64(uint8_t bits) {
    assert(bits <= 57);
    return bits == 0 ? 0 : (1ull << bits) - 1;
  }

  class Writer {
    OutputStream* out = nullptr;
    char* target = nullptr;
    std::vector<char>* vectorTarget = nullptr;
    uint64_t pending = 0;
    uint64_t count = 0;
    uint64_t written = 0;
    uint8_t bits = 0;
    uint8_t pendingBits = 0;
    bool finished = false;

    void writeByte(uint8_t value) {
      if (out != nullptr) {
        out->write((char)value);
      } else if (vectorTarget != nullptr) {
        vectorTarget->push_back((char)value);
      } else {
        *target++ = (char)value;
      }
      written++;
    }

  public:
    Writer(OutputStream& out, uint8_t bits) : out(&out), bits(bits) {
      assert(bits <= 57);
    }

    Writer(char* target, uint8_t bits) : target(target), bits(bits) {
      assert(target != nullptr);
      assert(bits <= 57);
    }

    Writer(std::vector<char>& target, uint8_t bits)
        : vectorTarget(&target), bits(bits) {
      assert(bits <= 57);
    }

    void append(uint64_t value) {
      assert(!finished);
      assert(bits == 0 ? value == 0 : value <= mask64(bits));
      if (bits != 0) {
        pending |= value << pendingBits;
        uint8_t totalBits = pendingBits + bits;
        while (totalBits >= 8) {
          writeByte((uint8_t)pending);
          pending >>= 8;
          totalBits -= 8;
        }
        pendingBits = totalBits;
      }
      count++;
    }

    // writeTailPad appends TAIL_PAD zero bytes so a standalone buffer (e.g. an
    // in-RAM OrdMap column) can be read with the bulk/point loads' overread.
    // Index files reserve their own trailing slack (SVB_OVERREAD_PAD per data
    // file) and pass false, keeping the pad -- an artifact of the reader's SIMD
    // load width, not of the data -- out of the on-disk format.
    uint64_t finish(bool writeTailPad = true) {
      assert(!finished);
      if (pendingBits != 0) {
        writeByte((uint8_t)pending);
      }
      if (writeTailPad) {
        for (uint8_t i = 0; i < TAIL_PAD; i++) {
          writeByte(0);
        }
      }
      finished = true;
      assert(written == (writeTailPad ? byteSize(count, bits)
                                      : packedByteSize(count, bits)));
      return written;
    }
  };

  static uint32_t select32(const char* base, uint64_t idx, uint8_t bits,
                           uint32_t mask) {
    assert(bits <= 32);
    if (bits == 0) return 0;
    uint64_t bitPos = (uint64_t)idx * bits;
    uint64_t word = loadUnaligned<uint64_t>(base + (bitPos >> 3));
    return (uint32_t)(word >> (bitPos & 7)) & mask;
  }

  static uint64_t select64(const char* base, uint64_t idx, uint8_t bits,
                           uint64_t mask) {
    assert(bits <= 57);
    if (bits == 0) return 0;
    uint64_t bitPos = idx * bits;
    uint64_t word = loadUnaligned<uint64_t>(base + (bitPos >> 3));
    return (word >> (bitPos & 7)) & mask;
  }

private:
#if defined(__AVX2__)
  // One AVX2 kernel per bit width. LinearPack.cpp defines them and
  // instantiates N = 128, so they compile once instead of in every translation
  // unit that decodes a packed column.
  template <uint32_t N, uint8_t Bits>
  [[gnu::noinline]] static void unpackBlockBits(
      const char* base, uint64_t idx, uint32_t count, uint32_t mask,
      uint32_t* values);

public:
  template <uint32_t N>
  static void unpackBlock(const char* base, uint64_t idx, uint32_t count,
                          uint8_t bits, uint32_t mask, uint32_t* values) {
    assert(count <= N);
    switch (bits) {
      case 1: unpackBlockBits<N, 1>(base, idx, count, mask, values); return;
      case 2: unpackBlockBits<N, 2>(base, idx, count, mask, values); return;
      case 3: unpackBlockBits<N, 3>(base, idx, count, mask, values); return;
      case 4: unpackBlockBits<N, 4>(base, idx, count, mask, values); return;
      case 5: unpackBlockBits<N, 5>(base, idx, count, mask, values); return;
      case 6: unpackBlockBits<N, 6>(base, idx, count, mask, values); return;
      case 7: unpackBlockBits<N, 7>(base, idx, count, mask, values); return;
      case 8: unpackBlockBits<N, 8>(base, idx, count, mask, values); return;
      case 9: unpackBlockBits<N, 9>(base, idx, count, mask, values); return;
      case 10: unpackBlockBits<N, 10>(base, idx, count, mask, values); return;
      case 11: unpackBlockBits<N, 11>(base, idx, count, mask, values); return;
      case 12: unpackBlockBits<N, 12>(base, idx, count, mask, values); return;
      case 13: unpackBlockBits<N, 13>(base, idx, count, mask, values); return;
      case 14: unpackBlockBits<N, 14>(base, idx, count, mask, values); return;
      case 15: unpackBlockBits<N, 15>(base, idx, count, mask, values); return;
      case 16: unpackBlockBits<N, 16>(base, idx, count, mask, values); return;
      case 17: unpackBlockBits<N, 17>(base, idx, count, mask, values); return;
      case 18: unpackBlockBits<N, 18>(base, idx, count, mask, values); return;
      case 19: unpackBlockBits<N, 19>(base, idx, count, mask, values); return;
      case 20: unpackBlockBits<N, 20>(base, idx, count, mask, values); return;
      case 21: unpackBlockBits<N, 21>(base, idx, count, mask, values); return;
      case 22: unpackBlockBits<N, 22>(base, idx, count, mask, values); return;
      case 23: unpackBlockBits<N, 23>(base, idx, count, mask, values); return;
      case 24: unpackBlockBits<N, 24>(base, idx, count, mask, values); return;
      case 25: unpackBlockBits<N, 25>(base, idx, count, mask, values); return;
      case 26: unpackBlockBits<N, 26>(base, idx, count, mask, values); return;
      case 27: unpackBlockBits<N, 27>(base, idx, count, mask, values); return;
      case 28: unpackBlockBits<N, 28>(base, idx, count, mask, values); return;
      case 29: unpackBlockBits<N, 29>(base, idx, count, mask, values); return;
      case 30: unpackBlockBits<N, 30>(base, idx, count, mask, values); return;
      case 31: unpackBlockBits<N, 31>(base, idx, count, mask, values); return;
      case 32: unpackBlockBits<N, 32>(base, idx, count, mask, values); return;
      default: assert(false); return;
    }
  }
#endif

public:
  static void unpack128(const char* base, uint64_t idx, uint32_t count,
                        uint8_t bits, uint32_t mask, uint32_t* values) {
    assert(count <= 128);
    assert(bits <= 32);
    // Production chunks are 128-aligned. Multiples of 8 are sufficient to
    // make idx * bits byte-aligned for every bit width.
    assert(idx % 8 == 0);
    if (bits == 0) {
      memset(values, 0, count * sizeof(*values));
      return;
    }

#if defined(__AVX2__)
    unpackBlock<128>(base, idx, count, bits, mask, values);
#else
    for (uint32_t i = 0; i < count; i++)
      values[i] = select32(base, idx + i, bits, mask);
#endif
  }

  static void unpack128(const char* base, uint64_t idx, uint32_t count,
                        uint8_t bits, uint64_t mask, uint64_t* values) {
    assert(count <= 128);
    for (uint32_t i = 0; i < count; i++) {
      values[i] = select64(base, idx + i, bits, mask);
    }
  }
};

} // namespace luxir
