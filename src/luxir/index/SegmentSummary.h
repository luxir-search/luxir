// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "luxir/api/luxir_index.hpp"
#include "luxir/store/Directory.h"

namespace luxir {

// Immutable, decoded view of one segment's summary sidecar
// (s<seg>__summary_00_00): the mandatory per-field metadata that lets a
// query rule a segment out without opening its data files. Every answer is
// conservative: NO is a proof that no document of the segment matches,
// MAYBE proves nothing. Bounds may include deleted documents. See
// docs/design/segment-summaries.md.
//
// File layout, header first and readable as one object:
//   [0, 8)    MAGIC
//   [8, 12)   u32 FORMAT_VERSION, little-endian
//   [12, end) encoded api::SegmentSummaryInfo
class SegmentSummary {
  std::pmr::monotonic_buffer_resource arena;
  api::SegmentSummaryInfo info;
  size_t imageBytes = 0;

public:
  // api::FieldSummary::summary_flags
  enum Flags : uint32_t {
    HAS_NUM_VALUES = 1u << 0,         // num_values valid (column fields)
    MULTI_VALUED = 1u << 1,           // num_values may exceed docs_with_field
    EMPTY = 1u << 2,                  // the field record exists but no document has a value
    HAS_NUMERIC = 1u << 3,            // num_min/num_max valid
    HAS_GEO_BOX = 1u << 4,            // geo_* valid
    HAS_TERM_BOUNDS = 1u << 5,        // term_min/term_max valid
    TERM_BOUNDS_TRUNCATED = 1u << 6,  // a bound was cut to TERM_BOUND_MAX_LEN bytes
    TERM_MAX_UNBOUNDED = 1u << 7,     // term_max could not be rounded up: treat as +infinity
  };

  enum class Presence : uint8_t { NO, MAYBE };

  // The sidecar is a segment overlay (SegmentInfo.overlays) written once per
  // segment at generation 0, so it never takes a generation from the
  // overlays that get rebuilt (vector indexes).
  static constexpr std::string_view OVERLAY_KIND = "segment_summary";
  static constexpr std::string_view OVERLAY_NAME = "summary";
  static constexpr uint64_t OVERLAY_GEN = 0;
  static std::string overlayFileName(uint64_t segId);

  static constexpr std::string_view MAGIC = "LUXIRSUM";
  static constexpr uint32_t FORMAT_VERSION = 1;
  static constexpr size_t HEADER_BYTES = 12;
  static constexpr size_t TERM_BOUND_MAX_LEN = 32;

  // Opens a sidecar through `dir`. Null when the file does not exist; throws
  // std::runtime_error when it is present but not a readable summary.
  static std::shared_ptr<const SegmentSummary> open(Directory& dir, std::string_view fileName,
                                                    bool expectSynced = false);
  // Decodes a complete sidecar image; the summary keeps its own copy.
  static std::shared_ptr<const SegmentSummary> fromBytes(std::span<const std::byte> image);
  // A summary over a collection's aggregate (IndexInfo.field_summaries),
  // with its own copy of the records; seg_id and max_doc are 0.
  static std::shared_ptr<const SegmentSummary> fromAggregate(std::span<const api::FieldSummary> fields,
                                                             bool complete);

  bool complete() const noexcept { return info.complete; }
  uint64_t segId() const noexcept { return info.seg_id; }
  int32_t maxDoc() const noexcept { return info.max_doc; }
  // Size of the sidecar image.
  size_t bytes() const noexcept { return imageBytes; }
  std::span<const api::FieldSummary> fields() const noexcept { return info.fields; }

  // The record of a physical field, or null when the summary has none.
  const api::FieldSummary* field(std::string_view physicalName) const noexcept;

  // What the bounds of `record` prove. `term` and `prefix` hold the fitted
  // dictionary bytes a query looks up.
  static bool boundsExcludeTerm(const api::FieldSummary& record, std::string_view term) noexcept;
  // Terms in [prefix, successor(prefix)).
  static bool boundsExcludePrefix(const api::FieldSummary& record, std::string_view prefix) noexcept;
  // Term range [lower, upper] with optional open ends.
  static bool boundsExcludeTermRange(const api::FieldSummary& record,
                                     std::optional<std::string_view> lower, bool lowerInclusive,
                                     std::optional<std::string_view> upper, bool upperInclusive) noexcept;
  // Numeric values in the column's encoded int64 space, inclusive.
  static Presence mayIntersectNumeric(const api::FieldSummary& record, int64_t lo, int64_t hi) noexcept;
  // A geo box in encoded int32 lat/lon; a query box with minLon > maxLon
  // wraps the antimeridian.
  static Presence mayIntersectGeo(const api::FieldSummary& record, int32_t minLat, int32_t maxLat,
                                  int32_t minLon, int32_t maxLon) noexcept;

  // The smallest string greater than every string with prefix `s`: strip
  // trailing 0xFF bytes and increment the last; nullopt when no such string
  // exists (empty or all 0xFF).
  static std::optional<std::string> successor(std::string_view s);

  SegmentSummary() = default;
  SegmentSummary(const SegmentSummary&) = delete;
  SegmentSummary& operator=(const SegmentSummary&) = delete;

private:
  void decode(std::span<const std::byte> image);
  void decodeBody(std::span<const std::byte> body);
};

} // namespace luxir
