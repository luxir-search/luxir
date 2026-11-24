// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once
// Segment pruning: evaluates logical query trees against the segments' field
// record headers (Query::mayMatch over a CatalogSummaryView) and turns the
// verdicts into a root weight's membership mask. Masks belong to the query
// and its validation context. See docs/design/storage.md (Pruning).

#include <map>
#include <string>
#include <string_view>

#include <boost/unordered/unordered_node_map.hpp>
#include <boost/unordered/unordered_flat_map.hpp>

#include "SegmentMask.h"
#include "Query.h"

namespace luxir {

class QueryPruning {
  friend class CatalogSummaryView;
  using PlanKey = std::pair<const Query::PlanningContext*, const Query*>;
  boost::unordered_node_map<PlanKey, SegmentMask> membershipMemo;

  // All plans on a reader share its positive, absent and unusable fields.
  using FieldMemo = boost::unordered_flat_map<std::string, std::optional<IndexReader::FieldSegments>,
                                               PackedTermHash, PackedTermEqual>;
  std::map<const IndexReader*, FieldMemo> fields;

public:
  // Disables every proof, including the skipped statistics seeks, so pruned
  // and unpruned execution can be compared.
  static inline bool disableForTests = false;
  // Verify skipped statistics seeks against the dictionary.
  static inline bool crossCheck = false;

  // The segments where `query`'s header proofs allow a match, deletes
  // ignored, by ordinal, memoized for the request. A segment outside it holds
  // no document matching `query`, deleted or not, so its cached membership
  // is exactly empty.
  const SegmentMask& membership(const Query& query, const Query::PlanningContext& planning);

  // Thrown by cross-check mode when pruned work finds a match.
  [[noreturn]] static void crossCheckFailed(std::string_view what);
};

} // namespace luxir
