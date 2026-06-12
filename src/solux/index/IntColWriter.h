#pragma once

#include "PostingsWriter.h"
#include "solux/reader/IntColReader.h"

namespace solux {

//
// Integer column writing
// TODO: currently all values must be written before all docs!  Decouple this so we can write columns incrementally?
// Wait until we implement deleted docs (and hence we need to know the docids as we write the values) before thinking
// about how to refactor this.
// To increase locality and decrease seeks, we could ensure that docsWithField always immediately follow the values.
// We could use an OutputStream that writes to RAM or even a MemPool implementation (or OutputStream writing to MemPool)
//


class IntColWriter {
public:
  constexpr static uint32_t BLOCK_SIZE = Postings::NUMERIC_BLOCK_SIZE;

private:
  OutputStream& colOutput;
  size_t colStart;
  int64_t nAdded = 0;
  int64_t overallMin = std::numeric_limits<int64_t>::max();
  int64_t overallMax = std::numeric_limits<int64_t>::min();

  std::vector<IntColReader::NumericBlockInfo> blockInfo;
  std::vector<int64_t> values;
  std::vector<int32_t> ivalues;

public:
  IntColWriter(OutputStream& target) : colOutput(target) {
    colStart = colOutput.size();
  }

  void addInt64(int64_t val) {
    nAdded++;
    values.push_back(val);
    // values[nAdded % BLOCK_SIZE] = val;
    // if (nAdded % BLOCK_SIZE == 0) {
    if (values.size() == BLOCK_SIZE) {
      addBlock(values);
      values.resize(0);
    }
  }

  void addBlock(std::span<int64_t> arr) {
    int64_t min = arr[0];
    int64_t max = arr[0];
    for (size_t i=1; i<arr.size(); i++) {
      min = std::min(min, arr[i]);
      max = std::max(max, arr[i]);
    }

    // Track overall min/max across all blocks
    overallMin = std::min(overallMin, min);
    overallMax = std::max(overallMax, max);

    // gcd of the deltas from min, computed in uint64: the deltas are exact
    // for any int64 pair (a block's range can exceed int64, e.g. sortable
    // double bit patterns near both extremes), std::gcd on signed values is
    // UB for INT64_MIN, and gcd-of-deltas >= gcd-of-values so compression
    // only improves.  The decode formula (delta * gcd + min) only needs the
    // gcd to divide the deltas.
    uint64_t gcd = 0;
    for (auto v : arr) {
      // If gcd hits 1 it can't change from that.  If it's 0, it could still go up.
      if (gcd == 1) {
        break;
      }
      gcd = std::gcd(gcd, uint64_t(v) - uint64_t(min));
    }
    if (gcd == 0) {
      gcd = 1;  // all values equal... we can't divide by 0 though.
    }

    // number of bits needed to represent the deltas after dividing by the gcd
    uint64_t range = (uint64_t(max) - uint64_t(min)) / gcd;
    uint32_t bits = std::bit_width(range);
    colOutput.align(4); // just a guess for now... we should really test.
    // the original SIMD code wrote length, min, max (which is 12 bytes, only 4 byte aligned when the SIMD magic starts happening)
    int64_t off = colOutput.size() - colStart;
    blockInfo.push_back({(int64_t)gcd, min, max, bits, off});

    if (bits > 32) {
      // we can't handle this case yet
      // throw std::runtime_error(std::format("IntColWriter: block range too large in field {}, min={}, max={}", (std::string_view)fieldInfo.fieldname, min, max));
      // for now, just write the values uncompressed to get things working.
      colOutput.write((const char*)arr.data(), arr.size() * sizeof(int64_t));
      return;
    }

    // Currently or For codec can only handle 32 bit integers, so we need to make a copy.
    // We should make a version that can accept 64 bit integers that just use the lower half.
    ivalues.reserve(arr.size());
    ivalues.resize(0);
    for (auto v : arr) {
      // The delta is an unsigned 32-bit quantity (bits <= 32 covers the full
      // uint32 range); the codec and readers consume it as uint32 and
      // zero-extend before applying gcd/min, so keep the unsigned bit pattern.
      uint32_t delta = (uint32_t)((uint64_t(v) - uint64_t(min)) / gcd);
      ivalues.push_back((int32_t)delta);
      assert(int64_t(uint64_t(delta) * gcd + uint64_t(min)) == v);
    }

    if (false && bits > 28) {  // future... need to manage our own metadata (bits,min) for this to work on reading side.
      // If too many bits are needed, simply copy the values to the output stream.  For something like hashes that
      // normally consume the whole range, we don't want to get lucky and save 1 bit in just 1 block since that
      // will preclude optimizations on decode.
      for (size_t i=0; i<arr.size(); i++) {
        colOutput.write((const char*)ivalues.data(), ivalues.size() * sizeof(int32_t));
      }
    } else {
      // TODO: figure out how much buffer space we actually need.  For codec writes two extra 32 bit values (min and max)
      // and I don't think any more space is needed.
      std::vector<char> compressed_output(ivalues.size() * sizeof(int32_t) + 32);
      uint32_t compressedSize = compressed_output.size(); // this gets changed to the actual size
      // Postings::numericCodec.encodeBlock((uint32_t*)ivalues.data(), ivalues.size(), compressed_output.data(), compressedSize);
      IndexCodec::numericCodec.encodeWithMeta((uint32_t*)ivalues.data(), ivalues.size(), compressed_output.data(), compressedSize, 0, bits);
      //   void encodeWithMeta(uint32_t* in, uint32_t inSz, char* target, uint32_t &outSz, uint32_t minval, uint8_t bits) {
      /*
      if (compressedSize > ivalues.size() * sizeof(int32_t) + 32) {
        // debugging check.... something is causing an issue (Heap-buffer-overflow)
        LOG_ERROR("IntColWriter: compressed size too large in field {}, num={}, min={}, max={}, csize={}", (std::string_view)fieldInfo.fieldname, ivalues.size(), min, max, compressedSize);
      }
      */

      colOutput.write(compressed_output.data(), compressedSize);
      // SIMDFor implementation can read up to 31 extra bytes after the end of compressedSize.
      // In this case we are fine because we write extra info after the last block (like BlockInfo array) which
      // is always larger than that.  See SoluxSIMDFor comment.
    }
  }

  struct ColData {
    int64_t columnLoc;     // location of the start of the column
    int64_t columnMetaOff; // offset from columnLoc to the start of the block info array
    int64_t numValues;     // number of values in the column
  };

  // Fills in fieldInfo with numeric column info.
  // returns ColData with info about the column needed to read it.
  ColData finish() {
    // bool allDocsHaveValue = nAdded == postingsWriter.getMaxDoc();
    if (!values.empty()) {
      addBlock(values);
      values.resize(0);
    }

    int64_t metaOff = colOutput.size() - colStart;
    colOutput.write((const char*)blockInfo.data(), blockInfo.size() * sizeof(IntColReader::NumericBlockInfo));
    
    // Write overall min/max after the block metadata as vlongs
    if (nAdded == 0) {
      overallMin = 0;
      overallMax = 0;
    }
    colOutput.writeVlong(overallMin);
    colOutput.writeVlong(overallMax);

    return {(int64_t)colStart, metaOff, nAdded};
  }


  // Fills in fieldInfo with numeric column info.
  // returns number of values written
  int64_t finish(PostingsWriter::IndexFieldInfo& fieldInfo) {
    auto data = finish();

    fieldInfo.columnLoc = seg_location(colOutput.streamNumber, data.columnLoc);
    fieldInfo.columnMetaOff = data.columnMetaOff;
    fieldInfo.numValues = data.numValues;  // TODO: do this here?
    return nAdded;
  }
};


class MonoWriter {
private:
  MemPool& pool;
  OutputStream& out;
  size_t colStart;
  size_t nAdded = 0;

  std::vector<MonoReader::BlockInfo> blockInfo;
  std::vector<int64_t> values;
  std::vector<int32_t> ivalues;

public:
  /// "output" column metadata that is filled in / valid after finish() is called.
  /// Both are currently needed for MonoReader.
  seg_location blockLoc;
  int64_t metaOff;

  MonoWriter(MemPool& pool, OutputStream& out) : pool(pool), out(out) {
    unused(this->pool);
    colStart = out.size();
  }

  OutputStream& getOutputStream() {
    return out;
  }

  void addInt64(int64_t val) {
    nAdded++;
    values.push_back(val);
    // values[nAdded % BLOCK_SIZE] = val;
    // if (nAdded % BLOCK_SIZE == 0) {
    if (values.size() == MonoReader::BLOCK_SIZE) {
      addBlock(values);
      values.resize(0);
    }
  }

  void addBlock(std::span<int64_t> arr) {
    auto intercept = arr.front();
    auto max = arr.back();

    double slope = arr.size() == 1 ? 0 : (max - intercept) / (arr.size() - 1);
    uint64_t scaled_slope = (uint64_t)(slope * MonoReader::SLOPE_SCALE);

    // deltas from the expected (interpolated) value
    int64_t minDelta = 0;
    int64_t maxDelta = 0;

    for (size_t i=0; i<arr.size(); i++) {
      uint64_t expected = intercept + (uint64_t)(i * scaled_slope / MonoReader::SLOPE_SCALE);
      int64_t delta = arr[i] - expected;
      minDelta = std::min(minDelta, delta);
      maxDelta = std::max(maxDelta, delta);
    }

    auto bits = std::bit_width(uint64_t(maxDelta - minDelta));
    // We could zig-zag encode to make all of the deltas positive, but we can save a little by just
    // lowering the intercept by minDelta.  Although this will raise the average delta, it should not
    // change the maximum number of bits needed to represent the largest.
    intercept += minDelta;
    blockInfo.push_back({out.size() - colStart, scaled_slope, intercept, (uint8_t)bits});

    if (bits > 32) {
      out.write((const char*)arr.data(), arr.size() * sizeof(int64_t));
      return;
    }

    // Currently our For codec can only handle 32 bit integers, so we need to make a copy.
    // We should make a version that can accept 64 bit integers that just use the lower half.
    ivalues.reserve(arr.size());
    ivalues.resize(0);
    for (size_t i=0; i<arr.size(); i++) {
      uint64_t expected = intercept + (uint64_t)(i * scaled_slope / MonoReader::SLOPE_SCALE);
      uint32_t delta = (uint32_t)(arr[i] - expected);
      // if bits=32, this assert may not be true (and we changed delta to be unsigned to account for this)
      // assert(delta >= 0);
      assert(int64_t((uint64_t(scaled_slope * i) / MonoReader::SLOPE_SCALE) + delta + intercept) == arr[i]);
      ivalues.push_back(delta);
    }

    // TODO: we should probably use a faster codec for random access for this.
    std::vector<char> compressed_output(ivalues.size() * sizeof(int32_t) + 32);
    uint32_t compressedSize = compressed_output.size(); // this gets changed to the actual size
    // Postings::numericCodec.encodeBlock((uint32_t*)ivalues.data(), ivalues.size(), compressed_output.data(), compressedSize);
    IndexCodec::numericCodec.encodeWithMeta((uint32_t*)ivalues.data(), ivalues.size(), compressed_output.data(), compressedSize, 0, bits);
    out.write(compressed_output.data(), compressedSize);
    // SIMDFor implementation can read up to 31 extra bytes after the end of compressedSize.  See SoluxSIMDFor comment.
    // TODO: FIXME just a single MonoReader::BlockInfo that come after the blocks may not be enough!
    // We could always pad the end of a file rather than pad each field.
  }

  // returns number of values written and sets metadata to be read.
  size_t finish() {
    // bool allDocsHaveValue = nAdded == postingsWriter.getMaxDoc();
    if (!values.empty()) {
      addBlock(values);
      values.resize(0);
    }

    // If we added enough values, ensure alignment of the block meta array.
    if (nAdded >= 128) {  // This is just a guess and the exact value is not needed for correctness.
      out.align(8);
    }
    // TODO: we should align the blocks as well if we know we will add enough values.

    blockLoc = seg_location(out.streamNumber, colStart);
    metaOff = out.size() - colStart;

    // the number of blocks can be derived from nAdded.
    out.write((const char*)blockInfo.data(), blockInfo.size() * sizeof(MonoReader::BlockInfo));
    return nAdded;
  }

};

} // end namespace