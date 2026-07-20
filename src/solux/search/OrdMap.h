#pragma once
#include <memory>
#include <vector>

#include "IndexReader.h"
#include "solux/codec/LinearPack.h"
#include "solux/reader/IntColReader.h"

namespace solux {

class IndexReader;

/// An OrdMap can map between segment ordinals and global ordinals.
/// This is used for fast sorting and faceting across multiple segments.
class OrdMap {
public:
  struct SegToGlobal {
    int64_t numOrds = 0;
    const char* deltas = nullptr;
    uint64_t mask = 0;
    uint8_t bits = 0;

    int64_t deltaAt(int64_t segmentOrd) const {
      assert(segmentOrd >= 0 && segmentOrd < numOrds);
      return (int64_t)LinearPack::select64(
          deltas, (uint64_t)segmentOrd, bits, mask);
    }

    int64_t globalOrd(int64_t segmentOrd) const {
      return segmentOrd + deltaAt(segmentOrd);
    }

    void unpackDeltas(uint64_t start, uint32_t count, uint64_t* values) const {
      assert(start + count <= (uint64_t)numOrds);
      LinearPack::unpack128(deltas, start, count, bits, mask, values);
    }
  };

private:
  std::unique_ptr<char[]> data; // the raw data for the OrdMap
  int64_t start;
  int64_t end;

  int64_t nOrds;
  int firstFull = -1; // first segment that has all the ords, or -1 if none
  std::vector<SegToGlobal> segToGlobal;    // per-segment mapping from segment ord to global ord
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
        uint8_t bits = (uint8_t)dataIS.readVint();
        assert(bits <= 57);
        uint64_t loc = bits == 0 ? 0 : dataIS.readVlong();
        const char* deltas = bits == 0 ? nullptr : this->data.get() + start + loc;
        segToGlobal.push_back({(int64_t)nValues, deltas,
                               LinearPack::mask64(bits), bits});
      } else {
        if (nValues == (uint64_t)nOrds && firstFull == -1) {
          firstFull = (int)i;
        }
        segToGlobal.push_back({(int64_t)nValues, nullptr, 0, 0});
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

  /// Gets the mapping from segment ord to global ord. bits == 0 is identity.
  SegToGlobal getSegToGlobal(int seg) const {
    // If segToGlobal is empty, only firstFull has values and its mapping is
    // identity.
    if (segToGlobal.empty()) {
      return seg == firstFull ? SegToGlobal{nOrds} : SegToGlobal{};
    }
    assert(seg >= 0 && seg < (int)segToGlobal.size());
    return segToGlobal[seg];
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
