// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <span>
#include "StrColReader.h"
#include "luxir/util/MemPool.h"
#include "luxir/util/screaming.h"

namespace luxir {

/// Thin wrapper around StrColReader that reinterprets the column's
/// fixed-size byte blobs as float vectors.  Dimensions are derived from the
/// column's fixedValueSize (dims = fixedValueSize / sizeof(float)); the
/// schema does not need to declare dims.
///
/// For single-valued fields, vectorAt(docRank) returns one span per doc.
/// For multi-valued, valueRange(docRank) gives [startValueRank, endValueRank) and
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

  /// Multi-valued accessor: [startValueRank, endValueRank) of the given doc rank.
  /// Caller iterates with vectorAtRank to materialize each vector.
  std::pair<int64_t, int64_t> valueRange(int32_t docRank) const {
    assert(isMultiValued());
    return strCol_.getStartEndValueRank(docRank);
  }

  /// Whether this segment carries the per-vector-rank -> docId reverse map.
  /// True for multi-valued vector fields; false for single-valued (use identity /
  /// the docs bitset) and for columns written before the map existed.
  bool hasValDocMap() { return strCol_.getValDocReader() != nullptr; }

  /// Multi-valued: map a global vector rank (0..numVectors()-1) to the segment-local
  /// docId that owns it.  Requires hasValDocMap().
  int32_t docForVectorRank(int64_t valueRank) {
    MonoReader* r = strCol_.getValDocReader();
    assert(r != nullptr);
    return (int32_t)r->valueAt(valueRank);
  }

  StrColReader& strColReader() { return strCol_; }

  using Iterator = StrColReader::DocIterator;
};

/// Build a pool-backed valueRank -> docId resolver for a sparse single-valued vector
/// column: docId is the valueRank-th set bit of the column's has-field bitset
/// (screaming::BitSet::Selector::select).  The Selector and its prefix are allocated in
/// `pool`, so the result outlives the (scratch) VectorReader the bitset came from; the
/// bitset's underlying data and `pool` must outlive the returned Selector.
inline screaming::BitSet::Selector* makeValueDocSelector(MemPool& pool,
                                                         const screaming::BitSet& bitset) {
  return pool.make<screaming::BitSet::Selector>(
      bitset, pool.make_span<int32_t>((size_t)bitset.nBuckets + 1));
}

} // namespace luxir
