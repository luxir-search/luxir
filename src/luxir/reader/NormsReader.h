// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <bit>
#include <cstdint>
#include <limits>

#include "DocsReader.h"
#include "NormsLayout.h"
#include "PostingsReader.h"
#include "luxir/store/InputStream.h"
#include "luxir/util/luxir_util.h"

namespace luxir {

class NormsReader {
public:
  static constexpr int32_t ENDDOC = std::numeric_limits<int32_t>::max();

private:
  DocsReader docs;
  InputStream normsIS;
  // FLAT and SPARSE: the raw norm bytes. PACKED: the packed codes.
  const uint8_t* normsBytes = nullptr;
  int32_t format = SegFieldInfo::NORMS_NONE;
  int64_t nbytes = 0;
  int32_t maxdoc = 0;
  // Values are found by docid (FLAT, CONSTANT, docid-addressed PACKED) rather
  // than by presence rank (SPARSE, rank-addressed PACKED).
  bool byDoc = false;
  uint8_t constant = 0;
  // PACKED decode: value(i) = table[(codes[i >> codeShift] >> ((i & idxMask) << bitsLog)) & codeMask]
  uint8_t bitsLog = 0;
  uint8_t codeShift = 0;
  uint8_t idxMask = 0;
  uint8_t codeMask = 0;
  uint8_t table[NormsLayout::MAX_PACKED_CODES] = {};

  void initPacked() {
    const uint8_t* p = normsBytes;
    uint8_t bits = p[0];
    assert(bits == 1 || bits == 2 || bits == 4);
    bitsLog = (uint8_t) std::countr_zero(bits);
    codeShift = (uint8_t) (3 - bitsLog);
    idxMask = (uint8_t) ((1u << codeShift) - 1);
    codeMask = (uint8_t) ((1u << bits) - 1);
    byDoc = p[1] == NormsLayout::BY_DOC;
    uint32_t tableLen = p[2];
    if (tableLen == 0) {
      // Frame of reference: code c is norm byte min + c.
      for (uint32_t c = 0; c <= codeMask; c++) table[c] = (uint8_t) (p[3] + c);
      normsBytes = p + 4;
    } else {
      assert(tableLen <= (uint32_t) codeMask + 1);
      std::copy(p + 3, p + 3 + tableLen, table);
      normsBytes = p + 3 + tableLen;
    }
    [[maybe_unused]] int64_t count = byDoc ? maxdoc : docs.numDocs();
    assert(normsBytes - p + NormsLayout::packedBytes(count, bits) == nbytes);
  }

  uint8_t packedValue(int32_t index) const LUXIR_INLINE {
    uint32_t code = (uint32_t) normsBytes[(uint32_t) index >> codeShift]
                    >> (((uint32_t) index & idxMask) << bitsLog);
    return table[code & codeMask];
  }

public:
  NormsReader(PostingsReader& postingsReader, const SegFieldInfo& fieldInfo)
      : docs(postingsReader, fieldInfo) {
    format = fieldInfo.normsFormat;
    nbytes = fieldInfo.normsLen;
    maxdoc = postingsReader.maxDoc();
    if (format != SegFieldInfo::NORMS_NONE && nbytes > 0) {
      normsIS = postingsReader.getInputStreamSeek(fieldInfo.normsLoc);
      normsBytes = (const uint8_t*) normsIS.ptr();
    }
    if (format == SegFieldInfo::NORMS_FLAT) {
      assert(nbytes == maxdoc);
      byDoc = true;
    } else if (format == SegFieldInfo::NORMS_SPARSE) {
      assert(nbytes == docs.numDocs());
      assert(docs.hasBitset());
    } else if (format == SegFieldInfo::NORMS_CONSTANT) {
      assert(nbytes == 1);
      constant = normsBytes[0];
      byDoc = true;
    } else if (format == SegFieldInfo::NORMS_PACKED) {
      initPacked();
      assert(byDoc || docs.hasBitset());
    }
  }

  const DocsReader& docsReader() const noexcept {
    return docs;
  }

  int32_t normsFormat() const noexcept {
    return format;
  }

  int64_t numBytes() const noexcept {
    return nbytes;
  }

  int32_t maxDoc() const noexcept {
    return maxdoc;
  }

  bool isFlat() const noexcept {
    return format == SegFieldInfo::NORMS_FLAT;
  }

  const uint8_t* flatBase() const noexcept {
    return isFlat() ? normsBytes : nullptr;
  }

  // True when both read the same stored norms region, so one decode serves both.
  bool sharesNorms(const NormsReader& other) const noexcept {
    return format == other.format && normsBytes == other.normsBytes && nbytes == other.nbytes;
  }

  bool isSparse() const noexcept {
    return format == SegFieldInfo::NORMS_SPARSE;
  }

  bool isConstant() const noexcept {
    return format == SegFieldInfo::NORMS_CONSTANT;
  }

  bool isPacked() const noexcept {
    return format == SegFieldInfo::NORMS_PACKED;
  }

  // True when a doc's norm is found from its docid alone, without a presence
  // rank: FLAT, CONSTANT and docid-addressed PACKED.
  bool isByDoc() const noexcept {
    return byDoc;
  }

  // Norm of a doc with the field, for CONSTANT and docid-addressed PACKED
  // (FLAT callers read flatBase() directly). The value for a doc without the
  // field is unspecified.
  uint8_t byDocValue(int32_t doc) const LUXIR_INLINE {
    assert(byDoc && format != SegFieldInfo::NORMS_FLAT);
    assert(doc >= 0 && doc < maxdoc);
    return format == SegFieldInfo::NORMS_CONSTANT ? constant : packedValue(doc);
  }

  // Bulk byDocValue for the docs of a postings block.
  void byDocValues(const int32_t* docIds, int32_t count, uint8_t* out) const {
    assert(byDoc && format != SegFieldInfo::NORMS_FLAT);
    if (format == SegFieldInfo::NORMS_CONSTANT) {
      std::fill(out, out + count, constant);
      return;
    }
    for (int32_t i = 0; i < count; i++) {
      out[i] = packedValue(docIds[i]);
    }
  }

  // The value for a doc without the field is 0 for FLAT and unspecified for
  // CONSTANT and PACKED.
  uint8_t value(int32_t doc) const {
    assert(format != SegFieldInfo::NORMS_NONE);
    assert(doc >= 0 && doc < maxdoc);
    if (format == SegFieldInfo::NORMS_FLAT) {
      return normsBytes[doc];
    }
    if (byDoc) {
      return byDocValue(doc);
    }
    screaming::BitSet::Iterator iter(docs.bitset());
    int32_t found = iter.advance(doc);
    assert(found == doc);
    return valueAtRank(iter.rank());
  }

  uint8_t valueAtRank(int32_t rank) const {
    assert(!byDoc);
    return format == SegFieldInfo::NORMS_SPARSE ? normsBytes[rank] : packedValue(rank);
  }

  class Iterator {
    const NormsReader& reader;
    screaming::BitSet::Iterator docsIter;
    int32_t docRank = -1;
    int32_t doc = -1;
    int32_t maxRank = 0;
    bool densePresence = false;
    bool flatDirectAdvance = false;
    // True: advance/next stop only on docs that have the field, even for
    // docid-addressed layouts (whose values are unspecified elsewhere).
    bool checkPresence = false;

  public:
    Iterator(const NormsReader& reader, bool checkPresence = false)
        : reader(reader), docsIter(reader.docs.bitset()), checkPresence(checkPresence) {
      maxRank = reader.docs.numDocs();
      densePresence = !reader.docs.hasBitset();
    }

    int32_t docId() const {
      return doc;
    }

    int32_t rank() const {
      return docRank;
    }

    uint8_t value() const {
      assert(reader.format != SegFieldInfo::NORMS_NONE);
      assert(doc >= 0 && doc != ENDDOC);
      if (reader.format == SegFieldInfo::NORMS_FLAT) {
        return reader.normsBytes[doc];
      }
      if (reader.byDoc) {
        return reader.byDocValue(doc);
      }
      assert(docRank >= 0 && docRank < maxRank);
      return reader.valueAtRank(docRank);
    }

    int32_t advance(int32_t target) {
      if (reader.byDoc && !checkPresence) {
        if (target >= reader.maxdoc) {
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
        if (doc == ENDDOC || doc + 1 >= reader.maxdoc) {
          doc = ENDDOC;
          return doc;
        }
        docsIter = screaming::BitSet::Iterator(reader.docs.bitset());
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
