// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <faiss/utils/distances.h>

#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string_view>
#include <vector>

#include "luxir/api/luxir_types.hpp"
#include "luxir/codec/VectorCodec.h"
#include "luxir/schema/FieldType.h"

namespace luxir {

/// Exact scoring of one float query against stored vector rows of one
/// VectorEncoding.  This is the single place column scores are computed (flat
/// column scan and the terminal rescore of ANN candidates), so "exact" means
/// exact over the stored values.  Faster per-encoding kernels plug in here.
///
/// Scores are higher-is-better: L2 -> 1 / (1 + squared distance), IP -> dot,
/// COSINE -> dot / |x|.  The query is already unit length for COSINE; an F32
/// column of unit vectors skips the division (normalizeColumn false).
///
/// F16, BF16 and SQ8 rows are decoded into a scratch buffer and scored with
/// the float32 kernels, so a score is the f32 score of the stored vector.  A
/// decoded unit vector is not exactly unit length, so their cosine always
/// divides by the stored norm.  SQ8 decodes first rather than scoring its codes
/// against per-vector offset and scale terms, which cancel catastrophically
/// for a query with a large dynamic range.
///
/// The scratch buffer makes a scorer single-threaded: one per scanning task.
class VectorScorer {
  const float* query;
  int32_t dims;
  int32_t metric;
  bool normalizeColumn;
  VectorEncoding encoding;
  mutable std::vector<float> scratch;

public:
  VectorScorer(const float* query, int32_t dims, int32_t metric, bool normalizeColumn,
               VectorEncoding encoding)
    : query(query), dims(dims), metric(metric), normalizeColumn(normalizeColumn),
      encoding(encoding) {
    assert(query != nullptr);
    if (encoding != VectorEncoding::F32) scratch.resize((size_t)dims);
  }

  /// Score of one stored row (vcodec::bytesPerVector(encoding, dims) bytes).
  /// fallbackScore is returned for a metric without a column score.
  float score(std::string_view row, float fallbackScore) const {
    switch (encoding) {
      case VectorEncoding::F32:
        return scoreFloats((const float*)row.data(), normalizeColumn, fallbackScore);
      case VectorEncoding::F16:
      case VectorEncoding::BF16:
      case VectorEncoding::SQ8:
      default:
        vcodec::decode(encoding, row.data(), dims, scratch.data());
        return scoreFloats(scratch.data(), true, fallbackScore);
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
  float scoreFloats(const float* vec, bool normalize, float fallbackScore) const {
    switch (metric) {
      case (int32_t)luxir::api::VectorMetric::L2: {
        float dist = faiss::fvec_L2sqr(query, vec, (size_t)dims);
        return finiteScore(1.0f / (1.0f + dist));
      }
      case (int32_t)luxir::api::VectorMetric::IP:
        return finiteScore(faiss::fvec_inner_product(query, vec, (size_t)dims));
      case (int32_t)luxir::api::VectorMetric::COSINE: {
        float sum = faiss::fvec_inner_product(query, vec, (size_t)dims);
        if (!normalize) return finiteScore(sum);
        float normSq = faiss::fvec_norm_L2sqr(vec, (size_t)dims);
        if (!std::isfinite(normSq) || normSq <= VectorFieldType::MIN_COSINE_NORM_SQ) return 0.0f;
        return finiteScore(sum / std::sqrt(normSq));
      }
      default:
        return fallbackScore;
    }
  }
};

} // namespace luxir
