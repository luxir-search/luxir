// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "luxir/search/SearchOverrides.h"
#include "luxir/search/ops/SearchOp.h"
#include "luxir/search/ops/DomainIter.h"
#include "luxir/search/ops/FacetExecution.h"
#include "luxir/search/ops/StrFacetReplay.h"
#include "luxir/reader/FieldReader.h"
#include "luxir/reader/OrdColReader.h"
#include "luxir/util/NumericUtils.h"
#include "luxir/util/SegmentMergeDriver.h"
#include "luxir/value/AggregateExpr.h"

namespace luxir {

// Expression aggregate over an incoming domain, with ordinary bucket-domain,
// inline string-facet, and selected-column replay execution bindings.
class ExprStatsOp : public SearchOp {
  static constexpr std::string_view FACET_STATE_BREAKER =
      "facet aggregate state";

  AggregateProgram& aggregate;
  const DenseFacetStateOps* denseFacetState = nullptr;

  const DenseFacetStateOps* resolveDenseFacetState() {
    if (disableDenseFacetStateForTests) return nullptr;
    if (aggregate.root().kind != AggregateNodeKind::AGGREGATE
        || aggregate.leaves.size() != 1) {
      return nullptr;
    }
    const AggregateLeaf& leaf = aggregate.leaves.front();
    const ValueNode& input = leaf.input->root();
    if (leaf.resolved.denseFacetState == nullptr
        || input.kind != ValueNodeKind::COLUMN
        || input.type != ValueType::INT64
        || input.columnMultiValued) {
      return nullptr;
    }
    for (auto& segment : req.reader->segments()) {
      auto& postings = segment.postingsReader();
      if (postings.maxDoc() == 0) continue;
      FieldReader reader(postings);
      if (!reader.seek(input.text)) return nullptr;
      SegFieldInfo info;
      reader.readFieldInfo(info);
      if (info.docsWithField != postings.maxDoc()) return nullptr;
    }
    return leaf.resolved.denseFacetState;
  }

  size_t chargeFacetState(size_t preferredBytes, size_t minimumBytes,
                          std::string_view detail) {
    size_t charged = req.memoryTracker.chargeUpTo(
        preferredBytes, minimumBytes, FACET_STATE_BREAKER, detail);
    if (facetAggregateStateReservationCounterForTests != nullptr) {
      facetAggregateStateReservationCounterForTests->fetch_add(
          1, std::memory_order_relaxed);
    }
    return charged;
  }

  void warnFailure(const BucketScalar& result) const {
    if (result.failure == AggregateFailure::NONE) return;
    req.warnOnce("aggregate_eval_failed", fmt::format(
        "aggregate op '{}' emitted null: {}", name,
        aggregateFailureReason(result.failure)));
  }

public:
  ExprStatsOp(SearchRequest& req, std::string_view name,
              AggregateProgram& aggregate)
      : SearchOp(req, name), aggregate(aggregate) {}

  void init() override {
    SearchOp::init();
    denseFacetState = resolveDenseFacetState();
  }

  bool canEmitAsBucketChild() const override { return true; }

  class Calc : public Calculator {
    static constexpr size_t BATCH_SIZE = 1024;

    AggregateEvalScratch scratch;
    SegmentMergeDriver<AggregateAccumulator> driver;

    ExprStatsOp& thisOp() { return (ExprStatsOp&)getOp(); }

    void emitResult(const AggregateAccumulator& accumulator) {
      BucketScalar result = accumulator.finish(scratch);
      thisOp().warnFailure(result);
      auto& mr = op.req.lastResponse->mr;
      getTarget(nullptr, [&](api::Val& val) {
        routeTarget<api::ArrVal>(val, mr, [&](api::Val& target) {
          writeAggregateValue(target, result);
        });
      });
    }

    void accumulateColumn(AggregateAccumulator& accumulator,
                          BoundAggregateInput& binding, DocSet* domain,
                          int32_t maxDoc) {
      const ValueNode& root = binding.leaf->input->root();
      BoundValueNode& bound = binding.values->nodes[
          binding.leaf->input->rootNode];
      if (!bound.column) return;
      std::array<int64_t, BATCH_SIZE> values;
      size_t count = 0;
      auto flush = [&] {
        accumulator.addRawBatch(
            binding.leafIndex,
            std::span<const int64_t>(values.data(), count), root.columnType);
        count = 0;
      };
      int64_t missing = 0;
      forEachIntColValue(
          domain, *bound.column, maxDoc, missing,
          [&](int32_t docid, int64_t value) LUXIR_INLINE {
            unused(docid);
            values[count++] = value;
            if (count == values.size()) flush();
          });
      if (count != 0) flush();
    }

    void accumulateExpression(AggregateAccumulator& accumulator,
                              BoundAggregateInput& binding, DocSet* domain,
                              int32_t maxDoc) {
      std::array<int32_t, BATCH_SIZE> docs;
      std::array<ScalarValueResult, BATCH_SIZE> results;
      size_t count = 0;
      auto flush = [&] {
        std::span<const int32_t> docBatch(docs.data(), count);
        binding.evalBatch(docBatch,
                          std::span<ScalarValueResult>(results.data(), count));
        accumulator.addBatch(
            binding.leafIndex,
            std::span<const ScalarValueResult>(results.data(), count));
        count = 0;
      };
      forEachDomainDoc(domain, maxDoc, [&](int32_t doc) {
        docs[count++] = doc;
        if (count == docs.size()) flush();
      });
      if (count != 0) flush();
    }

  public:
    Calc(SearchOp& op, Calculator* parent, int64_t slot, int64_t numSlots)
        : Calculator(op, parent, slot, numSlots),
          driver(op.req.reader->segments().size(),
                 [this](std::unique_ptr<AggregateAccumulator> accumulator) {
                   emitResult(*accumulator);
                 }) {
      driver.setCreator([this]() {
        return new AggregateAccumulator(thisOp().aggregate);
      });
    }

    api::Val* getTargetForSub(SearchResponse* resp, Calculator* sub) override {
      unused(resp);
      unused(sub);
      return nullptr;
    }

    void calc(oneapi::tbb::task_group* tg, int32_t segnum,
              DomainHandle domainHandle) override {
      unused(tg);
      assert(domainHandle.isDeliverable());
      if (segnum == -1) {
        driver.completeEmpty();
        return;
      }

      driver.contribute([&](AggregateAccumulator& accumulator) {
        try {
          auto poolGuard = MemPool::threadLocalPoolGuard();
          MemPool& pool = poolGuard.pool();
          auto& segment = thisOp().req.reader->segments()[(size_t)segnum];
          std::vector<BoundAggregateInput> bindings = bindAggregateInputs(
              thisOp().aggregate, pool, segment);

          int32_t maxDoc = segment.postingsReader().maxDoc();
          for (BoundAggregateInput& binding : bindings) {
            if (binding.bareColumn) {
              accumulateColumn(
                  accumulator, binding, domainHandle.get(), maxDoc);
            } else {
              accumulateExpression(
                  accumulator, binding, domainHandle.get(), maxDoc);
            }
          }
        } catch (const ValueEvaluationError&) {
          accumulator.fail(AggregateFailure::VALUE_EVALUATION);
        }
      });
    }
  };

  class InlineCalc final : public InlineCalculator {
    std::vector<BoundAggregateInput> bindings;
    std::vector<uint64_t> finalizedValues;
    std::vector<uint64_t> finalizedValid;
    AggregateEvalScratch scratch;
    AggregateFailure segmentFailure = AggregateFailure::NONE;
    size_t trackedFinalizedBytes = 0;
    const DenseFacetStateOps* denseFacetState;
    BucketValueType resultType;
    uint32_t entryBytes;

    ExprStatsOp& thisOp() { return (ExprStatsOp&)getOp(); }

    AggregateStateView state(void* entry) {
      assert(denseFacetState == nullptr);
      return AggregateStateView(thisOp().aggregate, entry);
    }

    BucketScalar finishState(void* entry, int64_t count) {
      BucketScalar result;
      if (denseFacetState != nullptr) {
        result = denseFacetState->finish(entry, count);
        result.nature = thisOp().aggregate.root().nature;
      } else {
        result = state(entry).finish(scratch);
      }
      return result;
    }

    void addGenericDoc(void* entry, int32_t docid) {
      if (segmentFailure != AggregateFailure::NONE) {
        state(entry).fail(segmentFailure);
        return;
      }
      try {
        if (bindings.size() == 1) {
          bindings.front().accumulatePoint(entry, docid);
        } else {
          for (BoundAggregateInput& binding : bindings) {
            binding.accumulatePoint(entry, docid);
          }
        }
      } catch (const ValueEvaluationError&) {
        state(entry).fail(AggregateFailure::VALUE_EVALUATION);
      }
    }

    void addDenseDoc(void* entry, int32_t docid) {
      if (segmentFailure != AggregateFailure::NONE) {
        denseFacetState->fail(entry, segmentFailure);
        return;
      }
      // resolveDenseFacetState proved a scalar column present on every doc.
      // Its value rank is therefore the doc id: skip the generic iterator's
      // missing checks and feed the raw column value straight to packed state.
      assert(bindings.size() == 1);
      auto* iterator = bindings.front().columnIterator;
      assert(iterator != nullptr);
      denseFacetState->accumulateRawPoint(
          entry, iterator->values().valueAt(docid));
    }

    bool valid(size_t slot) const {
      return (finalizedValid[slot / 64] & (uint64_t{1} << (slot % 64))) != 0;
    }

    int compareResults(size_t a, size_t b) const {
      bool aValid = valid(a);
      bool bValid = valid(b);
      if (!aValid || !bValid) return (int)aValid - (int)bValid;
      if (resultType == BucketValueType::DOUBLE) {
        double aValue = std::bit_cast<double>(finalizedValues[a]);
        double bValue = std::bit_cast<double>(finalizedValues[b]);
        return aValue < bValue ? -1 : (aValue > bValue ? 1 : 0);
      }
      int64_t aValue = std::bit_cast<int64_t>(finalizedValues[a]);
      int64_t bValue = std::bit_cast<int64_t>(finalizedValues[b]);
      return aValue < bValue ? -1 : (aValue > bValue ? 1 : 0);
    }

    size_t finalizedCapacityBytes() const {
      return (finalizedValues.capacity() + finalizedValid.capacity())
          * sizeof(uint64_t);
    }

    void trackFinalizedCapacity() {
      size_t bytes = finalizedCapacityBytes();
      if (bytes == trackedFinalizedBytes
          || inlineAggregateStatsForTests == nullptr) {
        trackedFinalizedBytes = bytes;
        return;
      }
      auto& stats = *inlineAggregateStatsForTests;
      size_t current = bytes > trackedFinalizedBytes
          ? stats.finalizedBytes.fetch_add(
                bytes - trackedFinalizedBytes, std::memory_order_relaxed)
                + bytes - trackedFinalizedBytes
          : stats.finalizedBytes.fetch_sub(
                trackedFinalizedBytes - bytes, std::memory_order_relaxed)
                - (trackedFinalizedBytes - bytes);
      size_t peak = stats.peakFinalizedBytes.load(std::memory_order_relaxed);
      while (peak < current
             && !stats.peakFinalizedBytes.compare_exchange_weak(
                 peak, current, std::memory_order_relaxed)) {}
      trackedFinalizedBytes = bytes;
    }

  public:
    InlineCalc(SearchOp& op, Calculator* parent, int64_t slot,
               int64_t numSlots)
        : InlineCalculator(op, parent, slot, numSlots),
          denseFacetState(static_cast<ExprStatsOp&>(op).denseFacetState),
          resultType(static_cast<ExprStatsOp&>(op).aggregate.root().type),
          entryBytes(denseFacetState != nullptr
              ? denseFacetState->bytes
              : AggregateStateView::bytes(
                    static_cast<ExprStatsOp&>(op).aggregate)) {
      if (inlineAggregateStatsForTests != nullptr) {
        inlineAggregateStatsForTests->stateBytesPerBucket.store(
            entryBytes, std::memory_order_relaxed);
      }
    }

    ~InlineCalc() override {
      if (trackedFinalizedBytes != 0
          && inlineAggregateStatsForTests != nullptr) {
        inlineAggregateStatsForTests->finalizedBytes.fetch_sub(
            trackedFinalizedBytes, std::memory_order_relaxed);
      }
    }

    uint32_t fixedEntryBytes() const override { return entryBytes; }

    void insert(void* entry, int32_t docid) override {
      if (denseFacetState != nullptr) {
        denseFacetState->init(entry);
      } else {
        state(entry).init();
      }
      if (denseFacetState != nullptr) addDenseDoc(entry, docid);
      else addGenericDoc(entry, docid);
    }

    void update(void* entry, int32_t docid) override {
      if (denseFacetState != nullptr) addDenseDoc(entry, docid);
      else addGenericDoc(entry, docid);
    }

    void merge(void* target, void* from) override {
      if (denseFacetState != nullptr) {
        denseFacetState->merge(target, from);
      } else {
        state(target).merge(state(from));
      }
    }

    void mergeNew(void* target, void* from) override {
      if (denseFacetState != nullptr) {
        denseFacetState->init(target);
        denseFacetState->merge(target, from);
      } else {
        state(target).init();
        state(target).merge(state(from));
      }
    }

    void beginFinalize(size_t entries) override {
      finalizedValues.clear();
      finalizedValues.reserve(entries);
      finalizedValid.assign((entries + 63) / 64, 0);
      trackFinalizedCapacity();
    }

    void finalize(void* entry, int64_t count) override {
      BucketScalar result = finishState(entry, count);
      size_t slot = finalizedValues.size();
      if (result.valid) {
        finalizedValid[slot / 64] |= uint64_t{1} << (slot % 64);
      }
      finalizedValues.push_back(result.type == BucketValueType::DOUBLE
          ? std::bit_cast<uint64_t>(result.doubleValue)
          : (uint64_t)(int64_t)result.intValue);
    }

    int compare(size_t a, size_t b) override {
      return compareResults(a, b);
    }

    bool isMissing(size_t entry) override {
      return !valid(entry);
    }

    void startSeg(int32_t segnum) override {
      bindings.clear();
      segmentFailure = AggregateFailure::NONE;
      try {
        MemPool& pool = MemPool::threadLocal();
        auto& segment = thisOp().req.reader->segments()[(size_t)segnum];
        bindings = bindAggregateInputs(thisOp().aggregate, pool, segment);
      } catch (const ValueEvaluationError&) {
        bindings.clear();
        segmentFailure = AggregateFailure::VALUE_EVALUATION;
      }
    }

    void endSeg(int32_t segnum) override {
      unused(segnum);
      bindings.clear();
      segmentFailure = AggregateFailure::NONE;
    }

    void fillResult(std::span<char*> entries,
                    std::span<const int64_t> counts) override {
      assert(entries.size() == counts.size());

      auto& mr = op.req.lastResponse->mr;
      auto* value = getTarget(nullptr, [&](api::Val& target) {
        auto& arr = oneofMut<api::ArrVal>(target);
        if (arr.v.empty()) build::allocArray(arr.v, entries.size(), mr);
      });
      auto& arr = oneofMut<api::ArrVal>(*value);
      for (size_t i = 0; i < entries.size(); i++) {
        BucketScalar result = entries[i] == nullptr
            ? BucketScalar::missing(
                  resultType, thisOp().aggregate.root().nature)
            : finishState(entries[i], counts[i]);
        thisOp().warnFailure(result);
        writeAggregateValue(const_cast<api::Val&>(arr.v[i]), result);
        if (entries[i] != nullptr) entries[i] += entryBytes;
      }
    }
  };

  class ColumnReplayExecutor final : public FacetChildExecutor {
    ExprStatsOp& child;
    SearchOp::Calculator& parent;
    StringFacetColumnSource source;
    StrFacetSelectedOrdMap::Selected selectedBuckets;
    std::vector<std::byte> states;
    std::string chargeDetail;
    AggregateEvalScratch scratch;
    size_t chargedBytes = 0;
    uint32_t stride;

    void releaseStates() {
      if (chargedBytes == 0) return;
      std::vector<std::byte>().swap(states);
      child.req.memoryTracker.release(chargedBytes);
      chargedBytes = 0;
    }

    AggregateStateView state(int32_t owner) {
      assert(owner >= 0 && owner < (int32_t)source.buckets.size());
      return AggregateStateView(
          child.aggregate, states.data() + (size_t)owner * stride);
    }

    void* stateStorage(int32_t owner) {
      assert(owner >= 0 && owner < (int32_t)source.buckets.size());
      return states.data() + (size_t)owner * stride;
    }

    void failOwners(std::span<const int32_t> owners) {
      for (int32_t owner : owners) {
        state(owner).fail(AggregateFailure::VALUE_EVALUATION);
      }
    }

    void replaySegment(int32_t segnum) {
      static constexpr size_t BATCH_SIZE = 256;
      auto poolGuard = MemPool::threadLocalPoolGuard();
      auto& segment = child.req.reader->segments()[(size_t)segnum];
      auto& postings = segment.postingsReader();
      int32_t maxDoc = postings.maxDoc();

      FieldReader parentReader(postings);
      if (!parentReader.seek(source.field)) return;
      SegFieldInfo parentInfo;
      parentReader.readFieldInfo(parentInfo);
      OrdColReader parentColumn(postings, parentInfo);
      auto mapping = source.ordMap.getSegToGlobal(segnum);
      StrFacetSelectedOrdMap selected(mapping, selectedBuckets);

      std::vector<BoundAggregateInput> bindings;
      bool bindingFailed = false;
      try {
        bindings = bindAggregateInputs(
            child.aggregate, poolGuard.pool(), segment);
      } catch (const ValueEvaluationError&) {
        bindings.clear();
        bindingFailed = true;
      }
      std::array<int32_t, BATCH_SIZE> docs;
      std::array<size_t, BATCH_SIZE + 1> ownerOffsets;
      std::vector<int32_t> ownerIds;
      std::array<ScalarValueResult, BATCH_SIZE> values;
      std::vector<std::vector<ScalarValueResult>> ownerValues(
          source.buckets.size());
      ownerIds.reserve(BATCH_SIZE);
      size_t docCount = 0;

      auto flush = [&] {
        if (docCount == 0) return;
        std::span<const int32_t> docBatch(docs.data(), docCount);
        for (BoundAggregateInput& binding : bindings) {
          try {
            binding.evalBatch(docBatch,
                              std::span<ScalarValueResult>(values.data(),
                                                           docCount));
          } catch (const ValueEvaluationError&) {
            for (size_t i = 0; i < docCount; i++) {
              try {
                ValueResult value = binding.evalPoint(docs[i]);
                if (!value.valid) {
                  values[i] = ScalarValueResult{};
                } else if (binding.leaf->input->root().type
                           == ValueType::DOUBLE) {
                  values[i] = ScalarValueResult::floating(value.doubleValue);
                } else {
                  values[i] = ScalarValueResult::integer(value.intValue);
                }
              } catch (const ValueEvaluationError&) {
                values[i] = ScalarValueResult{};
                failOwners(std::span<const int32_t>(
                    ownerIds.data() + ownerOffsets[i],
                    ownerOffsets[i + 1] - ownerOffsets[i]));
              }
            }
          }
          for (std::vector<ScalarValueResult>& routed : ownerValues) {
            routed.clear();
          }
          for (size_t i = 0; i < docCount; i++) {
            for (size_t owner = ownerOffsets[i];
                 owner < ownerOffsets[i + 1]; owner++) {
              ownerValues[(size_t)ownerIds[owner]].push_back(values[i]);
            }
          }
          for (size_t owner = 0; owner < ownerValues.size(); owner++) {
            if (!ownerValues[owner].empty()) {
              binding.accumulateBatch(
                  stateStorage((int32_t)owner), ownerValues[owner]);
            }
          }
        }
        docCount = 0;
        ownerIds.clear();
      };

      selected.visit([&](const auto& selectedMap) {
        DocSet* domain = source.domains[(size_t)segnum].get();
        forEachSelectedBucketDoc(
            domain, parentColumn, maxDoc, selectedMap,
            [&](int32_t docid,
                std::span<const int32_t> owners) LUXIR_INLINE {
              if (bindingFailed) {
                failOwners(owners);
              } else {
                if (docCount == docs.size()) flush();
                docs[docCount] = docid;
                ownerOffsets[docCount] = ownerIds.size();
                ownerIds.insert(ownerIds.end(), owners.begin(), owners.end());
                docCount++;
                ownerOffsets[docCount] = ownerIds.size();
              }
            });
      });
      flush();
    }

    class ResultWriter final : public SearchOp::Calculator {
      ExprStatsOp& child;

    public:
      ResultWriter(ExprStatsOp& child, SearchOp::Calculator& parent,
                   int64_t numSlots)
          : Calculator(child, &parent, -1, numSlots), child(child) {}

      api::Val* getTargetForSub(SearchResponse* resp,
                                Calculator* sub) override {
        unused(resp);
        unused(sub);
        return nullptr;
      }

      void calc(oneapi::tbb::task_group* tg, int32_t segnum,
                DomainHandle domain) override {
        unused(tg);
        unused(segnum);
        unused(domain);
        assert(false);
      }

      void write(std::span<const BucketScalar> results,
                 std::span<const SelectedFacetBucket<std::string_view>> buckets) {
        for (const BucketScalar& result : results) child.warnFailure(result);
        auto& mr = child.req.lastResponse->mr;
        auto* value = getTarget(nullptr, [&](api::Val& target) {
          auto& arr = oneofMut<api::ArrVal>(target);
          if (arr.v.empty()) build::allocArray(arr.v, buckets.size(), mr);
        });
        auto& arr = oneofMut<api::ArrVal>(*value);
        for (const auto& bucket : buckets) {
          assert(bucket.owner.value >= 0);
          assert(bucket.output.value >= 0);
          writeAggregateValue(
              const_cast<api::Val&>(arr.v[(size_t)bucket.output.value]),
              results[(size_t)bucket.owner.value]);
        }
      }
    };

  public:
    ColumnReplayExecutor(ExprStatsOp& child, SearchOp::Calculator& parent,
                         const StringFacetColumnSource& source)
        : child(child), parent(parent), source(source),
          selectedBuckets(StrFacetSelectedOrdMap::selectedBuckets(source.buckets)),
          chargeDetail(fmt::format("facet '{}' metric '{}'",
                                   parent.getOp().name, child.name)),
          stride(AggregateStateView::bytes(child.aggregate)) {}

    ~ColumnReplayExecutor() override {
      releaseStates();
    }

    FacetFeedKind feedKind() const override {
      return FacetFeedKind::STRING_COLUMN_REPLAY;
    }

    // Capability only. AUTO stays on bucket domains until the aggregate replay
    // benchmark establishes a crossover rule.
    bool dominatesBucketDomains() const override { return false; }

    void execute() override {
      size_t numOwners = source.buckets.size();
      if (numOwners > std::numeric_limits<size_t>::max() / stride) {
        child.req.memoryTracker.chargeOverflow(
            FACET_STATE_BREAKER, chargeDetail);
      }
      size_t bytes = numOwners * stride;
      if (bytes != 0) {
        chargedBytes = child.chargeFacetState(bytes, bytes, chargeDetail);
      }
      states.resize(chargedBytes);
      for (size_t owner = 0; owner < numOwners; owner++) {
        state((int32_t)owner).init();
      }

      for (int32_t segnum = 0; segnum < (int32_t)source.domains.size();
           segnum++) {
        replaySegment(segnum);
      }

      std::vector<BucketScalar> results;
      results.reserve(numOwners);
      for (size_t owner = 0; owner < numOwners; owner++) {
        results.push_back(state((int32_t)owner).finish(scratch));
      }
      ResultWriter(child, parent, (int64_t)numOwners)
          .write(results, source.buckets);
      releaseStates();
    }
  };

  Calculator* createCalculator(Calculator* parent, int64_t slot = -1,
                               int64_t numSlots = -1) override {
    return new Calc(*this, parent, slot, numSlots);
  }

  InlineCalculator* createInlineCalculator(
      Calculator* parent, int64_t slot = -1,
      int64_t numSlots = -1) override {
    return new InlineCalc(*this, parent, slot, numSlots);
  }

  bool canInline() override { return true; }

  size_t facetBucketResidentBytes() const override {
    return AggregateStateView::bytes(aggregate);
  }

  FacetChildExecutor* bindFacetChild(
      const FacetChildContext& context) override {
    if (context.stringColumn == nullptr) return nullptr;
    for (const auto& bucket : context.stringColumn->buckets) {
      if (!bucket.id.has_value()) return nullptr;
    }
    return new ColumnReplayExecutor(*this, context.parent,
                                    *context.stringColumn);
  }
};

} // namespace luxir
