// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <faiss/utils/distances.h>

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdfloat>
#include <string_view>
#include <vector>

#include "luxir/api/luxir_types.hpp"
#include "luxir/codec/VectorCodec.h"

namespace luxir {

/// Exact scoring of one float query against stored vector rows of one
/// VectorEncoding.  This is the single place column scores are computed (flat
/// column scan and the terminal rescore of ANN candidates), so "exact" means
/// exact over the stored encoding.  Faster per-encoding kernels plug in here.
///
/// Scores are higher-is-better: L2 -> 1 / (1 + squared distance), IP -> dot,
/// COSINE -> dot / |x|.  The query is already unit length for COSINE; an F32
/// column of unit vectors skips the division (normalizeColumn false).
///
/// INT8 rows (x[i] = offset + scale * code[i]) are scored without
/// reconstructing x:
///   q.x  = offset * sum(q) + scale * sum(q[i] * code[i])
///   |x|^2 = dims * offset^2 + 2 * offset * scale * sum(code) + scale^2 * sum(code^2)
///   |q - x|^2 = |q|^2 - 2 q.x + |x|^2
/// with sum(q) and |q|^2 computed once per query.
class VectorScorer {
  const float* query;
  int32_t dims;
  int32_t metric;
  bool normalizeColumn;
  VectorEncoding encoding;
  double querySum = 0.0;
  double queryNormSq = 0.0;
  // Per-row conversion buffer (one scorer per scanning task).
  mutable std::vector<float> scratch;

public:
  static constexpr double MIN_COSINE_NORM_SQ = 1.0e-30;

  VectorScorer(const float* query, int32_t dims, int32_t metric, bool normalizeColumn,
               VectorEncoding encoding)
    : query(query), dims(dims), metric(metric), normalizeColumn(normalizeColumn),
      encoding(encoding) {
    assert(query != nullptr);
    if (encoding != VectorEncoding::F32) scratch.resize((size_t)dims);
    if (encoding == VectorEncoding::INT8) {
      for (int32_t i = 0; i < dims; i++) {
        querySum += (double)query[i];
        queryNormSq += (double)query[i] * (double)query[i];
      }
    }
  }

  /// Score of one stored row (vcodec::bytesPerVector(encoding, dims) bytes).
  /// fallbackScore is returned for a metric without a column score.
  float score(std::string_view row, float fallbackScore) const {
    switch (encoding) {
      case VectorEncoding::F16: return scoreF16(row.data(), fallbackScore);
      case VectorEncoding::INT8: return scoreInt8(row.data(), fallbackScore);
      case VectorEncoding::F32:
      default: return scoreF32((const float*)row.data(), fallbackScore);
    }
  }

  // The query vector is validated finite, but stored vectors are not, so a
  // distance can still come back NaN (a stored NaN, or inf - inf
  // accumulation).  NaN scores would break the strict-total-order contract
  // that makes bounded cuts deterministic, so map NaN to a worst-rank score.
  // One ULP above lowest(): TopScoreCollector admits only scores strictly
  // above lowest(), and a kNN hit must rank last, not vanish.
  static float finiteScore(float score) {
    if (!std::isnan(score)) [[likely]] return score;
    return std::nextafter(std::numeric_limits<float>::lowest(), 0.0f);
  }

private:
  float fromParts(double dot, double distSq, double normSq, float fallbackScore) const {
    switch (metric) {
      case (int32_t)luxir::api::VectorMetric::L2:
        return finiteScore((float)(1.0 / (1.0 + distSq)));
      case (int32_t)luxir::api::VectorMetric::IP:
        return finiteScore((float)dot);
      case (int32_t)luxir::api::VectorMetric::COSINE:
        // A quantized unit vector is not exactly unit length, and its norm
        // falls out of the same pass, so f16 / int8 cosine always divides by
        // the stored vector's norm: the exact cosine over the stored encoding.
        if (!std::isfinite(normSq) || normSq <= MIN_COSINE_NORM_SQ) return 0.0f;
        return finiteScore((float)(dot / std::sqrt(normSq)));
      default:
        return fallbackScore;
    }
  }

  float scoreF32(const float* vec, float fallbackScore) const {
    switch (metric) {
      case (int32_t)luxir::api::VectorMetric::L2: {
        float dist = faiss::fvec_L2sqr(query, vec, (size_t)dims);
        return finiteScore(1.0f / (1.0f + dist));
      }
      case (int32_t)luxir::api::VectorMetric::IP:
        return finiteScore(faiss::fvec_inner_product(query, vec, (size_t)dims));
      case (int32_t)luxir::api::VectorMetric::COSINE: {
        float sum = faiss::fvec_inner_product(query, vec, (size_t)dims);
        if (!normalizeColumn) return finiteScore(sum);
        float normSq = faiss::fvec_norm_L2sqr(vec, (size_t)dims);
        if (!std::isfinite(normSq) || normSq <= MIN_COSINE_NORM_SQ) return 0.0f;
        return finiteScore(sum / std::sqrt(normSq));
      }
      default:
        return fallbackScore;
    }
  }

  // F16: widen into scratch (a plain conversion loop the compiler
  // vectorizes), then the float32 kernels.
  float scoreF16(const char* row, float fallbackScore) const {
    float* x = scratch.data();
    for (int32_t i = 0; i < dims; i++) {
      std::float16_t h;
      std::memcpy(&h, row + 2 * i, 2);
      x[i] = (float)h;
    }
    double dot = (double)faiss::fvec_inner_product(query, x, (size_t)dims);
    if (metric == (int32_t)luxir::api::VectorMetric::IP) return fromParts(dot, 0, 0, fallbackScore);
    double normSq = (double)faiss::fvec_norm_L2sqr(x, (size_t)dims);
    double distSq = metric == (int32_t)luxir::api::VectorMetric::L2
        ? (double)faiss::fvec_L2sqr(query, x, (size_t)dims) : 0.0;
    return fromParts(dot, distSq, normSq, fallbackScore);
  }

  // INT8: the codes (not the decoded values) are widened to float for
  // sum(q[i] * code[i]); the code sums stay integer.  Offset and scale are
  // applied once per vector by the decomposition above.
  float scoreInt8(const char* row, float fallbackScore) const {
    vcodec::Int8Header h = vcodec::readInt8Header(row);
    auto* codes = (const uint8_t*)(row + vcodec::INT8_HEADER_BYTES);
    float* c = scratch.data();
    int64_t sumC = 0;
    int64_t sumCC = 0;
    for (int32_t i = 0; i < dims; i++) {
      int32_t code = codes[i];
      c[i] = (float)code;
      sumC += code;
      sumCC += code * code;
    }
    double qc = (double)faiss::fvec_inner_product(query, c, (size_t)dims);
    double offset = (double)h.offset;
    double scale = (double)h.scale;
    double dot = offset * querySum + scale * qc;
    double normSq = (double)dims * offset * offset + 2.0 * offset * scale * (double)sumC
        + scale * scale * (double)sumCC;
    double distSq = std::max(0.0, queryNormSq - 2.0 * dot + normSq);
    return fromParts(dot, distSq, normSq, fallbackScore);
  }
};

} // namespace luxir
