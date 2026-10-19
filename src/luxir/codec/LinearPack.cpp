// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/codec/LinearPack.h"

#if defined(__AVX2__)
#include <immintrin.h>
#include <utility>

namespace luxir {

namespace {

template <uint8_t Bits, uint8_t Phase>
[[gnu::always_inline]] inline void unpackGroup(
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
[[gnu::always_inline]] inline void unpackFull(
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
[[gnu::always_inline]] inline void unpackFull32(
    const char* in, uint32_t* values, std::index_sequence<Groups...>) {
  static_assert(sizeof...(Groups) == N / 8);
  (_mm256_storeu_si256(
       (__m256i*)(values + Groups * 8),
       _mm256_loadu_si256((const __m256i*)(in + Groups * 32))),
   ...);
}

} // namespace

template <uint32_t N, uint8_t Bits>
void LinearPack::unpackBlockBits(
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

template void LinearPack::unpackBlockBits<128, 1>(const char*, uint64_t, uint32_t, uint32_t, uint32_t*);
template void LinearPack::unpackBlockBits<128, 2>(const char*, uint64_t, uint32_t, uint32_t, uint32_t*);
template void LinearPack::unpackBlockBits<128, 3>(const char*, uint64_t, uint32_t, uint32_t, uint32_t*);
template void LinearPack::unpackBlockBits<128, 4>(const char*, uint64_t, uint32_t, uint32_t, uint32_t*);
template void LinearPack::unpackBlockBits<128, 5>(const char*, uint64_t, uint32_t, uint32_t, uint32_t*);
template void LinearPack::unpackBlockBits<128, 6>(const char*, uint64_t, uint32_t, uint32_t, uint32_t*);
template void LinearPack::unpackBlockBits<128, 7>(const char*, uint64_t, uint32_t, uint32_t, uint32_t*);
template void LinearPack::unpackBlockBits<128, 8>(const char*, uint64_t, uint32_t, uint32_t, uint32_t*);
template void LinearPack::unpackBlockBits<128, 9>(const char*, uint64_t, uint32_t, uint32_t, uint32_t*);
template void LinearPack::unpackBlockBits<128, 10>(const char*, uint64_t, uint32_t, uint32_t, uint32_t*);
template void LinearPack::unpackBlockBits<128, 11>(const char*, uint64_t, uint32_t, uint32_t, uint32_t*);
template void LinearPack::unpackBlockBits<128, 12>(const char*, uint64_t, uint32_t, uint32_t, uint32_t*);
template void LinearPack::unpackBlockBits<128, 13>(const char*, uint64_t, uint32_t, uint32_t, uint32_t*);
template void LinearPack::unpackBlockBits<128, 14>(const char*, uint64_t, uint32_t, uint32_t, uint32_t*);
template void LinearPack::unpackBlockBits<128, 15>(const char*, uint64_t, uint32_t, uint32_t, uint32_t*);
template void LinearPack::unpackBlockBits<128, 16>(const char*, uint64_t, uint32_t, uint32_t, uint32_t*);
template void LinearPack::unpackBlockBits<128, 17>(const char*, uint64_t, uint32_t, uint32_t, uint32_t*);
template void LinearPack::unpackBlockBits<128, 18>(const char*, uint64_t, uint32_t, uint32_t, uint32_t*);
template void LinearPack::unpackBlockBits<128, 19>(const char*, uint64_t, uint32_t, uint32_t, uint32_t*);
template void LinearPack::unpackBlockBits<128, 20>(const char*, uint64_t, uint32_t, uint32_t, uint32_t*);
template void LinearPack::unpackBlockBits<128, 21>(const char*, uint64_t, uint32_t, uint32_t, uint32_t*);
template void LinearPack::unpackBlockBits<128, 22>(const char*, uint64_t, uint32_t, uint32_t, uint32_t*);
template void LinearPack::unpackBlockBits<128, 23>(const char*, uint64_t, uint32_t, uint32_t, uint32_t*);
template void LinearPack::unpackBlockBits<128, 24>(const char*, uint64_t, uint32_t, uint32_t, uint32_t*);
template void LinearPack::unpackBlockBits<128, 25>(const char*, uint64_t, uint32_t, uint32_t, uint32_t*);
template void LinearPack::unpackBlockBits<128, 26>(const char*, uint64_t, uint32_t, uint32_t, uint32_t*);
template void LinearPack::unpackBlockBits<128, 27>(const char*, uint64_t, uint32_t, uint32_t, uint32_t*);
template void LinearPack::unpackBlockBits<128, 28>(const char*, uint64_t, uint32_t, uint32_t, uint32_t*);
template void LinearPack::unpackBlockBits<128, 29>(const char*, uint64_t, uint32_t, uint32_t, uint32_t*);
template void LinearPack::unpackBlockBits<128, 30>(const char*, uint64_t, uint32_t, uint32_t, uint32_t*);
template void LinearPack::unpackBlockBits<128, 31>(const char*, uint64_t, uint32_t, uint32_t, uint32_t*);
template void LinearPack::unpackBlockBits<128, 32>(const char*, uint64_t, uint32_t, uint32_t, uint32_t*);

} // namespace luxir

#endif
