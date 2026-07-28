#pragma once

#include "solux/search/Collector.h"
#include "solux/reader/PostingsReader.h"
#include "solux/reader/FieldReader.h"
#include "solux/reader/IntColReader.h"
#include "solux/reader/OrdColReader.h"
#include "solux/reader/StrColReader.h"
#include "solux/reader/TermsEnum.h"
#include "solux/search/OrdMap.h"
#include "solux/util/MemPool.h"
#include "solux/util/solux_util.h"
#include <bit>
#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>
#include <limits>
#include <cassert>
#include <optional>
#include <span>

namespace solux {

class FieldComparator {
public:
  // Optional batch sort-key capability for comparators whose complete sort
  // order is an ascending transformed int64 key. Valid for the current segment
  // only; setSegment resets its cursors. Implementations must not depend on
  // setBottom notifications while this capability is in use.
  // NOTE: no virtual destructor; never owned or deleted through this type.
  class KeyBatch {
  public:
    // Live comparator slot storage, read and written directly by the collector.
    std::span<int64_t> slotKeys;

    // Gather transformed keys for ascending in-segment docs.
    virtual void gatherKeys(std::span<const int32_t> docs,
                            std::span<int64_t> keys) = 0;
  };

  virtual ~FieldComparator() = default;

  // Set the current segment for this comparator
  virtual void setSegment(int32_t segment, PostingsReader* reader) = 0;

  virtual void setBottom(int32_t slot) { unused(slot); }

  // compare new document to bottom slot
  virtual int compareBottom(int32_t bottomSlot, segdoc bottomDoc, segdoc newDoc) = 0;

  // compare two slots
  virtual int compare(int32_t slotA, segdoc docA, int32_t slotB, segdoc docB) = 0;

  // compare a slot in this comparator with a slot in another comparator
  virtual int compare(int32_t slotA, segdoc docA, FieldComparator& other, int32_t slotB, segdoc docB) = 0;

  // Copy the value from a document to a slot.
  virtual void copy(int32_t slot, segdoc doc) = 0;

  // Copy the value from a different comparator to this comparator
  virtual void copy(int32_t slot, FieldComparator& other, int32_t otherSlot, segdoc otherDoc) = 0;

  virtual KeyBatch* keyBatch() { return nullptr; }

  enum MissingValue {
    MISSING_FIRST,
    MISSING_LAST
  };
};



// A comparator that can be used in a simplified collector that has values
// that can be compared across different segments.
class SimpleNumericFieldComparator : public FieldComparator,
                                     public FieldComparator::KeyBatch {
  // IntColBM break-even: decodeFrame wins at eight gathered values per frame.
  static constexpr size_t FRAME_DECODE_MIN = 8;

  std::string fieldName;
  std::optional<IntColReader> reader;
  std::optional<IntColReader::Iterator> iter;
  std::optional<IntColReader::SparseValues> pointValues;
  std::optional<screaming::BitSet::Iterator> docsIter;
  std::vector<int64_t> values; // Storage for bottom values
  int64_t sortMultiplier; // 1 for ascending, -1 for descending
  int64_t missingValueSubstitute;
  int64_t lastValue = 0;  // TODO: cache the last value lookup in compareBottom so we don't have to re-fetch if competitive
  segdoc lastDoc = {-1, -1};
  int64_t decoded[NumColumn::BULK_SIZE];
  int64_t decodedStart = -1;
  int64_t decodedEnd = -1;
  int32_t landedDoc = -1;
  int32_t landedRank = -1;
  bool multiValued = false;

  int64_t transformPresent(int64_t value) const {
    return sortMultiplier < 0 ? ~value : value;
  }

  void gatherDenseSingle(std::span<const int32_t> docs,
                         std::span<int64_t> keys) {
    size_t i = 0;
    while (i < docs.size()) {
      int64_t frameStart =
          (int64_t)docs[i] / NumColumn::BULK_SIZE * NumColumn::BULK_SIZE;
      int64_t frameEnd = frameStart + NumColumn::BULK_SIZE;
      size_t groupEnd = i + 1;
      while (groupEnd < docs.size() && docs[groupEnd] < frameEnd) {
        groupEnd++;
      }

      if (decodedStart == frameStart) {
        for (; i < groupEnd; i++) {
          keys[i] = transformPresent(decoded[docs[i] - decodedStart]);
        }
      } else if (groupEnd - i >= FRAME_DECODE_MIN) {
        uint32_t count;
        decodedStart = reader->decodeValueSubBlock(docs[i], decoded, count);
        decodedEnd = decodedStart + count;
        for (; i < groupEnd; i++) {
          assert(docs[i] >= decodedStart && docs[i] < decodedEnd);
          keys[i] = transformPresent(decoded[docs[i] - decodedStart]);
        }
      } else {
        for (; i < groupEnd; i++) {
          keys[i] = transformPresent(pointValues->valueAt(docs[i]));
        }
      }
    }
  }

  bool land(int32_t doc) {
    if (doc > landedDoc) {
      landedDoc = docsIter->advance(doc);
      landedRank = docsIter->rank();
    }
    return doc == landedDoc;
  }

  void gatherSparseSingle(std::span<const int32_t> docs,
                          std::span<int64_t> keys) {
    for (size_t i = 0; i < docs.size(); i++) {
      if (!land(docs[i])) {
        keys[i] = missingValueSubstitute;
      } else {
        keys[i] = transformPresent(pointValues->valueAt(landedRank));
      }
    }
  }

  void gatherMultiValued(std::span<const int32_t> docs,
                         std::span<int64_t> keys) {
    bool dense = !docsIter.has_value();
    for (size_t i = 0; i < docs.size(); i++) {
      int32_t rank;
      if (dense) {
        rank = docs[i];
      } else if (land(docs[i])) {
        rank = landedRank;
      } else {
        keys[i] = missingValueSubstitute;
        continue;
      }
      int64_t valueRank = reader->getStartValueRank(rank);
      keys[i] = transformPresent(pointValues->valueAt(valueRank));
    }
  }

public:
  SimpleNumericFieldComparator(const std::string& fieldName, int numHits, bool reversed, MissingValue missingValue)
    : fieldName(fieldName),
      values(numHits),
      sortMultiplier(reversed ? -1 : 1) {
    slotKeys = values;
    
    // Missing placement is a fixed edge regardless of direction (the string
    // comparators and expression sort keys follow the same convention), so
    // the substitute is chosen in the TRANSFORMED key space: getDocValue
    // ~-maps present values for descending sorts, and an untransformed
    // extreme lands at the same output edge either way.
    missingValueSubstitute = (missingValue == MISSING_FIRST)
                               ? std::numeric_limits<int64_t>::min()
                               : std::numeric_limits<int64_t>::max();
  }
  
  void setSegment(int32_t segment, PostingsReader* postingsReader) override {
    // Reset reader and iterator
    iter.reset();
    pointValues.reset();
    docsIter.reset();
    reader.reset();
    multiValued = false;
    decodedStart = -1;
    decodedEnd = -1;
    landedDoc = -1;
    landedRank = -1;

    if (!postingsReader) return;

    auto poolGuard = MemPool::threadLocalPoolGuard();
    FieldReader fieldReader(*postingsReader);
    if (fieldReader.seek(fieldName)) {
      SegFieldInfo fieldInfo;
      fieldReader.readFieldInfo(fieldInfo);
      if (fieldInfo.columnLoc.offset() > 0) {
        reader.emplace(*postingsReader, fieldInfo);
        iter.emplace(*reader);
        pointValues.emplace(*reader);
        multiValued = reader->multiValued();
        if (!reader->denseDocsWithValue()) {
          docsIter.emplace(reader->docsWithValueBitSet());
        }
      }
    }
  }

  FieldComparator::KeyBatch* keyBatch() override {
    return this;
  }

  void gatherKeys(std::span<const int32_t> docs,
                  std::span<int64_t> keys) override {
    assert(keys.size() == docs.size());
    if (!reader.has_value()) {
      std::fill(keys.begin(), keys.end(), missingValueSubstitute);
    } else if (multiValued) {
      gatherMultiValued(docs, keys);
    } else if (docsIter.has_value()) {
      gatherSparseSingle(docs, keys);
    } else {
      gatherDenseSingle(docs, keys);
    }
  }

  virtual int64_t getDocValue(int32_t docid) {
    if (!iter.has_value()) {
      return missingValueSubstitute;
    }

    if (iter->docId() < docid) {
      iter->advance(docid);
    }
    if (iter->docId() > docid) {
      return missingValueSubstitute;
    }
    // Multi-valued numeric columns store per-doc values unsorted, so the
    // cheap deterministic sort key is the FIRST stored value (storage order).
    // min/max over unsorted values would scan every value per doc; callers
    // that want that spell it explicitly (min(f)/max(f) sort expressions).
    int64_t value = multiValued
        ? iter->values().valueAt(reader->getStartValueRank(iter->rank()))
        : iter->value();
    // For descending sort, negate the value.
    // if we wanted to make this branchless, we could have a an adder and multiplier
    // but this will be a predictable branch anyway.
    if (sortMultiplier < 0) {
      // Descending: ~value == -(value+1) but with no signed-overflow UB.
      // -(value+1) overflows when value==INT64_MAX (and negating INT64_MIN is
      // UB too); ~value is the same order-reversing map over the full range.
      value = ~value;
    }
    return value;
  }
  
  int compare(int32_t slotA, segdoc docA, int32_t slotB, segdoc docB) override {
    // SimpleNumericFieldComparator can ignore segments when comparing values
    // since numeric values are comparable across segments
    assert(slotA >= 0 && slotA < (int32_t)values.size());
    assert(slotB >= 0 && slotB < (int32_t)values.size());
    int64_t valA = values[slotA];
    int64_t valB = values[slotB];
    return (valA > valB) - (valA < valB);
  }
  
  // NOINLINE: keeps the per-doc rejection path a single out-of-line unit with the
  // column iterator inlined into it, rather than speculative-devirt inlining a
  // half of it into the collect loop and spilling value()/advance() to calls.
  SOLUX_NOINLINE int compareBottom(int32_t bottomSlot, segdoc bottomDoc, segdoc newDoc) override {
    // Get the bottom value from the slot
    assert(bottomSlot >= 0 && bottomSlot < (int32_t)values.size());
    int64_t bottomVal = values[bottomSlot];

    // Get the new document's value
    int64_t newVal = getDocValue(newDoc.docId());

    return (bottomVal > newVal) - (bottomVal < newVal);
  }
  
  void copy(int32_t slot, segdoc doc) override {
    assert(slot >= 0 && slot < (int32_t)values.size());
    int64_t value = getDocValue(doc.docId());
    values[slot] = value;
  }
  
  int compare(int32_t slotA, segdoc docA, FieldComparator& other, int32_t slotB, segdoc docB) override {
    // For numeric comparators, we can compare the stored values directly
    assert(dynamic_cast<SimpleNumericFieldComparator*>(&other) != nullptr);
    auto* otherNumeric = static_cast<SimpleNumericFieldComparator*>(&other);
    
    assert(slotA >= 0 && slotA < (int32_t)values.size());
    assert(slotB >= 0 && slotB < (int32_t)otherNumeric->values.size());
    int64_t valA = values[slotA];
    int64_t valB = otherNumeric->values[slotB];
    return (valA > valB) - (valA < valB);
  }
  
  void copy(int32_t slot, FieldComparator& other, int32_t otherSlot, segdoc otherDoc) override {
    // Copy value from another comparator
    assert(dynamic_cast<SimpleNumericFieldComparator*>(&other) != nullptr);
    auto* otherNumeric = static_cast<SimpleNumericFieldComparator*>(&other);
    
    assert(slot >= 0 && slot < (int32_t)values.size());
    assert(otherSlot >= 0 && otherSlot < (int32_t)otherNumeric->values.size());
    values[slot] = otherNumeric->values[otherSlot];
  }
  
  ~SimpleNumericFieldComparator() override = default;
};

// A comparator for string fields that uses global ordinals for efficient cross-segment comparison
class GlobalOrdComparator : public FieldComparator {
  std::string fieldName;
  std::shared_ptr<OrdMap> ordMap;
  std::optional<OrdColReader> reader;
  std::vector<int64_t> globalOrds; // Storage for global ordinals
  OrdMap::SegToGlobal segmentMapping;
  int64_t sortMultiplier; // 1 for ascending, -1 for descending
  int64_t missingOrd;

public:
  GlobalOrdComparator(const std::string& fieldName, std::shared_ptr<OrdMap> ordMap, 
                      int numHits, bool reversed, MissingValue missingValue)
    : fieldName(fieldName),
      ordMap(ordMap),
      globalOrds(numHits),
      sortMultiplier(reversed ? -1 : 1) {
    
    // For string fields, ordinal 0 means missing value
    // For MISSING_FIRST: missing values should sort before all other values
    // For MISSING_LAST: missing values should sort after all other values
    // Real ords get the direction multiplier; missing is already placed at
    // the requested edge and must not be reversed a second time.
    if (missingValue == MISSING_FIRST) {
      missingOrd = std::numeric_limits<int64_t>::min();
    } else {
      missingOrd = std::numeric_limits<int64_t>::max();
    }
  }
  
  void setSegment(int32_t segment, PostingsReader* postingsReader) override {
    reader.reset();
    segmentMapping = {};
    
    if (!postingsReader) return;
    
    // Get segment-to-global ordinal mapping from OrdMap
    if (ordMap) {
      segmentMapping = ordMap->getSegToGlobal(segment);
    }
    
    // Load the ordinal column reader for this segment
    auto poolGuard = MemPool::threadLocalPoolGuard();
    FieldReader fieldReader(*postingsReader);
    if (fieldReader.seek(fieldName)) {
      SegFieldInfo fieldInfo;
      fieldReader.readFieldInfo(fieldInfo);
      if (!ordMap) {
        segmentMapping.numOrds = fieldInfo.nTerms;
      }
      if (fieldInfo.columnLoc.offset() > 0) {
        reader.emplace(*postingsReader, fieldInfo);
        assert(!reader->multiValued());
      }
    }
  }
  
  int64_t getGlobalOrd(int32_t docid) {
    if (!reader.has_value()) {
      return missingOrd;
    }

    int64_t segmentOrd = reader->ordAt(docid);
    if (segmentOrd == 0) {
      // Ordinal 0 means missing value
      return missingOrd;
    }
    
    // Convert segment ordinal to global ordinal
    // Segment ords are 1-based while the delta array is 0-based.
    int64_t globalOrd = segmentOrd + segmentMapping.deltaAt(segmentOrd - 1);
    
    // Apply sort multiplier for ascending/descending sort
    return sortMultiplier * globalOrd;
  }
  
  int compare(int32_t slotA, segdoc docA, int32_t slotB, segdoc docB) override {
    assert(slotA >= 0 && slotA < (int32_t)globalOrds.size());
    assert(slotB >= 0 && slotB < (int32_t)globalOrds.size());
    int64_t ordA = globalOrds[slotA];
    int64_t ordB = globalOrds[slotB];
    return (ordA > ordB) - (ordA < ordB);
  }
  
  // NOINLINE: see SimpleNumericFieldComparator::compareBottom.
  SOLUX_NOINLINE int compareBottom(int32_t bottomSlot, segdoc bottomDoc, segdoc newDoc) override {
    assert(bottomSlot >= 0 && bottomSlot < (int32_t)globalOrds.size());
    int64_t bottomOrd = globalOrds[bottomSlot];
    int64_t newOrd = getGlobalOrd(newDoc.docId());
    return (bottomOrd > newOrd) - (bottomOrd < newOrd);
  }
  
  void copy(int32_t slot, segdoc doc) override {
    assert(slot >= 0 && slot < (int32_t)globalOrds.size());
    globalOrds[slot] = getGlobalOrd(doc.docId());
  }
  
  int compare(int32_t slotA, segdoc docA, FieldComparator& other, int32_t slotB, segdoc docB) override {
    assert(dynamic_cast<GlobalOrdComparator*>(&other) != nullptr);
    auto* otherOrd = static_cast<GlobalOrdComparator*>(&other);
    
    assert(slotA >= 0 && slotA < (int32_t)globalOrds.size());
    assert(slotB >= 0 && slotB < (int32_t)otherOrd->globalOrds.size());
    int64_t ordA = globalOrds[slotA];
    int64_t ordB = otherOrd->globalOrds[slotB];
    return (ordA > ordB) - (ordA < ordB);
  }
  
  void copy(int32_t slot, FieldComparator& other, int32_t otherSlot, segdoc otherDoc) override {
    assert(dynamic_cast<GlobalOrdComparator*>(&other) != nullptr);
    auto* otherOrd = static_cast<GlobalOrdComparator*>(&other);
    
    assert(slot >= 0 && slot < (int32_t)globalOrds.size());
    assert(otherSlot >= 0 && otherSlot < (int32_t)otherOrd->globalOrds.size());
    globalOrds[slot] = otherOrd->globalOrds[otherSlot];
  }
  
  ~GlobalOrdComparator() override = default;
};

class SegmentOrdComparator : public FieldComparator {
  struct Slot {
    int32_t ord = 0;
    int32_t ordGen = -1;
    char* term = nullptr;
    char* buffer = nullptr;
    uint32_t length = 0;
    uint32_t capacity = 0;
  };

  std::string fieldName;
  MemPool termPool;
  MemPool enumPool;
  MemPool::save_point enumPoolStart;
  SegFieldInfo fieldInfo{};
  std::optional<OrdColReader> reader;
  std::optional<TermsEnum> termsEnum;
  std::vector<Slot> slots;
  int32_t currentSegment = -1;
  int32_t bottomOrd = 0;
  int32_t sortMultiplier;
  int32_t missingSortCmp;
  bool bottomMissing = true;
  bool bottomExact = false;

  static int compareTerms(std::string_view a, std::string_view b) {
    size_t len = std::min(a.size(), b.size());
    int cmp = memcmp(a.data(), b.data(), len);
    if (cmp != 0) return cmp;
    return (a.size() > b.size()) - (a.size() < b.size());
  }

  std::string_view term(const Slot& slot) const {
    assert(slot.ord != 0);
    assert(slot.term != nullptr);
    return {slot.term, slot.length};
  }

  int compareMissing(bool aMissing, bool bMissing) const {
    if (aMissing == bMissing) return 0;
    return aMissing ? missingSortCmp : -missingSortCmp;
  }

  int compareSlots(const Slot& a, const Slot& b) const {
    bool aMissing = a.ord == 0;
    bool bMissing = b.ord == 0;
    if (aMissing || bMissing) return compareMissing(aMissing, bMissing);
    if (a.ordGen == b.ordGen) {
      return (a.ord > b.ord) - (a.ord < b.ord);
    }
    return compareTerms(term(a), term(b));
  }

  void setMissing(Slot& slot) {
    slot.ord = 0;
    slot.ordGen = -1;
    slot.term = nullptr;
    slot.length = 0;
  }

  void copyTerm(Slot& slot, std::string_view value) {
    if (slot.capacity < value.size() || slot.buffer == nullptr) {
      size_t capacity = std::bit_ceil(std::max<size_t>(8, value.size()));
      slot.buffer = termPool.alloc(capacity);
      slot.capacity = (uint32_t)capacity;
    }
    if (!value.empty()) memmove(slot.buffer, value.data(), value.size());
    slot.term = slot.buffer;
    slot.length = (uint32_t)value.size();
  }

  int32_t maxOrdAt(int32_t docid) const {
    const DocsReader& docs = reader->docsReader();
    int32_t docRank;
    if (!docs.hasBitset()) {
      docRank = docid;
    } else {
      screaming::BitSet::Iterator iter(docs.bitset());
      if (iter.advance(docid) != docid) return 0;
      docRank = iter.rank();
    }
    auto [start, end] = reader->getStartEndValueRank(docRank);
    assert(start < end);
    OrdColReader::PointOrds ords(*reader);
    return ords.valueAt(end - 1);
  }

  int32_t selectedOrd(int32_t docid) const {
    if (!reader.has_value()) return 0;
    if (!reader->multiValued()) return reader->ordAt(docid);
    return sortMultiplier > 0 ? reader->firstOrdAt(docid) : maxOrdAt(docid);
  }

public:
  SegmentOrdComparator(const std::string& fieldName, int numHits,
                       bool reversed, MissingValue missingValue)
    : fieldName(fieldName),
      enumPoolStart(enumPool.getSavePoint()),
      slots(numHits),
      sortMultiplier(reversed ? -1 : 1),
      // GlobalOrdComparator keeps its missing sentinel at the requested edge.
      missingSortCmp((missingValue == MISSING_FIRST ? -1 : 1) * sortMultiplier) {}

  void setSegment(int32_t segment, PostingsReader* postingsReader) override {
    termsEnum.reset();
    reader.reset();
    enumPool.rewind(enumPoolStart);
    fieldInfo = {};
    currentSegment = segment;

    if (!postingsReader) return;

    FieldReader fieldReader(*postingsReader);
    if (!fieldReader.seek(fieldName)) return;
    fieldReader.readFieldInfo(fieldInfo);
    if (fieldInfo.nTerms == 0 || fieldInfo.columnLoc.offset() == 0 ||
        fieldInfo.ordFormat == SegFieldInfo::ORD_NONE) {
      return;
    }
    reader.emplace(*postingsReader, fieldInfo);
    termsEnum.emplace(enumPool, *postingsReader, fieldInfo);
  }

  void setBottom(int32_t slot) override {
    assert(slot >= 0 && slot < (int32_t)slots.size());
    Slot& bottom = slots[slot];
    bottomMissing = bottom.ord == 0;
    bottomExact = false;
    bottomOrd = 0;
    if (bottomMissing || !termsEnum.has_value()) return;

    if (bottom.ordGen == currentSegment) {
      bottomOrd = bottom.ord;
      bottomExact = true;
      return;
    }

    std::string_view value = term(bottom);
    if (!termsEnum->seekCeil(value)) {
      assert(fieldInfo.nTerms <= INT32_MAX);
      bottomOrd = (int32_t)fieldInfo.nTerms;
      return;
    }
    if (termsEnum->term() == value) {
      bottomOrd = (int32_t)termsEnum->ord() + 1;
      bottomExact = true;
      bottom.ord = bottomOrd;
      bottom.ordGen = currentSegment;
    } else {
      bottomOrd = (int32_t)termsEnum->ord();
    }
  }

  int compare(int32_t slotA, segdoc docA, int32_t slotB, segdoc docB) override {
    unused(docA, docB);
    assert(slotA >= 0 && slotA < (int32_t)slots.size());
    assert(slotB >= 0 && slotB < (int32_t)slots.size());
    return sortMultiplier * compareSlots(slots[slotA], slots[slotB]);
  }

  SOLUX_NOINLINE int compareBottom(int32_t bottomSlot, segdoc bottomDoc,
                                   segdoc newDoc) override {
    unused(bottomDoc);
    assert(bottomSlot >= 0 && bottomSlot < (int32_t)slots.size());
    assert(bottomMissing == (slots[bottomSlot].ord == 0));
    int32_t docOrd = selectedOrd(newDoc.docId());
    bool docMissing = docOrd == 0;
    int cmp;
    if (bottomMissing || docMissing) {
      cmp = compareMissing(bottomMissing, docMissing);
    } else if (bottomExact) {
      cmp = (bottomOrd > docOrd) - (bottomOrd < docOrd);
    } else {
      cmp = bottomOrd >= docOrd ? 1 : -1;
    }
    return sortMultiplier * cmp;
  }

  void copy(int32_t slot, segdoc doc) override {
    assert(slot >= 0 && slot < (int32_t)slots.size());
    Slot& target = slots[slot];
    int32_t ord = selectedOrd(doc.docId());
    if (ord == 0) {
      setMissing(target);
      return;
    }
    assert(termsEnum.has_value());
    termsEnum->seekOrd(ord - 1);
    copyTerm(target, (std::string_view)termsEnum->term());
    target.ord = ord;
    target.ordGen = currentSegment;
  }

  int compare(int32_t slotA, segdoc docA, FieldComparator& other,
              int32_t slotB, segdoc docB) override {
    unused(docA, docB);
    assert(dynamic_cast<SegmentOrdComparator*>(&other) != nullptr);
    auto* otherOrd = static_cast<SegmentOrdComparator*>(&other);
    assert(fieldName == otherOrd->fieldName);
    assert(slotA >= 0 && slotA < (int32_t)slots.size());
    assert(slotB >= 0 && slotB < (int32_t)otherOrd->slots.size());
    return sortMultiplier * compareSlots(slots[slotA], otherOrd->slots[slotB]);
  }

  void copy(int32_t slot, FieldComparator& other, int32_t otherSlot,
            segdoc otherDoc) override {
    unused(otherDoc);
    assert(dynamic_cast<SegmentOrdComparator*>(&other) != nullptr);
    auto* otherOrd = static_cast<SegmentOrdComparator*>(&other);
    assert(fieldName == otherOrd->fieldName);
    assert(slot >= 0 && slot < (int32_t)slots.size());
    assert(otherSlot >= 0 && otherSlot < (int32_t)otherOrd->slots.size());
    Slot& target = slots[slot];
    const Slot& source = otherOrd->slots[otherSlot];
    if (source.ord == 0) {
      setMissing(target);
      return;
    }
    copyTerm(target, otherOrd->term(source));
    target.ord = source.ord;
    target.ordGen = source.ordGen;
  }
};

// A comparator for non-indexed string columns that compares by value
class StrColComparator : public FieldComparator {
  std::string fieldName;
  std::optional<StrColReader> reader;
  std::optional<StrColReader::Iterator> iter;
  std::vector<std::string> values; // Storage for string values
  int sortMultiplier; // 1 for ascending, -1 for descending
  std::string missingValueStr;
  
public:
  StrColComparator(const std::string& fieldName, int numHits, bool reversed, MissingValue missingValue)
    : fieldName(fieldName),
      values(numHits),
      sortMultiplier(reversed ? -1 : 1) {
    
    // For missing values, use empty string for MISSING_FIRST in ASC (sorts before all others)
    // or a very high value string for MISSING_LAST in ASC
    if (missingValue == MISSING_FIRST) {
      missingValueStr = reversed ? "\xFF\xFF\xFF\xFF" : "";
    } else {
      missingValueStr = reversed ? "" : "\xFF\xFF\xFF\xFF";
    }
  }
  
  void setSegment(int32_t segment, PostingsReader* postingsReader) override {
    // Reset reader and iterator
    reader.reset();
    iter.reset();
    
    if (!postingsReader) return;
    
    auto poolGuard = MemPool::threadLocalPoolGuard();
    FieldReader fieldReader(*postingsReader);
    if (fieldReader.seek(fieldName)) {
      SegFieldInfo fieldInfo;
      fieldReader.readFieldInfo(fieldInfo);
      if (fieldInfo.columnLoc.offset() > 0) {
        reader.emplace(*postingsReader, fieldInfo);
        iter.emplace(*reader);
      }
    }
  }
  
  std::string_view getDocValue(int32_t docid) {
    if (!iter.has_value()) {
      return missingValueStr;
    }
    
    // Advance iterator to docid if needed
    if (iter->docId() < docid) {
      int32_t foundDoc = iter->advance(docid);
      if (foundDoc == StrColReader::Iterator::ENDDOC || foundDoc > docid) {
        return missingValueStr;
      }
    } else if (iter->docId() > docid) {
      return missingValueStr;
    }
    
    return iter->value();
  }
  
  int compare(int32_t slotA, segdoc docA, int32_t slotB, segdoc docB) override {
    assert(slotA >= 0 && slotA < (int32_t)values.size());
    assert(slotB >= 0 && slotB < (int32_t)values.size());
    const auto& valA = values[slotA];
    const auto& valB = values[slotB];
    int cmp = valA.compare(valB);
    return sortMultiplier * cmp;
  }
  
  // NOINLINE: see SimpleNumericFieldComparator::compareBottom.
  SOLUX_NOINLINE int compareBottom(int32_t bottomSlot, segdoc bottomDoc, segdoc newDoc) override {
    assert(bottomSlot >= 0 && bottomSlot < (int32_t)values.size());
    const auto& bottomVal = values[bottomSlot];
    std::string_view newVal = getDocValue(newDoc.docId());
    int cmp = bottomVal.compare(newVal);
    return sortMultiplier * cmp;
  }
  
  void copy(int32_t slot, segdoc doc) override {
    assert(slot >= 0 && slot < (int32_t)values.size());
    values[slot] = std::string(getDocValue(doc.docId()));
  }
  
  int compare(int32_t slotA, segdoc docA, FieldComparator& other, int32_t slotB, segdoc docB) override {
    assert(dynamic_cast<StrColComparator*>(&other) != nullptr);
    auto* otherStr = static_cast<StrColComparator*>(&other);
    
    assert(slotA >= 0 && slotA < (int32_t)values.size());
    assert(slotB >= 0 && slotB < (int32_t)otherStr->values.size());
    const auto& valA = values[slotA];
    const auto& valB = otherStr->values[slotB];
    int cmp = valA.compare(valB);
    return sortMultiplier * cmp;
  }
  
  void copy(int32_t slot, FieldComparator& other, int32_t otherSlot, segdoc otherDoc) override {
    assert(dynamic_cast<StrColComparator*>(&other) != nullptr);
    auto* otherStr = static_cast<StrColComparator*>(&other);
    
    assert(slot >= 0 && slot < (int32_t)values.size());
    assert(otherSlot >= 0 && otherSlot < (int32_t)otherStr->values.size());
    values[slot] = otherStr->values[otherSlot];
  }
  
  ~StrColComparator() override = default;
};

} // namespace solux
