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

  ~NumericFieldComparator() {
    for (auto& segReader : segmentReaders) {
      delete segReader.reader;
    }
  }
};

} // namespace solux
