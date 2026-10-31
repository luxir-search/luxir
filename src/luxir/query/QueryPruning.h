// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once
// Segment pruning: evaluates logical query trees against segment summaries
// (Query::mayMatch) and turns the verdicts into the segments an op may
// match. Evidence is evaluated bottom-up, once per query and segment of a
// request; nothing here opens a segment's data. See
// docs/design/segment-summaries.md.

#include <string>
#include <string_view>

#include <boost/unordered/unordered_flat_map.hpp>

#include "SegmentMask.h"
#include "luxir/search/IndexReader.h"

namespace luxir {

class Query;

class QueryPruning {
  IndexReader& reader;
  boost::unordered_flat_map<const Query*, SegmentMask> memo;
  boost::unordered_flat_map<const Query*, SegmentMask> membershipMemo;

public:
  // Disables every proof, including the skipped statistics seeks, so pruned
  // and unpruned execution can be compared.
  static inline bool disableForTests = false;

  explicit QueryPruning(IndexReader& reader) : reader(reader) {}

  // The segments where `query`'s summary proofs allow a match, deletes
  // ignored, by ordinal, memoized for the request. A segment outside it holds
  // no document matching `query`, deleted or not, so its cached membership
  // is exactly empty.
  const SegmentMask& membership(const Query& query);

  // The segments `query` may match: membership() without the segments that
  // have no live documents. Memoized for the request.
  const SegmentMask& viable(const Query& query);

  // Why `query` cannot match segment `ord`, for explain and profile notes.
  std::string reason(const Query& query, size_t ord) const;

  // Whether `segment`'s summary proves `term` absent from the dictionary of
  // `field`, so its statistics are exactly zero there and need no seek.
  static bool termAbsent(const IndexReader::Segment& segment, std::string_view field, std::string_view term);
};

} // namespace luxir
