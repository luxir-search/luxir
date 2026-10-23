// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "SegmentSummary.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "luxir/api/padded_input.h"
#include "luxir/reader/Postings.h"
#include "luxir/util/BinaryFuse8.h"
#include "luxir/util/BloomFilter.h"
#include "luxir/util/TermHash.h"

namespace luxir {

namespace {

std::string_view viewOf(::hpp_proto::bytes_view bytes) {
  return std::string_view((const char*)bytes.data(), bytes.size());
}

uint32_t readU32(std::span<const std::byte> bytes, size_t offset) {
  uint32_t value;
  std::memcpy(&value, bytes.data() + offset, sizeof(value));
  return value;
}

} // namespace

std::string SegmentSummary::overlayFileName(uint64_t segId, uint64_t gen) {
  return Postings::getSegmentOverlayFileName(segId, OVERLAY_NAME, gen, 0);
}

std::shared_ptr<const SegmentSummary> SegmentSummary::open(Directory& dir, std::string_view fileName,
                                                           bool expectSynced) {
  auto file = dir.openFile(fileName, expectSynced);
  if (!file) return nullptr;
  return fromFile(std::move(file));
}

std::shared_ptr<const SegmentSummary> SegmentSummary::fromFile(std::shared_ptr<InputFile> file) {
  auto summary = std::make_shared<SegmentSummary>();
  std::string_view data = file->read();
  summary->image = std::span<const std::byte>((const std::byte*)data.data(), data.size());
  summary->file = std::move(file);
  summary->decodeImage();
  return summary;
}

std::shared_ptr<const SegmentSummary> SegmentSummary::fromBytes(std::shared_ptr<const std::vector<std::byte>> bytes) {
  auto summary = std::make_shared<SegmentSummary>();
  summary->image = std::span<const std::byte>(bytes->data(), bytes->size());
  summary->keepAlive = std::move(bytes);
  summary->decodeImage();
  return summary;
}

std::shared_ptr<const SegmentSummary> SegmentSummary::fromAggregate(std::span<const api::FieldSummary> fields,
                                                                    bool complete,
                                                                    std::shared_ptr<const void> keepAlive) {
  auto summary = std::make_shared<SegmentSummary>();
  summary->keepAlive = std::move(keepAlive);
  summary->info.version = FORMAT_VERSION;
  summary->info.hash_version = summary::HASH_VERSION;
  summary->info.complete = complete;
  summary->info.fields = fields;
  summary->aggregate = true;
  return summary;
}

void SegmentSummary::decodeImage() {
  if (image.size() < HEADER_BYTES) throw std::runtime_error("segment summary: file too short");
  if (std::string_view((const char*)image.data(), Postings::SUMMARY_HEADER.size()) != Postings::SUMMARY_HEADER) {
    throw std::runtime_error("segment summary: bad header");
  }
  uint32_t infoLen = readU32(image, 8);
  uint32_t indexLen = readU32(image, 12);
  if ((size_t)infoLen + HEADER_BYTES > indexLen || indexLen > image.size() || indexLen % PAYLOAD_ALIGN != 0) {
    throw std::runtime_error("segment summary: inconsistent section lengths");
  }
  payloadBase = indexLen;
  arena = std::make_unique<std::pmr::monotonic_buffer_resource>();
  auto section = image.subspan(HEADER_BYTES, infoLen);
  if (!api::decode(info, api::copyToPaddedInput(section, *arena), *arena)) {
    throw std::runtime_error("segment summary: undecodable field records");
  }
  if (info.version != FORMAT_VERSION) {
    throw std::runtime_error("segment summary: unsupported format version " + std::to_string(info.version));
  }
  if (!std::ranges::is_sorted(info.fields, {}, &api::FieldSummary::fieldname)) {
    throw std::runtime_error("segment summary: field records are not sorted");
  }
}

const api::FieldSummary* SegmentSummary::field(std::string_view physicalName) const noexcept {
  auto it = std::ranges::lower_bound(info.fields, physicalName, {}, &api::FieldSummary::fieldname);
  if (it == info.fields.end() || it->fieldname != physicalName) return nullptr;
  return &*it;
}

const api::SummaryPayload* SegmentSummary::payload(const api::FieldSummary& record, PayloadKind kind,
                                                   uint32_t maxParam) const noexcept {
  const api::SummaryPayload* best = nullptr;
  for (const auto& p : record.payloads) {
    if (p.kind != kind || p.version != PAYLOAD_VERSION || p.param > maxParam) continue;
    if (best == nullptr || p.param > best->param) best = &p;
  }
  return best;
}

std::span<const std::byte> SegmentSummary::payloadBytes(const api::SummaryPayload& payload) const noexcept {
  if (aggregate) return {};
  uint64_t begin = (uint64_t)payloadBase + payload.offset;
  if (payload.offset > image.size() || begin > image.size() || payload.length > image.size() - begin) return {};
  return image.subspan((size_t)begin, (size_t)payload.length);
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

std::optional<bool> SegmentSummary::exactSetContains(const api::FieldSummary& record,
                                                     std::string_view term) const noexcept {
  const auto* p = payload(record, EXACT_SET);
  if (p == nullptr) return std::nullopt;
  auto bytes = payloadBytes(*p);
  size_t count = (size_t)p->count;
  if (bytes.size() < count * sizeof(uint32_t)) return std::nullopt;
  auto entry = [&](size_t i) -> std::optional<std::string_view> {
    uint32_t offset = readU32(bytes, i * sizeof(uint32_t));
    if ((size_t)offset + 2 > bytes.size()) return std::nullopt;
    uint16_t len;
    std::memcpy(&len, bytes.data() + offset, sizeof(len));
    if ((size_t)offset + 2 + len > bytes.size()) return std::nullopt;
    return std::string_view((const char*)bytes.data() + offset + 2, len);
  };
  size_t lo = 0, hi = count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    auto value = entry(mid);
    if (!value) return std::nullopt;
    if (*value < term) lo = mid + 1;
    else hi = mid;
  }
  if (lo == count) return false;
  auto value = entry(lo);
  if (!value) return std::nullopt;
  return *value == term;
}

SegmentSummary::Presence SegmentSummary::filterMayContain(const api::FieldSummary& record,
                                                          std::string_view term) const noexcept {
  if (info.hash_version != summary::HASH_VERSION) return Presence::MAYBE;
  uint64_t key = summary::termKey(term);
  if (const auto* p = payload(record, FUSE8)) {
    auto bytes = payloadBytes(*p);
    if (!bytes.empty() && !BinaryFuse8::mayContain(bytes, key)) return Presence::NO;
    return Presence::MAYBE;
  }
  if (const auto* p = payload(record, BLOOM)) {
    auto bytes = payloadBytes(*p);
    if (!bytes.empty() && !BloomFilter::mayContain(bytes, key)) return Presence::NO;
  }
  return Presence::MAYBE;
}

SegmentSummary::Presence SegmentSummary::mayContainTerm(const api::FieldSummary& record,
                                                        std::string_view term) const noexcept {
  if (boundsExcludeTerm(record, term)) return Presence::NO;
  if (auto exact = exactSetContains(record, term)) return *exact ? Presence::MAYBE : Presence::NO;
  return filterMayContain(record, term);
}

bool SegmentSummary::boundsExcludePrefix(const api::FieldSummary& record, std::string_view prefix) noexcept {
  if (record.summary_flags & EMPTY) return true;
  if ((record.summary_flags & HAS_TERM_BOUNDS) == 0) return false;
  // Every term with the prefix lies in [prefix, successor(prefix)).
  if (auto next = successor(prefix); next && *next <= viewOf(record.term_min)) return true;
  if ((record.summary_flags & TERM_MAX_UNBOUNDED) == 0 && prefix > viewOf(record.term_max)) return true;
  return false;
}

std::optional<bool> SegmentSummary::exactSetHasPrefix(const api::FieldSummary& record,
                                                      std::string_view prefix) const noexcept {
  const auto* p = payload(record, EXACT_SET);
  if (p == nullptr) return std::nullopt;
  auto bytes = payloadBytes(*p);
  size_t count = (size_t)p->count;
  if (bytes.size() < count * sizeof(uint32_t)) return std::nullopt;
  auto entry = [&](size_t i) -> std::optional<std::string_view> {
    uint32_t offset = readU32(bytes, i * sizeof(uint32_t));
    if ((size_t)offset + 2 > bytes.size()) return std::nullopt;
    uint16_t len;
    std::memcpy(&len, bytes.data() + offset, sizeof(len));
    if ((size_t)offset + 2 + len > bytes.size()) return std::nullopt;
    return std::string_view((const char*)bytes.data() + offset + 2, len);
  };
  size_t lo = 0, hi = count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    auto value = entry(mid);
    if (!value) return std::nullopt;
    if (*value < prefix) lo = mid + 1;
    else hi = mid;
  }
  if (lo == count) return false;
  auto value = entry(lo);
  if (!value) return std::nullopt;
  return value->starts_with(prefix);
}

SegmentSummary::Presence SegmentSummary::prefixFilterMayContain(const api::FieldSummary& record,
                                                                std::string_view prefix) const noexcept {
  if (info.hash_version != summary::HASH_VERSION || prefix.empty()) return Presence::MAYBE;
  const auto* p = payload(record, PREFIX_FUSE8, (uint32_t)std::min<size_t>(prefix.size(), UINT32_MAX));
  if (p == nullptr || p->param == 0 || p->param > prefix.size()) return Presence::MAYBE;
  auto bytes = payloadBytes(*p);
  if (bytes.empty()) return Presence::MAYBE;
  uint64_t key = summary::termKey(prefix.substr(0, p->param));
  return BinaryFuse8::mayContain(bytes, key) ? Presence::MAYBE : Presence::NO;
}

SegmentSummary::Presence SegmentSummary::mayContainPrefix(const api::FieldSummary& record,
                                                          std::string_view prefix) const noexcept {
  if (boundsExcludePrefix(record, prefix)) return Presence::NO;
  if (auto exact = exactSetHasPrefix(record, prefix)) return *exact ? Presence::MAYBE : Presence::NO;
  return prefixFilterMayContain(record, prefix);
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

std::optional<SegmentSummary::VectorCentroid> SegmentSummary::vectorCentroid(
    const api::FieldSummary& record) const noexcept {
  const auto* p = payload(record, VECTOR_CENTROID);
  if (p == nullptr) return std::nullopt;
  auto bytes = payloadBytes(*p);
  constexpr size_t head = 24;
  if (bytes.size() < head) return std::nullopt;
  VectorCentroid result;
  result.dims = readU32(bytes, 0);
  std::memcpy(&result.count, bytes.data() + 8, sizeof(result.count));
  std::memcpy(&result.radius, bytes.data() + 16, sizeof(result.radius));
  size_t vectorBytes = (size_t)result.dims * sizeof(float);
  if (result.dims == 0 || bytes.size() < head + 3 * vectorBytes) return std::nullopt;
  const float* floats = (const float*)(bytes.data() + head);
  result.centroid = std::span<const float>(floats, result.dims);
  result.boxMin = std::span<const float>(floats + result.dims, result.dims);
  result.boxMax = std::span<const float>(floats + 2 * result.dims, result.dims);
  return result;
}

} // namespace luxir
