// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "luxir/codec/NumBlockCodec.h"
#include "luxir/codec/StreamVByte.h"
#include "luxir/reader/FieldStats.h"

namespace luxir {

static_assert(SVB_OVERREAD_PAD >= LinearPack::TAIL_PAD);

// Shared physical writer for generic and monotonic numeric columns. Callers
// choose whether to persist zone metadata. The finish result carries extrema.
class NumColumnWriter {
public:
  struct FinishData {
    int64_t columnLoc;
    int64_t columnMetaOff;
    int64_t numValues;
    uint64_t blocksBytes;
    uint64_t indexBytes;
    field_parts::ScalarBounds bounds;
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
  NumBlockPlanner::Payload payload;
  NumColumnFormat::BlockCodecs codecs;
  bool useGcd = true;
  bool zonesEnabled = true;
  NumColumnFormat::SketchMode sketchMode = NumColumnFormat::SketchMode::NONE;

  void addBlock(std::span<const int64_t> block) {
    NumColumnFormat::BlockPlan plan =
        NumColumnFormat::planBlock(block, quotients, useGcd, zonesEnabled);
    if (codecs.enabled) {
      NumBlockPlanner::plan(block, plan, codecs.domain, payload);
    }
    overallMin = std::min(overallMin, plan.zone.min);
    overallMax = std::max(overallMax, plan.zone.max);

    NumBlockCodec::write(out, colStart, block, plan, quotients, &payload);
    blockInfo.push_back(plan.info);
    blockZones.push_back(plan.zone);
    leafZones.insert(leafZones.end(), plan.leafZones,
                     plan.leafZones + plan.leafCount);
    if (sketchMode == NumColumnFormat::SketchMode::PERSIST) sketcher.add(block);
  }

public:
  // useGcd == false is the monotonic contract: gcd stays 1 so readers can drop
  // the field load and the multiply.  It also skips the write-side gcd scan.
  // writeZones == false declares at construction that finish() will not
  // persist zones, so planning skips the per-leaf extrema pass and nothing
  // accumulates (monotonic sidecars are the large writers on this path).
  // `sketch` keeps a value sketch with the zones; it is for the columns
  // field-sort planning reads (FieldType::isNumericColumn, single valued).
  // PERSIST writes one for a column of at least SKETCH_MIN_VALUES values and
  // otherwise leaves it to the reader, as FROM_ZONES always does (BOOLEAN
  // columns). Every other zone-bearing column (geo points, multi-valued
  // columns, a query-time OrdMap's internal columns) has none and skips the
  // sketcher: two tail-buffer passes per value and up to 512 KB while
  // writing.
  // `codecs` lets blocks take the kinds other than FOR (NumBlockPlanner):
  // field value columns enable it, and only with useGcd, since monotonic
  // columns are read through NumColumnT<false>, which decodes FOR only.
  explicit NumColumnWriter(OutputStream& out, bool useGcd = true,
                           bool writeZones = true,
                           NumColumnFormat::SketchMode sketch = NumColumnFormat::SketchMode::NONE,
                           NumColumnFormat::BlockCodecs codecs = {})
      : out(out), colStart(out.size()), codecs(codecs), useGcd(useGcd),
        zonesEnabled(writeZones), sketchMode(sketch) {
    assert(sketch == NumColumnFormat::SketchMode::NONE || writeZones);
    assert(!codecs.enabled || useGcd);
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

  FinishData finish(bool writeZones) {
    assert(!finished);
    assert(!writeZones || zonesEnabled);
    if (!values.empty()) {
      addBlock(values);
      values.clear();
    }

    uint64_t blocksBytes = out.size() - colStart;
    auto indexOffset = NumColumnFormat::indexOffset(out.size());
    out.align(MAX_ALIGN);
    int64_t metaOff = indexOffset - colStart;
    if (!blockInfo.empty()) {
      out.write(blockInfo.data(), blockInfo.size() * sizeof(NumBlockInfo));
    }
    if (writeZones) {
      out.align(MAX_ALIGN);
      if (!blockZones.empty()) {
        out.write(blockZones.data(), blockZones.size() * sizeof(NumBlockZone));
      }
      // Leaf zones directly follow the block zones (both 16-byte entries, so
      // alignment is preserved), then the value sketch (NumColumnFormat::
      // ValueSketcher) as a uint32 count (0 for a column without one,
      // SKETCH_FROM_ZONES for one left to the reader) and that many (rank,
      // value) int64 pairs. Readers locate them from the block count and
      // ceil(numValues / LEAF_ZONE_SIZE). The leaf visit orders are derived
      // from the zones on load (NumColumnFormat::LeafMeta::derive).
      if (!leafZones.empty()) {
        assert((int64_t)leafZones.size()
               == (nAdded + NumColumnFormat::LEAF_ZONE_SIZE - 1)
                   / NumColumnFormat::LEAF_ZONE_SIZE);
        out.write(leafZones.data(), leafZones.size() * sizeof(NumBlockZone));
        using SketchMode = NumColumnFormat::SketchMode;
        bool persist = sketchMode == SketchMode::PERSIST
            && nAdded >= NumColumnFormat::SKETCH_MIN_VALUES;
        std::vector<NumColumnFormat::SketchPoint> sketch;
        if (persist) sketch = sketcher.finish();
        assert(sketch.empty() != persist && (int64_t)sketch.size() <= nAdded);
        uint32_t sketchCount = sketchMode == SketchMode::NONE ? 0
            : persist ? (uint32_t)sketch.size() : NumColumnFormat::SKETCH_FROM_ZONES;
        out.write(&sketchCount, sizeof(sketchCount));
        out.write(sketch.data(),
                  sketch.size() * sizeof(NumColumnFormat::SketchPoint));
      }
    }
    uint64_t indexBytes = out.size() - colStart - metaOff;
    finished = true;
    return {(int64_t)colStart, metaOff, nAdded, blocksBytes, indexBytes, {overallMin, overallMax}};
  }
};


} // namespace luxir
