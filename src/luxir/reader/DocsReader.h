// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstring>
#include "luxir/reader/SegmentCore.h"
#include "PostingsReader.h"
#include "luxir/util/screaming.h"

namespace luxir {
class DocsReader {
  screaming::BitSet bits;
  int32_t ndocs;

public:

  /// initialize from docsWithField for the field if it exists
  /// fieldInfo is only used in the constructor.
  /// `deferred` reads a lazy file's set by range instead of materializing the
  /// file (a vector column's set shares the file with its values).
  DocsReader(PostingsReader &postingsReader, const SegFieldInfo &fieldInfo, bool deferred = false) {
    ndocs = fieldInfo.docsWithField;
    if (ndocs != postingsReader.maxDoc()) {
      seg_location end = fieldInfo.docsWithFieldEndLoc;
      SegmentCore& core = postingsReader.core();
      InputFile& file = *core.getFile(end.filenum());
      if (deferred && file.lazy() && !core.residentStream(end.filenum())) {
        bits.set(lazySetEnd(file, end.offset()));
        return;
      }
      InputStream docsWithValIs = core.getInputStreamSeek(end);
      bits.set( docsWithValIs.ptr() );
    }
  }

  // The end of a serialized set ending at `end` in a lazy file, reading only
  // its bucket count, last descriptor and then its bytes.
  static const char* lazySetEnd(InputFile& file, uint64_t end) {
    using Set = screaming::BitSet;
    uint16_t nBuckets;
    std::memcpy(&nBuckets, file.rangeStream(end - sizeof(uint16_t), sizeof(uint16_t)).ptr(), sizeof(nBuckets));
    Set::BucketDescriptor last{};
    if (nBuckets > 0) {
      uint64_t lastAt = end - sizeof(uint16_t) - sizeof(Set::BucketDescriptor);
      std::memcpy(&last, file.rangeStream(lastAt, sizeof(last)).ptr(), sizeof(last));
    }
    uint64_t bytes = Set::serializedBytes(nBuckets, last);
    return file.rangeStream(end - bytes, bytes).ptr() + bytes;
  }

  explicit DocsReader(int32_t docsWithField) {
    ndocs = docsWithField;
  }

  int32_t numDocs() const noexcept {
    return ndocs;
  }

  bool hasBitset() const noexcept {
    return !bits.empty();
  }

  const screaming::BitSet& bitset() const noexcept {
    return bits;
  }
};

}