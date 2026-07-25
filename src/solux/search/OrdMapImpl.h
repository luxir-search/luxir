#pragma once
#include <algorithm>
#include <bit>
#include <cstdlib>
#include <limits>
#include <string>
#include <variant>
#include <vector>

#include "OrdMap.h"
#include "solux/index/IntColWriter.h"
#include "solux/reader/TermsEnum.h"
#include "solux/util/heap.h"
#include "solux/util/log.h"

namespace solux {

//
// This is private, only used by OrdMap::build().  This header is only included in one .cpp file.
//
// Format (designed for index-time on-disk as well as on-demand in RAM):
// [seg1-deltas][seg2-deltas]...[segN-deltas]
// [firstSegs][globDeltas]
// [numGlobalOrds (vLong)]
// [numSegs (vLong)]
// [seg1-meta][seg2-meta]...[segN-meta]
// [firstSegs-meta][globDeltas-meta]
// [meta-size (int32_t)]  // 4 byte size of metadata, starting at [numSegs], not including this size field.
//
// seg1-deltas contains deltas to map segment ords into global ord space.
// Each segment is encoded as a flat exact-bpv LinearPack region, predicted
// 4096-value blocks, or one whole-segment linear fit with packed residuals.
//
// seg1-meta contains delta metadata (bits, encoding, offsets, nValues)
//   nValues (vLong) = number of values in this column.  if nValues == 0 or numGlobalOrds, no other metadata is present.
//   bits (vInt) = maximum delta width.  bits == 0 is an identity mapping and has no payload.
//   encoding (vInt) = FLAT, PREDICTED, or SINGLE_FIT, present only when bits != 0.
//   FLAT: payload offset (vLong).
//   PREDICTED: payload offset (vLong), block-meta offset (vLong), max residual bits (vInt).
//   SINGLE_FIT: intercept (long), scaled slope (long), residual bits (vInt),
//               and payload offset (vLong) when residual bits != 0.
//
// There is a single firstSegs column that contains the first segment number that the global ord appeared in.
// There is a single globDeltas column that contains the delta that was used to map from that segment ord to the global ord.
// Neither of these global columns are monotonic, so they are encoded with IntColWriter.
//   firstSegs-meta and globalDeltas-meta are similar to the per-segment meta.
//
// The metadata that the reader needs is simply the start and end locations of the entire structure.
//

class OrdMapBuilder {
  std::string_view field;
  IndexReader& reader;
  OrdMap::DeltaEncoding deltaEncoding;
  TopTermsBuilder topTermsBuilder;

  struct SegmentStat {
    const char* encoding = "none";
    uint8_t bits = 0;
  };
  std::vector<SegmentStat> segmentStats;

  void logSize() const {
    std::string segments;
    for (size_t i = 0; i < segmentStats.size(); i++) {
      if (i != 0) segments.push_back(',');
      segments += std::to_string(i);
      segments.push_back('=');
      segments += segmentStats[i].encoding;
      segments.push_back(':');
      segments += std::to_string(segmentStats[i].bits);
      segments.push_back('b');
    }
    LOG_INFO("OrdMap field={} bytes={} topTerms={} unlistedBound={} segments=[{}]",
             field, size, topTerms.entries.size(), topTerms.unlistedBound,
             segments);
  }

  struct PackedDeltaRuns {
    struct Run {
      uint64_t startIndex;
      uint8_t width;
      uint64_t byteOffset;
    };

    std::vector<char> packed;
    std::vector<Run> runs;
    std::optional<LinearPack::Writer> writer;
    uint64_t count = 0;
    uint64_t firstDelta = 0;
    uint64_t lastDelta = 0;

    void startRun(uint8_t width) {
      if (writer) writer->finish();
      runs.push_back({count, width, packed.size()});
      assert(runs.size() <= 58);
      writer.emplace(packed, width);
    }

    void add(int64_t delta) {
      assert(delta >= 0);
      auto value = (uint64_t)delta;
      // Consecutive local ords differ by one while their global ords advance
      // by at least one, so segment-to-global deltas cannot decrease.
      assert(count == 0 || value >= lastDelta);
      uint8_t width = (uint8_t)std::bit_width(value);
      assert(width <= 57);
      if (!writer || width > runs.back().width) startRun(width);
      writer->append(value);
      if (count == 0) firstDelta = value;
      lastDelta = value;
      count++;
    }

    void finish() {
      assert(writer);
      writer->finish();
      writer.reset();
      assert(bits() == (uint8_t)std::bit_width(lastDelta));
    }

    uint8_t bits() const {
      assert(!runs.empty());
      return runs.back().width;
    }

    template <class Acceptor>
    void forEachValue(Acceptor&& acceptor) const {
      assert(!writer);
      uint64_t seen = 0;
      uint64_t values[128];
      for (size_t runIdx = 0; runIdx < runs.size(); runIdx++) {
        const auto& run = runs[runIdx];
        uint64_t endIndex = runIdx + 1 < runs.size()
            ? runs[runIdx + 1].startIndex : count;
        uint64_t runCount = endIndex - run.startIndex;
        const char* base = packed.data() + run.byteOffset;
        uint64_t mask = LinearPack::mask64(run.width);
        for (uint64_t start = 0; start < runCount; start += 128) {
          uint32_t n = (uint32_t)std::min<uint64_t>(128, runCount - start);
          LinearPack::unpack128(base, start, n, run.width, mask, values);
          for (uint32_t i = 0; i < n; i++) {
            acceptor(run.startIndex + start + i, values[i]);
          }
          seen += n;
        }
      }
      assert(seen == count);
    }

    OrdColumnFormat::PredictedBlockInfo planSingleFit() const {
      assert(!writer && count > 0);
      auto slope = OrdColumnFormat::endpointSlope(
          (int64_t)firstDelta, (int64_t)lastDelta, count,
          OrdColumnFormat::SINGLE_FIT_SLOPE_SHIFT);
      int64_t scaledSlope = slope.value_or(0);
      int64_t intercept = (int64_t)firstDelta;
      __int128 minError = std::numeric_limits<__int128>::max();
      __int128 maxError = std::numeric_limits<__int128>::min();
      forEachValue([&](uint64_t index, uint64_t value) {
        __int128 error = (__int128)value - OrdColumnFormat::predict(
            intercept, scaledSlope, index,
            OrdColumnFormat::SINGLE_FIT_SLOPE_SHIFT);
        minError = std::min(minError, error);
        maxError = std::max(maxError, error);
      });
      auto plan = OrdColumnFormat::finishPlan(
          intercept, scaledSlope, minError, maxError);
      assert(plan);
      return *plan;
    }

    void writeFinal(OutputStream& out) const {
      assert(!writer);
      uint8_t finalBits = bits();
      assert(finalBits != 0);

      if (runs.size() == 1) {
        assert(runs[0].startIndex == 0);
        assert(runs[0].byteOffset == 0);
        assert(packed.size() == LinearPack::byteSize(count, finalBits));
        out.write(packed.data(), packed.size());
        return;
      }

      LinearPack::Writer finalWriter(out, finalBits);
      forEachValue([&](uint64_t, uint64_t value) {
        finalWriter.append(value);
      });
      uint64_t written = finalWriter.finish();
      assert(written == LinearPack::byteSize(count, finalBits));
    }

    void writeSingleFit(
        OutputStream& out,
        const OrdColumnFormat::PredictedBlockInfo& plan) const {
      assert(!writer);
      if (plan.bits == 0) return;
      LinearPack::Writer residualWriter(out, plan.bits);
      uint64_t mask = LinearPack::mask64(plan.bits);
      forEachValue([&](uint64_t index, uint64_t value) {
        uint64_t residual = OrdColumnFormat::residual(
            plan, index, value, OrdColumnFormat::SINGLE_FIT_SLOPE_SHIFT);
        assert(residual <= mask);
        residualWriter.append(residual);
      });
      uint64_t written = residualWriter.finish();
      assert(written == LinearPack::byteSize(count, plan.bits));
    }

    void release() {
      assert(!writer);
      std::vector<char>().swap(packed);
      std::vector<Run>().swap(runs);
    }
  };

  struct PredictedDeltaBlocks {
    std::unique_ptr<uint64_t[]> block =
        std::make_unique_for_overwrite<uint64_t[]>(OrdColumnFormat::BLOCK_SIZE);
    std::vector<char> payload;
    std::vector<OrdColumnFormat::PredictedBlockInfo> blocks;
    uint64_t count = 0;
    uint64_t lastDelta = 0;
    uint32_t blockCount = 0;
    uint8_t maxResidualBits = 0;
    bool finished = false;

    void flushBlock() {
      if (blockCount == 0) return;
      auto plan = OrdColumnFormat::planBlock(
          std::span<const uint64_t>(block.get(), blockCount));
      assert(plan.count == blockCount);
      plan.info.payloadOffset = payload.size();
      maxResidualBits = std::max(maxResidualBits, plan.info.bits);
      if (plan.info.bits != 0) {
        LinearPack::Writer writer(payload, plan.info.bits);
        uint64_t mask = LinearPack::mask64(plan.info.bits);
        for (uint32_t i = 0; i < blockCount; i++) {
          uint64_t residual = OrdColumnFormat::residual(
              plan.info, i, block[i]);
          assert(residual <= mask);
          writer.append(residual);
        }
        writer.finish();
      }
      blocks.push_back(plan.info);
      blockCount = 0;
    }

    void add(int64_t delta) {
      assert(!finished && delta >= 0);
      uint64_t value = (uint64_t)delta;
      assert(count == 0 || value >= lastDelta);
      assert(std::bit_width(value) <= 57);
      block[blockCount++] = value;
      lastDelta = value;
      count++;
      if (blockCount == OrdColumnFormat::BLOCK_SIZE) flushBlock();
    }

    void finish() {
      assert(!finished && count > 0);
      flushBlock();
      block.reset();
      finished = true;
      assert(bits() == (uint8_t)std::bit_width(lastDelta));
      assert(blocks.size() ==
             (count + OrdColumnFormat::BLOCK_SIZE - 1) /
                 OrdColumnFormat::BLOCK_SIZE);
    }

    uint8_t bits() const {
      return (uint8_t)std::bit_width(lastDelta);
    }

    uint64_t writeFinal(OutputStream& out) const {
      assert(finished && bits() != 0);
      out.write(payload.data(), payload.size());
      uint64_t blockMetaLoc = out.size();
      out.write(blocks.data(),
                blocks.size() * sizeof(OrdColumnFormat::PredictedBlockInfo));
      return blockMetaLoc;
    }

    void release() {
      assert(finished);
      std::vector<char>().swap(payload);
      std::vector<OrdColumnFormat::PredictedBlockInfo>().swap(blocks);
    }
  };

  // Collect TermsEnum for each segment.  Keep track of the index so we can visit in ascending order one at a time.
  struct TermsEnumIdx {
    size_t idx;
    TermsEnum tenum;
    OrdMap::DeltaEncoding encoding;
    std::variant<PackedDeltaRuns, PredictedDeltaBlocks> deltas;
    std::optional<OrdColumnFormat::PredictedBlockInfo> singleFitPlan;

    TermsEnumIdx(size_t idx, MemPool& pool, PostingsReader& postingsReader,
                 SegFieldInfo& finfo, OrdMap::DeltaEncoding encoding)
        : idx(idx), tenum(pool, postingsReader, finfo), encoding(encoding) {
      if (encoding == OrdMap::DeltaEncoding::PREDICTED) {
        deltas.emplace<PredictedDeltaBlocks>();
      }
    }

    void addDelta(int64_t delta) {
      std::visit([&](auto& values) { values.add(delta); }, deltas);
    }

    void finishDeltas() {
      std::visit([](auto& values) { values.finish(); }, deltas);
      if (encoding == OrdMap::DeltaEncoding::SINGLE_FIT) {
        singleFitPlan = std::get<PackedDeltaRuns>(deltas).planSingleFit();
      }
    }

    uint64_t deltaCount() const {
      return std::visit([](const auto& values) { return values.count; }, deltas);
    }

    uint8_t bits() const {
      return std::visit([](const auto& values) { return values.bits(); }, deltas);
    }

    void releaseDeltas() {
      std::visit([](auto& values) { values.release(); }, deltas);
    }
  };

public:
  OrdMapBuilder(std::string_view field, IndexReader& reader)
      : field(field), reader(reader),
        deltaEncoding(OrdMap::configuredDeltaEncoding()),
        topTermsBuilder(TopTermsBuilder::thresholdFor(reader.maxDoc())) {}

  // Build fills these in currently.  In the future, the output may be written to disk.
  std::unique_ptr<char[]> data;
  int64_t start = 0;
  int64_t size = 0;
  TopTerms topTerms;
  
  // For single segment with values case
  bool isSingleSegment = false;
  int segmentWithValues = -1;
  int64_t numTerms = 0;

  void build() {
    auto poolGuard = MemPool::threadLocalPoolGuard();
    auto& pool = poolGuard.pool();

    const auto& segs = reader.segments();
    auto nsegs = segs.size();
    segmentStats.resize(nsegs);

    // the vector isn't in the pool, but the TermsEnumIdx instances will be.
    // This will "own" the TermsEnumIdx instances and call destructors.
    std::vector<u_ptr<TermsEnumIdx>> allTermsEnums;
    allTermsEnums.reserve(nsegs);

    int segsWithValue = 0;
    for (auto i=0u; i<nsegs; i++) {
      auto& seg = segs[i];
      // Allocate FieldReader in pool so it has same lifetime as SegFieldInfo
      auto* fieldReader = pool.make<FieldReader>(seg.postingsReader());
      if (!fieldReader->seek(field)) {
        // Field not found in this segment
        allTermsEnums.emplace_back(nullptr);
        continue;
      }
      auto* fieldInfo = pool.make<SegFieldInfo>();  // Allocate in pool to avoid stack-use-after-scope
      fieldReader->readFieldInfo(*fieldInfo);
      //       TermsEnumIdx(size_t idx, MemPool& pool, PostingsReader& postingsReader, SegFieldInfo& finfo) : idx(idx), tenum(pool, postingsReader, finfo), deltas(pool) {}


      allTermsEnums.emplace_back(pool.make_unique<TermsEnumIdx>(
          i, pool, seg.postingsReader(), *fieldInfo, deltaEncoding));
      // position the TermsEnum on the first term
      if (!allTermsEnums.back()->tenum.nextTerm()) {
        // defensive coding, every field should have at least one term.
        allTermsEnums.back().reset();
        continue;
      }
      segsWithValue++;
    }

    if (segsWithValue == 0) {
      topTerms = topTermsBuilder.finish();
      logSize();
      return;
    }
    
    // Fast path: if only one segment has values, no OrdMap is needed (identity mapping)
    if (segsWithValue == 1) {
      // Find which segment has values and get term count from fieldInfo
      for (size_t i = 0; i < allTermsEnums.size(); i++) {
        if (allTermsEnums[i]) {
          // Store values for OrdMap constructor
          this->segmentWithValues = i;
          this->numTerms = allTermsEnums[i]->tenum.numTerms();  // Get directly from fieldInfo
          this->isSingleSegment = true;
          segmentStats[i] = {"identity", 0};
          // This leaves the enum on its final term, and this fast path returns
          // without using it again.
          allTermsEnums[i]->tenum.forEachDocFreq(
              [&](int64_t ord, int32_t df) {
                topTermsBuilder.add(ord, df);
              });
          break;
        }
      }
      topTerms = topTermsBuilder.finish();
      logSize();
      return;
    }

    // TODO: fast path when all segments are full or empty? (i.e. fast path check for full)

    // Fill in a compressed version (no nulls) of the TermsEnumIdx pointers for the priority queue.
    auto tenums = pool.make_span<TermsEnumIdx*>(segsWithValue);
    size_t tenumIdx = 0;
    for (auto& tenum : allTermsEnums) {
      if (tenum) {
        tenums[tenumIdx++] = tenum.get();
      }
    }

    auto termCmp = [](const TermsEnumIdx& a, const TermsEnumIdx& b) -> bool {
      // For a min-heap (smallest term first), we need: return true when a > b
      // This makes std::make_heap put the smallest element at the top
      auto cmp = a.tenum.term() <=> b.tenum.term();
      if (cmp != 0) {
        return cmp > 0; // return true when a > b (for min-heap)
      }

      // tiebreak by number of terms in the segment (largest first) so that "firstSegment" will normally
      // be the same segment for good locality during lookup.
      auto cmp2 = a.tenum.numTerms() - b.tenum.numTerms();
      if (cmp2 != 0) {
        return cmp2 < 0; // For min-heap: return true when a should come before b in heap order
      }

      // tiebreak by index last to prefer first segments over later segments.
      return a.idx > b.idx; // For min-heap: return true when a should come before b in heap order
    };
    IndirectPQ<TermsEnumIdx, decltype(termCmp)> termPQ(tenums);

    char packedTermData[PackedTerm::MAX_BYTES];
    PackedTerm packedTerm(packedTermData,0);  // the current term while iterating over all terms

    RAMFile firstSegsFile("firstSegs");
    OutputStream firstSegsOut(&firstSegsFile);
    IntColWriter firstSegs(firstSegsOut);

    RAMFile globDeltasFile("globDeltas");
    OutputStream globDeltasOut(&globDeltasFile);
    IntColWriter globDeltas(globDeltasOut);

    // iterate through the terms in sorted order
    int64_t globalOrd = -1;
    while (termPQ.size() > 0) {
      globalOrd++;
      TermsEnumIdx& first = termPQ.top();
      
      // There is a tradeoff here.  We need to find other TermsEnumIdx that are positioned on the same term.
      // We could: 1) make a copy of the term and then advance it with updateTop()
      //           2) remove to from the PQ, then after processing all matching segments, advance and reinsert it.
      // Option 1's expense is making a copy of the term, option 2's expense is an extra heap operation.

      // Option 1: make a copy of the term since it will be invalidated after tenum.nextTerm()
      first.tenum.term().copyTo(packedTerm);

      // calculate the mapping back from global ord to the segment ord space for term lookup
      auto firstSeg = first.idx;
      auto firstDelta = globalOrd - first.tenum.ord();
      firstSegs.addInt64(firstSeg);
      globDeltas.addInt64(firstDelta);

      int64_t globalDf = 0;
      do {
        TermsEnumIdx& entry = termPQ.top();
        globalDf += entry.tenum.docFreq();
        auto delta = globalOrd - entry.tenum.ord();
        entry.addDelta(delta);

        // advance this entry to the next term, removing from pq if exhausted.
        if (entry.tenum.nextTerm()) {
          termPQ.updateTop();
        } else {
          termPQ.removeTop();
        }
        // continue while more enums are positioned on the same term
        // FUTURE OPT: we could special case when we are down to a single segment.  All deltas
        //             will be the same from then on.
      } while (termPQ.size() > 0 && termPQ.top().tenum.term() == packedTerm);
      topTermsBuilder.add(globalOrd, globalDf);
    }

    auto firstSegsInfo = firstSegs.finish();
    firstSegsOut.flush(true);
    auto globDeltasInfo = globDeltas.finish();
    globDeltasOut.flush(true);

    RAMFile metaOutFile("metaOut");
    OutputStream metaOut(&metaOutFile);
    metaOut.writeVlong(globalOrd + 1);  // numGlobalOrds
    metaOut.writeVlong(allTermsEnums.size());  // numSegs

    int64_t ordMapStart = 0;  // currently just one ord map per file/buffer, and no header.
    RAMFile payloadFile("ordMapPayload");
    OutputStream payloadOut(&payloadFile);
    bool needGlobalDeltas = false;

    for (auto& tenum : allTermsEnums) {
      bool writeDeltas = tenum && tenum->tenum.numTerms() < (globalOrd + 1);
      auto nTerms = tenum ? tenum->tenum.numTerms() : 0;
      metaOut.writeVlong(nTerms);
      if (tenum) {
        tenum->finishDeltas();
        assert((int64_t)tenum->deltaCount() == nTerms);
      }
      if (writeDeltas) {
        needGlobalDeltas = true;
        uint8_t bits = tenum->bits();
        assert(bits <= 57);
        metaOut.writeVint(bits);
        if (bits != 0) {
          metaOut.writeVint((uint8_t)deltaEncoding);
          if (deltaEncoding == OrdMap::DeltaEncoding::FLAT) {
            uint64_t loc = payloadOut.size();
            metaOut.writeVlong(loc);
            std::get<PackedDeltaRuns>(tenum->deltas).writeFinal(payloadOut);
            segmentStats[tenum->idx] = {"flat", bits};
          } else if (deltaEncoding == OrdMap::DeltaEncoding::PREDICTED) {
            uint64_t loc = payloadOut.size();
            metaOut.writeVlong(loc);
            auto& predicted = std::get<PredictedDeltaBlocks>(tenum->deltas);
            uint64_t blockMetaLoc = predicted.writeFinal(payloadOut);
            metaOut.writeVlong(blockMetaLoc);
            metaOut.writeVint(predicted.maxResidualBits);
            segmentStats[tenum->idx] = {"predicted", predicted.maxResidualBits};
          } else {
            assert(deltaEncoding == OrdMap::DeltaEncoding::SINGLE_FIT);
            assert(tenum->singleFitPlan);
            const auto& fit = *tenum->singleFitPlan;
            metaOut.writeLong(fit.intercept);
            metaOut.writeLong(fit.scaledSlope);
            metaOut.writeVint(fit.bits);
            if (fit.bits != 0) {
              uint64_t loc = payloadOut.size();
              metaOut.writeVlong(loc);
              std::get<PackedDeltaRuns>(tenum->deltas)
                  .writeSingleFit(payloadOut, fit);
            }
            segmentStats[tenum->idx] = {"fit", fit.bits};
          }
        } else {
          segmentStats[tenum->idx] = {"identity", 0};
        }
      } else if (tenum) {
        segmentStats[tenum->idx] = {"identity", 0};
      }
      if (tenum) tenum->releaseDeltas();
    }
    payloadOut.flush(true);

    uint64_t cumulativeSize = payloadFile.size();

    if (needGlobalDeltas) {
      // redundant numValues for the global columns, but it makes reading simpler.
      assert(firstSegsInfo.numValues == globDeltasInfo.numValues);

      metaOut.writeVlong(firstSegsInfo.numValues);
      metaOut.writeVlong(firstSegsInfo.columnLoc + cumulativeSize);
      metaOut.writeVlong(firstSegsInfo.columnMetaOff);
      cumulativeSize += firstSegsFile.size();
      payloadFile.destructiveAppend(firstSegsFile);
      assert(payloadFile.size() == cumulativeSize);


      metaOut.writeVlong(globDeltasInfo.numValues);
      metaOut.writeVlong(globDeltasInfo.columnLoc + cumulativeSize);
      metaOut.writeVlong(globDeltasInfo.columnMetaOff);
      cumulativeSize += globDeltasFile.size();
      payloadFile.destructiveAppend(globDeltasFile);
      assert(payloadFile.size() == cumulativeSize);
    } else {
      metaOut.writeVlong(0);
      metaOut.writeVlong(0);
    }

    // finally write the size of the metadata, then we can flush and add to outFile.
    auto metaSize = metaOut.size();
    cumulativeSize += metaSize;
    metaOut.writeInt((int32_t)metaSize);
    metaOut.flush(true);
    cumulativeSize += sizeof(int32_t);
    payloadFile.destructiveAppend(metaOutFile);

    assert(payloadFile.size() == cumulativeSize);

    size = payloadFile.size();
    start = ordMapStart;
    data.reset(new char[size]);
    payloadFile.copyTo(data.get());
    topTerms = topTermsBuilder.finish();
    logSize();
  }
};

OrdMap::DeltaEncoding OrdMap::configuredDeltaEncoding() {
  DeltaEncoding override = deltaEncodingOverride.load();
  if (override != DeltaEncoding::DEFAULT) return override;

  const char* configured = std::getenv("SOLUX_ORDMAP_DELTAS");
  if (configured == nullptr || *configured == '\0' ||
      std::string_view(configured) == "flat") {
    return DeltaEncoding::FLAT;
  }
  if (std::string_view(configured) == "predicted") {
    return DeltaEncoding::PREDICTED;
  }
  if (std::string_view(configured) == "fit") {
    return DeltaEncoding::SINGLE_FIT;
  }
  LOG_WARN("Ignoring invalid SOLUX_ORDMAP_DELTAS='{}'; expected flat, predicted, or fit",
           configured);
  return DeltaEncoding::FLAT;
}


// In the future, we prob want to be able to accept a span of postings readers as well
// so that IndexWriter could pre-create a OrdMap for a field without constructing an IndexReader.
// Or we could just make IndexReader easier to construct w/o taking a Directory, etc.
std::shared_ptr<OrdMap> OrdMap::build(std::string_view field, IndexReader& reader) {
  OrdMapBuilder builder(field, reader);
  builder.build();
  
  // Handle single segment with values case
  if (builder.isSingleSegment) {
    return std::make_shared<OrdMap>(
        builder.numTerms, builder.segmentWithValues, std::move(builder.topTerms));
  }
  
  // Handle normal case or no values case
  if (!builder.data) {
    return {};
  }
  return std::make_shared<OrdMap>(
      std::move(builder.data), builder.start, builder.size,
      std::move(builder.topTerms));
}


}
