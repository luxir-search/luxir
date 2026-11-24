// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once
// Shared Query::mayMatch evaluations over a CatalogSummaryView, one per
// predicate shape. EMPTY is a proof; UNKNOWN is the fallback. Term proofs
// compare the bytes execution seeks (analyzed and fitted, as the query holds
// them) with the dictionary bounds, which cover the fitted bytes the
// dictionary stores; there are no membership structures, so a term inside
// the bounds is never proven absent.

#include <climits>
#include <optional>
#include <string_view>

#include "CatalogSummaryView.h"

namespace luxir::proof {

using MatchState = Query::MatchState;
using field_parts::DictionaryBounds;

// Whether `bounds` prove no dictionary term equals `term`.
inline bool boundsExcludeTerm(const DictionaryBounds& bounds, std::string_view term) noexcept {
  auto lower = std::string_view(bounds.lower);
  // ROUNDED: lower is a strict prefix of the smallest term.
  if (term < lower || (term == lower && bounds.lowerMode == DictionaryBounds::Lower::ROUNDED)) return true;
  switch (bounds.upperMode) {
    case DictionaryBounds::Upper::INCLUSIVE: return term > std::string_view(bounds.upper);
    case DictionaryBounds::Upper::EXCLUSIVE: return term >= std::string_view(bounds.upper);
    case DictionaryBounds::Upper::UNBOUNDED: return false;
  }
  return false;
}

// Whether `bounds` prove no dictionary term starts with `prefix`.
inline bool boundsExcludePrefix(const DictionaryBounds& bounds, std::string_view prefix) noexcept {
  if (prefix.empty()) return false;
  // Every term is >= lower: a lower above every string with the prefix (greater
  // and not itself starting with it) leaves none.
  auto lower = std::string_view(bounds.lower);
  if (lower > prefix && !lower.starts_with(prefix)) return true;
  switch (bounds.upperMode) {
    case DictionaryBounds::Upper::INCLUSIVE: return std::string_view(bounds.upper) < prefix;
    case DictionaryBounds::Upper::EXCLUSIVE: return std::string_view(bounds.upper) <= prefix;
    case DictionaryBounds::Upper::UNBOUNDED: return false;
  }
  return false;
}

// Whether `bounds` prove no dictionary term lies in the range. A missing
// side is unbounded.
inline bool boundsExcludeTermRange(const DictionaryBounds& bounds,
                                   std::optional<std::string_view> lower, bool lowerInclusive,
                                   std::optional<std::string_view> upper, bool upperInclusive) noexcept {
  if (upper) {
    // Matching terms are <= upper (< when exclusive); every dictionary term is
    // >= its lower bound (> when ROUNDED).
    auto dictLower = std::string_view(bounds.lower);
    bool strict = !upperInclusive || bounds.lowerMode == DictionaryBounds::Lower::ROUNDED;
    if (*upper < dictLower || (*upper == dictLower && strict)) return true;
  }
  if (lower) {
    switch (bounds.upperMode) {
      case DictionaryBounds::Upper::INCLUSIVE: {
        auto dictUpper = std::string_view(bounds.upper);
        if (*lower > dictUpper || (*lower == dictUpper && !lowerInclusive)) return true;
        break;
      }
      case DictionaryBounds::Upper::EXCLUSIVE:
        if (*lower >= std::string_view(bounds.upper)) return true;
        break;
      case DictionaryBounds::Upper::UNBOUNDED:
        break;
    }
  }
  return false;
}

// Field absence and a record without documents prove every value-requiring
// predicate empty. Returns the record to consult for bounds, or null with
// `out` set.
inline const DecodedFieldRecord* requireField(const CatalogSummaryView& view, std::string_view field,
                                             MatchState& out) {
  out = MatchState::UNKNOWN;
  auto evidence = view.record(field);
  if (!evidence) return nullptr;
  const auto* record = *evidence;
  if (record == nullptr) {
    view.prove("field absent");
    out = MatchState::EMPTY;
    return nullptr;
  }
  if (record->docsWithField == 0) {
    view.prove("field has no values");
    out = MatchState::EMPTY;
    return nullptr;
  }
  return record;
}

// Whether the record proves `term` absent from the field's dictionary: no
// terms, or term bounds that exclude it. Null (no record) is absence too.
inline bool recordExcludesTerm(const DecodedFieldRecord* record, std::string_view term) noexcept {
  if (record == nullptr || record->docsWithField == 0) return true;
  if ((record->flags & FieldType::INDEX_DOCS) && record->indexedTotals().numTerms == 0) return true;
  const auto* bounds = record->dictionaryBounds();
  return bounds != nullptr && boundsExcludeTerm(*bounds, term);
}

// The dictionary bounds of an indexed record, or null with `out` set when
// the field indexes no terms.
inline const DictionaryBounds* dictionary(const CatalogSummaryView& view, const DecodedFieldRecord& record,
                                          MatchState& out) {
  if ((record.flags & FieldType::INDEX_DOCS) && record.indexedTotals().numTerms == 0) {
    view.prove("field has no terms");
    out = MatchState::EMPTY;
    return nullptr;
  }
  return record.dictionaryBounds();
}

inline MatchState termMatch(const CatalogSummaryView& view, std::string_view field, std::string_view term) {
  MatchState state;
  const auto* record = requireField(view, field, state);
  if (record == nullptr) return state;
  const auto* bounds = dictionary(view, *record, state);
  if (bounds != nullptr && boundsExcludeTerm(*bounds, term)) {
    view.prove("term outside term bounds");
    return MatchState::EMPTY;
  }
  return state;
}

inline MatchState prefixMatch(const CatalogSummaryView& view, std::string_view field, std::string_view prefix) {
  MatchState state;
  const auto* record = requireField(view, field, state);
  if (record == nullptr) return state;
  const auto* bounds = dictionary(view, *record, state);
  if (bounds != nullptr && boundsExcludePrefix(*bounds, prefix)) {
    view.prove("prefix outside term bounds");
    return MatchState::EMPTY;
  }
  return state;
}

inline MatchState termRangeMatch(const CatalogSummaryView& view, std::string_view field,
                                 std::optional<std::string_view> lower, bool lowerInclusive,
                                 std::optional<std::string_view> upper, bool upperInclusive) {
  MatchState state;
  const auto* record = requireField(view, field, state);
  if (record == nullptr) return state;
  const auto* bounds = dictionary(view, *record, state);
  if (bounds != nullptr && boundsExcludeTermRange(*bounds, lower, lowerInclusive, upper, upperInclusive)) {
    view.prove("term range outside term bounds");
    return MatchState::EMPTY;
  }
  return state;
}

// Expansions are unknown without the dictionary; a field without terms
// proves them empty.
inline MatchState termsMatch(const CatalogSummaryView& view, std::string_view field) {
  MatchState state;
  const auto* record = requireField(view, field, state);
  if (record == nullptr) return state;
  dictionary(view, *record, state);
  return state;
}

// `intersects(min, max)` answers whether the predicate admits a value inside
// the column's encoded bounds (NumericPredicateQuery::intersects handles
// value sets).
template <typename Intersects>
inline MatchState numericMatch(const CatalogSummaryView& view, std::string_view field, Intersects&& intersects) {
  MatchState state;
  const auto* record = requireField(view, field, state);
  if (record == nullptr) return state;
  const auto* bounds = record->scalarBounds();
  if (bounds != nullptr && !bounds->empty() && !intersects(bounds->min, bounds->max)) {
    view.prove("numeric bounds disjoint");
    return MatchState::EMPTY;
  }
  return MatchState::UNKNOWN;
}

inline MatchState existsMatch(const CatalogSummaryView& view, std::string_view field) {
  MatchState state;
  requireField(view, field, state);
  return state;
}

// An encoded box against the field's encoded bounding box. A box whose
// longitude interval wraps the dateline has minLon > maxLon.
inline MatchState geoMatch(const CatalogSummaryView& view, std::string_view field, int32_t minLat, int32_t maxLat,
                           int32_t minLon, int32_t maxLon) {
  MatchState state;
  const auto* record = requireField(view, field, state);
  if (record == nullptr) return state;
  const auto* bounds = record->geoBounds();
  if (bounds == nullptr || bounds->lat.empty() || bounds->lon.empty()) return MatchState::UNKNOWN;
  auto overlaps = [](const field_parts::AxisBounds<int32_t>& axis, int32_t lo, int32_t hi) {
    return !(hi < axis.min || lo > axis.max);
  };
  bool lonOverlaps = minLon <= maxLon
      ? overlaps(bounds->lon, minLon, maxLon)
      : overlaps(bounds->lon, minLon, INT32_MAX) || overlaps(bounds->lon, INT32_MIN, maxLon);
  if (minLat > maxLat || !overlaps(bounds->lat, minLat, maxLat) || !lonOverlaps) {
    view.prove("geo bounds disjoint");
    return MatchState::EMPTY;
  }
  return MatchState::UNKNOWN;
}

// A range field query is one inclusive box over each interval's encoded
// (low, high) endpoints; the record bounds every low and every high.
inline MatchState intervalMatch(const CatalogSummaryView& view, std::string_view field,
                                const int64_t lo[2], const int64_t hi[2]) {
  MatchState state;
  const auto* record = requireField(view, field, state);
  if (record == nullptr) return state;
  const auto* bounds = record->intervalBounds();
  if (bounds == nullptr || bounds->low.empty() || bounds->high.empty()) return MatchState::UNKNOWN;
  auto disjoint = [](const field_parts::ScalarBounds& axis, int64_t qlo, int64_t qhi) {
    return axis.max < qlo || qhi < axis.min;
  };
  if (disjoint(bounds->low, lo[0], hi[0]) || disjoint(bounds->high, lo[1], hi[1])) {
    view.prove("interval bounds disjoint");
    return MatchState::EMPTY;
  }
  return MatchState::UNKNOWN;
}

} // namespace luxir::proof
