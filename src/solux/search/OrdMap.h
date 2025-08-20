#pragma once
#include <memory>
#include <vector>

#include "IndexReader.h"
#include "solux/reader/IntColReader.h"

namespace solux {

class IndexReader;

/// An OrdMap can map between segment ordinals and global ordinals.
/// This is used for fast sorting and faceting across multiple segments.
class OrdMap {
public:
  struct SegToGlobal {
    int64_t numOrds;
    MonoReader* deltas; // null if segment had no values for the field, or if it has all of the values
  };

private:
  struct SegToGlobalHolder {
    int64_t numOrds;
    std::unique_ptr<MonoReader> deltas;

    SegToGlobal get() const {
      return {numOrds, deltas.get()};
    }
  };


  std::unique_ptr<char[]> data; // the raw data for the OrdMap
  int64_t start;
  int64_t end;

  int64_t nOrds;
  int firstFull = -1; // first segment that has all the ords, or -1 if none
  std::vector<SegToGlobalHolder> segToGlobal;    // per-segment mapping from segment ord to global ord
  std::optional<IntColReader> firstSegs;  // for each global ord, what is the first segment it appeared in
  std::optional<IntColReader> globDeltas; // for each global ord, what delta was applied to the segment ord to get the global ord

public:
  // Constructor for single segment with values case (identity mapping)
  OrdMap(int64_t numOrds, int segmentWithValues) 
    : data(nullptr), start(0), end(0), nOrds(numOrds), firstFull(segmentWithValues) {
    // Keep segToGlobal empty - getSegToGlobal will return default values
    // No global columns needed since one segment has all terms
  }
  
  OrdMap(std::unique_ptr<char[]>&& data, int64_t start, int64_t end) : data(std::move(data)), start(start), end(end) {
    InputStream dataIS(this->data.get() + start, this->data.get() + start + end);

    // Read size of metadata to we can skip to the start of it
    // See OrdMapBuilder in OrdMapImpl.h for the layout of the data.
    dataIS.seek(end - sizeof(int32_t));
    auto metaSize = dataIS.readInt();

    dataIS.seek(end - sizeof(int32_t) - metaSize);
    nOrds = dataIS.readVlong();
    auto nSegs = dataIS.readVlong();
    segToGlobal.reserve(nSegs);
    for (auto i = 0u; i < nSegs; i++) {
      auto nValues = dataIS.readVlong();

      if (nValues > 0 && nValues != (uint64_t)nOrds) {
        auto loc = dataIS.readVlong();
        auto metaOff = dataIS.readVlong();
        segToGlobal.emplace_back((int64_t)nValues,
          std::make_unique<MonoReader>(dataIS, start + loc, metaOff, nValues));
      } else {
        if (nValues == (uint64_t)nOrds && firstFull == -1) {
          firstFull = (int)i;
        }
        segToGlobal.emplace_back((int64_t)nValues, nullptr);
      }
    }

    // now the global columns
    auto nValues = dataIS.readVlong();
    if (nValues > 0 && firstFull == -1) {  // Create global columns if no segment has all terms
      auto loc = dataIS.readVlong();
      auto metaOff = dataIS.readVlong();
      firstSegs.emplace(dataIS, start + loc, metaOff, nValues);
    }

    nValues = dataIS.readVlong();
    if (nValues > 0 && firstFull == -1) {  // Create global columns if no segment has all terms
      auto loc = dataIS.readVlong();
      auto metaOff = dataIS.readVlong();
      globDeltas.emplace(dataIS, start + loc, metaOff, nValues);
    }
  }

  /// Build an OrdMap for the given field across all segments in the reader.
  static std::shared_ptr<OrdMap> build(std::string_view field, IndexReader& reader);
  
  /// Get the total number of unique terms across all segments
  int64_t numOrds() const { return nOrds; }

  /// Returns -1, or the first segment that has all the ords.
  int firstFullSeg() const { return firstFull; }

  /// Gets the mapping from segment ord to global ord, or null if the segment had no or all values for the field.
  SegToGlobal getSegToGlobal(int seg) const {
    // If segToGlobal is empty (single segment with values case), return default
    if (segToGlobal.empty()) {
      return {0, nullptr};
    }
    assert(seg >= 0 && seg < (int)segToGlobal.size());
    return segToGlobal[seg].get();
  }

  /// Returns the firstSegs IntColReader, or null if not present/needed since at least one segment had all the ords.
  IntColReader* getFirstSegs() {
    return firstSegs ? &(*firstSegs) : nullptr;
  }

  /// Returns the globDeltas IntColReader, or null if not present/needed since at least one segment had all the ords.
  IntColReader* getGlobDeltas() {
    return globDeltas ? &(*globDeltas) : nullptr;
  }

  size_t sizeInBytes() {
    return end - start;
  }
  // TODO: create some convenience mapping classes to handle all the "null" and full-segment edge cases.

};

}
