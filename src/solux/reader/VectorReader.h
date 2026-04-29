#pragma once

#include <span>
#include "StrColReader.h"

namespace solux {

/// Thin wrapper around StrColReader that reinterprets the column's
/// fixed-size byte blobs as float vectors.  Dimensions are derived from the
/// column's fixedValueSize (dims = fixedValueSize / sizeof(float)); the
/// schema does not need to declare dims.
///
/// For single-valued fields, vectorAt(docRank) returns one span per doc.
/// For multi-valued, valueRange(docRank) gives [startRank, endRank) and
/// vectorAtRank(r) returns the r-th vector in the flat per-segment list.
class VectorReader {
  StrColReader strCol_;
  int32_t dims_;

public:
  VectorReader(PostingsReader& postingsReader, const SegFieldInfo& fieldInfo)
    : strCol_(postingsReader, fieldInfo),
      dims_(strCol_.fixedValueSize() / (int32_t)sizeof(float)) {
    assert(strCol_.isFixedSize());
    assert(strCol_.fixedValueSize() % (int32_t)sizeof(float) == 0);
  }

  int32_t dims() const { return dims_; }

  int32_t docsWithValue() const { return strCol_.docsWithValue(); }

  int64_t numVectors() const { return strCol_.numValues(); }

  bool isMultiValued() const { return strCol_.isMultiValued(); }

  /// Vector at a global value rank (0..numVectors()-1).
  std::span<const float> vectorAtRank(int64_t valueRank) const {
    auto bytes = strCol_.valueAt(valueRank);
    return std::span<const float>((const float*)bytes.data(), (size_t)dims_);
  }

  /// Single-valued accessor: vector for the doc at the given doc rank.
  std::span<const float> singleVectorAt(int32_t docRank) const {
    assert(!isMultiValued());
    return vectorAtRank((int64_t)docRank);
  }

  /// Multi-valued accessor: [startRank, endRank) of the given doc rank.
  /// Caller iterates with vectorAtRank to materialize each vector.
  std::pair<int64_t, int64_t> valueRange(int32_t docRank) const {
    assert(isMultiValued());
    return strCol_.getStartEndValueRank(docRank);
  }

  StrColReader& strColReader() { return strCol_; }

  using Iterator = StrColReader::DocIterator;
};

} // namespace solux
