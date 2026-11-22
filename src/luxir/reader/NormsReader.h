// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "luxir/reader/SegmentCore.h"
#include <cstdint>
#include <limits>

#include "DocsReader.h"
#include "PostingsReader.h"
#include "luxir/store/InputStream.h"

namespace luxir {

// Reads the length norm for each doc in a text field.
class NormsReader {
public:
  static constexpr int32_t ENDDOC = std::numeric_limits<int32_t>::max();

private:
  DocsReader docs;  // Maps sparse norms to doc IDs.
  const uint8_t* values = nullptr;  // Mapped norm bytes, if written.
  field_parts::NormsMode mode = field_parts::NormsMode::SPARSE;  // Whether norms are doc- or rank-indexed.
  int32_t maxDoc = 0;

public:
  NormsReader(PostingsReader& reader, const DecodedFieldRecord& fieldInfo)
      : docs(reader, fieldInfo) {
    maxDoc = reader.maxDoc();
    if (const auto* norms = fieldInfo.norms()) {
      mode = norms->mode;
      auto slice = reader.core().slice(fieldInfo.normsValues(maxDoc));
      values = (const uint8_t*)slice.data();
      assert(mode == field_parts::NormsMode::FLAT || docs.bitset());
    }
  }

  const DocsReader& docsReader() const noexcept {
    return docs;
  }

  bool isFlat() const noexcept {
    return mode == field_parts::NormsMode::FLAT;
  }

  const uint8_t* flatBase() const noexcept {
    return isFlat() ? values : nullptr;
  }

  bool isSparse() const noexcept {
    return values && mode == field_parts::NormsMode::SPARSE;
  }

  uint8_t value(int32_t doc) const {
    assert(values);
    assert(doc >= 0 && doc < maxDoc);
    if (mode == field_parts::NormsMode::FLAT) {
      return values[doc];
    }
    auto iter = docs.bitmapIterator();
    [[maybe_unused]] int32_t found = iter.advance(doc);
    assert(found == doc);
    return values[iter.rank()];
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
      if (reader.mode == field_parts::NormsMode::FLAT) {
        return reader.values[doc];
      }
      assert(docRank >= 0 && docRank < maxRank);
      return reader.values[docRank];
    }

    int32_t advance(int32_t target) {
      if (reader.mode == field_parts::NormsMode::FLAT) {
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
