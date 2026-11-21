// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <cstring>
#include <vector>
#include <format>
#include <span>
#include <stdexcept>
#include "StrColReader.h"
#include "luxir/codec/VectorCodec.h"
#include "luxir/util/MemPool.h"
#include "luxir/util/screaming.h"

namespace luxir {

/// Thin wrapper around StrColReader that interprets the column's fixed-size
/// byte blobs as vectors in the segment's VectorEncoding (recorded on the
/// column descriptor, not taken from the schema).  Dimensions are derived from
/// the column's fixedValueSize and the encoding; the schema does not need to
/// declare dims.
///
/// rowAtRank(r) returns the r-th stored value in its encoding (score it with a
/// VectorScorer), decodeAtRank(r) materializes it as floats.  For multi-valued
/// fields, valueRange(docRank) gives [startValueRank, endValueRank).
class VectorReader {
  StrColReader column;
  VectorEncoding vectorEncoding;
  int32_t dimensions;
  // Deferred columns only: the ranges acquired ahead, sorted by file offset and
  // disjoint, and the one-row hold of the last read that no range covered. A
  // reader belongs to one thread, so this needs no lock.
  struct Held {
    uint64_t offset;
    HeldRange hold;
  };
  mutable std::vector<Held> held;
  mutable HeldRange single;
  static constexpr size_t RELEASE_BEHIND = 16;

  const char* heldRow(int64_t valueRank) const {
    uint64_t offset = rowOffset(valueRank), length = (uint64_t)valueBytes();
    auto it = std::upper_bound(held.begin(), held.end(), offset,
        [](uint64_t value, const Held& entry) { return value < entry.offset; });
    if (it != held.begin()) {
      auto entry = std::prev(it);
      if (offset + length <= entry->offset + entry->hold.getLength()) {
        const char* row = entry->hold.getData() + (offset - entry->offset);
        // Reads move forward: release the ranges behind this one, in steps.
        if ((size_t)(entry - held.begin()) >= RELEASE_BEHIND) held.erase(held.begin(), entry);
        return row;
      }
    }
    // Not acquired ahead: one blocking range read. It is kept apart, so a row
    // read this way never shadows a wider range that covers its neighbours.
    single = column.valuesFile()->acquire(ByteRange{offset, length});
    return single.getData();
  }

  uint64_t rowOffset(int64_t valueRank) const {
    return (uint64_t)column.valuesOffset() + (uint64_t)valueRank * (uint64_t)valueBytes();
  }

public:
  /// WHOLE acquires the column's value bytes at open. DEFERRED leaves them on
  /// a file that fetches by range (an object store), where acquiring them would
  /// download the whole column: planning, filter folds and rank-live bitmaps
  /// read only the has-field bitset and the mono columns, and vectors are read
  /// through acquire() and the windows the caller holds. On a resident file
  /// (RAM, a mapping) the values are mapped either way and deferred() is false.
  enum class Values { WHOLE, DEFERRED };

  /// The encoding recorded on a vector field's column descriptor.
  static VectorEncoding encodingOf(const DecodedFieldRecord& fieldInfo) {
    const auto* bytes = fieldInfo.bytesColumn();
    return (VectorEncoding)(bytes ? bytes->vectorEncoding : 0);
  }

  VectorReader(PostingsReader& reader, const DecodedFieldRecord& fieldInfo, Values values = Values::WHOLE)
    : column(reader, fieldInfo, values == Values::DEFERRED),
      vectorEncoding(encodingOf(fieldInfo)),
      dimensions(0) {
    if (!vcodec::valid((int32_t)vectorEncoding) || !column.isFixedSize()) {
      throw std::runtime_error(std::format(
        "vector field '{}' has an unsupported column layout (encoding {})",
        (std::string_view)fieldInfo.name, (int32_t)vectorEncoding));
    }
    dimensions = vcodec::dimsFromBytes(vectorEncoding, column.fixedValueSize());
    if (dimensions < 0) {
      throw std::runtime_error(std::format(
        "vector field '{}' value size {} is invalid for encoding {}",
        (std::string_view)fieldInfo.name, column.fixedValueSize(), vcodec::name(vectorEncoding)));
    }
  }

  int32_t dims() const { return dimensions; }

  VectorEncoding encoding() const { return vectorEncoding; }

  /// Stored bytes per vector.
  int32_t valueBytes() const { return column.fixedValueSize(); }

  int32_t docsWithField() const { return column.docsWithField(); }

  /// True when the value bytes are acquired by range (see Values).
  bool deferred() const { return column.deferredValues(); }

  /// The file holding the values, and the range of vectors [from, to) in it.
  InputFile* file() const { return column.valuesFile(); }
  ByteRange windowRange(int64_t from, int64_t to) const {
    return {rowOffset(from), (uint64_t)(to - from) * (uint64_t)valueBytes()};
  }

  /// Deferred columns: acquires the ranges together (one batch, so a backend
  /// merges and issues them at once) and keeps the holds, so reads of the
  /// vectors in them make no request. The ranges must not overlap each other
  /// or ranges acquired before. Holds are released as reads move past them
  /// (ascending ranks), so a walk that declares batches ahead of its reads
  /// keeps the unread part of each; a read below the held ranges falls back
  /// to a single blocking acquire.
  ///
  /// A view returned by rowAtRank() or vectorAtRank() of a
  /// deferred column is valid only until the next read of this reader or the
  /// next acquire(): use it at once.
  void acquire(std::span<const ByteRange> ranges) {
    if (ranges.empty()) return;
    auto holds = column.valuesFile()->acquireAll(ranges);
    for (size_t i = 0; i < ranges.size(); i++) {
      auto it = std::upper_bound(held.begin(), held.end(), ranges[i].offset,
          [](uint64_t value, const Held& entry) { return value < entry.offset; });
      held.insert(it, Held{ranges[i].offset, std::move(holds[i])});
    }
  }

  int64_t numVectors() const { return column.numValues(); }

  bool multiValued() const { return column.multiValued(); }

  /// Stored value (in this segment's encoding) at a global value rank
  /// (0..numVectors()-1).
  std::string_view rowAtRank(int64_t valueRank) const {
    if (deferred()) return {heldRow(valueRank), (size_t)valueBytes()};
    return column.valueAt(valueRank);
  }

  /// Decodes the vector at a global value rank into out[0..dims()).
  void decodeAtRank(int64_t valueRank, float* out) const {
    vcodec::decode(vectorEncoding, rowAtRank(valueRank).data(), dimensions, out);
  }

  /// Decodes n consecutive vectors starting at valueRank into out (n * dims()).
  void decodeRange(int64_t valueRank, int64_t n, float* out) const {
    if (n <= 0) return;
    assert(!deferred());  // reads consecutive rows through one view
    const char* base = rowAtRank(valueRank).data();
    int64_t stride = valueBytes();
    if (vectorEncoding == VectorEncoding::F32) {
      std::memcpy(out, base, (size_t)(n * stride));
      return;
    }
    for (int64_t i = 0; i < n; i++) {
      vcodec::decode(vectorEncoding, base + i * stride, dimensions, out + i * dimensions);
    }
  }

  /// F32 columns only: the vector at a global value rank, without a copy.
  /// Other encodings must go through rowAtRank or decodeAtRank; reading their
  /// bytes as floats would return garbage, so this is checked in every build.
  LUXIR_INLINE std::span<const float> vectorAtRank(int64_t valueRank) const {
    if (vectorEncoding != VectorEncoding::F32) [[unlikely]] notF32();
    auto bytes = rowAtRank(valueRank);
    return std::span<const float>((const float*)bytes.data(), (size_t)dimensions);
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

} // namespace luxir
