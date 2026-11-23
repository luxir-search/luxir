// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstring>
#include <format>
#include <span>
#include <stdexcept>
#include "StrColReader.h"
#include "luxir/codec/VectorCodec.h"
#include "luxir/schema/FieldType.h"
#include "luxir/util/MemPool.h"
#include "luxir/util/screaming.h"

namespace luxir {

/// Thin wrapper around StrColReader that interprets the column's fixed-size
/// byte blobs as vectors in the segment's VectorEncoding (recorded on the
/// column descriptor, not taken from the schema).  Dimensions are derived from
/// the column's fixedValueSize and the encoding; the schema does not need to
/// declare dims.
///
/// rowAtRank(r) returns the r-th stored row in its encoding, with the norm
/// prefix when hasNorms() (score it with a VectorScorer); decodeAtRank(r)
/// materializes it as floats.  Rows are fixed-size and contiguous, so
/// rowsBase() + r * valueBytes() addresses row r directly.  For multi-valued
/// fields, valueRange(docRank) gives [startValueRank, endValueRank).
class VectorReader {
  StrColReader column;
  VectorEncoding vectorEncoding;
  bool norms;
  int32_t dimensions;

  static VectorEncoding columnEncoding(const DecodedFieldRecord& fieldInfo) {
    const auto* bytes = fieldInfo.bytesColumn();
    return (VectorEncoding)(bytes ? bytes->vectorEncoding : 0);
  }

  static bool columnNorms(const DecodedFieldRecord& fieldInfo) {
    const auto* bytes = fieldInfo.bytesColumn();
    return bytes && bytes->vectorNorms;
  }

public:
  VectorReader(PostingsReader& reader, const DecodedFieldRecord& fieldInfo)
    : column(reader, fieldInfo),
      vectorEncoding(columnEncoding(fieldInfo)),
      norms(columnNorms(fieldInfo)),
      dimensions(0) {
    if (!vcodec::valid((int32_t)vectorEncoding) || !column.isFixedSize()) {
      throw std::runtime_error(std::format(
        "vector field '{}' has an unsupported column layout (encoding {})",
        (std::string_view)fieldInfo.name, (int32_t)vectorEncoding));
    }
    dimensions = vcodec::dimsFromRowBytes(vectorEncoding, column.fixedValueSize(), norms);
    if (dimensions < 0) {
      throw std::runtime_error(std::format(
        "vector field '{}' value size {} is invalid for encoding {}",
        (std::string_view)fieldInfo.name, column.fixedValueSize(), vcodec::name(vectorEncoding)));
    }
  }

  int32_t dims() const { return dimensions; }

  VectorEncoding encoding() const { return vectorEncoding; }

  /// Whether rows carry the |x|^2 prefix (vcodec::rowNormSq).
  bool hasNorms() const { return norms; }

  /// Stored bytes per vector (row stride).
  int32_t valueBytes() const { return column.fixedValueSize(); }

  int32_t docsWithField() const { return column.docsWithField(); }

  int64_t numVectors() const { return column.numValues(); }

  bool multiValued() const { return column.multiValued(); }

  /// Stored value (in this segment's encoding) at a global value rank
  /// (0..numVectors()-1).
  std::string_view rowAtRank(int64_t valueRank) const {
    return column.valueAt(valueRank);
  }

  /// The first row; row r is at rowsBase() + r * valueBytes().  Requires
  /// numVectors() > 0.
  const char* rowsBase() const { return rowAtRank(0).data(); }

  /// Decodes the vector at a global value rank into out[0..dims()).
  void decodeAtRank(int64_t valueRank, float* out) const {
    vcodec::decode(vectorEncoding, vcodec::rowPayload(rowAtRank(valueRank).data(), norms), dimensions, out);
  }

  /// Decodes n consecutive vectors starting at valueRank into out (n * dims()).
  void decodeRange(int64_t valueRank, int64_t n, float* out) const {
    if (n <= 0) return;
    const char* base = rowAtRank(valueRank).data();
    int64_t stride = valueBytes();
    if (vectorEncoding == VectorEncoding::F32 && !norms) {
      std::memcpy(out, base, (size_t)(n * stride));
      return;
    }
    for (int64_t i = 0; i < n; i++) {
      vcodec::decode(vectorEncoding, vcodec::rowPayload(base + i * stride, norms), dimensions,
                     out + i * dimensions);
    }
  }

  /// F32 columns only: the vector at a global value rank, without a copy.
  /// Other encodings must go through rowAtRank or decodeAtRank; reading their
  /// bytes as floats would return garbage, so this is checked in every build.
  LUXIR_INLINE std::span<const float> vectorAtRank(int64_t valueRank) const {
    if (vectorEncoding != VectorEncoding::F32) [[unlikely]] notF32();
    auto bytes = column.valueAt(valueRank);
    return std::span<const float>((const float*)vcodec::rowPayload(bytes.data(), norms),
                                  (size_t)dimensions);
  }

  /// F32 columns only, single-valued: the vector for the doc at a doc rank.
  std::span<const float> singleVectorAt(int32_t docRank) const {
    assert(!multiValued());
    return vectorAtRank((int64_t)docRank);
  }

  /// Multi-valued accessor: [startValueRank, endValueRank) of the given doc rank.
  std::pair<int64_t, int64_t> valueRange(int32_t docRank) const {
    assert(multiValued());
    return column.getStartEndValueRank(docRank);
  }

  /// Whether this segment carries the per-vector-rank -> docId reverse map.
  /// Single-valued fields use the identity map or the docs bitmap.
  bool hasOwners() { return column.ownerIndex() != nullptr; }

  /// Multi-valued: map a global vector rank (0..numVectors()-1) to the segment-local
  /// docId that owns it.  Requires hasOwners().
  int32_t docForVectorRank(int64_t valueRank) {
    MonoReader* index = column.ownerIndex();
    assert(index != nullptr);
    return (int32_t)index->valueAt(valueRank);
  }

  StrColReader& strColReader() { return column; }

  using Iterator = StrColReader::DocIterator;

private:
  [[noreturn]] void notF32() const {
    throw std::logic_error(std::format(
      "f32 vector accessor used on a {} column", vcodec::name(vectorEncoding)));
  }
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

/// Finds a vector field's keep_original companion column in one segment, or
/// null when it is missing or not aligned value-for-value with the encoded
/// column.  The two are written in lockstep, so equal value and doc counts
/// mean every encoded value has its original at the same rank; a segment
/// merged from inputs written before keep_original was enabled holds a
/// partial companion, which is ignored.
inline const DecodedFieldRecord* findOriginalColumn(PostingsReader& postingsReader,
                                                    const DecodedFieldRecord& encoded) {
  std::string name = VectorFieldType::originalColumnName((std::string_view)encoded.name);
  const DecodedFieldRecord* original = postingsReader.fieldInfo(name);
  if (original == nullptr || original->type != FieldType::VECTOR) return nullptr;
  const auto* bytes = original->bytesColumn();
  if (bytes == nullptr || bytes->vectorEncoding != (uint8_t)VectorEncoding::F32) return nullptr;
  if (original->numValues != encoded.numValues || original->docsWithField != encoded.docsWithField) {
    return nullptr;
  }
  return original;
}

} // namespace luxir
