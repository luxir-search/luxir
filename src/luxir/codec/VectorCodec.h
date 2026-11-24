// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdfloat>
#include <string_view>

namespace luxir {

/// Storage encoding of one vector field's column.  Every encoding stores one
/// fixed-size, self-describing value per vector, so a segment column is still
/// a plain fixed-size binary column and a merge of inputs in one encoding
/// copies values verbatim.
///
///   F32:  dims little-endian IEEE float32 values (4 * dims bytes).
///   F16:  dims IEEE binary16 values (2 * dims bytes).
///   BF16: dims bfloat16 values, the high half of each float32 rounded to
///         nearest even (2 * dims bytes).
///   SQ8:  per-vector scalar quantization in the style of LVQ (Aguerrebere et
///         al., VLDB 2023): float offset, float scale, then dims uint8 codes,
///         with x[i] ~= offset + scale * code[i] (8 + dims bytes).  offset and
///         scale are the vector's own min and (max - min) / 255, so a value
///         needs no per-segment or per-dimension statistics.
///   BIT:  one bit per dimension (dims / 8 bytes; dims a multiple of 8), set
///         when the element is > 0, most significant bit first within a byte.
///         Binary embeddings and fingerprints, scored only by the hamming
///         metric.  It is not a precision of the float encodings: a merge
///         never converts between BIT and them (see VectorTranscoder).
///
/// The numeric values are persisted in segment field metadata; do not renumber.
enum class VectorEncoding : uint8_t { F32 = 0, F16 = 1, BF16 = 2, SQ8 = 3, BIT = 4 };

namespace vcodec {

inline constexpr int32_t SQ8_HEADER_BYTES = 2 * (int32_t)sizeof(float);
inline constexpr int32_t SQ8_LEVELS = 255;
// Largest finite binary16 value.
inline constexpr float F16_MAX = 65504.0f;

inline bool valid(int32_t encoding) {
  return encoding >= (int32_t)VectorEncoding::F32 && encoding <= (int32_t)VectorEncoding::BIT;
}

inline const char* name(VectorEncoding e) {
  switch (e) {
    case VectorEncoding::F16: return "f16";
    case VectorEncoding::BF16: return "bf16";
    case VectorEncoding::SQ8: return "sq8";
    case VectorEncoding::BIT: return "bit";
    case VectorEncoding::F32:
    default: return "f32";
  }
}

inline int32_t bytesPerVector(VectorEncoding e, int32_t dims) {
  switch (e) {
    case VectorEncoding::F16:
    case VectorEncoding::BF16: return dims * 2;
    case VectorEncoding::SQ8: return SQ8_HEADER_BYTES + dims;
    case VectorEncoding::BIT: return dims / 8;
    case VectorEncoding::F32:
    default: return dims * (int32_t)sizeof(float);
  }
}

/// Dimensions of a stored value of the given byte size, or -1 when the size
/// is not a valid value of this encoding.  An empty column (0 bytes) has 0 dims.
inline int32_t dimsFromBytes(VectorEncoding e, int32_t bytes) {
  if (bytes == 0) return 0;
  switch (e) {
    case VectorEncoding::F16:
    case VectorEncoding::BF16: return bytes % 2 == 0 ? bytes / 2 : -1;
    case VectorEncoding::SQ8: return bytes > SQ8_HEADER_BYTES ? bytes - SQ8_HEADER_BYTES : -1;
    case VectorEncoding::BIT: return bytes * 8;
    case VectorEncoding::F32:
    default: return bytes % (int32_t)sizeof(float) == 0 ? bytes / (int32_t)sizeof(float) : -1;
  }
}

struct Sq8Header {
  float offset;
  float scale;
};

inline Sq8Header readSq8Header(const char* in) {
  Sq8Header h;
  std::memcpy(&h.offset, in, sizeof(float));
  std::memcpy(&h.scale, in + sizeof(float), sizeof(float));
  return h;
}

/// Whether a column can be converted between encodings a and b: BIT holds
/// signs, not values, so it converts only to itself.
inline bool convertible(VectorEncoding a, VectorEncoding b) {
  return (a == VectorEncoding::BIT) == (b == VectorEncoding::BIT);
}

/// Encodes dims finite floats into bytesPerVector(e, dims) bytes at out.
/// Callers validate finiteness and VectorFieldType::MAX_ELEMENT_MAGNITUDE
/// (and the F16 range) first.  BIT needs dims to be a multiple of 8.
inline void encode(VectorEncoding e, const float* in, int32_t dims, char* out) {
  switch (e) {
    case VectorEncoding::F16:
      for (int32_t i = 0; i < dims; i++) {
        std::float16_t h = (std::float16_t)in[i];
        std::memcpy(out + 2 * i, &h, 2);
      }
      return;
    case VectorEncoding::BF16:
      // Round to nearest even on the float bits with integer arithmetic, so
      // the stored bytes do not depend on the compiler or CPU.  bf16 keeps
      // the float32 exponent, and inputs are at most 2^40 in magnitude, so
      // rounding up never reaches infinity: no range check or clamp needed.
      for (int32_t i = 0; i < dims; i++) {
        uint32_t bits = std::bit_cast<uint32_t>(in[i]);
        auto h = (uint16_t)((bits + 0x7FFFu + ((bits >> 16) & 1u)) >> 16);
        std::memcpy(out + 2 * i, &h, 2);
      }
      return;
    case VectorEncoding::SQ8: {
      float lo = in[0];
      float hi = in[0];
      for (int32_t i = 1; i < dims; i++) {
        lo = std::min(lo, in[i]);
        hi = std::max(hi, in[i]);
      }
      // double: hi - lo can overflow float for extreme finite inputs.
      double range = (double)hi - (double)lo;
      Sq8Header h{lo, (float)(range / SQ8_LEVELS)};
      auto* codes = (uint8_t*)(out + SQ8_HEADER_BYTES);
      if (h.scale > 0.0f) {
        double inv = 1.0 / (double)h.scale;
        for (int32_t i = 0; i < dims; i++) {
          double q = std::nearbyint(((double)in[i] - (double)lo) * inv);
          codes[i] = (uint8_t)std::clamp(q, 0.0, (double)SQ8_LEVELS);
        }
      } else {
        // Constant vector (or a range below float resolution): offset alone
        // reconstructs it exactly.
        h.scale = 0.0f;
        std::memset(codes, 0, (size_t)dims);
      }
      std::memcpy(out, &h.offset, sizeof(float));
      std::memcpy(out + sizeof(float), &h.scale, sizeof(float));
      return;
    }
    case VectorEncoding::BIT:
      for (int32_t byte = 0; byte < dims / 8; byte++) {
        uint32_t bits = 0;
        for (int32_t i = 0; i < 8; i++) bits = (bits << 1) | (in[8 * byte + i] > 0.0f ? 1u : 0u);
        out[byte] = (char)bits;
      }
      return;
    case VectorEncoding::F32:
    default:
      std::memcpy(out, in, (size_t)dims * sizeof(float));
      return;
  }
}

/// Decodes one stored value (dims elements) into out.
inline void decode(VectorEncoding e, const char* in, int32_t dims, float* out) {
  switch (e) {
    case VectorEncoding::F16:
      for (int32_t i = 0; i < dims; i++) {
        std::float16_t h;
        std::memcpy(&h, in + 2 * i, 2);
        out[i] = (float)h;
      }
      return;
    case VectorEncoding::BF16:
      for (int32_t i = 0; i < dims; i++) {
        uint16_t h;
        std::memcpy(&h, in + 2 * i, 2);
        out[i] = std::bit_cast<float>((uint32_t)h << 16);
      }
      return;
    case VectorEncoding::SQ8: {
      Sq8Header h = readSq8Header(in);
      auto* codes = (const uint8_t*)(in + SQ8_HEADER_BYTES);
      for (int32_t i = 0; i < dims; i++) out[i] = h.offset + h.scale * (float)codes[i];
      return;
    }
    case VectorEncoding::BIT:
      for (int32_t i = 0; i < dims; i++) out[i] = (float)(((uint8_t)in[i >> 3] >> (7 - (i & 7))) & 1u);
      return;
    case VectorEncoding::F32:
    default:
      std::memcpy(out, in, (size_t)dims * sizeof(float));
      return;
  }
}

} // namespace vcodec
} // namespace luxir
