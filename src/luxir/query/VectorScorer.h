// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

#include "luxir/codec/VectorCodec.h"

namespace luxir {

/// Exact scoring of one float query against stored vector rows of one
/// segment column (one VectorEncoding, with or without the norm prefix).
/// This is the single place column scores are computed (flat column scan and
/// the terminal rescore of ANN candidates), so "exact" means exact over the
/// stored encoding.
///
/// Scores are higher-is-better: L2 -> 1 / (1 + squared distance), IP -> dot,
/// COSINE -> dot / |x|.  The query is already unit length for COSINE; an F32
/// column of unit vectors skips the division (normalizeColumn false).  Every
/// other cosine column, and int8 L2, reads |x|^2 from the row's norm prefix
/// (written once at index time, see VectorCodec.h) instead of recomputing it.
/// A segment written under field settings that did not need the prefix (a
/// metric change since) computes the same value per row until a merge adds it.
///
/// The per-element work runs in the SIMD kernels of VectorKernels.h: one
/// pass over the stored bytes per row, widening f16 / uint8 in registers.
/// INT8 rows (x[i] = offset + scale * code[i]) are scored without
/// reconstructing x in memory: q.x = offset * sum(q) + scale * dot(q, code),
/// with sum(q) computed once per query, so the inner loop is a pure
/// dot(q, code).  L2 decodes x in registers and takes the difference
/// directly, since |q|^2 - 2 q.x + |x|^2 cancels catastrophically when the
/// offset is large.
///
/// Batched: scoreRows takes a block of row pointers (contiguous column rows
/// for a scan, rank-sorted candidates for a rescore), dispatches on the
/// encoding and metric once per block, and prefetches ahead when rows are not
/// adjacent.  A row's score depends only on the query and the row, never on
/// its block or thread, so parallel and serial searches stay bit-identical.
class VectorScorer {
public:
  enum class Kind : uint8_t {
    NONE,
    F32_L2, F32_DOT, F32_COS,
    F16_L2, F16_DOT, F16_COS,
    I8_L2, I8_DOT, I8_COS,
  };

private:
  const float* query;
  int32_t dims;
  int32_t rowBytes;
  bool norms;
  Kind kind = Kind::NONE;
  double querySum = 0.0;

public:
  static constexpr double MIN_COSINE_NORM_SQ = 1.0e-30;

  /// norms: the segment's rows carry the norm prefix.
  VectorScorer(const float* query, int32_t dims, int32_t metric, bool normalizeColumn,
               VectorEncoding encoding, bool norms);

  /// False for a metric without a column score (callers keep a fallback).
  bool scoresColumn() const { return kind != Kind::NONE; }

  Kind getKind() const { return kind; }

  /// Scores n stored rows (each vcodec::rowBytes(encoding, dims, norms)
  /// bytes) into out[0..n).  A metric without a column score scores 0.
  void scoreRows(const char* const* rows, size_t n, float* out) const;

  /// Score of one stored row.
  float score(const char* row) const {
    float s;
    scoreRows(&row, 1, &s);
    return s;
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
  double normSq(const char* row, VectorEncoding encoding) const;
  template <Kind K>
  float scoreRow(const char* row) const;
  template <Kind K>
  void scoreRowsAs(const char* const* rows, size_t n, float* out) const;
};

} // namespace luxir
