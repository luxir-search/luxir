// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "luxir/reader/SegmentCore.h"
#include <algorithm>
#include <bit>
#include <cstdint>
#include <limits>

#include "DocsReader.h"
#include "NormsLayout.h"
#include "PostingsReader.h"
#include "ReaderErrors.h"
#include "luxir/store/InputStream.h"
#include "luxir/util/luxir_util.h"

namespace luxir {

// Reads the length norm for each doc in a text field.
class NormsReader {
public:
  static constexpr int32_t ENDDOC = std::numeric_limits<int32_t>::max();

private:
  using NormsMode = field_parts::NormsMode;

  DocsReader docs;  // Maps rank-addressed norms to doc IDs.
  // FLAT and SPARSE: the mapped norm bytes. PACKED: the packed codes.
  const uint8_t* values = nullptr;
  NormsMode mode = NormsMode::SPARSE;
  int32_t maxDoc = 0;
  // Every docid in [0, maxDoc) decodes directly, and a doc without the field
  // reads 0: FLAT, docid-addressed PACKED, and CONSTANT without a docs
  // bitmap. SPARSE and rank-addressed PACKED are read by presence rank;
  // CONSTANT with a bitmap reads its byte for every doc, so callers that see
  // docs without the field walk the bitmap.
  bool byDoc = false;
  uint8_t constant = 0;
  // PACKED decode: value(i) = table[(codes[i >> codeShift] >> ((i & idxMask) << bitsLog)) & codeMask]
  uint8_t bitsLog = 0;
  uint8_t codeShift = 0;
  uint8_t idxMask = 0;
  uint8_t codeMask = 0;
  uint8_t table[NormsLayout::MAX_PACKED_CODES] = {};

  // The catalog checked that the region lies in its file and holds more than
  // the fixed header; this checks the header against the region size.
  void initPacked(const SegmentReadContext& context, uint64_t bytes) {
    const uint8_t* p = values;
    uint32_t bits = p[0];
    uint32_t addressing = p[1];
    uint32_t tableLen = p[2];
    if ((bits != 1 && bits != 2 && bits != 4)
        || (addressing != NormsLayout::BY_DOC && addressing != NormsLayout::BY_RANK)
        || tableLen > (1u << bits)) {
      context.invalid("packed norms header");
    }
    bitsLog = (uint8_t) std::countr_zero(bits);
    codeShift = (uint8_t) (3 - bitsLog);
    idxMask = (uint8_t) ((1u << codeShift) - 1);
    codeMask = (uint8_t) ((1u << bits) - 1);
    byDoc = addressing == NormsLayout::BY_DOC;
    int64_t header = NormsLayout::PACKED_HEADER_BYTES + (tableLen == 0 ? 1 : (int64_t) tableLen);
    int64_t count = byDoc ? maxDoc : docs.docsWithField();
    if (!byDoc && docs.allRows()) context.invalid("rank-addressed packed norms require a docs bitmap");
    if ((uint64_t) (header + NormsLayout::packedBytes(count, bits)) != bytes) {
      context.invalid("packed norms size");
    }
    if (tableLen == 0) {
      // Frame of reference: code c is norm byte min + c.
      for (uint32_t c = 0; c <= codeMask; c++) table[c] = (uint8_t) (p[3] + c);
    } else {
      std::copy(p + 3, p + 3 + tableLen, table);
    }
    values = p + header;
  }

  uint8_t packedValue(int32_t index) const LUXIR_INLINE {
    assert(isPacked());
    uint32_t code = (uint32_t) values[(uint32_t) index >> codeShift]
                    >> (((uint32_t) index & idxMask) << bitsLog);
    return table[code & codeMask];
  }

public:
  NormsReader(PostingsReader& reader, const DecodedFieldRecord& fieldInfo)
      : docs(reader, fieldInfo) {
    maxDoc = reader.maxDoc();
    if (const auto* norms = fieldInfo.norms()) {
      mode = norms->mode;
      auto region = fieldInfo.normsValues(maxDoc);
      auto slice = reader.core().slice(region);
      values = (const uint8_t*)slice.data();
      assert(mode != NormsMode::SPARSE || docs.bitset());
      if (mode == NormsMode::FLAT) {
        byDoc = true;
      } else if (mode == NormsMode::CONSTANT) {
        constant = values[0];
        byDoc = docs.allRows();
      } else if (mode == NormsMode::PACKED) {
        initPacked(segmentReadContext(reader.core(), fieldInfo, region.location.filenum()), region.bytes);
      }
    }
  }

  const DocsReader& docsReader() const noexcept {
    return docs;
  }

  bool isFlat() const noexcept {
    return mode == NormsMode::FLAT;
  }

  const uint8_t* flatBase() const noexcept {
    return isFlat() ? values : nullptr;
  }

  bool isSparse() const noexcept {
    return values && mode == NormsMode::SPARSE;
  }

  bool isConstant() const noexcept {
    return values && mode == NormsMode::CONSTANT;
  }

  // The norm byte of every doc with a CONSTANT field.
  uint8_t constantValue() const noexcept {
    assert(isConstant());
    return constant;
  }

  bool isPacked() const noexcept {
    return values && mode == NormsMode::PACKED;
  }

  bool isPackedByDoc() const noexcept {
    return isPacked() && byDoc;
  }

  // True when byDocValue(doc) is the norm of every doc in the segment, 0 for
  // a doc without the field: FLAT, docid-addressed PACKED, and CONSTANT
  // without a docs bitmap.
  bool isByDoc() const noexcept {
    return values && byDoc;
  }

  // True when both read the same stored norms, so one decode serves both.
  bool sharesNorms(const NormsReader& other) const noexcept {
    return mode == other.mode && values == other.values;
  }

  // O(1) norm by docid for the layouts that do not need a presence rank:
  // FLAT, CONSTANT and docid-addressed PACKED. A doc without the field reads
  // 0, except under CONSTANT with a docs bitmap (see isByDoc).
  uint8_t byDocValue(int32_t doc) const LUXIR_INLINE {
    assert(doc >= 0 && doc < maxDoc);
    if (mode == NormsMode::FLAT) return values[doc];
    if (mode == NormsMode::CONSTANT) return constant;
    assert(isPackedByDoc());
    return packedValue(doc);
  }

  // byDocValue for docid-addressed PACKED, without the layout dispatch.
  uint8_t packedByDocValue(int32_t doc) const LUXIR_INLINE {
    assert(isPackedByDoc());
    assert(doc >= 0 && doc < maxDoc);
    return packedValue(doc);
  }

  // Bulk norms of docs that have the field, for CONSTANT and docid-addressed
  // PACKED (FLAT callers index flatBase() directly).
  void byDocValues(const int32_t* docIds, int32_t count, uint8_t* out) const {
    if (mode == NormsMode::CONSTANT) {
      std::fill(out, out + count, constant);
      return;
    }
    assert(isPackedByDoc());
    for (int32_t i = 0; i < count; i++) {
      out[i] = packedValue(docIds[i]);
    }
  }

  // The norm of a doc with the field.
  uint8_t value(int32_t doc) const {
    assert(values);
    assert(doc >= 0 && doc < maxDoc);
    if (byDoc || mode == NormsMode::CONSTANT) {
      return byDocValue(doc);
    }
    auto iter = docs.bitmapIterator();
    [[maybe_unused]] int32_t found = iter.advance(doc);
    assert(found == doc);
    return valueAtRank(iter.rank());
  }

  uint8_t valueAtRank(int32_t rank) const {
    assert(!byDoc);
    if (mode == NormsMode::SPARSE) return values[rank];
    if (mode == NormsMode::CONSTANT) return constant;
    return packedValue(rank);
  }

  class Iterator {
    const NormsReader& reader;
    screaming::BitSet::Iterator docsIter;
    int32_t docRank = -1;
    int32_t doc = -1;
    int32_t maxRank = 0;
    bool densePresence = false;
    bool flatDirectAdvance = false;

  public:
    Iterator(const NormsReader& reader)
        : reader(reader), docsIter(reader.docs.bitmapIterator()) {
      maxRank = reader.docs.docsWithField();
      densePresence = reader.docs.allRows();
    }

    int32_t docId() const {
      return doc;
    }

    int32_t rank() const {
      return docRank;
    }

    uint8_t value() const {
      assert(reader.values);
      assert(doc >= 0 && doc != ENDDOC);
      if (reader.byDoc) {
        return reader.byDocValue(doc);
      }
      assert(docRank >= 0 && docRank < maxRank);
      return reader.valueAtRank(docRank);
    }

    // Lands on its target directly for isByDoc() layouts, where a doc
    // without the field reads 0, and walks the docs bitmap otherwise.
    int32_t advance(int32_t target) {
      if (reader.byDoc) {
        if (target >= reader.maxDoc) {
          doc = ENDDOC;
        } else {
          doc = target;
          docRank = densePresence ? target : -1;
        }
        flatDirectAdvance = !densePresence;
      } else if (densePresence) {
        if (target >= maxRank) {
          doc = ENDDOC;
        } else {
          doc = docRank = target;
        }
      } else {
        doc = docsIter.advance(target);
        docRank = docsIter.rank();
      }
      return doc;
    }

    int32_t next() {
      if (flatDirectAdvance) {
        flatDirectAdvance = false;
        if (doc == ENDDOC || doc + 1 >= reader.maxDoc) {
          doc = ENDDOC;
          return doc;
        }
        docsIter.reset();
        doc = docsIter.advance(doc + 1);
        docRank = doc == ENDDOC ? maxRank : docsIter.rank();
        return doc;
      }
      if (docRank + 1 >= maxRank) {
        doc = ENDDOC;
        return doc;
      }
      docRank++;
      if (densePresence) {
        doc++;
      } else {
        doc = docsIter.next();
      }
      return doc;
    }
  };
};

} // namespace luxir
