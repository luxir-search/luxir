#pragma once

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <immintrin.h>
#include <utility>
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
  template <uint8_t Bits, uint8_t Phase>
  [[gnu::always_inline]] static inline void unpackGroup(
      const char* packedStart, __m128i bitMask, uint32_t* values) {
    static_assert(Bits > 0 && Bits < 32);
    static_assert(Phase == 0 || Phase == 4);

    constexpr uint8_t offset0 = Phase >> 3;
    constexpr uint8_t offset1 = (Phase + Bits) >> 3;
    constexpr uint8_t offset2 = (Phase + 2 * Bits) >> 3;
    constexpr uint8_t offset3 = (Phase + 3 * Bits) >> 3;
    const __m128i shuffle = _mm_setr_epi8(
        (char)offset0, (char)(offset0 + 1), (char)(offset0 + 2),
        (char)(offset0 + 3), (char)offset1, (char)(offset1 + 1),
        (char)(offset1 + 2), (char)(offset1 + 3), (char)offset2,
        (char)(offset2 + 1), (char)(offset2 + 2), (char)(offset2 + 3),
        (char)offset3, (char)(offset3 + 1), (char)(offset3 + 2),
        (char)(offset3 + 3));
    const __m128i shifts = _mm_setr_epi32(
        Phase, (Phase + Bits) & 7, (Phase + 2 * Bits) & 7,
        (Phase + 3 * Bits) & 7);

    // The final packed group plus TAIL_PAD covers these fixed-width loads:
    // <=10 bits hold 4 values plus a 7-bit phase in 8 bytes; wider widths take
    // one 16-byte load whose last-group overread is bounded by TAIL_PAD.
    __m128i packed;
    if constexpr (Bits <= 10) {
      packed = _mm_loadl_epi64((const __m128i*)packedStart);
    } else {
      packed = _mm_loadu_si128((const __m128i*)packedStart);
    }

    __m128i unpacked =
        _mm_srlv_epi32(_mm_shuffle_epi8(packed, shuffle), shifts);
    if constexpr (Bits > 25) {
      __m128i high = _mm_shuffle_epi8(
          _mm_loadu_si128(
              (const __m128i*)(packedStart + sizeof(uint32_t))),
          shuffle);
      high = _mm_sllv_epi32(
          high, _mm_sub_epi32(_mm_set1_epi32(32), shifts));
      unpacked = _mm_or_si128(unpacked, high);
    }
    unpacked = _mm_and_si128(unpacked, bitMask);
    _mm_storeu_si128((__m128i*)values, unpacked);
  }

  template <uint32_t N, uint8_t Bits, size_t... Groups>
  [[gnu::always_inline]] static inline void unpackFull(
      const char* in, __m128i bitMask, uint32_t* values,
      std::index_sequence<Groups...>) {
    static_assert(sizeof...(Groups) == N / 4);
    // Four even-width values return to phase 0. Odd widths alternate phases
    // 0 and 4, so each instantiation uses its minimal period.
    (unpackGroup<Bits, (Bits & 1) == 0 ? 0 : (Groups & 1) * 4>(
         in + (Groups * 4 * Bits >> 3), bitMask, values + Groups * 4),
     ...);
  }

  template <uint32_t N, size_t... Groups>
  [[gnu::always_inline]] static inline void unpackFull32(
      const char* in, uint32_t* values, std::index_sequence<Groups...>) {
    static_assert(sizeof...(Groups) == N / 8);
    (_mm256_storeu_si256(
         (__m256i*)(values + Groups * 8),
         _mm256_loadu_si256((const __m256i*)(in + Groups * 32))),
     ...);
  }

  template <uint32_t N, uint8_t Bits>
  [[gnu::noinline]] static void unpackBlockBits(
      const char* base, uint64_t idx, uint32_t count, uint32_t mask,
      uint32_t* values) {
    static_assert(N > 0 && N % 8 == 0);
    static_assert(Bits > 0 && Bits <= 32);

    const char* in = base + (idx * Bits >> 3);
    uint32_t i = 0;
    if constexpr (Bits == 32) {
      if (count == N) {
        unpackFull32<N>(
            in, values, std::make_index_sequence<N / 8>());
        return;
      }
      for (; i + 8 <= count; i += 8) {
        _mm256_storeu_si256(
            (__m256i*)(values + i),
            _mm256_loadu_si256((const __m256i*)(in + i * sizeof(uint32_t))));
      }
    } else {
      const __m128i bitMask = _mm_set1_epi32((int)mask);
      if (count == N) {
        unpackFull<N, Bits>(
            in, bitMask, values, std::make_index_sequence<N / 4>());
        return;
      }

      if constexpr ((Bits & 1) == 0) {
        for (; i + 4 <= count; i += 4) {
          unpackGroup<Bits, 0>(
              in + (i * Bits >> 3), bitMask, values + i);
        }
      } else {
        for (; i + 8 <= count; i += 8) {
          unpackGroup<Bits, 0>(
              in + (i * Bits >> 3), bitMask, values + i);
          unpackGroup<Bits, 4>(
              in + ((i + 4) * Bits >> 3), bitMask, values + i + 4);
        }
        if (i + 4 <= count) {
          unpackGroup<Bits, 0>(
              in + (i * Bits >> 3), bitMask, values + i);
          i += 4;
        }
      }
    }

    for (; i < count; i++) {
      values[i] = select32(base, idx + i, Bits, mask);
    }
  }

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
