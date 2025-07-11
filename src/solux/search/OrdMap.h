#pragma once
#include <memory>
#include <vector>
#include <cstdint>

#include "solux/reader/IntColReader.h"

namespace solux {

class IndexReader;

/// An OrdMap can map between segment ordinals and global ordinals.
/// This is used for fast sorting and faceting across multiple segments.
class OrdMap {
  std::unique_ptr<char[]> data; // the raw data for the OrdMap

  int64_t nords;
  std::vector<std::unique_ptr<MonoReader>> segToGlobal;    // per-segment mapping from segment ord to global ord
  std::optional<IntColReader> firstSegs;  // for each global ord, what is the first segment it appeared in
  std::optional<IntColReader> globDeltas; // for each global ord, what delta was applied to the segment ord to get the global ord

public:
  OrdMap(std::unique_ptr<char[]>&& data, int64_t start, int64_t end) : data(std::move(data)) {
    InputStream dataIS(this->data.get() + start, this->data.get() + start + end);

    // Read size of metadata to we can skip to the start of it
    // See OrdMapBuilder in OrdMapImpl.h for the layout of the data.
    dataIS.seek(end - sizeof(int32_t));
    auto metaSize = dataIS.readInt();

    dataIS.seek(end - sizeof(int32_t) - metaSize);
    nords = dataIS.readVlong();
    auto nSegs = dataIS.readVlong();
    segToGlobal.reserve(nSegs);
    for (auto i = 0u; i < nSegs; i++) {
      auto nValues = dataIS.readVlong();
      auto loc = dataIS.readVlong();
      auto metaOff = dataIS.readVlong();
      if (nValues > 0) {
        segToGlobal.emplace_back(std::make_unique<MonoReader>(dataIS, start + loc, metaOff, nValues));
      } else {
        segToGlobal.emplace_back(nullptr);
      }
    }

    // now the global columns
    auto nValues = dataIS.readVlong();
    if (nValues > 0) {
      auto loc = dataIS.readVlong();
      auto metaOff = dataIS.readVlong();
      firstSegs.emplace(dataIS, start + loc, metaOff);
    }

    nValues = dataIS.readVlong();
    if (nValues > 0) {
      auto loc = dataIS.readVlong();
      auto metaOff = dataIS.readVlong();
      globDeltas.emplace(dataIS, start + loc, metaOff);
    }
  }

  /// Build an OrdMap for the given field across all segments in the reader.
  static std::shared_ptr<OrdMap> build(std::string_view field, IndexReader& reader);
  
  /// Get the total number of unique terms across all segments
  int64_t numOrds() const { return nords; }
};

}
