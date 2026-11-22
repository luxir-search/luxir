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

class OrdColWriterSettings {
public:
  enum class FormatOverride : uint8_t {
    AUTO,
    DIRECT,
    PREDICTED
  };

protected:
  inline static std::atomic<FormatOverride> formatOverride = FormatOverride::AUTO;
public:
  static FormatOverride setFormatOverrideForTests(FormatOverride value) {
    return formatOverride.exchange(value);
  }
};

// The collector supplies doc runs; the disk codec supports arbitrary ord order.
template <class Collector>
class BasicOrdColWriter : public OrdColWriterSettings {
  using BlockPlan = OrdColumnFormat::PredictedBlockPlan;

  MemPool& pool;
  PostingsWriter& postingsWriter;
  SpaceOrdinal space;
  int32_t flags;
  Collector& ords;
  u_ptr<MonoWriter> endValueRankWriter;
  PostingsWriter::OutputStreamPtr endValueRankOutput;

  bool declaredMultiValued() const {
    return (flags & FieldType::MULTI_VALUED) != 0;
  }

  template <class Acceptor>
  void forEachRankValue(Acceptor&& acceptor) const {
    for (int32_t slot = 0; slot < ords.size(); slot++) {
      ords.pushValues(slot, acceptor);
    }
  }

  template <class Acceptor>
  void forEachEncodedValue(field_parts::OrdIndexing indexing, std::span<const int32_t> rankToDoc,
                           Acceptor&& acceptor) const {
    if (indexing == field_parts::OrdIndexing::RANK) {
      forEachRankValue(acceptor);
      return;
    }

    assert(indexing == field_parts::OrdIndexing::DOC_ID);
    int32_t maxDoc = postingsWriter.getMaxDoc(space);
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

  std::vector<BlockPlan> planPredicted(field_parts::OrdIndexing indexing,
                                       std::span<const int32_t> rankToDoc,
                                       uint64_t& byteSize) const {
    std::vector<BlockPlan> plans;
    std::array<uint32_t, OrdColumnFormat::BLOCK_SIZE> block;
    uint32_t count = 0;
    auto flush = [&]() {
      if (count == 0) return;
      BlockPlan plan = OrdColumnFormat::planBlock(
          std::span<const uint32_t>(block.data(), count));
      byteSize += sizeof(OrdColumnFormat::PredictedBlockInfo);
      if (plan.info.bits != 0) {
        byteSize += LinearPack::packedByteSize(count, plan.info.bits);
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

  void writeDirect(OutputStream& out, field_parts::OrdIndexing indexing,
                   std::span<const int32_t> rankToDoc, uint8_t bits) const {
    LinearPack::Writer writer(out, bits);
    forEachEncodedValue(indexing, rankToDoc,
                        [&](uint32_t ord) { writer.append(ord); });
    writer.finish(false);  // segment data files reserve SVB_OVERREAD_PAD
  }

  int64_t writePredicted(OutputStream& out, field_parts::OrdIndexing indexing,
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

    int64_t indexOffset = out.size() - start;
    for (const BlockPlan& plan : plans) {
      out.write(&plan.info, sizeof(plan.info));
    }
    return indexOffset;
  }

public:
  BasicOrdColWriter(MemPool& pool, PostingsWriter& postingsWriter,
                    SpaceOrdinal space, int32_t flags, Collector& ords)
      : pool(pool), postingsWriter(postingsWriter), space(space), flags(flags), ords(ords) {
    assert(!ords.multiValued() || declaredMultiValued());
    if (ords.multiValued()) {
      endValueRankOutput = postingsWriter.getOutputStream();
      endValueRankWriter = pool.make_unique<MonoWriter>(pool, *endValueRankOutput);
    }
  }

  [[nodiscard]] WrittenColumn<field_parts::OrdColumn> finish(int64_t numTerms, std::span<const int32_t> rankToDoc = {}) {
    int32_t maxDoc = postingsWriter.getMaxDoc(space);
    int32_t nSlots = ords.size();
    assert(rankToDoc.empty() || (int32_t)rankToDoc.size() == nSlots);
    assert(numTerms >= 0 && numTerms <= INT32_MAX);
    bool multi = ords.multiValued();
    assert(!multi || declaredMultiValued());
    assert(multi == (endValueRankWriter != nullptr));
    int32_t docsWithField = ords.docsWithField();
    if (!docsWithField) return {};

    int64_t numValues = 0;
    for (int32_t slot = 0; slot < nSlots; slot++) {
      int64_t before = numValues;
      ords.pushValues(slot, [&](uint32_t ord) {
        assert(ord <= INT32_MAX);
        numValues++;
      });
      if (numValues != before && endValueRankWriter) {
        endValueRankWriter->addInt64(numValues);
      }
    }

    field_parts::OrdIndexing indexing = !multi && useDocIdIndexing(docsWithField, maxDoc)
        ? field_parts::OrdIndexing::DOC_ID : field_parts::OrdIndexing::RANK;
    uint8_t bits = OrdColumnFormat::width(numTerms);
    auto encodedValues = field_parts::OrdColumn::encodedCount(indexing, maxDoc, numValues);
    uint64_t directSize = (uint64_t)OrdColumnFormat::directBytes(encodedValues, bits);
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
    std::optional<int64_t> predictedIndexOffset;
    if (predicted) {
      predictedIndexOffset = writePredicted(out, indexing, rankToDoc, plans, start);
    } else {
      writeDirect(out, indexing, rankToDoc, bits);
    }

    field_parts::Slice endValueRanks;
    if (endValueRankWriter) {
      endValueRanks = endValueRankWriter->finish(postingsWriter, std::move(endValueRankOutput));
      endValueRankWriter.reset();
    }

    WrittenPresence presence{docsWithField, {}};
    if (docsWithField != maxDoc) {
      auto guard = pool.rewindScopeGuard();
      PresenceWriter docsWriter(pool, postingsWriter, space);
      for (int32_t slot = 0; slot < nSlots; slot++) {
        if (ords.hasValues(slot)) {
          docsWriter.startDoc(rankToDoc.empty() ? slot : rankToDoc[slot]);
        }
      }
      presence = docsWriter.finish();
    }
    auto slice = postingsWriter.finishStream(std::move(output));
    auto mode = predicted ? field_parts::OrdMode::PREDICTED : field_parts::OrdMode::DIRECT;
    auto valuesBytes = predictedIndexOffset ? (uint64_t)*predictedIndexOffset : directSize;
    if (predicted) {
      assert(slice.bytes - start - valuesBytes == OrdColumnFormat::metadataBytes(encodedValues));
    }
    return {field_parts::OrdColumn{slice.location.addOffset(start), valuesBytes, mode, indexing},
            numValues, presence, endValueRanks, {}, {}};
  }
};

using OrdColWriter = BasicOrdColWriter<OrdCollector>;

} // namespace luxir
