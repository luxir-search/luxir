// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <format>
#include <span>
#include <stdexcept>
#include "FieldReader.h"
#include "StrColReader.h"
#include "luxir/codec/VectorCodec.h"
#include "luxir/util/MemPool.h"
#include "luxir/util/screaming.h"

namespace luxir {

/// Thin wrapper around StrColReader that interprets the column's fixed-size
/// byte blobs as vectors in the segment's VectorEncoding (recorded in the
/// segment field metadata, not taken from the schema).  Dimensions are derived
/// from the column's fixedValueSize and the encoding; the schema does not need
/// to declare dims.
///
/// rowAtRank(r) returns the r-th stored value in its encoding (score it with a
/// VectorScorer), decodeAtRank(r) materializes it as floats.  For multi-valued
/// fields, valueRange(docRank) gives [startValueRank, endValueRank).
class VectorReader {
  StrColReader strCol_;
  VectorEncoding encoding_;
  int32_t dims_;

public:
  VectorReader(PostingsReader& postingsReader, const SegFieldInfo& fieldInfo)
    : strCol_(postingsReader, fieldInfo),
      encoding_((VectorEncoding)fieldInfo.vectorEncoding),
      dims_(0) {
    if (!vcodec::valid(fieldInfo.vectorEncoding) || !strCol_.isFixedSize()) {
      throw std::runtime_error(std::format(
        "vector field '{}' has an unsupported column layout (encoding {})",
        (std::string_view)fieldInfo.fieldname, fieldInfo.vectorEncoding));
    }
    dims_ = vcodec::dimsFromBytes(encoding_, strCol_.fixedValueSize());
    if (dims_ < 0) {
      throw std::runtime_error(std::format(
        "vector field '{}' value size {} is invalid for encoding {}",
        (std::string_view)fieldInfo.fieldname, strCol_.fixedValueSize(),
        vcodec::name(encoding_)));
    }
  }

  int32_t dims() const { return dims_; }

  VectorEncoding encoding() const { return encoding_; }

  /// Stored bytes per vector.
  int32_t valueBytes() const { return strCol_.fixedValueSize(); }

  int32_t docsWithValue() const { return strCol_.docsWithValue(); }

  int64_t numVectors() const { return strCol_.numValues(); }

  bool isMultiValued() const { return strCol_.isMultiValued(); }

  /// Stored value (in this segment's encoding) at a global value rank
  /// (0..numVectors()-1).
  std::string_view rowAtRank(int64_t valueRank) const {
    return strCol_.valueAt(valueRank);
  }

  /// Decodes the vector at a global value rank into out[0..dims()).
  void decodeAtRank(int64_t valueRank, float* out) const {
    vcodec::decode(encoding_, rowAtRank(valueRank).data(), dims_, out);
  }

  /// Decodes n consecutive vectors starting at valueRank into out (n * dims()).
  void decodeRange(int64_t valueRank, int64_t n, float* out) const {
    if (n <= 0) return;
    const char* base = rowAtRank(valueRank).data();
    int64_t stride = valueBytes();
    if (encoding_ == VectorEncoding::F32) {
      std::memcpy(out, base, (size_t)(n * stride));
      return;
    }
    for (int64_t i = 0; i < n; i++) {
      vcodec::decode(encoding_, base + i * stride, dims_, out + i * dims_);
    }
  }

  /// F32 columns only: the vector at a global value rank, without a copy.
  std::span<const float> vectorAtRank(int64_t valueRank) const {
    assert(encoding_ == VectorEncoding::F32);
    auto bytes = strCol_.valueAt(valueRank);
    return std::span<const float>((const float*)bytes.data(), (size_t)dims_);
  }

  /// F32 columns only, single-valued: the vector for the doc at a doc rank.
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

/// Finds a vector field's keep_original companion column in one segment and
/// returns true when it is aligned value-for-value with the encoded column.
/// The two are written in lockstep, so equal value and doc counts mean every
/// encoded value has its original at the same rank; a segment merged from
/// inputs written before keep_original was enabled holds a partial companion,
/// which is ignored.
inline bool findOriginalColumn(PostingsReader& postingsReader, const SegFieldInfo& encoded,
                               SegFieldInfo& original) {
  std::string name = VectorFieldType::originalColumnName((std::string_view)encoded.fieldname);
  const SegFieldInfo* info = postingsReader.fieldInfo(name);
  if (info == nullptr) return false;
  original = *info;
  return original.type == FieldType::VECTOR
      && original.vectorEncoding == (int32_t)VectorEncoding::F32
      && original.numValues == encoded.numValues
      && original.docsWithField == encoded.docsWithField;
}

} // namespace luxir
