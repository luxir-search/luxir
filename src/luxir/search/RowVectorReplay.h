// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <faiss/utils/distances.h>

#include <limits>
#include <optional>
#include <string_view>
#include <vector>

#include "luxir/query/KnnQuery.h"
#include "luxir/reader/VectorReader.h"
#include "luxir/search/IndexReader.h"

namespace luxir {

// A kNN clause replayed within one family by a row operation (from_match).
// A kNN selects rows across the whole index, which a per-row operation cannot
// repeat, so the replay scores each row of the family exactly against the
// clause's query vector instead: the same score the clause gives a row it
// selects, and the same similarity threshold. The row operation matches the
// rows that have a vector (and pass the threshold), ranked by that score.
struct RowVectorReplay {
  std::string_view field;  // the vector field, a physical name of the replayed view
  std::vector<float> query;  // unit length for COSINE, as the search uses it
  int32_t metric = 0;
  bool normalizeColumn = false;
  float floor = -std::numeric_limits<float>::infinity();
  float boost = 1.0f;
  // False under a constant_score wrapper: the clause still restricts by the
  // threshold, but the row operation keeps its own (constant) score.
  bool scores = true;

  explicit RowVectorReplay(const KnnQuery& knn, float boost, bool scores)
      : field(knn.getField()), query(knn.getQueryVec().begin(), knn.getQueryVec().end()),
        metric((int32_t)knn.getFieldType().metric()),
        normalizeColumn(knn.getFieldType().metric() == VectorFieldType::METRIC_COSINE
            && !knn.getFieldType().normalized() && !knn.getFieldType().normalizeOnWrite()),
        floor(knn.similarityScoreFloor()), boost(boost), scores(scores) {
    if (metric == (int32_t)api::VectorMetric::COSINE && !query.empty()) {
      faiss::fvec_renorm_L2(query.size(), 1, query.data());
    }
  }

  // Scores rows of one segment of the replayed view. Rows of one family are
  // asked in ascending order; startFamily() rewinds for the next family.
  class Segment {
    const RowVectorReplay& replay;
    std::optional<VectorReader> vectors;
    std::optional<StrColReader::DocIterator> rows;

  public:
    Segment(const RowVectorReplay& replay, IndexReader& view, IndexReader::Segment& segment)
        : replay(replay) {
      auto* info = view.field(replay.field).at((size_t)segment.ord);
      if (info && info->docsWithField > 0) vectors.emplace(segment.postingsReader(), *info);
      if (vectors && vectors->dims() != (int32_t)replay.query.size()) {
        throw RequestError("from_match: query vector dims do not match field '" + std::string(replay.field) + "'");
      }
    }

    void startFamily() {
      if (vectors) rows.emplace(vectors->strColReader());
    }

    // The clause's score for `row`, or nothing when the row has no vector or
    // falls below the similarity threshold.
    std::optional<float> score(int32_t row) {
      if (!rows) return std::nullopt;
      if (rows->docId() < row && rows->advance(row) != row) return std::nullopt;
      if (rows->docId() != row) return std::nullopt;
      int32_t dims = (int32_t)replay.query.size();
      auto at = [&](int64_t rank) {
        return KnnQuery::exactScore(replay.query.data(), vectors->vectorAtRank(rank), dims,
            replay.metric, replay.normalizeColumn, 0.0f);
      };
      float best;
      if (vectors->isMultiValued()) {
        auto [begin, end] = rows->valueRange();
        best = -std::numeric_limits<float>::infinity();
        for (int64_t rank = begin; rank < end; rank++) best = std::max(best, at(rank));
      } else {
        best = at(rows->rank());
      }
      if (best < replay.floor) return std::nullopt;
      return best * replay.boost;
    }
  };
};

} // namespace luxir
