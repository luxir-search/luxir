#pragma once

#include "solux/search/Collector.h"
#include "solux/reader/PostingsReader.h"
#include "solux/reader/FieldReader.h"
#include "solux/reader/IntColReader.h"
#include "solux/search/OrdMap.h"
#include "solux/util/MemPool.h"
#include <memory>
#include <vector>
#include <limits>
#include <cassert>
#include <optional>

namespace solux {

class FieldComparator {
public:
  virtual ~FieldComparator() = default;

  // Set the current segment for this comparator
  virtual void setSegment(int32_t segment, PostingsReader* reader) = 0;

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

  // Get a comparable value that can be used across segments
  // This should return a value that maintains the same ordering as compareSlots
  // For numeric fields, this would be the actual value (possibly negated for DESC)
  // For other fields, this could be a hash or ordinal that preserves ordering
  // Only used in FieldSortCollector2.
  virtual int64_t getComparableValue(int32_t slot) const { return 0; }

  enum MissingValue {
    MISSING_FIRST,
    MISSING_LAST
  };
};



// A comparator that can be used in a simplified collector that has values
// that can be compared across different segments.
class SimpleNumericFieldComparator : public FieldComparator {
  std::string fieldName;
  std::optional<IntColReader> reader;
  std::optional<IntColReader::Iterator> iter;
  std::vector<int64_t> values; // Storage for bottom values
  int64_t sortMultiplier; // 1 for ascending, -1 for descending
  int64_t missingValueSubstitute;
  int64_t lastValue = 0;  // TODO: cache the last value lookup in compareBottom so we don't have to re-fetch if competitive
  segdoc lastDoc = {-1, -1};

public:
  SimpleNumericFieldComparator(const std::string& fieldName, int numHits, bool reversed, MissingValue missingValue)
    : fieldName(fieldName),
      values(numHits),
      sortMultiplier(reversed ? -1 : 1) {
    
    missingValueSubstitute = (missingValue == MISSING_FIRST)
                               ? std::numeric_limits<int64_t>::min()
                               : std::numeric_limits<int64_t>::max();

    if (reversed) {
      missingValueSubstitute = -(missingValueSubstitute + 1);
      // +1 handles inability to negate INT64_MIN:
      //   MAX + 1 overflows to MIN, which stays min when negated
      //   MIN + 1 when negated becomes MAX
    }
  }
  
  void setSegment(int32_t segment, PostingsReader* postingsReader) override {
    // Reset reader and iterator
    reader.reset();
    iter.reset();
    
    if (!postingsReader) return;
    
    auto poolGuard = MemPool::threadLocalPoolGuard();
    FieldReader fieldReader(poolGuard.pool(), *postingsReader);
    if (fieldReader.seek(fieldName)) {
      SegFieldInfo fieldInfo;
      fieldReader.readFieldInfo(fieldInfo);
      if (fieldInfo.columnLoc.offset() > 0) {
        reader.emplace(*postingsReader, fieldInfo);
        iter.emplace(*reader);
      }
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
    int64_t value = iter->value();
    // For descending sort, negate the value.
    // if we wanted to make this branchless, we could have a an adder and multiplier
    // but this will be a predictable branch anyway.
    if (sortMultiplier < 0) {
      // This will shift the space to correctly negate both INT64_MIN to INT64_MAX
      value = -(value + 1);
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
  
  int compareBottom(int32_t bottomSlot, segdoc bottomDoc, segdoc newDoc) override {
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
  
  int64_t getComparableValue(int32_t slot) const override {
    // For numeric comparator, return the stored value
    assert(slot >= 0 && slot < (int32_t)values.size());
    return values[slot];
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
  // Accessing the ord for a document will be in-order and should use a bulk iterator (which IntColReader::Iterator is)
  // when the domain consists of many documents.
  // Accessing the deltas to convert to a global ord will not be in-order, and bulk iterator should not be used.
  std::optional<IntColReader> reader;
  std::optional<IntColReader::Iterator> iter;
  std::vector<int64_t> globalOrds; // Storage for global ordinals
  MonoReader* segmentDeltas = nullptr; // Current segment's ord mapping
  int64_t segmentNumOrds = 0;
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
    // Combined with ASC/DESC to determine the actual comparison value
    if (missingValue == MISSING_FIRST) {
      // Missing sorts first
      missingOrd = reversed ? std::numeric_limits<int64_t>::max() 
                            : std::numeric_limits<int64_t>::min();
    } else {
      // Missing sorts last  
      missingOrd = reversed ? std::numeric_limits<int64_t>::min()
                            : std::numeric_limits<int64_t>::max();
    }
  }
  
  void setSegment(int32_t segment, PostingsReader* postingsReader) override {
    // Reset reader and iterator
    reader.reset();
    iter.reset();
    segmentDeltas = nullptr;
    segmentNumOrds = 0;
    
    if (!postingsReader) return;
    
    // Get segment-to-global ordinal mapping from OrdMap
    if (ordMap) {
      auto segToGlobal = ordMap->getSegToGlobal(segment);
      segmentNumOrds = segToGlobal.numOrds;
      segmentDeltas = segToGlobal.deltas;
    }
    
    // Load the ordinal column reader for this segment
    auto poolGuard = MemPool::threadLocalPoolGuard();
    FieldReader fieldReader(poolGuard.pool(), *postingsReader);
    if (fieldReader.seek(fieldName)) {
      SegFieldInfo fieldInfo;
      fieldReader.readFieldInfo(fieldInfo);
      if (fieldInfo.columnLoc.offset() > 0) {
        reader.emplace(*postingsReader, fieldInfo);
        iter.emplace(*reader);
      }
    }
  }
  
  int64_t getGlobalOrd(int32_t docid) {
    if (!iter.has_value()) {
      return missingOrd;
    }

    if (iter->docId() < docid) {
      iter->advance(docid);
    }
    if (iter->docId() > docid) {
      return missingOrd;
    }
    
    int64_t segmentOrd = iter->value();
    if (segmentOrd == 0) {
      // Ordinal 0 means missing value
      return missingOrd;
    }
    
    // Convert segment ordinal to global ordinal
    int64_t globalOrd = segmentOrd;
    if (segmentDeltas) {
      // Apply delta to get global ordinal
      // Note: segmentOrd is 1-based, but deltas array is 0-based
      globalOrd = segmentOrd + segmentDeltas->valueAt(segmentOrd - 1);
    }
    // If no deltas, either single segment or this segment has all terms (identity mapping)
    
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
  
  int compareBottom(int32_t bottomSlot, segdoc bottomDoc, segdoc newDoc) override {
    assert(bottomSlot >= 0 && bottomSlot < (int32_t)globalOrds.size());
    int64_t bottomOrd = globalOrds[bottomSlot];
    int64_t newOrd = getGlobalOrd(newDoc.docId());
    return (bottomOrd > newOrd) - (bottomOrd < newOrd);
  }
  
  void copy(int32_t slot, segdoc doc) override {
    assert(slot >= 0 && slot < (int32_t)globalOrds.size());
    globalOrds[slot] = getGlobalOrd(doc.docId());
  }
  
  int64_t getComparableValue(int32_t slot) const override {
    assert(slot >= 0 && slot < (int32_t)globalOrds.size());
    return globalOrds[slot];
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

} // namespace solux
