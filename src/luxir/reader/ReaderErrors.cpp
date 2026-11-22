// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "ReaderErrors.h"
#include "FieldRecordCodec.h"
#include "Postings.h"
#include "SegmentCore.h"

#include <format>
#include <stdexcept>

namespace luxir {

void corruptSegment(std::string_view segment, std::string_view reason,
                    std::string_view field, uint32_t fileNumber) {
  auto context = std::format("Segment '{}'", segment);
  if (fileNumber != UINT32_MAX) context += std::format(", file '{}'", Postings::getIndexFileName(segment, fileNumber));
  if (!field.empty()) context += std::format(", field '{}'", field);
  throw SegmentCorruptError(std::format("{} is corrupt or in an unsupported format: {}", context, reason));
}

void SegmentReadContext::invalid(std::string_view reason, std::string_view prefix) const {
  std::string message;
  if (!prefix.empty()) {
    message = std::format("{}: {}", prefix, reason);
    reason = message;
  }
  if (core) {
    auto name = field.isNull() ? std::string_view() : std::string_view(field);
    corruptSegment(core->segmentName(), reason, name, file);
  }
  throw SegmentCorruptError(std::string(reason));
}

SegmentReadContext segmentReadContext(const SegmentCore& core, const DecodedFieldRecord& fieldInfo,
                                      uint32_t file) {
  return {&core, fieldInfo.name, file};
}

void missingColumn(const DecodedFieldRecord& fieldInfo, std::string_view kind) {
  throw std::invalid_argument(std::format("Field '{}' has no {} column", std::string_view(fieldInfo.name), kind));
}

} // namespace luxir
