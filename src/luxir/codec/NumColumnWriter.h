// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "luxir/codec/NumBlockCodec.h"
#include "luxir/codec/StreamVByte.h"

namespace luxir {

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

  // Bounds over every value added, complete once finish() ran; meaningless
  // when no value was added.
  int64_t min() const noexcept { return overallMin; }
  int64_t max() const noexcept { return overallMax; }
  int64_t count() const noexcept { return nAdded; }

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

    NumBlockCodec::write(out, colStart, block, plan, quotients);
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


} // namespace luxir
