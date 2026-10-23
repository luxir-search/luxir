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
#include <vector>

#include "luxir/api/luxir_index.hpp"
#include "luxir/store/Directory.h"

namespace luxir {

// Immutable, decoded view of one segment's summary sidecar (s<seg>__Y<gen>),
// or of a collection's aggregate FieldSummary records from the manifest
// root. Every answer is conservative: NO is a proof that no document of the
// summarized data matches, MAYBE proves nothing. Bounds may include deleted
// documents. See docs/design/segment-summaries.md.
//
// Sidecar layout, header first so a remote directory can serve the compact
// part with one small read:
//   [0, 8)    Postings::SUMMARY_HEADER
//   [8, 12)   u32 infoLen    encoded api::SegmentSummaryInfo length
//   [12, 16)  u32 indexLen   end of section 1 including padding; payload area base
//   [16, ...) section 1: SegmentSummaryInfo (field records, payload directory)
//   [indexLen, size) payloads, each 8-aligned; SummaryPayload.offset is relative to indexLen
class SegmentSummary {
public:
  // api::FieldSummary::flags
  enum Flags : uint32_t {
    HAS_NUM_VALUES = 1u << 0,         // num_values valid (column fields)
    MULTI_VALUED = 1u << 1,           // num_values may exceed docs_with_field
    EMPTY = 1u << 2,                  // the field record exists but no document has a value
    HAS_NUMERIC = 1u << 3,            // num_min/num_max valid
    HAS_GEO_BOX = 1u << 4,            // geo_* valid
    HAS_TERM_BOUNDS = 1u << 5,        // term_min/term_max valid
    TERM_BOUNDS_TRUNCATED = 1u << 6,  // bounds were cut to TERM_BOUND_MAX_LEN bytes
    TERM_MAX_UNBOUNDED = 1u << 7,     // term_max could not be rounded up: treat as +infinity
    TERMS_OVERFLOW = 1u << 8,         // the term set exceeded its budget; no presence structure
    MAY_HAVE_FITTED = 1u << 9,        // some term may be a truncated or hashed long value
  };

  // api::SummaryPayload::kind
  enum PayloadKind : uint32_t {
    EXACT_SET = 1,       // u32 offsets[count] (relative to the payload) then u16 len + bytes, sorted
    FUSE8 = 2,           // BinaryFuse8 over summary::termKey(term)
    BLOOM = 3,           // BloomFilter over summary::termKey(term)
    PREFIX_FUSE8 = 4,    // BinaryFuse8 over summary::termKey(term.substr(0, param))
    VECTOR_CENTROID = 5, // u32 dims, u32 pad, u64 count, f32 radius, f32 pad, f32 centroid[], boxMin[], boxMax[]
  };

  enum class Presence : uint8_t { NO, MAYBE };

  // The sidecar is a segment overlay (SegmentInfo.overlays): kind, name and
  // file naming. gen is a per-segment overlay generation like any overlay's.
  static constexpr std::string_view OVERLAY_KIND = "segment_summary";
  static constexpr std::string_view OVERLAY_NAME = "summary";
  static std::string overlayFileName(uint64_t segId, uint64_t gen);

  static constexpr uint32_t FORMAT_VERSION = 1;
  static constexpr uint32_t PAYLOAD_VERSION = 1;
  static constexpr size_t TERM_BOUND_MAX_LEN = 32;
  static constexpr size_t HEADER_BYTES = 16;
  static constexpr size_t PAYLOAD_ALIGN = 8;

  struct VectorCentroid {
    uint32_t dims = 0;
    uint64_t count = 0;
    float radius = 0;
    std::span<const float> centroid;
    std::span<const float> boxMin;
    std::span<const float> boxMax;
  };

  // Opens a sidecar through `dir`. Null when the file does not exist; throws
  // std::runtime_error when it is present but not a readable summary.
  static std::shared_ptr<const SegmentSummary> open(Directory& dir, std::string_view fileName,
                                                    bool expectSynced = false);
  // Decodes a complete sidecar image that `file` keeps alive.
  static std::shared_ptr<const SegmentSummary> fromFile(std::shared_ptr<InputFile> file);
  // Decodes an owned image (the writer's copy of what it just wrote).
  static std::shared_ptr<const SegmentSummary> fromBytes(std::shared_ptr<const std::vector<std::byte>> image);
  // A view over aggregate records (no payloads). `keepAlive` owns what the
  // records point into, typically the manifest bytes.
  static std::shared_ptr<const SegmentSummary> fromAggregate(std::span<const api::FieldSummary> fields,
                                                             bool complete,
                                                             std::shared_ptr<const void> keepAlive);

  bool complete() const noexcept { return info.complete; }
  bool isAggregate() const noexcept { return aggregate; }
  uint64_t segId() const noexcept { return info.seg_id; }
  int32_t maxDoc() const noexcept { return info.max_doc; }
  uint32_t hashVersion() const noexcept { return info.hash_version; }
  // Bytes of the backing image: the sidecar size, 0 for an aggregate.
  size_t bytes() const noexcept { return image.size(); }
  std::span<const api::FieldSummary> fields() const noexcept { return info.fields; }

  // The record of a physical field, or null when the summary has none.
  const api::FieldSummary* field(std::string_view physicalName) const noexcept;

  // The payload of `kind` on `record`; for PREFIX_FUSE8 the longest prefix
  // length not above `maxParam`. Null when absent.
  const api::SummaryPayload* payload(const api::FieldSummary& record, PayloadKind kind,
                                     uint32_t maxParam = UINT32_MAX) const noexcept;
  // The bytes of a payload, empty when it lies outside the image.
  std::span<const std::byte> payloadBytes(const api::SummaryPayload& payload) const noexcept;

  // Term presence, tiered from cheapest to most expensive. `term` holds the
  // fitted dictionary bytes the query side computes.
  static bool boundsExcludeTerm(const api::FieldSummary& record, std::string_view term) noexcept;
  // nullopt when the record has no exact set.
  std::optional<bool> exactSetContains(const api::FieldSummary& record, std::string_view term) const noexcept;
  // MAYBE when there is no usable filter (none, unknown kind, hash version mismatch).
  Presence filterMayContain(const api::FieldSummary& record, std::string_view term) const noexcept;
  Presence mayContainTerm(const api::FieldSummary& record, std::string_view term) const noexcept;

  // Prefix presence: terms in [prefix, successor(prefix)).
  static bool boundsExcludePrefix(const api::FieldSummary& record, std::string_view prefix) noexcept;
  std::optional<bool> exactSetHasPrefix(const api::FieldSummary& record, std::string_view prefix) const noexcept;
  Presence prefixFilterMayContain(const api::FieldSummary& record, std::string_view prefix) const noexcept;
  Presence mayContainPrefix(const api::FieldSummary& record, std::string_view prefix) const noexcept;

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

  std::optional<VectorCentroid> vectorCentroid(const api::FieldSummary& record) const noexcept;

  // The smallest string greater than every string with prefix `s`: strip
  // trailing 0xFF bytes and increment the last; nullopt when no such string
  // exists (empty or all 0xFF).
  static std::optional<std::string> successor(std::string_view s);

  SegmentSummary() = default;
  SegmentSummary(const SegmentSummary&) = delete;
  SegmentSummary& operator=(const SegmentSummary&) = delete;

private:
  std::shared_ptr<InputFile> file;
  std::shared_ptr<const void> keepAlive;
  std::unique_ptr<std::pmr::monotonic_buffer_resource> arena;
  api::SegmentSummaryInfo info;
  std::span<const std::byte> image;
  size_t payloadBase = 0;
  bool aggregate = false;

  void decodeImage();
};

} // namespace luxir
