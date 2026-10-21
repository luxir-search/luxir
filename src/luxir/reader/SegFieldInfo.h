// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <bit>
#include <type_traits>
#include "luxir/schema/FieldType.h"
#include "luxir/store/InputStream.h"
#include "luxir/store/OutputStream.h"

namespace luxir {

struct TermRangeTableHeader {
  uint32_t nRanges;
  uint32_t totalBlocks;
};

struct TermRangeRow {
  uint64_t firstTermOrd;
  uint32_t firstBlockOrd;
  uint32_t padding = 0;
  seg_location docsBase;
  seg_location posBase;
  seg_location termsBase;
  uint64_t docsBytes;
  uint64_t posBytes;
};

static_assert(sizeof(TermRangeTableHeader) == 8);
static_assert(sizeof(TermRangeRow) == 56);
static_assert(std::is_trivially_copyable_v<TermRangeRow>);
static_assert(std::endian::native == std::endian::little);

struct SegFieldInfo {
  enum NormsFormat : uint8_t {
    NORMS_NONE = 0,
    NORMS_FLAT = 1,
    NORMS_SPARSE = 2
  };

  enum OrdFormat : uint8_t {
    ORD_NONE = 0,
    ORD_DIRECT = 1,
    ORD_PREDICTED = 2
  };

  enum OrdIndexing : uint8_t {
    ORD_INDEX_NONE = 0,
    ORD_DOCID = 1,
    ORD_RANK = 2
  };

  PackedTerm fieldname;
  FieldType::Type type;
  int32_t flags;  // from FieldType
  seg_location termBlockIndexLoc;  // location of index into the terms blocks
  seg_location termsLoc;
  seg_location docsLoc;
  seg_location posLoc;
  int64_t nTerms;
  int32_t docsWithField;

  // Immutable header totals used directly for BM25 field statistics. These
  // include deleted documents; live-doc filtering does not change them.
  int64_t sumDocFreq;
  int64_t sumTotalTermFreq;
  seg_location trieLoc;
  int64_t trieRootOff;
  seg_location rangeTableLoc;

  // column
  seg_location docsWithFieldEndLoc;
  seg_location columnLoc;    // location of the start of the column
  int64_t columnMetaOff;     // offset from the start of the column to the metadata
  int64_t numValues;         // total number of values in the column across all docs.
                             // For single-valued fields numValues == docsWithField; for multi-valued it is >=.
  int32_t ordFormat = ORD_NONE;
  int32_t ordIndexing = ORD_INDEX_NONE;
  int32_t ordBits = 0;

  // Optional 1-D sorted-leaf points index. Absence is pointsMetaOff == 0;
  // pointsLoc.offset() may be zero for a present index in a nonzero file.
  seg_location pointsLoc;
  int64_t pointsMetaOff = 0;

  // Dedicated text norms column.  The docs-with-field bitset remains in
  // docsWithFieldEndLoc/docsWithField; this region stores only the raw norm bytes.
  int32_t normsFormat;
  seg_location normsLoc;
  int64_t normsLen;

  seg_location monoLoc;   // location of the monotonic column
  int64_t monoMetaOff;    // offset from the start of the mono column to the metadata

  // Optional second mono column.  Currently used by StrCol to carry both a per-doc
  // endValueRankReader (in monoLoc, only when multi-valued) and a per-value
  // endOffsetReader (in mono2Loc, only when variable-size).  When mono2Loc is unset
  // and the column is fixed-size, mono2MetaOff holds the fixed value size.
  seg_location mono2Loc;
  int64_t mono2MetaOff;

  // Optional third mono column: per-value rank -> owning segment-local docId.
  // Monotonic non-decreasing (values are written in doc order).  Written only for
  // multi-valued columns whose consumers need the reverse vector-rank -> doc lookup
  // (vector fields, for kNN hit grouping).  Unset (valDocLoc == {0,0}) otherwise; for
  // single-valued fields valueRank == docRank so no map is needed.
  seg_location valDocLoc;
  int64_t valDocMetaOff;

  // VECTOR only: the column's VectorEncoding, as written for this segment.
  // Serialized only for VECTOR fields.
  int32_t vectorEncoding = 0;

  // Within-file references are relative; these are the complete set of
  // segment-global bases.  Relocate them together before FieldInfo is
  // serialized so adding a new absolute base cannot silently update only one
  // writer path.  {0,0} is the null sentinel, not file 0 offset 0.
  template <typename Relocator>
  void relocateLocations(Relocator&& relocator) {
    auto relocate = [&](seg_location& location) {
      if (!location.isNull()) location = relocator(location);
    };
    relocate(termBlockIndexLoc);
    relocate(termsLoc);
    relocate(docsLoc);
    relocate(posLoc);
    relocate(trieLoc);
    relocate(rangeTableLoc);
    relocate(docsWithFieldEndLoc);
    relocate(columnLoc);
    relocate(pointsLoc);
    relocate(normsLoc);
    relocate(monoLoc);
    relocate(mono2Loc);
    relocate(valDocLoc);
  }
};

} // namespace luxir
