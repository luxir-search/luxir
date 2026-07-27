#pragma once
#include <numeric>

#include "PostingsWriter.h"
#include "solux/codec/LinearPack.h"
#include "solux/codec/NumColumnFormat.h"
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


static_assert(SVB_OVERREAD_PAD >= LinearPack::TAIL_PAD);

// Shared physical writer for generic and monotonic numeric columns. Callers
// choose whether to persist zone metadata and the column-wide bounds trailer.
class NumColumnWriter {
public:
  struct FinishData {
    int64_t columnLoc;
    int64_t columnMetaOff;
    int64_t numValues;
  };

private:
  OutputStream& out;
  size_t colStart;
  int64_t nAdded = 0;
  int64_t overallMin = std::numeric_limits<int64_t>::max();
  int64_t overallMax = std::numeric_limits<int64_t>::min();
  bool finished = false;
  std::vector<NumBlockInfo> blockInfo;
  std::vector<NumBlockZone> blockZones;
  std::vector<int64_t> values;
  std::vector<uint64_t> quotients;

  void addBlock(std::span<const int64_t> block) {
    NumColumnFormat::BlockPlan plan =
        NumColumnFormat::planBlock(block, quotients);
    overallMin = std::min(overallMin, plan.zone.min);
    overallMax = std::max(overallMax, plan.zone.max);

    if (plan.raw()) out.align(8);
    plan.info.payloadOffset = out.size() - colStart;
    if (plan.raw()) {
      out.write(block.data(), block.size_bytes());
    } else {
      LinearPack::Writer writer(out, plan.info.bits);
      for (uint64_t i = 0; i < block.size(); i++) {
        uint64_t residual =
            NumColumnFormat::residual(plan, i, quotients[(size_t)i]);
        assert(residual <= LinearPack::mask64(plan.info.bits));
        writer.append(residual);
      }
      writer.finish(false);
    }
    blockInfo.push_back(plan.info);
    blockZones.push_back(plan.zone);
  }

public:
  explicit NumColumnWriter(OutputStream& out)
      : out(out), colStart(out.size()) {
    values.reserve(NumColumnFormat::BLOCK_SIZE);
    quotients.reserve(NumColumnFormat::BLOCK_SIZE);
  }

  void addInt64(int64_t val) {
    assert(!finished);
    nAdded++;
    values.push_back(val);
    if (values.size() == NumColumnFormat::BLOCK_SIZE) {
      addBlock(values);
      values.clear();
    }
  }

  size_t columnStart() const {
    return colStart;
  }

  seg_location columnLocation() const {
    return seg_location(out.streamNumber, colStart);
  }

  FinishData finish(bool writeZones, bool writeBounds) {
    assert(!finished);
    assert(!writeBounds || writeZones);
    if (!values.empty()) {
      addBlock(values);
      values.clear();
    }

    out.align(8);
    int64_t metaOff = out.size() - colStart;
    if (!blockInfo.empty()) {
      out.write(blockInfo.data(), blockInfo.size() * sizeof(NumBlockInfo));
    }
    if (writeZones) {
      out.align(8);
      if (!blockZones.empty()) {
        out.write(blockZones.data(), blockZones.size() * sizeof(NumBlockZone));
      }
    }
    if (writeBounds) {
      if (nAdded == 0) {
        overallMin = 0;
        overallMax = 0;
      }
      out.writeVlong(overallMin);
      out.writeVlong(overallMax);
    }
    finished = true;
    return {(int64_t)colStart, metaOff, nAdded};
  }
};

class IntColWriter {
public:
  static constexpr uint32_t BLOCK_SIZE = NumColumnFormat::BLOCK_SIZE;
  using ColData = NumColumnWriter::FinishData;

private:
  NumColumnWriter writer;

public:
  explicit IntColWriter(OutputStream& target) : writer(target) {}

  void addInt64(int64_t val) {
    writer.addInt64(val);
  }

  ColData finish() {
    return writer.finish(true, true);
  }

  // Fills in fieldInfo with numeric column info.
  // returns number of values written
  int64_t finish(PostingsWriter::IndexFieldInfo& fieldInfo) {
    auto data = finish();

    fieldInfo.columnLoc = writer.columnLocation();
    fieldInfo.columnMetaOff = data.columnMetaOff;
    fieldInfo.numValues = data.numValues;  // TODO: do this here?
    return data.numValues;
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
    // The +32 on compressed_output above is encode-output headroom for tails that
    // pack up to a word boundary. SoluxSIMDFor decodes exactly the encoded byte
    // range, so this block needs no reader-side slack. The old
    // SIMDCompressionAndIntersection codec did read ~31 bytes past the data; that
    // was removed in the FastPFOR migration and is covered by
    // PostingsTest.codecFileOverreadBounds. The only postings decoder that
    // over-reads now is the StreamVByte tail, for which PostingsWriter::finish()
    // reserves SVB_OVERREAD_PAD trailing bytes per data file.
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
