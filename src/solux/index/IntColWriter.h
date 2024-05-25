#pragma once

#include "solux/reader/IntColReader.h"

namespace solux {

//
// Integer column writing
// TODO: currently all values must be written before all docs!  Decouple this so we can write columns incrementally!
// To increase locality and decrease seeks, we could ensure that docsWithField always immediately follow the values.
// We could use an OutputStream that writes to RAM or even a MemPool implementation (or OutputStream writing to MemPool)
//
class IntColWriterSimple {
  PostingsWriter& postingsWriter;
  PostingsWriter::IndexFieldInfo& fieldInfo;
  OutputStream& colOutput;
  int64_t colStart;
  int32_t nAdded = 0;            // number of values added. redundant with numDocsWithValue, for sanity check
public:

  // This class allocates from the pool but does not do any visible rollbacks.
  IntColWriterSimple(MemPool& pool, PostingsWriter& postingsWriter, PostingsWriter::IndexFieldInfo& fieldInfo)
  : postingsWriter(postingsWriter), fieldInfo(fieldInfo), colOutput(postingsWriter.files[5].out) {
    unused(pool, postingsWriter);
    colStart = colOutput.size();
  }

  void startField() {
  }

  void addInt64(int64_t val) {
    nAdded++;
    // temporary worst-case implementation with no compression
    colOutput.writeLong(val);
  }

  // returns number of values written
  int32_t finish() {
    // bool allDocsHaveValue = nAdded == postingsWriter.getMaxDoc();

    // FUTURE:write index into value blocks here
    fieldInfo.flags |= 0x02;  // int64 values
    fieldInfo.docsWithField = nAdded;
    fieldInfo.columnLoc = seg_location(colOutput.streamNumber, colStart);
    return nAdded;
  }
};

class IntColWriter {
public:
  constexpr static uint32_t BLOCK_SIZE = Postings::NUMERIC_BLOCK_SIZE;

private:
  PostingsWriter& postingsWriter;
  PostingsWriter::IndexFieldInfo& fieldInfo;
  OutputStream& colOutput;
  size_t colStart;
  size_t nAdded = 0;            // number of values added. redundant with numDocsWithValue, for sanity check

  std::vector<IntColReader::NumericBlockInfo> blockInfo;
  std::vector<int64_t> values;
  std::vector<int32_t> ivalues;

public:
  // This class allocates from the pool but does not do any visible rollbacks.
  IntColWriter(MemPool& pool, PostingsWriter& postingsWriter, PostingsWriter::IndexFieldInfo& fieldInfo)
          : postingsWriter(postingsWriter), fieldInfo(fieldInfo), colOutput(postingsWriter.files[5].out) {
    unused(pool, postingsWriter);
    colStart = colOutput.size();
  }

  void startField() {
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
    // since gcd(a,b) = gcd(a, b-a), it doesn't matter if we subtract min first (except maybe it could be faster?)
    // we also do the gcd calculation in signed integers, because gcd(-10,10) on unsigned bit patterns yields 2.
    int64_t gcd = arr[0];
    int64_t min = arr[0];
    int64_t max = arr[0];
    for (size_t i=1; i<arr.size(); i++) {
      min = std::min(min, arr[i]);
      max = std::max(max, arr[i]);
      // If gcd hits 1 it can't change from that.  If it's 0, it could still go up.
      if (gcd != 1) {
        gcd = std::gcd(gcd, arr[i]);
      }
    }

    // get number of bits needed to represent values if we divide everything by the gcd
    if (gcd == 0) {
      gcd = 1;  // all values 0... we can't divide by 0 though.
    }

    uint32_t bits = std::bit_width((uint64_t)((max - min) / gcd));
    colOutput.align(4); // just a guess for now... we should really test.
    // the original SIMD code wrote length, min, max (which is 12 bytes, only 4 byte aligned when the SIMD magic starts happening)
    int64_t off = colOutput.size() - colStart;
    blockInfo.push_back({gcd, min, max, bits, off});

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
    for (size_t i=0; i<arr.size(); i++) {
      ivalues.push_back((arr[i] - min) / gcd);
      assert(int64_t(ivalues.back()) * gcd + min == arr[i]);
    }



    // codec calculates its own min and max... it's redundant with what we have done here,
    // and the integers we pass will always have a min of 0 now.
    // We should also be able to write an internal mini-block at a time (128 values for For) so we don't have to buffer
    // the whole output block.

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
      Postings::numericCodec.encodeBlock((uint32_t*)ivalues.data(), ivalues.size(), compressed_output.data(), compressedSize);
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

  // returns number of values written
  int32_t finish() {
    // bool allDocsHaveValue = nAdded == postingsWriter.getMaxDoc();
    if (!values.empty()) {
      addBlock(values);
      values.resize(0);
    }

    // TODO: for dense iteration, we prob don't want to unpack a whole block at once.  We should be
    // able to unpack some multiple of 128 values at a time and keep the same speed?
    fieldInfo.columnMeta = seg_location(colOutput.streamNumber, colOutput.size());
    // the number of blocks can be derived from nAdded.
    colOutput.write((const char*)blockInfo.data(), blockInfo.size() * sizeof(IntColReader::NumericBlockInfo));

    fieldInfo.flags |= 0x02;  // int64 values
    fieldInfo.docsWithField = nAdded;
    fieldInfo.columnLoc = seg_location(colOutput.streamNumber, colStart);
    return nAdded;
  }
};




} // end namespace