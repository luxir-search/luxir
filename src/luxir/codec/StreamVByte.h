#pragma once
#include <cstdint>

// StreamVByte declarations from FastPFOR deps/FastPFOR/src/streamvbyte.c, exposed
// through C linkage for the postings docs tail (the < DOCS_BLOCK_SIZE leftover
// after full blocks). The _d1 variants delta-decode docids with a prefix sum; the
// plain variants are used for term freqs. Both formats use separate control (key)
// and data streams.
//
// Reads use the AVX decoders. They process full 32-value chunks with SIMD and hand
// any partial final group to the scalar decoder, so they write exactly `count`
// outputs and return an exact data-end pointer. The input-side hazard is the final
// 16-byte SIMD load, which may read up to SVB_OVERREAD_PAD bytes past the encoded
// data. Store files are exact-sized (RAMDir buffers; FSDirectory mmaps), so
// PostingsWriter::finish() reserves that trailing slack on every pure-data file.
// File 0 is covered by the field index and segment-info trailer that always follows
// its postings.
extern "C" {
  uint8_t* svb_encode_scalar_d1_init(const uint32_t* in, uint8_t* keyPtr, uint8_t* dataPtr,
                                     uint32_t count, uint32_t prev);
  uint8_t* svb_encode_scalar(const uint32_t* in, uint8_t* keyPtr, uint8_t* dataPtr, uint32_t count);
  uint8_t* svb_decode_avx_d1_init(uint32_t* out, uint8_t* keyPtr, uint8_t* dataPtr,
                                  uint64_t count, uint32_t prev);
  uint8_t* svb_decode_avx_simple(uint32_t* out, uint8_t* keyPtr, uint8_t* dataPtr, uint64_t count);
}

namespace luxir {
// Number of control (key) bytes for `count` values: 2 bits each, packed 4 per byte.
inline uint32_t svbKeyBytes(uint32_t count) { return (count + 3) / 4; }

// Total encoded bytes for one StreamVByte run without decoding its values.
// Each 2-bit control code stores the data width minus one.  The key stream is
// followed immediately by the data stream, so callers can skip a tail whose
// format has no separate byte-length prefix.
inline uint32_t svbEncodedBytes(const uint8_t* keys, uint32_t count) {
  const uint32_t keyBytes = svbKeyBytes(count);
  uint32_t dataBytes = 0;
  for (uint32_t i = 0; i < count; i += 4) {
    const uint8_t key = keys[i >> 2];
    const uint32_t values = count - i < 4 ? count - i : 4;
    dataBytes += values;
    for (uint32_t j = 0; j < values; j++) {
      dataBytes += (key >> (j * 2)) & 3u;
    }
  }
  return keyBytes + dataBytes;
}

// Bytes the StreamVByte AVX decoders may read past the encoded data (one 16-byte SIMD
// load on the final group).  Pure-data postings files reserve this much trailing slack.
inline constexpr uint32_t SVB_OVERREAD_PAD = 16;
}
