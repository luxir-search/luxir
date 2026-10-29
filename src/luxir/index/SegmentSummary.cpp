// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "SegmentSummary.h"

#include <algorithm>
#include <climits>
#include <cstring>
#include <stdexcept>

#include "luxir/api/padded_input.h"
#include "luxir/reader/Postings.h"

namespace luxir {

namespace {

std::string_view viewOf(::hpp_proto::bytes_view bytes) {
  return std::string_view((const char*)bytes.data(), bytes.size());
}

} // namespace

std::string SegmentSummary::overlayFileName(uint64_t segId) {
  return Postings::getSegmentOverlayFileName(segId, OVERLAY_NAME, OVERLAY_GEN, 0);
}

std::shared_ptr<const SegmentSummary> SegmentSummary::open(Directory& dir, std::string_view fileName,
                                                           bool expectSynced) {
  auto file = dir.openFile(fileName, expectSynced);
  if (!file) return nullptr;
  std::string_view data = file->read();
  return fromBytes(std::span<const std::byte>((const std::byte*)data.data(), data.size()));
}

std::shared_ptr<const SegmentSummary> SegmentSummary::fromBytes(std::span<const std::byte> image) {
  auto summary = std::make_shared<SegmentSummary>();
  summary->decode(image);
  return summary;
}

void SegmentSummary::decode(std::span<const std::byte> image) {
  if (image.size() < HEADER_BYTES) throw std::runtime_error("segment summary: file too short");
  if (std::string_view((const char*)image.data(), MAGIC.size()) != MAGIC) {
    throw std::runtime_error("segment summary: bad header");
  }
  uint32_t version;
  std::memcpy(&version, image.data() + MAGIC.size(), sizeof(version));
  if (version != FORMAT_VERSION) {
    throw std::runtime_error("segment summary: unsupported format version " + std::to_string(version));
  }
  // Decoded views point into the arena's padded copy, so the image need not
  // outlive the summary.
  if (!api::decode(info, api::copyToPaddedInput(image.subspan(HEADER_BYTES), arena), arena)) {
    throw std::runtime_error("segment summary: undecodable field records");
  }
  if (!std::ranges::is_sorted(info.fields, {}, &api::FieldSummary::fieldname)) {
    throw std::runtime_error("segment summary: field records are not sorted");
  }
  imageBytes = image.size();
}

const api::FieldSummary* SegmentSummary::field(std::string_view physicalName) const noexcept {
  auto it = std::ranges::lower_bound(info.fields, physicalName, {}, &api::FieldSummary::fieldname);
  if (it == info.fields.end() || it->fieldname != physicalName) return nullptr;
  return &*it;
}

std::optional<std::string> SegmentSummary::successor(std::string_view s) {
  std::string result(s);
  while (!result.empty() && (uint8_t)result.back() == 0xFF) result.pop_back();
  if (result.empty()) return std::nullopt;
  result.back() = (char)((uint8_t)result.back() + 1);
  return result;
}

bool SegmentSummary::boundsExcludeTerm(const api::FieldSummary& record, std::string_view term) noexcept {
  if (record.summary_flags & EMPTY) return true;
  if ((record.summary_flags & HAS_TERM_BOUNDS) == 0) return false;
  if (term < viewOf(record.term_min)) return true;
  if ((record.summary_flags & TERM_MAX_UNBOUNDED) == 0 && term > viewOf(record.term_max)) return true;
  return false;
}

bool SegmentSummary::boundsExcludePrefix(const api::FieldSummary& record, std::string_view prefix) noexcept {
  if (record.summary_flags & EMPTY) return true;
  if ((record.summary_flags & HAS_TERM_BOUNDS) == 0) return false;
  // Every term with the prefix lies in [prefix, successor(prefix)).
  if (auto next = successor(prefix); next && *next <= viewOf(record.term_min)) return true;
  if ((record.summary_flags & TERM_MAX_UNBOUNDED) == 0 && prefix > viewOf(record.term_max)) return true;
  return false;
}

bool SegmentSummary::boundsExcludeTermRange(const api::FieldSummary& record,
                                            std::optional<std::string_view> lower, bool lowerInclusive,
                                            std::optional<std::string_view> upper, bool upperInclusive) noexcept {
  if (record.summary_flags & EMPTY) return true;
  if ((record.summary_flags & HAS_TERM_BOUNDS) == 0) return false;
  if (upper) {
    auto termMin = viewOf(record.term_min);
    if (*upper < termMin || (*upper == termMin && !upperInclusive)) return true;
  }
  if (lower && (record.summary_flags & TERM_MAX_UNBOUNDED) == 0) {
    auto termMax = viewOf(record.term_max);
    if (*lower > termMax || (*lower == termMax && !lowerInclusive)) return true;
  }
  return false;
}

SegmentSummary::Presence SegmentSummary::mayIntersectNumeric(const api::FieldSummary& record,
                                                             int64_t lo, int64_t hi) noexcept {
  if (lo > hi || (record.summary_flags & EMPTY)) return Presence::NO;
  if ((record.summary_flags & HAS_NUMERIC) == 0) return Presence::MAYBE;
  return (hi < record.num_min || lo > record.num_max) ? Presence::NO : Presence::MAYBE;
}

SegmentSummary::Presence SegmentSummary::mayIntersectGeo(const api::FieldSummary& record, int32_t minLat,
                                                         int32_t maxLat, int32_t minLon, int32_t maxLon) noexcept {
  if (minLat > maxLat || (record.summary_flags & EMPTY)) return Presence::NO;
  if ((record.summary_flags & HAS_GEO_BOX) == 0) return Presence::MAYBE;
  if (maxLat < record.geo_min_lat || minLat > record.geo_max_lat) return Presence::NO;
  auto overlaps = [&](int32_t qMin, int32_t qMax) {
    return !(qMax < record.geo_min_lon || qMin > record.geo_max_lon);
  };
  if (minLon <= maxLon) return overlaps(minLon, maxLon) ? Presence::MAYBE : Presence::NO;
  // Wrapping query interval: [minLon, +max] or [-max, maxLon].
  return overlaps(minLon, INT32_MAX) || overlaps(INT32_MIN, maxLon) ? Presence::MAYBE : Presence::NO;
}

} // namespace luxir
