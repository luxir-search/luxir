// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <stdfloat>
#include <string_view>

namespace luxir {

/// Storage encoding of one vector field's column.  Every encoding stores one
/// fixed-size, self-describing value per vector, so a segment column is still
/// a plain fixed-size binary column and merges copy values verbatim.
///
///   F32:  dims little-endian IEEE float32 values (4 * dims bytes).
///   F16:  dims IEEE binary16 values (2 * dims bytes).
///   INT8: per-vector scalar quantization in the style of LVQ (Aguerrebere et
///         al., VLDB 2023): float offset, float scale, then dims uint8 codes,
///         with x[i] ~= offset + scale * code[i] (8 + dims bytes).  offset and
///         scale are the vector's own min and (max - min) / 255, so a value
///         needs no per-segment or per-dimension statistics.
///
/// The numeric values are persisted in segment field metadata; do not renumber.
///
/// Norm prefix: a segment column may also record each vector's squared L2
/// norm (SegFieldInfo.vectorNorms).  Its rows are then a 4-byte float |x|^2
/// of the vector the payload decodes to (for INT8 the exact offset/scale
/// reconstruction), followed by the encoding's payload above.  Scoring reads
/// the norm instead of recomputing it per query: cosine over a column whose
/// vectors are not unit length (raw f32, any f16 / int8) and int8 L2.  The
/// prefix keeps f32 payloads 4-byte aligned relative to the row, travels with
/// the row through merges (rows stay self-describing), and sits in the same
/// cache line the scorer reads first.
enum class VectorEncoding : uint8_t { F32 = 0, F16 = 1, INT8 = 2 };

namespace vcodec {

inline constexpr int32_t INT8_HEADER_BYTES = 2 * (int32_t)sizeof(float);
inline constexpr int32_t NORM_BYTES = (int32_t)sizeof(float);
inline constexpr int32_t INT8_LEVELS = 255;
// Largest finite binary16 value.
inline constexpr float F16_MAX = 65504.0f;

inline bool valid(int32_t encoding) {
  return encoding >= (int32_t)VectorEncoding::F32 && encoding <= (int32_t)VectorEncoding::INT8;
}

inline const char* name(VectorEncoding e) {
  switch (e) {
    case VectorEncoding::F16: return "f16";
    case VectorEncoding::INT8: return "int8";
    case VectorEncoding::F32:
    default: return "f32";
  }
}

inline int32_t bytesPerVector(VectorEncoding e, int32_t dims) {
  switch (e) {
    case VectorEncoding::F16: return dims * 2;
    case VectorEncoding::INT8: return INT8_HEADER_BYTES + dims;
    case VectorEncoding::F32:
    default: return dims * (int32_t)sizeof(float);
  }
}

/// Dimensions of a stored value of the given byte size, or -1 when the size
/// is not a valid value of this encoding.  An empty column (0 bytes) has 0 dims.
inline int32_t dimsFromBytes(VectorEncoding e, int32_t bytes) {
  if (bytes == 0) return 0;
  switch (e) {
    case VectorEncoding::F16: return bytes % 2 == 0 ? bytes / 2 : -1;
    case VectorEncoding::INT8: return bytes > INT8_HEADER_BYTES ? bytes - INT8_HEADER_BYTES : -1;
    case VectorEncoding::F32:
    default: return bytes % (int32_t)sizeof(float) == 0 ? bytes / (int32_t)sizeof(float) : -1;
  }
}

/// Merge order: a merge of segments with different encodings writes the most
/// precise one, so a merge never loses precision.
inline int32_t precision(VectorEncoding e) {
  switch (e) {
    case VectorEncoding::INT8: return 0;
    case VectorEncoding::F16: return 1;
    case VectorEncoding::F32:
    default: return 2;
  }
}

struct Int8Header {
  float offset;
  float scale;
};

inline Int8Header readInt8Header(const char* in) {
  Int8Header h;
  std::memcpy(&h.offset, in, sizeof(float));
  std::memcpy(&h.scale, in + sizeof(float), sizeof(float));
  return h;
}

/// Encodes dims finite floats into bytesPerVector(e, dims) bytes at out.
/// Callers validate finiteness (and the F16 range) first.
inline void encode(VectorEncoding e, const float* in, int32_t dims, char* out) {
  switch (e) {
    case VectorEncoding::F16:
      for (int32_t i = 0; i < dims; i++) {
        std::float16_t h = (std::float16_t)in[i];
        std::memcpy(out + 2 * i, &h, 2);
      }
      return;
    case VectorEncoding::INT8: {
      float lo = in[0];
      float hi = in[0];
      for (int32_t i = 1; i < dims; i++) {
        lo = std::min(lo, in[i]);
        hi = std::max(hi, in[i]);
      }
      // double: hi - lo can overflow float for extreme finite inputs.
      double range = (double)hi - (double)lo;
      Int8Header h{lo, (float)(range / INT8_LEVELS)};
      auto* codes = (uint8_t*)(out + INT8_HEADER_BYTES);
      if (h.scale > 0.0f) {
        double inv = 1.0 / (double)h.scale;
        for (int32_t i = 0; i < dims; i++) {
          double q = std::nearbyint(((double)in[i] - (double)lo) * inv);
          codes[i] = (uint8_t)std::clamp(q, 0.0, (double)INT8_LEVELS);
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
    case VectorEncoding::INT8: {
      Int8Header h = readInt8Header(in);
      auto* codes = (const uint8_t*)(in + INT8_HEADER_BYTES);
      for (int32_t i = 0; i < dims; i++) out[i] = h.offset + h.scale * (float)codes[i];
      return;
    }
    case VectorEncoding::F32:
    default:
      std::memcpy(out, in, (size_t)dims * sizeof(float));
      return;
  }
}

/// Squared L2 norm of the vector a payload decodes to, in double.  INT8 uses
/// the exact reconstruction offset + scale * code (the same decomposition the
/// scorer uses for q.x), not the float-rounded decode.
inline double payloadNormSq(VectorEncoding e, const char* in, int32_t dims) {
  double sum = 0.0;
  switch (e) {
    case VectorEncoding::F16:
      for (int32_t i = 0; i < dims; i++) {
        std::float16_t h;
        std::memcpy(&h, in + 2 * i, 2);
        double d = (double)(float)h;
        sum += d * d;
      }
      return sum;
    case VectorEncoding::INT8: {
      Int8Header h = readInt8Header(in);
      auto* codes = (const uint8_t*)(in + INT8_HEADER_BYTES);
      int64_t sumC = 0;
      int64_t sumCC = 0;
      for (int32_t i = 0; i < dims; i++) {
        int64_t code = codes[i];
        sumC += code;
        sumCC += code * code;
      }
      double offset = (double)h.offset;
      double scale = (double)h.scale;
      return (double)dims * offset * offset + 2.0 * offset * scale * (double)sumC
          + scale * scale * (double)sumCC;
    }
    case VectorEncoding::F32:
    default:
      for (int32_t i = 0; i < dims; i++) {
        float x;
        std::memcpy(&x, in + 4 * i, sizeof(float));
        sum += (double)x * (double)x;
      }
      return sum;
  }
}

/// Whether a field stores the norm prefix.  Cosine needs |x| unless the
/// column holds f32 unit vectors (normalized on write, or trusted normalized);
/// int8 L2 needs |x|^2 so its inner loop is a pure dot(q, codes).
inline bool needsNorms(VectorEncoding e, bool cosine, bool l2, bool unitF32Column) {
  if (cosine) return e != VectorEncoding::F32 || !unitF32Column;
  return l2 && e == VectorEncoding::INT8;
}

inline int32_t rowBytes(VectorEncoding e, int32_t dims, bool norms) {
  return bytesPerVector(e, dims) + (norms ? NORM_BYTES : 0);
}

/// dimsFromBytes for a stored row (with or without the norm prefix).
inline int32_t dimsFromRowBytes(VectorEncoding e, int32_t bytes, bool norms) {
  if (!norms || bytes == 0) return dimsFromBytes(e, bytes);
  return bytes > NORM_BYTES ? dimsFromBytes(e, bytes - NORM_BYTES) : -1;
}

inline const char* rowPayload(const char* row, bool norms) {
  return norms ? row + NORM_BYTES : row;
}

/// The stored |x|^2 of a row written with the norm prefix.
inline float rowNormSq(const char* row) {
  float n;
  std::memcpy(&n, row, sizeof(float));
  return n;
}

/// Writes the norm prefix for the payload at out + NORM_BYTES.
inline void writeRowNorm(VectorEncoding e, char* out, int32_t dims) {
  float n = (float)payloadNormSq(e, out + NORM_BYTES, dims);
  std::memcpy(out, &n, sizeof(float));
}

/// Encodes dims floats into a row of rowBytes(e, dims, norms) bytes.
inline void encodeRow(VectorEncoding e, bool norms, const float* in, int32_t dims, char* out) {
  encode(e, in, dims, norms ? out + NORM_BYTES : out);
  if (norms) writeRowNorm(e, out, dims);
}

} // namespace vcodec
} // namespace luxir
