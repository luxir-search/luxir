#pragma once

#include <cstdint>

namespace luxir {

static constexpr int32_t DOCID_INDEXING_DENSITY_DENOMINATOR = 2;

inline bool useDocIdIndexing(int32_t docsWithField, int32_t maxDoc) {
  return (int64_t)docsWithField * DOCID_INDEXING_DENSITY_DENOMINATOR >=
         (int64_t)maxDoc;
}

} // namespace luxir
