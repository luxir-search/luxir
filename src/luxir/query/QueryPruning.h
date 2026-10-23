// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once
// Segment and collection pruning: evaluates a logical query tree against
// segment summaries (Query::mayMatch) and turns the verdicts into the mask of
// segments a weight may execute against. Nothing here opens a segment.

#include <string>
#include <vector>

#include "SegmentMask.h"
#include "SummaryView.h"
#include "luxir/search/IndexReader.h"

namespace luxir {

class Query;

struct SegmentVerdict {
  uint64_t segId = 0;
  int32_t ord = 0;
  bool pruned = false;
  std::string reason;  // only when pruned and explain was requested
};

class QueryPruning {
public:
  // Disables every proof so pruned and unpruned execution can be compared.
  static inline bool disableForTests = false;

  // The segments `query` may match, by ordinal, evaluated cheap tier first
  // (counts, presence, bounds, exact sets) and the filter tier only for the
  // survivors. A segment without live documents is never viable. `verdicts`,
  // when given, receives one entry per segment, with reasons when `explain`.
  static SegmentMask viableSegments(const Query& query, IndexReader& reader, MemPool& pool,
                                    std::vector<SegmentVerdict>* verdicts = nullptr, bool explain = false);

  // Whether the collection whose manifest carries `aggregate` may hold a
  // match. An absent or incomplete aggregate means it may.
  static bool collectionMayMatch(const Query& query, const SegmentSummary* aggregate, std::string* reason = nullptr);
};

} // namespace luxir
