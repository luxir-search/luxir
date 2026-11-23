// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once
// Segment pruning: evaluates logical query trees against the segments' field
// record headers (Query::mayMatch over a CatalogSummaryView) and turns the
// verdicts into the segments an op may match. Evidence is evaluated
// bottom-up, once per query and segment of a request; nothing here reads a
// segment's data. See docs/design/storage.md (Pruning).

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
  // Whether the merged headers prove the query empty, by query.
  boost::unordered_flat_map<const Query*, bool> collectionMemo;

public:
  // Disables every proof, including the skipped statistics seeks, so pruned
  // and unpruned execution can be compared.
  static inline bool disableForTests = false;
  // Cross-check mode (tests, and the hidden --search.pruning-cross-check
  // flag): work a proof skips still runs, and a match it finds is an error.
  // Responses are unchanged.
  static inline bool crossCheck = false;

  explicit QueryPruning(IndexReader& reader) : reader(reader) {}

  // Whether the segments' headers, merged per field, prove that no document
  // of any segment, deleted or not, matches `query`. Evaluated once, before
  // any weight exists, and memoized. A query with a part that offers no
  // proof is never proven empty here, as that part may use fields the proof
  // did not look up (and so did not type check).
  bool collectionEmpty(const Query& query);

  // Why the merged headers prove `query` empty, for explain and profile
  // notes; empty when they do not.
  std::string collectionReason(const Query& query);

  // The segments where `query`'s header proofs allow a match, deletes
  // ignored, by ordinal, memoized for the request. A segment outside it holds
  // no document matching `query`, deleted or not, so its cached membership
  // is exactly empty. A query proven empty for the collection gets the empty
  // mask without a proof per segment.
  const SegmentMask& membership(const Query& query);

  // The segments `query` may match: membership() without the segments that
  // have no live documents. Memoized for the request.
  const SegmentMask& viable(const Query& query);

  // Why `query` cannot match segment `ord`, for explain and profile notes.
  std::string reason(const Query& query, size_t ord);

  // Whether segment `segment`'s header of `field` proves `term` absent from
  // the field's dictionary, so its statistics are exactly zero there and
  // need no seek.
  static bool termAbsent(const IndexReader::FieldSegments& field, size_t segment, std::string_view term);

  // Thrown by cross-check mode when pruned work finds a match.
  [[noreturn]] static void crossCheckFailed(std::string_view what);
};

} // namespace luxir
