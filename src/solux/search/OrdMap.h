#pragma once
#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

#include "IndexReader.h"
#include "solux/codec/LinearPack.h"
#include "solux/codec/OrdColumnFormat.h"
#include "solux/reader/IntColReader.h"

namespace solux {

class IndexReader;

/// An OrdMap can map between segment ordinals and global ordinals.
/// This is used for fast sorting and faceting across multiple segments.
class OrdMap {
public:
  enum class DeltaEncoding : uint8_t {
    DEFAULT,
    FLAT,
    PREDICTED
  };

  struct SegToGlobal {
    int64_t numOrds = 0;
    const char* deltas = nullptr;
    const char* blockMeta = nullptr;
    uint64_t mask = 0;
    uint8_t bits = 0;
    uint8_t residualBits = 0;
    DeltaEncoding encoding = DeltaEncoding::FLAT;

    bool predicted() const noexcept {
      return bits != 0 && encoding == DeltaEncoding::PREDICTED;
    }

    int64_t deltaAt(int64_t segmentOrd) const {
      assert(segmentOrd >= 0 && segmentOrd < numOrds);
      if (bits == 0) return 0;
      if (encoding == DeltaEncoding::FLAT) {
        return (int64_t)LinearPack::select64(
            deltas, (uint64_t)segmentOrd, bits, mask);
      }

      assert(encoding == DeltaEncoding::PREDICTED);
      uint64_t block = (uint64_t)segmentOrd / OrdColumnFormat::BLOCK_SIZE;
      uint32_t rankInBlock =
          (uint32_t)((uint64_t)segmentOrd % OrdColumnFormat::BLOCK_SIZE);
      OrdColumnFormat::PredictedBlockInfo info;
      memcpy(&info, blockMeta + block * sizeof(info), sizeof(info));
      uint64_t residual = LinearPack::select64(
          deltas + info.payloadOffset, rankInBlock, info.bits,
          LinearPack::mask64(info.bits));
      int64_t delta = OrdColumnFormat::predict(info, rankInBlock) +
                      (int64_t)residual;
      assert(delta >= 0 && (uint64_t)delta <= LinearPack::mask64(bits));
      return delta;
    }

    int64_t globalOrd(int64_t segmentOrd) const {
      return segmentOrd + deltaAt(segmentOrd);
    }

    void unpackDeltas(uint64_t start, uint32_t count, uint64_t* values) const {
      assert(start + count <= (uint64_t)numOrds);
      assert(count <= OrdColumnFormat::BULK_SIZE);
      if (count == 0) return;
      if (bits == 0) {
        std::fill_n(values, count, 0);
        return;
      }
      if (encoding == DeltaEncoding::FLAT) {
        LinearPack::unpack128(deltas, start, count, bits, mask, values);
        return;
      }

      assert(encoding == DeltaEncoding::PREDICTED);
      uint64_t block = start / OrdColumnFormat::BLOCK_SIZE;
      uint32_t rankInBlock = (uint32_t)(start % OrdColumnFormat::BLOCK_SIZE);
      assert(rankInBlock + count <= OrdColumnFormat::BLOCK_SIZE);
      OrdColumnFormat::PredictedBlockInfo info;
      memcpy(&info, blockMeta + block * sizeof(info), sizeof(info));
      LinearPack::unpack128(deltas + info.payloadOffset, rankInBlock, count,
                            info.bits, LinearPack::mask64(info.bits), values);
      for (uint32_t i = 0; i < count; i++) {
        __int128 delta = (__int128)OrdColumnFormat::predict(
            info, rankInBlock + i) + values[i];
        assert(delta >= 0 && delta <= std::numeric_limits<int64_t>::max());
        values[i] = (uint64_t)delta;
        assert(values[i] <= LinearPack::mask64(bits));
      }
    }
  };

private:
  inline static std::atomic<DeltaEncoding> deltaEncodingOverride =
      DeltaEncoding::DEFAULT;
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
        if (bits == 0) {
          segToGlobal.push_back({(int64_t)nValues});
          continue;
        }

        auto encoding = (DeltaEncoding)dataIS.readVint();
        assert(encoding == DeltaEncoding::FLAT ||
               encoding == DeltaEncoding::PREDICTED);
        uint64_t loc = dataIS.readVlong();
        const char* deltas = this->data.get() + start + loc;
        if (encoding == DeltaEncoding::FLAT) {
          SegToGlobal mapping;
          mapping.numOrds = (int64_t)nValues;
          mapping.deltas = deltas;
          mapping.mask = LinearPack::mask64(bits);
          mapping.bits = bits;
          mapping.residualBits = bits;
          mapping.encoding = encoding;
          segToGlobal.push_back(mapping);
        } else {
          uint64_t blockMetaLoc = dataIS.readVlong();
          uint8_t residualBits = (uint8_t)dataIS.readVint();
          assert(residualBits <= 57);
          SegToGlobal mapping;
          mapping.numOrds = (int64_t)nValues;
          mapping.deltas = deltas;
          mapping.blockMeta = this->data.get() + start + blockMetaLoc;
          mapping.bits = bits;
          mapping.residualBits = residualBits;
          mapping.encoding = encoding;
          segToGlobal.push_back(mapping);
        }
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

  static DeltaEncoding configuredDeltaEncoding();

  static DeltaEncoding setDeltaEncodingForTests(DeltaEncoding value) {
    return deltaEncodingOverride.exchange(value);
  }
  
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
