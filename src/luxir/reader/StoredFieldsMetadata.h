// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <optional>
#include <string_view>
#include <vector>

#include "StoredChunkCodec.h"

namespace luxir {

class PostingsReader;
struct SegFieldInfo;

// Immutable after Field publication. Views and the DDict borrow the segment
// mapping; PostingsReader destroys its Fields before releasing the files.
struct StoredFieldsMetadata {
  StoredChunkFormat format;  // level and training capacity are not recorded
  int64_t maxChunkBytes = 0;
  int32_t numDirtyChunks = 0;
  std::vector<std::string_view> fieldNames;
  std::string_view dictionary;
  std::optional<StoredDecompressionDictionary> ddict;

  StoredFieldsMetadata(PostingsReader& reader, const SegFieldInfo& field);
  size_t allocatedBytes() const {
    return sizeof(*this) + fieldNames.capacity() * sizeof(fieldNames[0])
        + (ddict ? ddict->allocatedBytes() : 0);
  }
};

} // namespace luxir
