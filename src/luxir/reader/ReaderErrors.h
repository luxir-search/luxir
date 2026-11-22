// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <stdexcept>
#include <string_view>

#include "luxir/util/StrRef.h"

namespace luxir {

class DecodedFieldRecord;
class SegmentCore;

// Segment bytes that fail validation: a damaged or foreign file rather than a
// bug in the caller.
class SegmentCorruptError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

// Identifies the source of corrupt bytes. Standalone decoders have no segment.
struct SegmentReadContext {
  const SegmentCore* core = nullptr;  // Supplies the segment name on error.
  PackedTerm field = PackedTerm(nullptr);  // Borrowed field name, read only when reporting an error.
  uint32_t file = UINT32_MAX;  // No file when the context does not identify one.

  [[noreturn]] void invalid(std::string_view reason, std::string_view prefix = std::string_view()) const;
};

SegmentReadContext segmentReadContext(const SegmentCore& core, const DecodedFieldRecord& fieldInfo,
                                      uint32_t file);

// Throws SegmentCorruptError with the context the caller has.
[[noreturn]] void corruptSegment(std::string_view segment, std::string_view reason,
                                 std::string_view field = std::string_view(),
                                 uint32_t fileNumber = UINT32_MAX);
// Reports a request to open a column the field does not have.
[[noreturn]] void missingColumn(const DecodedFieldRecord& fieldInfo, std::string_view kind);

} // namespace luxir
