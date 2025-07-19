#pragma once

#include "solux/search/Collector.h"
#include "solux/reader/PostingsReader.h"
#include "solux/reader/FieldReader.h"
#include "solux/reader/IntColReader.h"
#include "solux/util/MemPool.h"
#include <memory>
#include <vector>
#include <limits>

namespace solux {

class FieldComparator {
public:
  virtual ~FieldComparator() = default;

  virtual void setSegment(int32_t segment, PostingsReader* reader) = 0;

  virtual int compare(int32_t docA, int32_t segA, int32_t docB, int32_t segB) = 0;

  virtual int compareBottom(int32_t doc, int32_t segment) = 0;

  virtual void setBottom(int32_t slot) = 0;

  virtual void copy(int32_t slot, int32_t doc, int32_t segment) = 0;
  
  virtual bool isReversed() const { return false; }
  
  virtual int64_t getValue(int32_t slot) const { return 0; }
  
  // Get the sort value for a document in the current segment
  // The value should already be adjusted for sort order (negated if DESC)
  virtual int64_t getDocValue(int32_t docid) { return 0; }

  enum MissingValue {
    MISSING_FIRST,
    MISSING_LAST
  };
};

class NumericFieldComparator : public FieldComparator {
private:
  struct SegmentReader {
    IntColReader* reader = nullptr;
    bool hasValues = false;
  };

  std::string fieldName;
  std::vector<SegmentReader> segmentReaders;
  std::vector<int64_t> values;
  int64_t bottomValue = 0;
  int bottomSlot = -1;
  bool reversed;
  MissingValue missingValue;
  int64_t missingValueSubstitute;

  int64_t getValueSafe(int32_t doc, int32_t segment) {
    if (segment < 0 || segment >= (int32_t)segmentReaders.size()) {
      return missingValueSubstitute;
    }
    auto& segReader = segmentReaders[segment];
    if (!segReader.hasValues || !segReader.reader) {
      return missingValueSubstitute;
    }

    // Use the iterator to find if this doc has a value
    IntColReader::Iterator iter(*segReader.reader);
    int32_t foundDoc = iter.advance(doc);

    if (foundDoc != doc) {
      // Document doesn't have this field
      return missingValueSubstitute;
    }

    // Get the value for this document
    return iter.value();
  }

public:
  NumericFieldComparator(const std::string& fieldName, int numHits, bool reversed, MissingValue missingValue)
    : fieldName(fieldName),
      values(numHits),
      reversed(reversed),
      missingValue(missingValue) {

    missingValueSubstitute = (missingValue == MISSING_FIRST)
                               ? std::numeric_limits<int64_t>::min()
                               : std::numeric_limits<int64_t>::max();

    if (reversed) {
      missingValueSubstitute = -missingValueSubstitute;
    }
  }

  void setSegment(int32_t segment, PostingsReader* reader) override {
    if (segment >= (int32_t)segmentReaders.size()) {
      segmentReaders.resize(segment + 1);
    }

    auto& segReader = segmentReaders[segment];
    segReader.reader = nullptr;
    segReader.hasValues = false;

    if (!reader) return;

    auto poolGuard = MemPool::threadLocalPoolGuard();
    FieldReader fieldReader(poolGuard.pool(), *reader);
    if (fieldReader.seek(fieldName)) {
      SegFieldInfo fieldInfo;
      fieldReader.readFieldInfo(fieldInfo);
      if (fieldInfo.columnLoc.offset() > 0) {
        segReader.reader = new IntColReader(*reader, fieldInfo);
        segReader.hasValues = true;
      }
    }
  }

  int compare(int32_t docA, int32_t segA, int32_t docB, int32_t segB) override {
    int64_t valA = getValueSafe(docA, segA);
    int64_t valB = getValueSafe(docB, segB);

    if (reversed) {
      valA = -valA;
      valB = -valB;
    }

    return (valA > valB) - (valA < valB);
  }

  int compareBottom(int32_t doc, int32_t segment) override {
    int64_t val = getValueSafe(doc, segment);
    if (reversed) {
      val = -val;
    }
    return (val > bottomValue) - (val < bottomValue);
  }

  void setBottom(int32_t slot) override {
    bottomSlot = slot;
    bottomValue = values[slot];
  }

  void copy(int32_t slot, int32_t doc, int32_t segment) override {
    int64_t val = getValueSafe(doc, segment);
    if (reversed) {
      val = -val;
    }
    values[slot] = val;
  }
  
  bool isReversed() const override {
    return reversed;
  }
  
  int64_t getValue(int32_t slot) const override {
    if (slot >= 0 && slot < (int32_t)values.size()) {
      return values[slot];
    }
    return 0;
  }
  
  int64_t getDocValue(int32_t docid) override {
    // This implementation still requires segment to be passed
    // For compatibility only - use SimpleNumericFieldComparator instead
    return 0;
  }

  ~NumericFieldComparator() {
    for (auto& segReader : segmentReaders) {
      delete segReader.reader;
    }
  }
};

// Simplified comparator that only works with the current segment
class SimpleNumericFieldComparator : public FieldComparator {
private:
  std::string fieldName;
  IntColReader* reader = nullptr;
  std::unique_ptr<IntColReader::Iterator> iter;
  std::vector<int64_t> values; // Storage for bottom values
  int64_t bottomValue = 0;
  bool reversed;
  int64_t missingValueSubstitute;
  int32_t currentSegment = -1;
  
public:
  SimpleNumericFieldComparator(const std::string& fieldName, int numHits, bool reversed, MissingValue missingValue)
    : fieldName(fieldName),
      values(numHits),
      reversed(reversed) {
    
    missingValueSubstitute = (missingValue == MISSING_FIRST)
                               ? std::numeric_limits<int64_t>::min()
                               : std::numeric_limits<int64_t>::max();
    
    if (reversed) {
      missingValueSubstitute = -missingValueSubstitute;
    }
  }
  
  void setSegment(int32_t segment, PostingsReader* postingsReader) override {
    currentSegment = segment;
    
    // Clean up old reader and iterator
    delete reader;
    reader = nullptr;
    iter.reset();
    
    if (!postingsReader) return;
    
    auto poolGuard = MemPool::threadLocalPoolGuard();
    FieldReader fieldReader(poolGuard.pool(), *postingsReader);
    if (fieldReader.seek(fieldName)) {
      SegFieldInfo fieldInfo;
      fieldReader.readFieldInfo(fieldInfo);
      if (fieldInfo.columnLoc.offset() > 0) {
        reader = new IntColReader(*postingsReader, fieldInfo);
        iter = std::make_unique<IntColReader::Iterator>(*reader);
      }
    }
  }
  
  int64_t getDocValue(int32_t docid) override {
    if (!iter) {
      // std::cout << "  getDocValue(" << docid << ") - no reader, returning " << missingValueSubstitute << "\n";
      return missingValueSubstitute;
    }

    if (iter->docId() < docid) {
      iter->advance(docid);
    }
    if (iter->docId() > docid) {
      // std::cout << "  getDocValue(" << docid << ") - doc not found, returning " << missingValueSubstitute << "\n";
      return missingValueSubstitute;
    }
    int64_t value = iter->value();
    int64_t result = reversed ? -value : value;
    // std::cout << "  getDocValue(" << docid << ") - found value=" << value << " returning " << result << "\n";
    return result;
  }
  
  // These methods now only work with current segment docs
  int compare(int32_t docA, int32_t segA, int32_t docB, int32_t segB) override {
    // This should only be called for same segment
    assert(segA == currentSegment && segB == currentSegment);
    int64_t valA = getDocValue(docA);
    int64_t valB = getDocValue(docB);
    return (valA > valB) - (valA < valB);
  }
  
  int compareBottom(int32_t doc, int32_t segment) override {
    assert(segment == currentSegment);
    int64_t val = getDocValue(doc);
    return (val > bottomValue) - (val < bottomValue);
  }
  
  void setBottom(int32_t slot) override {
    if (slot >= 0 && slot < (int32_t)values.size()) {
      bottomValue = values[slot];
    }
  }
  
  void copy(int32_t slot, int32_t doc, int32_t segment) override {
    assert(segment == currentSegment);
    if (slot >= 0 && slot < (int32_t)values.size()) {
      values[slot] = getDocValue(doc);
    }
  }
  
  bool isReversed() const override {
    return reversed;
  }
  
  int64_t getValue(int32_t slot) const override {
    if (slot >= 0 && slot < (int32_t)values.size()) {
      return values[slot];
    }
    return 0;
  }
  
  ~SimpleNumericFieldComparator() override {
    delete reader;
  }
};

} // namespace solux
