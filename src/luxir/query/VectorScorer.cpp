// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "VectorScorer.h"

#include <algorithm>

#include "luxir/api/luxir_types.hpp"
#include "luxir/codec/VectorKernels.h"

namespace luxir {

namespace {

constexpr int32_t CACHE_LINE = 64;
// Rows ahead to prefetch when a block is not contiguous (rescore gathers).
constexpr size_t PREFETCH_ROWS = 2;
// Cap on lines prefetched per row; the hardware prefetcher follows the rest.
constexpr int32_t PREFETCH_MAX_LINES = 32;

float cosineScore(double dot, double normSq) {
  if (!std::isfinite(normSq) || normSq <= VectorScorer::MIN_COSINE_NORM_SQ) return 0.0f;
  return VectorScorer::finiteScore((float)(dot / std::sqrt(normSq)));
}

} // namespace

VectorScorer::VectorScorer(const float* query, int32_t dims, int32_t metric,
                           bool normalizeColumn, VectorEncoding encoding, bool norms)
  : query(query), dims(dims), rowBytes(vcodec::rowBytes(encoding, dims, norms)), norms(norms) {
  assert(query != nullptr);
  bool l2 = metric == (int32_t)luxir::api::VectorMetric::L2;
  bool ip = metric == (int32_t)luxir::api::VectorMetric::IP;
  bool cosine = metric == (int32_t)luxir::api::VectorMetric::COSINE;
  if (!l2 && !ip && !cosine) return;
  switch (encoding) {
    case VectorEncoding::F16:
      kind = l2 ? Kind::F16_L2 : ip ? Kind::F16_DOT : Kind::F16_COS;
      break;
    case VectorEncoding::INT8:
      kind = l2 ? Kind::I8_L2 : ip ? Kind::I8_DOT : Kind::I8_COS;
      for (int32_t i = 0; i < dims; i++) {
        querySum += (double)query[i];
        queryNormSq += (double)query[i] * (double)query[i];
      }
      break;
    case VectorEncoding::F32:
    default:
      kind = l2 ? Kind::F32_L2 : (cosine && normalizeColumn) ? Kind::F32_COS : Kind::F32_DOT;
      break;
  }
}

// The stored |x|^2, or for a segment written without the prefix (field
// settings that did not need it, e.g. before a metric change) the same value
// computed from the payload, until a merge adds the prefix.
inline double VectorScorer::normSq(const char* row, VectorEncoding encoding) const {
  if (norms) [[likely]] return (double)vcodec::rowNormSq(row);
  return (double)(float)vcodec::payloadNormSq(encoding, row, dims);
}

template <VectorScorer::Kind K>
inline float VectorScorer::scoreRow(const char* row) const {
  const char* x = vcodec::rowPayload(row, norms);
  if constexpr (K == Kind::F32_L2) {
    float dist = vkernel::l2sqF32(query, x, dims);
    return finiteScore(1.0f / (1.0f + dist));
  } else if constexpr (K == Kind::F32_DOT) {
    return finiteScore(vkernel::dotF32(query, x, dims));
  } else if constexpr (K == Kind::F32_COS) {
    float dot = vkernel::dotF32(query, x, dims);
    float xx = (float)normSq(row, VectorEncoding::F32);
    if (!std::isfinite(xx) || xx <= MIN_COSINE_NORM_SQ) return 0.0f;
    return finiteScore(dot / std::sqrt(xx));
  } else if constexpr (K == Kind::F16_L2) {
    float dist = vkernel::l2sqF16(query, x, dims);
    return finiteScore(1.0f / (1.0f + dist));
  } else if constexpr (K == Kind::F16_DOT) {
    return finiteScore(vkernel::dotF16(query, x, dims));
  } else if constexpr (K == Kind::F16_COS) {
    return cosineScore((double)vkernel::dotF16(query, x, dims), normSq(row, VectorEncoding::F16));
  } else {
    vcodec::Int8Header h = vcodec::readInt8Header(x);
    double qc = (double)vkernel::dotU8(query, (const uint8_t*)(x + vcodec::INT8_HEADER_BYTES), dims);
    double dot = (double)h.offset * querySum + (double)h.scale * qc;
    if constexpr (K == Kind::I8_DOT) {
      return finiteScore((float)dot);
    } else if constexpr (K == Kind::I8_COS) {
      return cosineScore(dot, normSq(row, VectorEncoding::INT8));
    } else {
      double distSq = std::max(0.0, queryNormSq - 2.0 * dot + normSq(row, VectorEncoding::INT8));
      return finiteScore((float)(1.0 / (1.0 + distSq)));
    }
  }
}

template <VectorScorer::Kind K>
void VectorScorer::scoreRowsAs(const char* const* rows, size_t n, float* out) const {
  int32_t lines = std::min(PREFETCH_MAX_LINES, (rowBytes + CACHE_LINE - 1) / CACHE_LINE);
  for (size_t i = 0; i < n; i++) {
    size_t ahead = i + PREFETCH_ROWS;
    if (ahead < n && rows[ahead] != rows[ahead - 1] + rowBytes) {
      for (int32_t l = 0; l < lines; l++) __builtin_prefetch(rows[ahead] + l * CACHE_LINE);
    }
    out[i] = scoreRow<K>(rows[i]);
  }
}

void VectorScorer::scoreRows(const char* const* rows, size_t n, float* out) const {
  switch (kind) {
    case Kind::F32_L2: return scoreRowsAs<Kind::F32_L2>(rows, n, out);
    case Kind::F32_DOT: return scoreRowsAs<Kind::F32_DOT>(rows, n, out);
    case Kind::F32_COS: return scoreRowsAs<Kind::F32_COS>(rows, n, out);
    case Kind::F16_L2: return scoreRowsAs<Kind::F16_L2>(rows, n, out);
    case Kind::F16_DOT: return scoreRowsAs<Kind::F16_DOT>(rows, n, out);
    case Kind::F16_COS: return scoreRowsAs<Kind::F16_COS>(rows, n, out);
    case Kind::I8_L2: return scoreRowsAs<Kind::I8_L2>(rows, n, out);
    case Kind::I8_DOT: return scoreRowsAs<Kind::I8_DOT>(rows, n, out);
    case Kind::I8_COS: return scoreRowsAs<Kind::I8_COS>(rows, n, out);
    case Kind::NONE:
    default:
      assert(false);
      std::fill(out, out + n, 0.0f);
      return;
  }
}

} // namespace luxir
