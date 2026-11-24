// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <bit>
#include <cassert>
#include <cstdint>
#include <cstring>

#include "luxir/util/luxir_util.h"

namespace luxir {

// Tight bit-packing for a term's last, partial docs block (at most
// MAX_VALUES docs), written by PostingsWriter::endTerm:
//
//   [widths: u8][tfBits: u8, only when the widths byte escapes it]
//   [bit stream: n x docBits, then n x tfBits, LSB first, padded to a byte]
//
// The widths byte holds docBits in its low 5 bits and a tf width code in its
// high 3 bits: codes 0-6 are tfBits itself, code 7 means tfBits (7-31)
// follows in its own byte.  Docs are stored as gap - 1 from the previous doc
// (the last doc of the term's previous block, or -1 for a term with no full
// block, so its first doc is stored as is).  Tfs are stored as tf - 1, so a
// tail whose tfs are all 1 (and every docs-only tail) has tfBits 0 and no tf
// bits at all; likewise consecutive docs have docBits 0.
//
// Decoding reads 8-byte little-endian words with unaligned loads, so it may
// read up to OVERREAD bytes past the encoded bytes.  Postings files reserve
// SVB_OVERREAD_PAD (16) trailing bytes, which covers that.
namespace PackedTail {

static constexpr uint32_t MAX_VALUES = 127;
static constexpr uint32_t OVERREAD = 7;
static constexpr uint32_t TF_ESCAPE = 7;
// Header bytes plus a full bit stream of 31-bit docs and 31-bit tfs.
static constexpr uint32_t MAX_BYTES = 2 + (MAX_VALUES * 62 + 7) / 8;

struct Header {
  uint32_t docBits = 0;
  uint32_t tfBits = 0;
  uint32_t headerBytes = 1;

  // Bytes of the whole encoded tail of n values, header included.
  uint32_t encodedBytes(uint32_t n) const {
    return headerBytes + (n * (docBits + tfBits) + 7) / 8;
  }
};

inline uint32_t bitWidth(uint32_t v) {
  return 32 - (uint32_t) std::countl_zero(v);
}

inline Header readHeader(const uint8_t* in) {
  Header h;
  h.docBits = in[0] & 31u;
  uint32_t code = (uint32_t) in[0] >> 5;
  if (code == TF_ESCAPE) {
    h.tfBits = in[1];
    h.headerBytes = 2;
    assert(h.tfBits >= TF_ESCAPE && h.tfBits <= 31);
  } else {
    h.tfBits = code;
  }
  return h;
}

namespace detail {

inline uint64_t load64(const uint8_t* p) {
  uint64_t w;
  memcpy(&w, p, sizeof(w));
  static_assert(std::endian::native == std::endian::little);
  return w;
}

// Visit n values of BITS bits starting at bit `shift` (0-7) of `in`.  Groups
// of 8 values span exactly BITS bytes, so the unrolled body addresses every
// value at a fixed offset from the group start.
template <uint32_t BITS, typename Fn>
LUXIR_INLINE inline void unpackFixed(const uint8_t* in, uint32_t shift, uint32_t n, Fn&& fn) {
  static_assert(BITS >= 1 && BITS <= 31);
  constexpr uint64_t MASK = (1ull << BITS) - 1;
  uint32_t i = 0;
  for (; i + 8 <= n; i += 8, in += BITS) {
#pragma GCC unroll 8
    for (uint32_t j = 0; j < 8; j++) {
      const uint32_t bit = shift + j * BITS;
      fn(i + j, (uint32_t) ((load64(in + (bit >> 3)) >> (bit & 7)) & MASK));
    }
  }
  for (uint32_t j = 0; i < n; i++, j++) {
    const uint32_t bit = shift + j * BITS;
    fn(i, (uint32_t) ((load64(in + (bit >> 3)) >> (bit & 7)) & MASK));
  }
}

template <typename Fn>
LUXIR_INLINE inline void unpack(const uint8_t* in, uint32_t shift, uint32_t n, uint32_t bits,
                         Fn&& fn) {
  switch (bits) {
#define LUXIR_PACKED_TAIL_CASE(b) \
  case b: unpackFixed<b>(in, shift, n, fn); return;
    LUXIR_PACKED_TAIL_CASE(1) LUXIR_PACKED_TAIL_CASE(2) LUXIR_PACKED_TAIL_CASE(3)
    LUXIR_PACKED_TAIL_CASE(4) LUXIR_PACKED_TAIL_CASE(5) LUXIR_PACKED_TAIL_CASE(6)
    LUXIR_PACKED_TAIL_CASE(7) LUXIR_PACKED_TAIL_CASE(8) LUXIR_PACKED_TAIL_CASE(9)
    LUXIR_PACKED_TAIL_CASE(10) LUXIR_PACKED_TAIL_CASE(11) LUXIR_PACKED_TAIL_CASE(12)
    LUXIR_PACKED_TAIL_CASE(13) LUXIR_PACKED_TAIL_CASE(14) LUXIR_PACKED_TAIL_CASE(15)
    LUXIR_PACKED_TAIL_CASE(16) LUXIR_PACKED_TAIL_CASE(17) LUXIR_PACKED_TAIL_CASE(18)
    LUXIR_PACKED_TAIL_CASE(19) LUXIR_PACKED_TAIL_CASE(20) LUXIR_PACKED_TAIL_CASE(21)
    LUXIR_PACKED_TAIL_CASE(22) LUXIR_PACKED_TAIL_CASE(23) LUXIR_PACKED_TAIL_CASE(24)
    LUXIR_PACKED_TAIL_CASE(25) LUXIR_PACKED_TAIL_CASE(26) LUXIR_PACKED_TAIL_CASE(27)
    LUXIR_PACKED_TAIL_CASE(28) LUXIR_PACKED_TAIL_CASE(29) LUXIR_PACKED_TAIL_CASE(30)
    LUXIR_PACKED_TAIL_CASE(31)
#undef LUXIR_PACKED_TAIL_CASE
    default:
      assert(bits == 0);
      for (uint32_t i = 0; i < n; i++) fn(i, 0u);
      return;
  }
}

// Append the low `bits` bits of each value to a byte-granular LSB-first stream.
class BitWriter {
  uint8_t* out;
  uint64_t acc = 0;
  uint32_t fill = 0;

public:
  explicit BitWriter(uint8_t* out) : out(out) {}

  void put(uint32_t value, uint32_t bits) {
    assert(bits <= 31 && (bits == 31 || value < (1u << bits)));
    acc |= (uint64_t) value << fill;
    fill += bits;
    while (fill >= 8) {
      *out++ = (uint8_t) acc;
      acc >>= 8;
      fill -= 8;
    }
  }

  uint8_t* finish() {
    if (fill > 0) {
      *out++ = (uint8_t) acc;
    }
    return out;
  }
};

}  // namespace detail

// Encode n (1..MAX_VALUES) ascending docs after prevDoc (UINT32_MAX for a
// term's first block) and, when tfs is not null, their tfs (each >= 1).
// Returns the encoded byte count; out must hold MAX_BYTES.
inline uint32_t encode(const int32_t* docs, const int32_t* tfs, uint32_t n, uint32_t prevDoc,
                       uint8_t* out) {
  assert(n >= 1 && n <= MAX_VALUES);
  uint32_t docOr = 0;
  uint32_t tfOr = 0;
  uint32_t prev = prevDoc;
  for (uint32_t i = 0; i < n; i++) {
    assert((uint32_t) docs[i] > prev || prev == UINT32_MAX);
    docOr |= (uint32_t) docs[i] - prev - 1;
    prev = (uint32_t) docs[i];
    if (tfs != nullptr) {
      assert(tfs[i] >= 1);
      tfOr |= (uint32_t) tfs[i] - 1;
    }
  }
  Header h;
  h.docBits = bitWidth(docOr);
  h.tfBits = bitWidth(tfOr);
  assert(h.docBits <= 31 && h.tfBits <= 31);
  uint8_t* p = out;
  if (h.tfBits < TF_ESCAPE) {
    *p++ = (uint8_t) (h.docBits | (h.tfBits << 5));
  } else {
    *p++ = (uint8_t) (h.docBits | (TF_ESCAPE << 5));
    *p++ = (uint8_t) h.tfBits;
    h.headerBytes = 2;
  }
  detail::BitWriter bits(p);
  prev = prevDoc;
  for (uint32_t i = 0; i < n; i++) {
    bits.put((uint32_t) docs[i] - prev - 1, h.docBits);
    prev = (uint32_t) docs[i];
  }
  if (h.tfBits != 0) {
    for (uint32_t i = 0; i < n; i++) {
      bits.put((uint32_t) tfs[i] - 1, h.tfBits);
    }
  }
  p = bits.finish();
  assert((uint32_t) (p - out) == h.encodedBytes(n));
  return (uint32_t) (p - out);
}

// Decode the n docs that follow the header into out; prevDoc as in encode().
inline void decodeDocs(const uint8_t* in, const Header& h, uint32_t n, uint32_t prevDoc,
                       int32_t* out) {
  const uint8_t* stream = in + h.headerBytes;
  uint32_t prev = prevDoc;
  detail::unpack(stream, 0, n, h.docBits, [&](uint32_t i, uint32_t v) {
    prev += v + 1;
    out[i] = (int32_t) prev;
  });
}

// Decode the n tfs (all 1 when tfBits is 0) into out.
inline void decodeTfs(const uint8_t* in, const Header& h, uint32_t n, int32_t* out) {
  const uint32_t bitOffset = n * h.docBits;
  const uint8_t* stream = in + h.headerBytes + (bitOffset >> 3);
  detail::unpack(stream, bitOffset & 7, n, h.tfBits, [&](uint32_t i, uint32_t v) {
    out[i] = (int32_t) (v + 1);
  });
}

// The last of the n docs, without storing any: prevDoc plus the n gaps.
inline int32_t lastDoc(const uint8_t* in, const Header& h, uint32_t n, uint32_t prevDoc) {
  uint32_t gapSum = 0;
  detail::unpack(in + h.headerBytes, 0, n, h.docBits, [&](uint32_t, uint32_t v) {
    gapSum += v;
  });
  return (int32_t) (prevDoc + n + gapSum);
}

// The first doc of a term's first (tail-only) block, without decoding the rest.
inline int32_t firstDoc(const uint8_t* in) {
  Header h = readHeader(in);
  if (h.docBits == 0) {
    return 0;
  }
  uint64_t word = detail::load64(in + h.headerBytes);
  return (int32_t) (word & ((1ull << h.docBits) - 1));
}

}  // namespace PackedTail
}  // namespace luxir
