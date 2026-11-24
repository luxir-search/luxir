// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <faiss/impl/ScalarQuantizer.h>
#include <faiss/utils/distances.h>

#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <string_view>
#include <vector>

#include "luxir/api/luxir_types.hpp"
#include "luxir/codec/VectorCodec.h"
#include "luxir/schema/FieldType.h"

namespace luxir {

/// Exact scoring of one float query against stored vector rows of one
/// VectorEncoding.  This is the single place column scores are computed (flat
/// column scan and the terminal rescore of ANN candidates), so "exact" means
/// exact over the stored values.
///
/// Scores are higher-is-better: L2 -> 1 / (1 + squared distance), IP -> dot,
/// COSINE -> dot / |x|.  The query is already unit length for COSINE; an F32
/// column of unit vectors skips the division (normalizeColumn false).  A
/// stored unit vector in another encoding is not exactly unit length, so its
/// cosine always divides by the stored norm.
///
/// F16 and BF16 rows are scored in place by FAISS's ScalarQuantizer distance
/// computers (QT_fp16 is IEEE binary16 and QT_bf16 the high half of the float
/// bits, the layouts VectorCodec writes; neither needs training).  Cosine
/// takes the dot from an inner-product computer and |x|^2 from an L2 computer
/// against an all-zero query, which sums x[i]^2 directly.  SQ8 rows decode
/// into a scratch buffer for the float32 kernels: scoring the codes against
/// per-vector offset and scale terms cancels catastrophically for a query
/// with a large dynamic range.
///
/// The computers and the scratch buffer make a scorer single-threaded: one per
/// scanning task.
class VectorScorer {
  using DistanceComputer = faiss::ScalarQuantizer::SQDistanceComputer;

  const float* query;
  int32_t dims;
  int32_t metric;
  bool normalizeColumn;
  VectorEncoding encoding;
  // SQ8: the decoded row.
  mutable std::vector<float> scratch;
  // F16 / BF16: computers built from quantizer, which owns any trained state
  // they point into (none for the 16-bit types).  distance returns the
  // squared L2 distance, or the dot product for IP and COSINE; norm returns
  // |x|^2 against zeros, for COSINE only.
  std::unique_ptr<faiss::ScalarQuantizer> quantizer;
  std::unique_ptr<DistanceComputer> distance;
  std::unique_ptr<DistanceComputer> norm;
  std::vector<float> zeros;

public:
  VectorScorer(const float* query, int32_t dims, int32_t metric, bool normalizeColumn,
               VectorEncoding encoding)
    : query(query), dims(dims), metric(metric), normalizeColumn(normalizeColumn),
      encoding(encoding) {
    assert(query != nullptr);
    if (encoding == VectorEncoding::SQ8) scratch.resize((size_t)dims);
    bool sixteenBit = encoding == VectorEncoding::F16 || encoding == VectorEncoding::BF16;
    bool scored = metric == (int32_t)api::VectorMetric::L2 || metric == (int32_t)api::VectorMetric::IP
        || metric == (int32_t)api::VectorMetric::COSINE;
    if (sixteenBit && scored) {
      quantizer = std::make_unique<faiss::ScalarQuantizer>(
          (size_t)dims, encoding == VectorEncoding::F16 ? faiss::ScalarQuantizer::QT_fp16
                                                        : faiss::ScalarQuantizer::QT_bf16);
      bool l2 = metric == (int32_t)api::VectorMetric::L2;
      distance.reset(quantizer->get_distance_computer(l2 ? faiss::METRIC_L2 : faiss::METRIC_INNER_PRODUCT));
      distance->set_query(query);
      if (metric == (int32_t)api::VectorMetric::COSINE) {
        zeros.assign((size_t)dims, 0.0f);
        norm.reset(quantizer->get_distance_computer(faiss::METRIC_L2));
        norm->set_query(zeros.data());
      }
    }
  }

  // The computers keep pointers to the query buffers.
  VectorScorer(const VectorScorer&) = delete;
  VectorScorer& operator=(const VectorScorer&) = delete;

  /// Score of one stored row (vcodec::bytesPerVector(encoding, dims) bytes).
  /// fallbackScore is returned for a metric without a column score.
  float score(std::string_view row, float fallbackScore) const {
    switch (encoding) {
      case VectorEncoding::F32:
        return scoreFloats((const float*)row.data(), normalizeColumn, fallbackScore);
      case VectorEncoding::F16:
      case VectorEncoding::BF16:
        return score16((const uint8_t*)row.data(), fallbackScore);
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
  static float cosine(float dot, float normSq) {
    if (!std::isfinite(normSq) || normSq <= VectorFieldType::MIN_COSINE_NORM_SQ) return 0.0f;
    return finiteScore(dot / std::sqrt(normSq));
  }

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
        return cosine(sum, faiss::fvec_norm_L2sqr(vec, (size_t)dims));
      }
      default:
        return fallbackScore;
    }
  }

  float score16(const uint8_t* row, float fallbackScore) const {
    switch (metric) {
      case (int32_t)luxir::api::VectorMetric::L2:
        return finiteScore(1.0f / (1.0f + distance->query_to_code(row)));
      case (int32_t)luxir::api::VectorMetric::IP:
        return finiteScore(distance->query_to_code(row));
      case (int32_t)luxir::api::VectorMetric::COSINE:
        return cosine(distance->query_to_code(row), norm->query_to_code(row));
      default:
        return fallbackScore;
    }
  }
};

} // namespace luxir
