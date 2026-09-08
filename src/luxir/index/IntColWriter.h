// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <numeric>

#include "PostingsWriter.h"
#include "luxir/codec/LinearPack.h"
#include "luxir/codec/NumColumnFormat.h"
#include "luxir/reader/IntColReader.h"

namespace luxir {

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
  std::vector<NumBlockZone> leafZones;
  std::vector<int64_t> values;
  std::vector<uint64_t> quotients;
  bool useGcd = true;
  bool zonesEnabled = true;

  void addBlock(std::span<const int64_t> block) {
    NumColumnFormat::BlockPlan plan =
        NumColumnFormat::planBlock(block, quotients, useGcd, zonesEnabled);
    overallMin = std::min(overallMin, plan.zone.min);
    overallMax = std::max(overallMax, plan.zone.max);

    if (plan.raw()) out.align(MAX_ALIGN);
    plan.info.setPayload(out.size() - colStart, plan.bits);
    if (plan.raw()) {
      out.write(block.data(), block.size_bytes());
    } else {
      LinearPack::Writer writer(out, plan.bits);
      for (uint64_t i = 0; i < block.size(); i++) {
        uint64_t residual =
            NumColumnFormat::residual(plan, i, quotients[(size_t)i]);
        assert(residual <= LinearPack::mask64(plan.bits));
        writer.append(residual);
      }
      writer.finish(false);
    }
    blockInfo.push_back(plan.info);
    blockZones.push_back(plan.zone);
    leafZones.insert(leafZones.end(), plan.leafZones,
                     plan.leafZones + plan.leafCount);
  }

public:
  // useGcd == false is the monotonic contract: gcd stays 1 so readers can drop
  // the field load and the multiply.  It also skips the write-side gcd scan.
  // writeZones == false declares at construction that finish() will not
  // persist zones, so planning skips the per-leaf extrema pass and nothing
  // accumulates (monotonic sidecars are the large writers on this path).
  explicit NumColumnWriter(OutputStream& out, bool useGcd = true,
                           bool writeZones = true)
      : out(out), colStart(out.size()), useGcd(useGcd),
        zonesEnabled(writeZones) {
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
    assert(!writeZones || zonesEnabled);
    if (!values.empty()) {
      addBlock(values);
      values.clear();
    }

    out.align(MAX_ALIGN);
    int64_t metaOff = out.size() - colStart;
    if (!blockInfo.empty()) {
      out.write(blockInfo.data(), blockInfo.size() * sizeof(NumBlockInfo));
    }
    if (writeZones) {
      out.align(MAX_ALIGN);
      if (!blockZones.empty()) {
        out.write(blockZones.data(), blockZones.size() * sizeof(NumBlockZone));
      }
      // Leaf zones directly follow the block zones (both 16-byte entries, so
      // alignment is preserved); readers locate them from the block count and
      // the bounds trailer from ceil(numValues / LEAF_ZONE_SIZE).
      if (!leafZones.empty()) {
        assert((int64_t)leafZones.size()
               == (nAdded + NumColumnFormat::LEAF_ZONE_SIZE - 1)
                   / NumColumnFormat::LEAF_ZONE_SIZE);
        out.write(leafZones.data(), leafZones.size() * sizeof(NumBlockZone));
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
  OutputStream& out;
  NumColumnWriter writer;

public:
  /// "output" column metadata that is filled in / valid after finish() is called.
  /// Both are currently needed for MonoReader.
  seg_location blockLoc;
  int64_t metaOff;

  MonoWriter(MemPool& pool, OutputStream& out)
      : out(out), writer(out, false, false) {
    unused(pool);
  }

  OutputStream& getOutputStream() {
    return out;
  }

  void addInt64(int64_t val) {
    writer.addInt64(val);
  }

  // returns number of values written and sets metadata to be read.
  size_t finish() {
    auto data = writer.finish(false, false);
    blockLoc = writer.columnLocation();
    metaOff = data.columnMetaOff;
    return (size_t)data.numValues;
  }

};

} // end namespace
