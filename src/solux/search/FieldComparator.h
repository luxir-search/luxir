#pragma once

#include "solux/search/Collector.h"
#include "solux/reader/PostingsReader.h"
#include "solux/reader/FieldReader.h"
#include "solux/reader/IntColReader.h"
#include "solux/reader/OrdColReader.h"
#include "solux/reader/StrColReader.h"
#include "solux/search/OrdMap.h"
#include "solux/util/MemPool.h"
#include "solux/util/solux_util.h"
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
      // ~x == -(x+1) but without the signed-overflow UB: -(INT64_MAX+1) and
      // negating INT64_MIN both overflow.  ~x is an order-reversing bijection
      // over the whole int64 range, exactly the descending transform we want.
      missingValueSubstitute = ~missingValueSubstitute;
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
