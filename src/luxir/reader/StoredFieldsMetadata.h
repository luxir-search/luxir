// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <atomic>
#include <vector>

#include "FieldParts.h"
#include "StoredChunkCodec.h"

namespace luxir {

class PostingsReader;
class DecodedFieldRecord;

// An opened stored resource. Member names borrow the segment mapping.
class StoredFieldsMetadata {
  friend class PostingsReader;
  mutable std::atomic<const StoredDecompressionDictionary*> decoder = nullptr;  // Opened for decompression.

  std::vector<PackedTerm> names;  // Borrowed names in stored field ID order.

public:
  std::span<const PackedTerm> fieldNames() const noexcept { return names; }

  StoredFieldsMetadata(PostingsReader& reader, const DecodedFieldRecord& fieldInfo);
  ~StoredFieldsMetadata() { delete decoder.load(std::memory_order_relaxed); }
  size_t allocatedBytes() const {
    return sizeof(*this) + names.capacity() * sizeof(names[0]);
  }
};

} // namespace luxir
