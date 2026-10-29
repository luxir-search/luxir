// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "SummaryCollector.h"

#include <algorithm>
#include <cstring>
#include <memory_resource>
#include <stdexcept>

#include "luxir/api/build.h"
#include "luxir/reader/SegFieldInfo.h"
#include "luxir/store/FileIOException.h"
#include "luxir/store/OutputStream.h"
#include "luxir/util/geo.h"

namespace luxir {

using Flags = SegmentSummary::Flags;

void FieldSummaryCollector::addGeoPacked(int64_t packed) noexcept {
  int32_t lat = geo::unpackLatitude(packed);
  int32_t lon = geo::unpackLongitude(packed);
  latMin = std::min(latMin, lat);
  latMax = std::max(latMax, lat);
  lonMin = std::min(lonMin, lon);
  lonMax = std::max(lonMax, lon);
  hasGeo = true;
}

void FieldSummaryCollector::setTermBounds(std::string_view first, std::string_view last, uint64_t nTerms) {
  if (nTerms == 0) return;
  constexpr size_t maxLen = SegmentSummary::TERM_BOUND_MAX_LEN;
  // A prefix of the first term sorts at or below it.
  termMin.assign(first.substr(0, maxLen));
  termMaxUnbounded = false;
  if (last.size() <= maxLen) {
    termMax.assign(last);
  } else {
    // A rounded-up prefix stays above every term that starts with it.
    auto next = SegmentSummary::successor(last.substr(0, maxLen));
    termMax = next ? std::move(*next) : std::string();
    termMaxUnbounded = !next;
  }
  termBoundsTruncated = first.size() > maxLen || last.size() > maxLen;
  hasTermBounds = true;
}

void SegmentSummaryBuilder::add(std::string_view name, const SegFieldInfo& field,
                                const FieldSummaryCollector& bounds) {
  auto record = std::make_unique<Record>();
  record->fieldname = std::string(name);
  record->bounds = bounds;
  api::FieldSummary& r = record->wire;
  r.type = (uint32_t)field.type;
  r.flags = (uint32_t)field.flags;
  r.type_flags = field.typeFlags;
  r.docs_with_field = (uint64_t)std::max<int32_t>(0, field.docsWithField);
  r.n_terms = (uint64_t)std::max<int64_t>(0, field.nTerms);
  bool column = (field.flags & FieldType::COLUMN_STORED) != 0;
  if (column) {
    r.summary_flags |= Flags::HAS_NUM_VALUES;
    r.num_values = (uint64_t)std::max<int64_t>(0, field.numValues);
  }
  if (field.flags & FieldType::MULTI_VALUED) r.summary_flags |= Flags::MULTI_VALUED;
  if (field.flags & FieldType::INDEX_DOCS) {
    r.sum_doc_freq = (uint64_t)std::max<int64_t>(0, field.sumDocFreq);
    r.sum_total_term_freq = (uint64_t)std::max<int64_t>(0, field.sumTotalTermFreq);
  }
  // Empty only when nothing at all was written for the field.
  if (r.docs_with_field == 0 && r.n_terms == 0 && r.num_values == 0) {
    r.summary_flags |= Flags::EMPTY;
  } else {
    if (bounds.hasNumeric) {
      r.summary_flags |= Flags::HAS_NUMERIC;
      r.num_min = bounds.numMin;
      r.num_max = bounds.numMax;
    }
    if (bounds.hasGeo) {
      r.summary_flags |= Flags::HAS_GEO_BOX;
      r.geo_min_lat = bounds.latMin;
      r.geo_max_lat = bounds.latMax;
      r.geo_min_lon = bounds.lonMin;
      r.geo_max_lon = bounds.lonMax;
    }
    if (bounds.hasTermBounds) {
      r.summary_flags |= Flags::HAS_TERM_BOUNDS;
      if (bounds.termBoundsTruncated) r.summary_flags |= Flags::TERM_BOUNDS_TRUNCATED;
      if (bounds.termMaxUnbounded) r.summary_flags |= Flags::TERM_MAX_UNBOUNDED;
    }
  }
  records.push_back(std::move(record));
}

std::vector<std::byte> SegmentSummaryBuilder::encode(uint64_t segId, int32_t maxDoc, bool complete) {
  std::ranges::sort(records, {}, [](const auto& r) -> std::string_view { return r->fieldname; });
  std::pmr::monotonic_buffer_resource arena;
  api::SegmentSummaryInfo info;
  info.seg_id = segId;
  info.max_doc = maxDoc;
  info.complete = complete;
  auto* fields = api::build::allocArray(info.fields, records.size(), arena);
  for (size_t i = 0; i < records.size(); i++) {
    Record& record = *records[i];
    fields[i] = record.wire;
    fields[i].fieldname = record.fieldname;
    if (record.wire.summary_flags & Flags::HAS_TERM_BOUNDS) {
      const auto& b = record.bounds;
      fields[i].term_min = ::hpp_proto::bytes_view((const std::byte*)b.termMin.data(), b.termMin.size());
      fields[i].term_max = ::hpp_proto::bytes_view((const std::byte*)b.termMax.data(), b.termMax.size());
    }
  }
  std::vector<std::byte> body;
  if (!api::encode(info, body)) throw std::runtime_error("segment summary: encoding failed");
  std::vector<std::byte> image(SegmentSummary::HEADER_BYTES + body.size());
  std::memcpy(image.data(), SegmentSummary::MAGIC.data(), SegmentSummary::MAGIC.size());
  uint32_t version = SegmentSummary::FORMAT_VERSION;
  std::memcpy(image.data() + SegmentSummary::MAGIC.size(), &version, sizeof(version));
  if (!body.empty()) std::memcpy(image.data() + SegmentSummary::HEADER_BYTES, body.data(), body.size());
  return image;
}

FileDescriptor SegmentSummaryBuilder::write(Directory& dir, std::string_view fileName, uint64_t segId,
                                            int32_t maxDoc, bool complete) {
  auto image = encode(segId, maxDoc, complete);
  Directory::FileCreateOptions options;
  options.expectedSize = image.size();
  auto file = dir.createFile(fileName, options);
  if (!file) throw FileIOException("Failed to create segment summary file " + std::string(fileName));
  OutputStream out(file.get());
  out.write(image.data(), image.size());
  out.close();
  dir.finishFile(*file);
  return file->descriptor();
}

} // namespace luxir
