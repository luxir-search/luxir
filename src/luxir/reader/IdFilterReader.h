// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <stdexcept>
#include <string_view>
#include <xxhash.h>

#include "PostingsReader.h"
#include "SegFieldInfo.h"
#include "luxir/util/BlockBloomFilter.h"

namespace luxir {

/// Reads the id membership filter that IdHandler (flush) and SegmentMerger
/// (merge) write for the id field: the field's min and max term plus a
/// split-block bloom filter keyed by the same XXH3 hash as the term block
/// hash bytes.  See TextWriter::finalizeField for the format.  Used to skip
/// segments and ids when applying deletes; never on the query path.
///
/// A field without a filter (FieldType::ID_FILTER unset) reports every id as
/// a possible member, so callers need no special case.
class IdFilterReader {
  std::string_view minTerm_;
  std::string_view maxTerm_;
  const char* blocks = nullptr;
  uint32_t numBlocks = 0;

public:
  /// The PostingsReader must outlive this reader.
  IdFilterReader(PostingsReader& reader, const SegFieldInfo& fieldInfo) {
    if ((fieldInfo.flags & FieldType::ID_FILTER) == 0) return;
    InputStream is = reader.getInputStreamSeek(fieldInfo.idFilterLoc);
    uint32_t n = is.readVint();
    minTerm_ = is.readStr();
    maxTerm_ = is.readStr();
    if ((uint64_t)is.left() < (uint64_t)n * BlockBloomFilter::BLOCK_BYTES) {
      throw std::runtime_error("id filter extends past the end of its file");
    }
    blocks = is.ptr();
    numBlocks = n;
  }

  bool present() const noexcept { return numBlocks != 0; }

  /// Only valid when present().
  std::string_view minTerm() const noexcept { return minTerm_; }
  std::string_view maxTerm() const noexcept { return maxTerm_; }

  /// False only when id is certainly not a term of the field.
  bool mayContain(std::string_view id) const noexcept {
    if (!present()) return true;
    if (id < minTerm_ || id > maxTerm_) return false;
    return BlockBloomFilter::mayContain(blocks, numBlocks, XXH3_64bits(id.data(), id.size()));
  }
};

} // namespace luxir
