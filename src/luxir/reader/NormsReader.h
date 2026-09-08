// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <limits>

#include "DocsReader.h"
#include "PostingsReader.h"
#include "luxir/store/InputStream.h"

namespace luxir {

class NormsReader {
public:
  static constexpr int32_t ENDDOC = std::numeric_limits<int32_t>::max();

private:
  DocsReader docs;
  InputStream normsIS;
  const uint8_t* normsBytes = nullptr;
  int32_t format = SegFieldInfo::NORMS_NONE;
  int64_t nbytes = 0;
  int32_t maxdoc = 0;

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
    } else if (format == SegFieldInfo::NORMS_SPARSE) {
      assert(nbytes == docs.numDocs());
      assert(docs.hasBitset());
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

  bool isSparse() const noexcept {
    return format == SegFieldInfo::NORMS_SPARSE;
  }

  uint8_t value(int32_t doc) const {
    assert(format != SegFieldInfo::NORMS_NONE);
    assert(doc >= 0 && doc < maxdoc);
    if (format == SegFieldInfo::NORMS_FLAT) {
      return normsBytes[doc];
    }
    screaming::BitSet::Iterator iter(docs.bitset());
    int32_t found = iter.advance(doc);
    assert(found == doc);
    return normsBytes[iter.rank()];
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
        : reader(reader), docsIter(reader.docs.bitset()) {
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
      assert(docRank >= 0 && docRank < maxRank);
      return reader.normsBytes[docRank];
    }

    int32_t advance(int32_t target) {
      if (reader.format == SegFieldInfo::NORMS_FLAT) {
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
