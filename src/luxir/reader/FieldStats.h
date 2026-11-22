// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>
#include <variant>

#include "luxir/schema/FieldType.h"

// Per-field statistics kept in the field record header: counts and value bounds.
// They describe the segment as written, before deletes, and can be read without
// opening any of the field's data (segment pruning, planning).
namespace luxir::field_parts {

// Bounds of a term dictionary, from its first and last term. Long
// endpoints are cut to a fixed cap, so each side says how exact it is.
struct DictionaryBounds {
  // EXACT: lower is the smallest term. ROUNDED: lower is a prefix of it, so
  // lower <= every term.
  enum class Lower : uint8_t { EXACT, ROUNDED, LAST = ROUNDED };
  // INCLUSIVE: upper is the largest term. EXCLUSIVE: upper is its cut prefix
  // rounded up, so every term < upper. UNBOUNDED: no bound could be formed.
  enum class Upper : uint8_t { INCLUSIVE, EXCLUSIVE, UNBOUNDED, LAST = UNBOUNDED };

  PackedTerm lower;  // Endpoint in the writer's pool or mapped record.
  PackedTerm upper;  // Endpoint in the same storage, null for UNBOUNDED.
  Lower lowerMode;
  Upper upperMode;

  bool operator==(const DictionaryBounds& other) const {
    return lowerMode == other.lowerMode && upperMode == other.upperMode
        && lower == other.lower && (upperMode == Upper::UNBOUNDED || upper == other.upper);
  }
};

// Inclusive [min, max] of values along one axis. Starts empty (min > max) and
// grows with include().
template <class T>
struct AxisBounds {
  T min = std::numeric_limits<T>::max();
  T max = std::numeric_limits<T>::min();

  bool empty() const noexcept { return min > max; }

  void include(T lo, T hi) noexcept {
    min = std::min(min, lo);
    max = std::max(max, hi);
  }

  bool operator==(const AxisBounds&) const = default;
};

// Bounds of a number column, in the column's int64 encoding (the same order the
// field sorts in).
using ScalarBounds = AxisBounds<int64_t>;

// Bounding box of a geo point field, in encoded latitude and longitude.
struct GeoBounds {
  AxisBounds<int32_t> lat;
  AxisBounds<int32_t> lon;

  void include(int32_t latitude, int32_t longitude) noexcept {
    lat.include(latitude, latitude);
    lon.include(longitude, longitude);
  }

  bool operator==(const GeoBounds&) const = default;
};

// Bounds of an interval (range) field: where its intervals start and where they end.
struct IntervalBounds {
  ScalarBounds low;   // Bounds over the low endpoints.
  ScalarBounds high;  // Bounds over the high endpoints.

  bool operator==(const IntervalBounds&) const = default;
};

// A field has the alternative its type calls for, or none (monostate) when the field
// has no values or its type keeps no bounds. The active alternative is determined by
// the field type when decoding.
using Bounds = std::variant<std::monostate, ScalarBounds, DictionaryBounds, GeoBounds, IntervalBounds>;

// The kind of bounds a field can carry.
enum class BoundsKind : uint8_t { NONE, SCALAR, DICTIONARY, GEO, INTERVAL, LAST = INTERVAL };

// Which bounds a field can have when there are values to bound.
inline constexpr BoundsKind boundsKind(FieldType::Type type, FieldType::flag_type flags) {
  if (FieldType::isNumericColumn(type)) return BoundsKind::SCALAR;
  if (type == FieldType::GEO_POINT) return BoundsKind::GEO;
  if (type == FieldType::INTERVAL) return BoundsKind::INTERVAL;
  auto storage = FieldType::storage(type);
  if ((flags & FieldType::INDEX_DOCS) && (storage == FieldType::Storage::TERMS || storage == FieldType::Storage::TEXT)) {
    return BoundsKind::DICTIONARY;
  }
  return BoundsKind::NONE;
}

// Dictionary and postings statistics of an indexed field, as written (before deletes).
struct IndexedTotals {
  int64_t numTerms = 0;          // Number of distinct terms.
  int64_t sumDocFreq = 0;        // Sum of the terms' doc frequencies.
  int64_t sumTotalTermFreq = 0;  // Sum of the term frequencies, at least sumDocFreq.

  bool operator==(const IndexedTotals&) const = default;
};

} // namespace luxir::field_parts
