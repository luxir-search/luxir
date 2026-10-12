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
  NumColumnFormat::ValueSketcher sketcher;
  std::vector<int64_t> values;
  std::vector<uint64_t> quotients;
  bool useGcd = true;
  bool zonesEnabled = true;
  bool sketchEnabled = false;

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
    if (sketchEnabled) sketcher.add(block);
  }

public:
  // useGcd == false is the monotonic contract: gcd stays 1 so readers can drop
  // the field load and the multiply.  It also skips the write-side gcd scan.
  // writeZones == false declares at construction that finish() will not
  // persist zones, so planning skips the per-leaf extrema pass and nothing
  // accumulates (monotonic sidecars are the large writers on this path).
  // writeSketch persists a value sketch with the zones; it is for the
  // columns field-sort planning reads (FieldType::sortsByIntColumn, single
  // valued). Every other zone-bearing column (geo points, multi-valued
  // columns, a query-time OrdMap's internal columns) writes an empty one
  // and skips the sketcher: two tail-buffer passes per value and up to
  // 512 KB while writing.
  explicit NumColumnWriter(OutputStream& out, bool useGcd = true,
                           bool writeZones = true, bool writeSketch = false)
      : out(out), colStart(out.size()), useGcd(useGcd),
        zonesEnabled(writeZones), sketchEnabled(writeSketch) {
    assert(!writeSketch || writeZones);
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
      // alignment is preserved), then the two leaf visit orders as uint32
      // leaf ids (NumColumnFormat::leafVisitOrder): by min ascending, then by
      // max descending, then the value sketch (NumColumnFormat::
      // ValueSketcher) as a uint32 count (0 for a column written without
      // one) and that many (rank, value) int64 pairs. Readers locate all of
      // them from the block count and ceil(numValues / LEAF_ZONE_SIZE), and
      // the bounds trailer after them.
      if (!leafZones.empty()) {
        assert((int64_t)leafZones.size()
               == (nAdded + NumColumnFormat::LEAF_ZONE_SIZE - 1)
                   / NumColumnFormat::LEAF_ZONE_SIZE);
        out.write(leafZones.data(), leafZones.size() * sizeof(NumBlockZone));
        std::vector<uint32_t> order(leafZones.size());
        for (bool byMaxDescending : {false, true}) {
          NumColumnFormat::leafVisitOrder(leafZones, byMaxDescending, order);
          out.write(order.data(), order.size() * sizeof(uint32_t));
        }
        std::vector<NumColumnFormat::SketchPoint> sketch;
        if (sketchEnabled) sketch = sketcher.finish();
        assert(sketch.empty() != sketchEnabled
               && (int64_t)sketch.size() <= nAdded);
        uint32_t sketchCount = (uint32_t)sketch.size();
        out.write(&sketchCount, sizeof(sketchCount));
        out.write(sketch.data(),
                  sketch.size() * sizeof(NumColumnFormat::SketchPoint));
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
  // `sketch`: whether the column persists a value sketch
  // (NumColumnWriter).
  explicit IntColWriter(OutputStream& target, bool sketch = false)
      : writer(target, true, true, sketch) {}

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
