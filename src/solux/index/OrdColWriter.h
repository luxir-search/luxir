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
#include "solux/codec/LinearPack.h"
#include "solux/codec/OrdColumnFormat.h"

namespace solux {

class OrdColWriter {
public:
  enum class FormatOverride : uint8_t {
    AUTO,
    DIRECT,
    PREDICTED
  };

private:
  struct BlockPlan {
    OrdColumnFormat::PredictedBlockInfo info;
    uint32_t count = 0;
  };

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

  static BlockPlan planBlock(std::span<const uint32_t> values) {
    assert(!values.empty() && values.size() <= OrdColumnFormat::BLOCK_SIZE);

    auto evaluate = [&](int64_t intercept, int64_t scaledSlope) {
      int64_t minError = INT64_MAX;
      int64_t maxError = INT64_MIN;
      for (uint32_t i = 0; i < values.size(); i++) {
        int64_t predicted = intercept +
            (((int64_t)i * scaledSlope) >> OrdColumnFormat::SLOPE_SHIFT);
        int64_t error = (int64_t)values[i] - predicted;
        minError = std::min(minError, error);
        maxError = std::max(maxError, error);
      }
      OrdColumnFormat::PredictedBlockInfo info{};
      info.intercept = intercept + minError;
      info.scaledSlope = scaledSlope;
      info.bits = (uint8_t)std::bit_width((uint64_t)(maxError - minError));
      return info;
    };

    auto constant = evaluate(values.front(), 0);
    int64_t scaledSlope = 0;
    if (values.size() > 1) {
      scaledSlope = ((int64_t)values.back() - (int64_t)values.front()) *
                    ((int64_t)1 << OrdColumnFormat::SLOPE_SHIFT) /
                    (int64_t)(values.size() - 1);
    }
    auto linear = evaluate(values.front(), scaledSlope);

    BlockPlan plan;
    plan.info = linear.bits < constant.bits ? linear : constant;
    plan.count = (uint32_t)values.size();
    return plan;
  }

  std::vector<BlockPlan> planPredicted(int32_t indexing,
                                       std::span<const int32_t> rankToDoc,
                                       uint64_t& byteSize) const {
    std::vector<BlockPlan> plans;
    std::array<uint32_t, OrdColumnFormat::BLOCK_SIZE> block;
    uint32_t count = 0;
    auto flush = [&]() {
      if (count == 0) return;
      BlockPlan plan = planBlock(std::span<const uint32_t>(block.data(), count));
      byteSize += sizeof(OrdColumnFormat::PredictedBlockInfo);
      if (plan.info.bits != 0) {
        byteSize += LinearPack::byteSize(count, plan.info.bits);
      }
      plans.push_back(plan);
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
    writer.finish();
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
          int64_t predicted = plan.info.intercept +
              (((int64_t)i * plan.info.scaledSlope) >> OrdColumnFormat::SLOPE_SHIFT);
          int64_t residual = (int64_t)block[i] - predicted;
          assert(residual >= 0 && (uint64_t)residual <=
                 LinearPack::mask32(plan.info.bits));
          writer.append((uint64_t)residual);
        }
        writer.finish();
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
    uint64_t directSize = LinearPack::byteSize((uint64_t)encodedValues, bits);
    uint64_t predictedSize = 0;
    std::vector<BlockPlan> plans = planPredicted(indexing, rankToDoc, predictedSize);

    FormatOverride override = formatOverride.load();
    bool predicted = override == FormatOverride::PREDICTED ||
        (override == FormatOverride::AUTO && predictedSize <= directSize / 2 &&
         directSize >= predictedSize + 4096);
    if (override == FormatOverride::DIRECT) predicted = false;

    auto output = postingsWriter.getOutputStream();
    OutputStream& out = *output;
    uint64_t start = out.size();
    fieldInfo.columnLoc = out.slocation();
    fieldInfo.columnMetaOff = 0;
    fieldInfo.ordFormat = predicted ? SegFieldInfo::ORD_PREDICTED
                                    : SegFieldInfo::ORD_DIRECT;
    fieldInfo.ordIndexing = indexing;
    fieldInfo.ordBits = bits;
    if (predicted) {
      writePredicted(out, indexing, rankToDoc, plans, start);
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

} // namespace solux
