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

  // Set the current segment for this comparator
  virtual void setSegment(int32_t segment, PostingsReader* reader) = 0;

  // Compare two documents in the current segment
  virtual int compare(int32_t docA, int32_t docB) = 0;

  // Compare a document in the current segment to the bottom value
  virtual int compareBottom(int32_t doc) = 0;

  // Set the bottom slot for comparison
  virtual void setBottom(int32_t slot) = 0;

  // Copy the value from a document in the current segment to a slot
  virtual void copy(int32_t slot, int32_t doc) = 0;
  
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



// A comparator that can be used in a simplified collector that has values
// that can be compared across different segments.
class SimpleNumericFieldComparator : public FieldComparator {
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
      // For descending sort, we need to negate values for comparison.
      // However, negating INT64_MIN results in INT64_MIN due to two's complement overflow.
      // To handle this, we use INT64_MAX for MISSING_FIRST and INT64_MIN+1 for MISSING_LAST.
      if (missingValue == MISSING_FIRST) {
        missingValueSubstitute = std::numeric_limits<int64_t>::max();
      } else {
        missingValueSubstitute = std::numeric_limits<int64_t>::min() + 1;
      }
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
    // For descending sort, negate the value. Special case for INT64_MIN.
    if (reversed) {
      if (value == std::numeric_limits<int64_t>::min()) {
        // Can't negate INT64_MIN, so use INT64_MAX instead
        value = std::numeric_limits<int64_t>::max();
      } else {
        value = -value;
      }
    }
    // std::cout << "  getDocValue(" << docid << ") - found value=" << value << " returning " << value << "\n";
    return value;
  }
  
  int compare(int32_t docA, int32_t docB) override {
    int64_t valA = getDocValue(docA);
    int64_t valB = getDocValue(docB);
    return (valA > valB) - (valA < valB);
  }
  
  int compareBottom(int32_t doc) override {
    int64_t val = getDocValue(doc);
    return (val > bottomValue) - (val < bottomValue);
  }
  
  void setBottom(int32_t slot) override {
    if (slot >= 0 && slot < (int32_t)values.size()) {
      bottomValue = values[slot];
    }
  }
  
  void copy(int32_t slot, int32_t doc) override {
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
