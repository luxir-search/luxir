// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once
// Builds a segment's summary sidecar while the segment is written. Field
// writers feed bounds into the collector of their field (held by the
// PostingsWriter's field info); PostingsWriter::finish takes presence,
// counts and BM25 totals from the field directory and records every field.
// Flush and merge share this code, so a merged segment's summary is
// recomputed from what the merge actually wrote.

#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "luxir/api/luxir_index.hpp"
#include "luxir/store/Directory.h"
#include "luxir/store/OutputStream.h"
#include "SegmentSummary.h"

namespace luxir {

struct SegFieldInfo;

// The bounds of one physical field, gathered while its data is written.
class FieldSummaryCollector {
public:
  int64_t numMin = std::numeric_limits<int64_t>::max();
  int64_t numMax = std::numeric_limits<int64_t>::min();
  int32_t latMin = std::numeric_limits<int32_t>::max();
  int32_t latMax = std::numeric_limits<int32_t>::min();
  int32_t lonMin = std::numeric_limits<int32_t>::max();
  int32_t lonMax = std::numeric_limits<int32_t>::min();
  std::string termMin;
  std::string termMax;
  bool hasNumeric = false;
  bool hasGeo = false;
  bool hasTermBounds = false;
  bool termBoundsTruncated = false;
  bool termMaxUnbounded = false;

  void addNumericBounds(int64_t min, int64_t max) noexcept {
    numMin = std::min(numMin, min);
    numMax = std::max(numMax, max);
    hasNumeric = true;
  }
  void addInt64(int64_t encoded) noexcept { addNumericBounds(encoded, encoded); }
  // A packed lat/lon column value (geo::packPoint).
  void addGeoPacked(int64_t packed) noexcept;
  // The dictionary's first and last terms; no bounds when nTerms is 0.
  // Bounds longer than TERM_BOUND_MAX_LEN are cut, the upper one rounded up
  // so it stays above every term that starts with the cut prefix.
  void setTermBounds(std::string_view first, std::string_view last, uint64_t nTerms);
};

// One segment's field records and the sidecar they produce.
class SegmentSummaryBuilder {
  struct Record {
    std::string fieldname;
    api::FieldSummary wire;  // fieldname and term views point into this record
    FieldSummaryCollector bounds;
  };
  std::vector<std::unique_ptr<Record>> records;

public:
  // Records the physical field `name` with the directory counts of `field`
  // and the bounds its writers collected. Called once per field of the
  // segment directory.
  void add(std::string_view name, const SegFieldInfo& field, const FieldSummaryCollector& bounds);
  size_t fieldCount() const noexcept { return records.size(); }

  // The complete sidecar image.
  std::vector<std::byte> encode(uint64_t segId, int32_t maxDoc, bool complete);

  // Writes the sidecar as `fileName` and returns the written file's
  // descriptor.
  FileDescriptor write(Directory& dir, std::string_view fileName, uint64_t segId, int32_t maxDoc, bool complete);
};

} // namespace luxir
