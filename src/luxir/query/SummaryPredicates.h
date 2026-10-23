// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once
// Shared Query::mayMatch evaluations over a SummaryView, one per predicate
// shape. Each consults evidence from the cheapest tier up and stops where the
// view stops allowing tiers. EMPTY is a proof; UNKNOWN is the fallback.

#include <optional>
#include <string_view>

#include "Query.h"

namespace luxir::summary {

using MatchState = Query::MatchState;
using Presence = SegmentSummary::Presence;

// Field presence and an empty record are the first two tiers of every
// value-requiring predicate.
inline const api::FieldSummary* requireField(const SummaryView& view, std::string_view field, MatchState& out) {
  if (view.allows(PruneTier::PRESENCE) && view.presence(field) == FieldPresence::ABSENT) {
    view.prove("field absent");
    out = MatchState::EMPTY;
    return nullptr;
  }
  const api::FieldSummary* record = view.record(field);
  if (record == nullptr) {
    out = MatchState::UNKNOWN;
    return nullptr;
  }
  if (view.allows(PruneTier::PRESENCE) && (record->summary_flags & SegmentSummary::EMPTY) != 0) {
    view.prove("field has no values");
    out = MatchState::EMPTY;
    return nullptr;
  }
  out = MatchState::UNKNOWN;
  return record;
}

inline MatchState termMatch(const SummaryView& view, std::string_view field, std::string_view term) {
  MatchState state;
  const auto* record = requireField(view, field, state);
  if (record == nullptr) return state;
  const SegmentSummary* summary = view.summaryOrNull();
  if (view.allows(PruneTier::BOUNDS) && SegmentSummary::boundsExcludeTerm(*record, term)) {
    view.prove("term outside term bounds");
    return MatchState::EMPTY;
  }
  if (view.allows(PruneTier::EXACT)) {
    if (auto exact = summary->exactSetContains(*record, term)) {
      if (!*exact) {
        view.prove("term not in exact set");
        return MatchState::EMPTY;
      }
      return MatchState::UNKNOWN;
    }
  }
  if (view.allows(PruneTier::FILTER) && summary->filterMayContain(*record, term) == Presence::NO) {
    view.prove("term filter: absent");
    return MatchState::EMPTY;
  }
  return MatchState::UNKNOWN;
}

inline MatchState prefixMatch(const SummaryView& view, std::string_view field, std::string_view prefix) {
  MatchState state;
  const auto* record = requireField(view, field, state);
  if (record == nullptr) return state;
  const SegmentSummary* summary = view.summaryOrNull();
  if (view.allows(PruneTier::BOUNDS) && SegmentSummary::boundsExcludePrefix(*record, prefix)) {
    view.prove("prefix outside term bounds");
    return MatchState::EMPTY;
  }
  if (view.allows(PruneTier::EXACT)) {
    if (auto exact = summary->exactSetHasPrefix(*record, prefix)) {
      if (!*exact) {
        view.prove("prefix not in exact set");
        return MatchState::EMPTY;
      }
      return MatchState::UNKNOWN;
    }
  }
  if (view.allows(PruneTier::FILTER) && summary->prefixFilterMayContain(*record, prefix) == Presence::NO) {
    view.prove("prefix filter: absent");
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
  if (view.allows(PruneTier::BOUNDS)
      && SegmentSummary::boundsExcludeTermRange(*record, lower, lowerInclusive, upper, upperInclusive)) {
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
  if (view.allows(PruneTier::BOUNDS) && (record->summary_flags & SegmentSummary::HAS_NUMERIC) != 0
      && !intersects(record->num_min, record->num_max)) {
    view.prove("numeric bounds disjoint");
    return MatchState::EMPTY;
  }
  return MatchState::UNKNOWN;
}

inline MatchState existsMatch(const SummaryView& view, std::string_view field) {
  MatchState state;
  const auto* record = requireField(view, field, state);
  if (record == nullptr) return state;
  if (view.allows(PruneTier::PRESENCE) && record->docs_with_field == 0) {
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
  if (view.allows(PruneTier::BOUNDS)
      && SegmentSummary::mayIntersectGeo(*record, minLat, maxLat, minLon, maxLon) == Presence::NO) {
    view.prove("geo bounds disjoint");
    return MatchState::EMPTY;
  }
  return MatchState::UNKNOWN;
}

} // namespace luxir::summary
