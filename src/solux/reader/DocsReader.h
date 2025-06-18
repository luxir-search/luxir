#pragma once

#include "FieldReader.h"

namespace solux {
class DocsReader {
  screaming::BitSet bits;
  int32_t ndocs;

public:

  // initialize from docsWithField for the field if it exists
  DocsReader(MemPool &pool, PostingsReader &postingsReader, const SegFieldInfo &fieldInfo) {
    unused(pool);
    ndocs = fieldInfo.docsWithField;

    if (ndocs != postingsReader.numDocs()) {
      InputStream docsWithValIs = postingsReader.getInputStreamSeek(fieldInfo.docsWithFieldEndLoc);
      bits.set( docsWithValIs.ptr() );
    }
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