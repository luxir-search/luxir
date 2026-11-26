// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <faiss/impl/ScalarQuantizer.h>
#include <faiss/utils/distances.h>

#include <cassert>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <vector>

#include "luxir/api/luxir_types.hpp"
#include "luxir/codec/VectorCodec.h"
#include "luxir/query/HammingKernel.h"
#include "luxir/query/Sq8Kernel.h"
#include "luxir/schema/FieldType.h"

namespace luxir {

/// Exact scoring of one float query against stored vector rows of one
/// VectorEncoding.  This is the single place column scores are computed (flat
/// column scan and the terminal rescore of ANN candidates), so "exact" means
/// exact over the stored values.
///
/// Scores are higher-is-better: L2 -> 1 / (1 + squared distance), IP -> dot,
/// COSINE -> dot / |x|, HAMMING -> 1 - hamming distance / dims.  HAMMING
/// scores only BIT rows and BIT rows score only under HAMMING: the query is
/// packed like a stored row once (element > 0) and compared by an integer
/// kernel chosen for the CPU and row size. For COSINE the query is already
/// unit length, and an F32 column of unit vectors skips the division
/// (normalizeColumn false). A stored unit vector in another encoding is not
/// exactly unit length, so its cosine always divides by the stored norm.
///
/// F16 and BF16 rows are scored in place by FAISS's ScalarQuantizer distance
/// computers (QT_fp16 is IEEE binary16 and QT_bf16 the high half of the float
/// bits, the layouts VectorCodec writes; neither needs training).  Cosine
/// takes the dot from an inner-product computer and |x|^2 from an L2 computer
/// against an all-zero query, which sums x[i]^2 directly.  SQ8 rows are
/// scored in registers by the sq8 kernels (Sq8Kernel.h), which decode each
/// element as offset + scale * code inside the distance or dot product (and,
/// for COSINE, sum |x|^2 in the same pass).  Scoring the codes against
/// per-vector offset and scale terms instead would cancel catastrophically
/// for a query with a large dynamic range.  GCC x86-64 builds select AVX2
/// or AVX-512 at run time.  With no supported kernel, SQ8 decodes into a
/// scratch buffer for the FAISS float32 kernels.
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
  // The selected register kernels for SQ8, if available.
  const sq8::Kernels* sq8Kernels;
  // SQ8 without the sq8 kernels: the decoded row.
  mutable std::vector<float> scratch;
  // BIT: the kernel and query packed like a stored row.
  hamming::KernelFn hammingKernel = nullptr;
  std::vector<uint8_t> queryBits;
  // F16 / BF16: computers built from quantizer, which owns any trained state
  // they point into (none for the 16-bit types).  distance returns the
  // squared L2 distance, or the dot product for IP and COSINE; norm returns
  // |x|^2 against zeros, for COSINE only.
  std::unique_ptr<faiss::ScalarQuantizer> quantizer;
  std::unique_ptr<DistanceComputer> distance;
  std::unique_ptr<DistanceComputer> norm;
  std::vector<float> zeros;

public:
  static bool isScorable(VectorEncoding encoding, int32_t metric) {
    return metric == (int32_t)api::VectorMetric::NONE
        || (metric == (int32_t)api::VectorMetric::HAMMING) == (encoding == VectorEncoding::BIT);
  }

  /// Throws when a segment's encoding cannot be scored under the metric (BIT
  /// without HAMMING or the reverse). An inverter pinned to an earlier schema
  /// can publish such a segment after a schema change.
  static void checkScorable(VectorEncoding encoding, int32_t metric, std::string_view field = {}) {
    if (!isScorable(encoding, metric)) {
      bool hamming = metric == (int32_t)api::VectorMetric::HAMMING;
      throw std::runtime_error(std::format(
          "vector segment encoding {} cannot be scored with metric {}{}; reindex the field",
          vcodec::name(encoding), hamming ? "hamming" : "l2/ip/cosine",
          field.empty() ? std::string() : std::format(" for field '{}'", field)));
    }
  }

  /// See checkScorable.
  VectorScorer(const float* query, int32_t dims, int32_t metric, bool normalizeColumn,
               VectorEncoding encoding)
    : query(query), dims(dims), metric(metric), normalizeColumn(normalizeColumn),
      encoding(encoding), sq8Kernels(encoding == VectorEncoding::SQ8 ? sq8::best() : nullptr) {
    assert(query != nullptr);
    checkScorable(encoding, metric);
    if (encoding == VectorEncoding::BIT) {
      queryBits.resize((size_t)vcodec::bytesPerVector(encoding, dims));
      hammingKernel = hamming::best(queryBits.size());
      vcodec::encode(encoding, query, dims, (char*)queryBits.data());
    }
    if (encoding == VectorEncoding::SQ8 && !sq8Kernels) scratch.resize((size_t)dims);
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
      case VectorEncoding::BIT: {
        if (metric != (int32_t)api::VectorMetric::HAMMING) return fallbackScore;
        uint64_t distance = hammingKernel(queryBits.data(), (const uint8_t*)row.data(), queryBits.size());
        return 1.0f - (float)distance / (float)dims;
      }
      case VectorEncoding::SQ8:
        if (sq8Kernels) return scoreSq8(row.data(), fallbackScore);
        [[fallthrough]];
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

  float scoreSq8(const char* row, float fallbackScore) const {
    vcodec::Sq8Header h = vcodec::readSq8Header(row);
    auto* codes = (const uint8_t*)(row + vcodec::SQ8_HEADER_BYTES);
    switch (metric) {
      case (int32_t)luxir::api::VectorMetric::L2:
        return finiteScore(1.0f / (1.0f + sq8Kernels->l2sq(query, codes, h.offset, h.scale, dims)));
      case (int32_t)luxir::api::VectorMetric::IP:
        return finiteScore(sq8Kernels->dot(query, codes, h.offset, h.scale, dims));
      case (int32_t)luxir::api::VectorMetric::COSINE: {
        float normSq;
        float sum = sq8Kernels->dotNorm(query, codes, h.offset, h.scale, dims, normSq);
        return cosine(sum, normSq);
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
