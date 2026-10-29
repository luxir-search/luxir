// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once
// Shared Query::mayMatch evaluations over a SummaryView, one per predicate
// shape. EMPTY is a proof; UNKNOWN is the fallback. Term and prefix proofs
// use field presence and term bounds only: the summary carries no
// membership structures.

#include <optional>
#include <string_view>

#include "Query.h"

namespace luxir::summary {

using MatchState = Query::MatchState;
using Presence = SegmentSummary::Presence;

// Field presence and an empty record prove every value-requiring predicate
// empty. Returns the record to consult for bounds, or null with `out` set.
inline const api::FieldSummary* requireField(const SummaryView& view, std::string_view field, MatchState& out) {
  out = MatchState::UNKNOWN;
  if (view.presence(field) == FieldPresence::ABSENT) {
    view.prove("field absent");
    out = MatchState::EMPTY;
    return nullptr;
  }
  const api::FieldSummary* record = view.record(field);
  if (record == nullptr) return nullptr;
  if ((record->summary_flags & SegmentSummary::EMPTY) != 0) {
    view.prove("field has no values");
    out = MatchState::EMPTY;
    return nullptr;
  }
  return record;
}

inline MatchState termMatch(const SummaryView& view, std::string_view field, std::string_view term) {
  MatchState state;
  const auto* record = requireField(view, field, state);
  if (record == nullptr) return state;
  if (SegmentSummary::boundsExcludeTerm(*record, term)) {
    view.prove("term outside term bounds");
    return MatchState::EMPTY;
  }
  return MatchState::UNKNOWN;
}

inline MatchState prefixMatch(const SummaryView& view, std::string_view field, std::string_view prefix) {
  MatchState state;
  const auto* record = requireField(view, field, state);
  if (record == nullptr) return state;
  if (SegmentSummary::boundsExcludePrefix(*record, prefix)) {
    view.prove("prefix outside term bounds");
    return MatchState::EMPTY;
  }
  return MatchState::UNKNOWN;
}

inline MatchState termRangeMatch(const SummaryView& view, std::string_view field,
                                 std::optional<std::string_view> lower, bool lowerInclusive,
                                 std::optional<std::string_view> upper, bool upperInclusive) {
  MatchState state;
  const auto* record = requireField(view, field, state);
  if (record == nullptr) return state;
  if (SegmentSummary::boundsExcludeTermRange(*record, lower, lowerInclusive, upper, upperInclusive)) {
    view.prove("term range outside term bounds");
    return MatchState::EMPTY;
  }
  return MatchState::UNKNOWN;
}

// `intersects(min, max)` answers whether the predicate admits a value inside
// the record's bounds (NumericPredicateQuery::intersects handles value sets).
template <typename Intersects>
inline MatchState numericMatch(const SummaryView& view, std::string_view field, Intersects&& intersects) {
  MatchState state;
  const auto* record = requireField(view, field, state);
  if (record == nullptr) return state;
  if ((record->summary_flags & SegmentSummary::HAS_NUMERIC) != 0 && !intersects(record->num_min, record->num_max)) {
    view.prove("numeric bounds disjoint");
    return MatchState::EMPTY;
  }
  return MatchState::UNKNOWN;
}

inline MatchState existsMatch(const SummaryView& view, std::string_view field) {
  MatchState state;
  const auto* record = requireField(view, field, state);
  if (record == nullptr) return state;
  if (record->docs_with_field == 0) {
    view.prove("field has no documents");
    return MatchState::EMPTY;
  }
  return MatchState::UNKNOWN;
}

inline MatchState geoMatch(const SummaryView& view, std::string_view field, int32_t minLat, int32_t maxLat,
                           int32_t minLon, int32_t maxLon) {
  MatchState state;
  const auto* record = requireField(view, field, state);
  if (record == nullptr) return state;
  if (SegmentSummary::mayIntersectGeo(*record, minLat, maxLat, minLon, maxLon) == Presence::NO) {
    view.prove("geo bounds disjoint");
    return MatchState::EMPTY;
  }
  return MatchState::UNKNOWN;
}

} // namespace luxir::summary
