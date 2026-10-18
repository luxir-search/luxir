// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cassert>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include "ColumnIndexing.h"
#include "IntColWriter.h"
#include "OrdCollector.h"
#include "PostingsWriter.h"
#include "luxir/codec/LinearPack.h"
#include "luxir/codec/OrdColumnFormat.h"

namespace luxir {

// Ord columns write no LinearPack tail pad (finish(false)); a column at a data
// file's tail relies on the SVB_OVERREAD_PAD every data file reserves. Enforce
// that the shared file pad covers LinearPack's wider bulk-decode overread.
static_assert(SVB_OVERREAD_PAD >= LinearPack::TAIL_PAD);

class OrdColWriter {
public:
  enum class FormatOverride : uint8_t {
    AUTO,
    DIRECT,
    PREDICTED,
    RLE
  };

private:
  using BlockPlan = OrdColumnFormat::PredictedBlockPlan;
  using RunPlan = OrdColumnFormat::RunBlockInfo;

  MemPool& pool;
  PostingsWriter& postingsWriter;
  PostingsWriter::IndexFieldInfo& fieldInfo;
  OrdCollector& ords;
  u_ptr<MonoWriter> endValueRankWriter;
  PostingsWriter::OutputStreamPtr endValueRankOutput;
  inline static std::atomic<FormatOverride> formatOverride = FormatOverride::AUTO;

  bool declaredMultiValued() const {
    return (fieldInfo.flags & FieldType::MULTI_VALUED) != 0;
  }

  template <class Acceptor>
  void forEachRankValue(Acceptor&& acceptor) const {
    for (int32_t slot = 0; slot < ords.size(); slot++) {
      ords.pushValues(slot, acceptor);
    }
  }

  template <class Acceptor>
  void forEachEncodedValue(int32_t indexing, std::span<const int32_t> rankToDoc,
                           Acceptor&& acceptor) const {
    if (indexing == SegFieldInfo::ORD_RANK) {
      forEachRankValue(acceptor);
      return;
    }

    assert(indexing == SegFieldInfo::ORD_DOCID);
    int32_t maxDoc = postingsWriter.getMaxDoc();
    if (rankToDoc.empty()) {
      assert(ords.size() == maxDoc);
      for (int32_t doc = 0; doc < maxDoc; doc++) {
        uint32_t ord = 0;
        int32_t count = 0;
        ords.pushValues(doc, [&](uint32_t value) {
          ord = value;
          count++;
        });
        assert(count <= 1);
        acceptor(ord);
      }
      return;
    }

    assert((int32_t)rankToDoc.size() == ords.size());
    int32_t slot = 0;
    for (int32_t doc = 0; doc < maxDoc; doc++) {
      if (slot < ords.size() && rankToDoc[slot] == doc) {
        uint32_t ord = 0;
        int32_t count = 0;
        ords.pushValues(slot, [&](uint32_t value) {
          ord = value;
          count++;
        });
        assert(count == 1);
        acceptor(ord);
        slot++;
      } else {
        acceptor(0);
      }
    }
    assert(slot == ords.size());
  }

  // Plans both block formats in one pass: predicted plans and the predicted
  // format's byte size, and run-length plans and theirs.
  std::vector<BlockPlan> planBlocks(int32_t indexing,
                                    std::span<const int32_t> rankToDoc,
                                    uint64_t& predictedSize,
                                    std::vector<RunPlan>& runPlans,
                                    uint64_t& runSize) const {
    std::vector<BlockPlan> plans;
    std::array<uint32_t, OrdColumnFormat::BLOCK_SIZE> block;
    uint32_t count = 0;
    auto flush = [&]() {
      if (count == 0) return;
      std::span<const uint32_t> values(block.data(), count);
      BlockPlan plan = OrdColumnFormat::planBlock(values);
      predictedSize += sizeof(OrdColumnFormat::PredictedBlockInfo);
      if (plan.info.bits != 0) {
        predictedSize += LinearPack::packedByteSize(count, plan.info.bits);
      }
      plans.push_back(plan);
      RunPlan runPlan = OrdColumnFormat::planRunBlock(values);
      runSize += sizeof(RunPlan) + OrdColumnFormat::runPayloadByteSize(runPlan);
      runPlans.push_back(runPlan);
      count = 0;
    };
    forEachEncodedValue(indexing, rankToDoc, [&](uint32_t ord) {
      block[count++] = ord;
      if (count == OrdColumnFormat::BLOCK_SIZE) flush();
    });
    flush();
    return plans;
  }

  void writeDirect(OutputStream& out, int32_t indexing,
                   std::span<const int32_t> rankToDoc, uint8_t bits) const {
    LinearPack::Writer writer(out, bits);
    forEachEncodedValue(indexing, rankToDoc,
                        [&](uint32_t ord) { writer.append(ord); });
    writer.finish(false);  // segment data files reserve SVB_OVERREAD_PAD
  }

  void writePredicted(OutputStream& out, int32_t indexing,
                      std::span<const int32_t> rankToDoc,
                      std::vector<BlockPlan>& plans, uint64_t start) const {
    std::array<uint32_t, OrdColumnFormat::BLOCK_SIZE> block;
    uint32_t count = 0;
    size_t planIndex = 0;
    auto flush = [&]() {
      if (count == 0) return;
      assert(planIndex < plans.size());
      BlockPlan& plan = plans[planIndex++];
      assert(plan.count == count);
      plan.info.payloadOffset = out.size() - start;
      if (plan.info.bits != 0) {
        LinearPack::Writer writer(out, plan.info.bits);
        for (uint32_t i = 0; i < count; i++) {
          uint64_t residual = OrdColumnFormat::residual(
              plan.info, i, block[i]);
          assert(residual <=
                 LinearPack::mask32(plan.info.bits));
          writer.append(residual);
        }
        writer.finish(false);  // blocks are contiguous; file reserves the pad
      }
      count = 0;
    };
    forEachEncodedValue(indexing, rankToDoc, [&](uint32_t ord) {
      block[count++] = ord;
      if (count == OrdColumnFormat::BLOCK_SIZE) flush();
    });
    flush();
    assert(planIndex == plans.size());

    fieldInfo.columnMetaOff = out.size() - start;
    for (const BlockPlan& plan : plans) {
      out.write(&plan.info, sizeof(plan.info));
    }
  }

  void writeRuns(OutputStream& out, int32_t indexing,
                 std::span<const int32_t> rankToDoc,
                 std::vector<RunPlan>& plans, uint64_t start) const {
    std::array<uint32_t, OrdColumnFormat::BLOCK_SIZE> block;
    uint32_t count = 0;
    size_t planIndex = 0;
    auto flush = [&]() {
      if (count == 0) return;
      assert(planIndex < plans.size());
      RunPlan& plan = plans[planIndex++];
      plan.payloadOffset = out.size() - start;
      LinearPack::Writer starts(out, plan.startBits);
      for (uint32_t i = 1; i < count; i++) {
        if (block[i] != block[i - 1]) starts.append(i);
      }
      starts.finish(false);  // blocks are contiguous; file reserves the pad
      LinearPack::Writer values(out, plan.valueBits);
      uint32_t runs = 0;
      for (uint32_t i = 0; i < count; i++) {
        if (i == 0 || block[i] != block[i - 1]) {
          values.append(block[i] - plan.base);
          runs++;
        }
      }
      assert(runs == plan.runs);
      values.finish(false);
      assert(out.size() - start - plan.payloadOffset
             == OrdColumnFormat::runPayloadByteSize(plan));
      count = 0;
    };
    forEachEncodedValue(indexing, rankToDoc, [&](uint32_t ord) {
      block[count++] = ord;
      if (count == OrdColumnFormat::BLOCK_SIZE) flush();
    });
    flush();
    assert(planIndex == plans.size());

    fieldInfo.columnMetaOff = out.size() - start;
    for (const RunPlan& plan : plans) {
      out.write(&plan, sizeof(plan));
    }
  }

public:
  OrdColWriter(MemPool& pool, PostingsWriter& postingsWriter,
               PostingsWriter::IndexFieldInfo& fieldInfo, OrdCollector& ords)
      : pool(pool), postingsWriter(postingsWriter), fieldInfo(fieldInfo), ords(ords) {
    assert(!ords.multiValued() || declaredMultiValued());
    if (ords.multiValued()) {
      endValueRankOutput = postingsWriter.getOutputStream();
      endValueRankWriter = pool.make_unique<MonoWriter>(pool, *endValueRankOutput);
    }
  }

  static FormatOverride setFormatOverrideForTests(FormatOverride value) {
    return formatOverride.exchange(value);
  }

  void finish(std::span<const int32_t> rankToDoc = {}) {
    int32_t maxDoc = postingsWriter.getMaxDoc();
    int32_t nSlots = ords.size();
    assert(rankToDoc.empty() || (int32_t)rankToDoc.size() == nSlots);
    assert(fieldInfo.nTerms >= 0 && fieldInfo.nTerms <= INT32_MAX);
    bool multi = ords.multiValued();
    assert(!multi || declaredMultiValued());
    assert(multi == (endValueRankWriter != nullptr));
    fieldInfo.docsWithField = ords.docsWithValue();

    int64_t nValues = 0;
    for (int32_t slot = 0; slot < nSlots; slot++) {
      int64_t before = nValues;
      ords.pushValues(slot, [&](uint32_t ord) {
        assert(ord <= INT32_MAX);
        nValues++;
      });
      if (nValues != before && endValueRankWriter) {
        endValueRankWriter->addInt64(nValues);
      }
    }
    fieldInfo.numValues = nValues;

    int32_t indexing = !multi && useDocIdIndexing(fieldInfo.docsWithField, maxDoc)
        ? SegFieldInfo::ORD_DOCID : SegFieldInfo::ORD_RANK;
    uint8_t bits = (uint8_t)std::bit_width((uint64_t)fieldInfo.nTerms);
    int64_t encodedValues = indexing == SegFieldInfo::ORD_DOCID ? maxDoc : nValues;
    uint64_t directSize = LinearPack::packedByteSize((uint64_t)encodedValues, bits);
    uint64_t predictedSize = 0;
    uint64_t runSize = 0;
    std::vector<RunPlan> runPlans;
    std::vector<BlockPlan> plans = planBlocks(indexing, rankToDoc, predictedSize,
                                              runPlans, runSize);

    // A block format must halve the size of the format it replaces and save
    // at least 4KB: runs against the better of direct and predicted.
    auto smaller = [](uint64_t size, uint64_t than) {
      return size <= than / 2 && than >= size + 4096;
    };
    FormatOverride override = formatOverride.load();
    int32_t format = SegFieldInfo::ORD_DIRECT;
    if (override == FormatOverride::AUTO) {
      bool predicted = smaller(predictedSize, directSize);
      if (predicted) format = SegFieldInfo::ORD_PREDICTED;
      if (smaller(runSize, predicted ? predictedSize : directSize)) {
        format = SegFieldInfo::ORD_RLE;
      }
    } else if (override == FormatOverride::PREDICTED) {
      format = SegFieldInfo::ORD_PREDICTED;
    } else if (override == FormatOverride::RLE) {
      format = SegFieldInfo::ORD_RLE;
    }

    auto output = postingsWriter.getOutputStream();
    OutputStream& out = *output;
    uint64_t start = out.size();
    fieldInfo.columnLoc = out.slocation();
    fieldInfo.columnMetaOff = 0;
    fieldInfo.ordFormat = format;
    fieldInfo.ordIndexing = indexing;
    fieldInfo.ordBits = bits;
    if (format == SegFieldInfo::ORD_PREDICTED) {
      writePredicted(out, indexing, rankToDoc, plans, start);
    } else if (format == SegFieldInfo::ORD_RLE) {
      writeRuns(out, indexing, rankToDoc, runPlans, start);
    } else {
      writeDirect(out, indexing, rankToDoc, bits);
    }

    if (endValueRankWriter) {
      endValueRankWriter->finish();
      fieldInfo.monoLoc = endValueRankWriter->blockLoc;
      fieldInfo.monoMetaOff = endValueRankWriter->metaOff;
      endValueRankOutput.reset();
      endValueRankWriter.reset();
    }

    if (fieldInfo.docsWithField != maxDoc) {
      auto guard = pool.rewindScopeGuard();
      DocsWithValWriter docsWriter(pool, postingsWriter, fieldInfo);
      for (int32_t slot = 0; slot < nSlots; slot++) {
        if (ords.hasValues(slot)) {
          docsWriter.startDoc(rankToDoc.empty() ? slot : rankToDoc[slot]);
        }
      }
      docsWriter.finish();
    }
  }
};

} // namespace luxir
