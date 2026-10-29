// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "luxir/search/SearchOverrides.h"
#include "luxir/search/DateOutput.h"
#include "luxir/search/ops/SearchOp.h"
#include "luxir/search/ops/DomainIter.h"
#include "luxir/search/ops/FacetExecution.h"
#include "luxir/search/ops/StrFacetReplay.h"
#include "luxir/reader/PostingsReader.h"
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
  std::span<const AggregateFieldInfos> fields;
  const DenseFacetStateOps* denseFacetState = nullptr;
  bool orderSensitive = false;
  bool passive = false;
  std::vector<ExprStatsOp*> members;

  const DenseFacetStateOps* resolveDenseFacetState() {
    if (disableDenseFacetStateForTests) return nullptr;
    if (aggregate.root().kind != AggregateNodeKind::AGGREGATE
        || aggregate.leaves.size() != 1) {
      return nullptr;
    }
    const AggregateProducer& producer = aggregate.producers[aggregate.leaves.front().producer];
    const ValueNode& input = producer.input->root();
    if (producer.resolved.denseFacetState == nullptr
        || input.kind != ValueNodeKind::COLUMN
        || input.type != ValueType::INT64
        || input.columnMultiValued) {
      return nullptr;
    }
    auto infos = fields[aggregate.leaves.front().producer].values[producer.input->rootNode];
    for (auto& segment : req.reader->segments()) {
      auto& postings = segment.postingsReader();
      if (postings.maxDoc() == 0) continue;
      auto* field = infos.at((size_t)segment.ord);
      if (!field) return nullptr;
      const auto& info = *field;
      if (info.docsWithField != postings.maxDoc()) return nullptr;
    }
    return producer.resolved.denseFacetState;
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

  template <typename Output>
  static void writeOutputValue(api::Val& target, const BucketScalar& value, const Output& output) {
    if constexpr (std::is_same_v<Output, NativeDateOutput>) {
      writeAggregateValue(target, value);
    } else if (!value.valid) {
      if constexpr (std::is_same_v<Output, IsoDateOutput>) {
        if (value.failure == AggregateFailure::NON_FINITE_SUM
            || value.failure == AggregateFailure::NON_FINITE_AVERAGE
            || value.failure == AggregateFailure::NON_FINITE_EXPRESSION) {
          throw RequestError("date_iso output for " + output.source + " is non-finite");
        }
      }
      target.kind = google::protobuf::NullValue::NULL_VALUE;
    } else if (value.type == BucketValueType::DOUBLE) {
      target.kind = output(value.doubleValue);
    } else {
      target.kind = output((int64_t)value.intValue);
    }
  }

  void warnFailure(const BucketScalar& result) const {
    if (result.failure == AggregateFailure::NONE) return;
    req.warnOnce("aggregate_eval_failed", fmt::format(
        "aggregate op '{}' emitted null: {}", name,
        aggregateFailureReason(result.failure)));
  }

public:
  ExprStatsOp(SearchRequest& req, std::string_view name,
              AggregateProgram& aggregate, std::span<const AggregateFieldInfos> fields)
      : SearchOp(req, name), aggregate(aggregate), fields(fields) {}

  template <typename Emit>
  void withOutput(uint64_t outputFlags, std::pmr::memory_resource& mr, Emit emit) const {
    withDateOutput(aggregate.root().nature, outputFlags, mr, [this] {
      std::string sources;
      for (const auto& producer : aggregate.producers) {
        auto description = producer.input->describeDateSources(producer.input->rootNode);
        if (description.empty()) continue;
        if (!sources.empty()) sources += ", ";
        sources += description;
      }
      return sources;
    }, emit);
  }

  bool hasOnlyIndirectProducers() const {
    return aggregate.hasIndirectStates
        && std::ranges::all_of(aggregate.producers, [](const AggregateProducer& producer) {
          return producer.resolved.state.destroy != nullptr;
        });
  }

  bool shareProducersFrom(ExprStatsOp& leader) {
    assert(!passive && members.empty() && !leader.passive && &leader != this);
    if (!aggregate.adoptProducers(leader.aggregate)) return false;
    fields = leader.fields;
    if (leader.members.empty()) leader.members.push_back(&leader);
    leader.members.push_back(this);
    passive = true;
    if (aggregateExecutionStatsForTests != nullptr) {
      aggregateExecutionStatsForTests->followers.fetch_add(1, std::memory_order_relaxed);
    }
    return true;
  }

  void init() override {
    SearchOp::init();
    denseFacetState = resolveDenseFacetState();
    orderSensitive = std::ranges::any_of(aggregate.producers, [](const AggregateProducer& producer) {
      return producer.resolved.state.orderSensitive;
    });
  }

  bool canEmitAsBucketChild() const override { return true; }

  template <bool Ordered>
  class Calc : public Calculator {
    static constexpr size_t BATCH_SIZE = 1024;

    AggregateEvalScratch scratch;
    // The driver destroys every accumulator before this resource is released.
    std::optional<RequestMemResource> stateResource;
    size_t observedStateBytes = 0;
    SegmentMergeDriver<AggregateAccumulator> driver;

    ExprStatsOp& thisOp() { return (ExprStatsOp&)getOp(); }

    void emitResult(const AggregateAccumulator& accumulator) {
      // Completion frees the accumulator before the bucket sizing hook runs.
      auto& mr = op.req.lastResponse->mr;
      auto emit = [&](ExprStatsOp& member, const BucketScalar& result) {
        member.warnFailure(result);
        member.withOutput(member.flags, mr, [&](auto output) {
          getTarget(nullptr, member, [&](api::Val& val) {
            routeTarget<api::ArrVal>(val, mr, [&](api::Val& target) {
              writeOutputValue(target, result, output);
            });
          });
        });
      };
      if (thisOp().members.empty()) emit(thisOp(), accumulator.finish(scratch));
      else for (ExprStatsOp* member : thisOp().members) {
        emit(*member, accumulator.finish(member->aggregate, scratch));
      }
      if (stateResource) observedStateBytes = stateResource->bytes();
      if (aggregateExecutionStatsForTests != nullptr) {
        aggregateExecutionStatsForTests->finishedStateBytes.fetch_add(
            stateResource ? stateResource->bytes() : 0, std::memory_order_relaxed);
      }
    }

    void accumulateColumn(AggregateAccumulator& accumulator,
                          BoundAggregateInput& binding, DocSet* domain,
                          int32_t maxDoc) {
      const ValueNode& root = binding.producer->input->root();
      BoundValueNode& bound = binding.values->nodes[
          binding.producer->input->rootNode];
      if (!bound.column) return;
      std::array<int64_t, BATCH_SIZE> values;
      size_t count = 0;
      auto flush = [&] {
        accumulator.addRawBatch(
            binding.producerIndex,
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
      if (!binding.scalar()) {
        forEachDomainDoc(domain, maxDoc, [&](int32_t doc) {
          binding.forEachValue(doc, [&](const ValueResult& value) {
            accumulator.add(binding.producerIndex, value);
          });
        });
        return;
      }
      std::array<int32_t, BATCH_SIZE> docs;
      std::array<ScalarValueResult, BATCH_SIZE> results;
      size_t count = 0;
      auto flush = [&] {
        std::span<const int32_t> docBatch(docs.data(), count);
        binding.evalBatch(docBatch,
                          std::span<ScalarValueResult>(results.data(), count));
        accumulator.addBatch(
            binding.producerIndex,
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
    Calc(SearchOp& op, Calculator* parent, int64_t slot, int64_t numSlots,
        const StructuralConstraint* structural = nullptr)
        : Calculator(op, parent, slot, numSlots, structural),
          driver(op.req.reader->segments().size(),
                 [this](std::unique_ptr<AggregateAccumulator> accumulator) {
                   emitResult(*accumulator);
                   if (aggregateExecutionStatsForTests != nullptr) {
                     accumulator.reset();
                     aggregateExecutionStatsForTests->retainedStateBytes.fetch_add(
                         stateResource ? stateResource->bytes() : 0, std::memory_order_relaxed);
                   }
                 }) {
      if (thisOp().aggregate.hasIndirectStates) {
        stateResource.emplace(op.req.memoryTracker, fmt::format("metric '{}'", op.name),
                              FACET_STATE_BREAKER);
      }
      driver.setCreator([this]() {
        if (aggregateExecutionStatsForTests != nullptr) {
          aggregateExecutionStatsForTests->producerStates.fetch_add(
              thisOp().aggregate.producers.size(), std::memory_order_relaxed);
        }
        return new AggregateAccumulator(thisOp().aggregate,
            stateResource ? &*stateResource : nullptr);
      });
      if constexpr (Ordered) {
        driver.setOrdered(stateResource ? &*stateResource : std::pmr::get_default_resource());
      }
    }

    size_t facetBucketObservedBytes() const override {
      return saturatingAdd(op.facetBucketResidentBytes(),
          stateResource ? std::max(observedStateBytes, stateResource->bytes()) : 0);
    }

    api::Val* getTargetForSub(SearchResponse* resp, SearchOp& sub) override {
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

      auto accumulate = [&](AggregateAccumulator& accumulator) {
        try {
          auto poolGuard = MemPool::threadLocalPoolGuard();
          MemPool& pool = poolGuard.pool();
          auto& segment = thisOp().req.reader->segments()[(size_t)segnum];
          std::vector<BoundAggregateInput> bindings = bindAggregateInputs(
              thisOp().aggregate, pool, segment, thisOp().fields);

          int32_t maxDoc = segment.maxDoc();
          for (BoundAggregateInput& binding : bindings) {
            if (binding.columnScan) {
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
      };
      if constexpr (Ordered) driver.contribute((size_t)segnum, accumulate);
      else driver.contribute(accumulate);
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
    bool keyedBatches;

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
          entryBytes(op.inlineEntryBytes()),
          keyedBatches(static_cast<ExprStatsOp&>(op).inlinesAsColumnBatch()
              && (denseFacetState == nullptr
                  || denseFacetState->accumulateKeyedRawBatch != nullptr)) {
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

    bool acceptsKeyedBatches() const override { return keyedBatches; }

    void initEntry(void* entry) override {
      if (denseFacetState != nullptr) {
        denseFacetState->init(entry);
      } else {
        state(entry).init();
      }
    }

    void updateKeyedBatch(char* states, size_t stride,
                          std::span<const int64_t> keys,
                          std::span<const int32_t> docs) override {
      assert(keyedBatches);
      if (segmentFailure != AggregateFailure::NONE) {
        for (int64_t key : keys) {
          void* entry = states + (size_t)key * stride;
          if (denseFacetState != nullptr) {
            denseFacetState->fail(entry, segmentFailure);
          } else {
            state(entry).fail(segmentFailure);
          }
        }
        return;
      }
      // Values are staged in stack chunks: stores into members could alias
      // the column iterator's own state and force reloads per document.
      static constexpr size_t CHUNK = 256;
      int64_t chunkKeys[CHUNK];
      int64_t chunkValues[CHUNK];
      for (BoundAggregateInput& binding : bindings) {
        assert(binding.bareColumn);
        for (size_t start = 0; start < docs.size(); start += CHUNK) {
          size_t end = std::min(docs.size(), start + CHUNK);
          const int64_t* runKeys = keys.data() + start;
          size_t present = end - start;
          if (denseFacetState != nullptr) {
            // Present on every doc (resolveDenseFacetState): rank is doc id.
            auto& column = binding.columnIterator->values();
            for (size_t i = start; i < end; i++) {
              chunkValues[i - start] = column.valueAt(docs[i]);
            }
          } else {
            present = 0;
            for (size_t i = start; i < end; i++) {
              int64_t raw;
              if (binding.readRawPoint(docs[i], raw)) {
                chunkKeys[present] = keys[i];
                chunkValues[present++] = raw;
              }
            }
            runKeys = chunkKeys;
          }
          if (present == 0) continue;
          std::span<const int64_t> keySpan(runKeys, present);
          std::span<const int64_t> valueSpan(chunkValues, present);
          if (denseFacetState != nullptr) {
            denseFacetState->accumulateKeyedRawBatch(
                states, stride, keySpan, valueSpan);
          } else {
            binding.producer->resolved.state.accumulateKeyedRawBatch(
                states + binding.producerStateOffset, stride, keySpan,
                valueSpan, binding.columnType, binding.arguments);
          }
        }
      }
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

    api::Val finalizedValue(size_t slot) const;

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
        bindings = bindAggregateInputs(thisOp().aggregate, pool, segment, thisOp().fields);
      } catch (const ValueEvaluationError&) {
        bindings.clear();
        segmentFailure = AggregateFailure::VALUE_EVALUATION;
      }
    }

    void LUXIR_NOINLINE rewind() override {
      // Bare-column inputs bypass the expression reader's restart detection.
      // Rebuild only those iterators when the driver starts another doc walk.
      for (BoundAggregateInput& binding : bindings) {
        if (binding.columnIterator == nullptr) continue;
        auto& node = binding.values->nodes[binding.producer->input->rootNode];
        node.bulkIterator.reset();
        node.bulkIterator.emplace(*node.column);
        binding.columnIterator = &*node.bulkIterator;
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
      thisOp().withOutput(thisOp().flags, mr, [&](auto output) {
        for (size_t i = 0; i < entries.size(); i++) {
          BucketScalar result = entries[i] == nullptr
              ? BucketScalar::missing(resultType, thisOp().aggregate.root().nature)
              : finishState(entries[i], counts[i]);
          thisOp().warnFailure(result);
          writeOutputValue(const_cast<api::Val&>(arr.v[i]), result, output);
          if (entries[i] != nullptr) entries[i] += entryBytes;
        }
      });
    }
  };

  class ColumnReplayExecutor final : public FacetChildExecutor {
    ExprStatsOp& child;
    SearchOp::Calculator& parent;
    StringFacetColumnSource source;
    StrFacetSelectedOrdMap::Selected selectedBuckets;
    std::string chargeDetail;
    std::optional<RequestMemResource> stateResource;
    std::vector<std::byte> states;
    std::vector<std::byte> mergedStates;
    AggregateEvalScratch scratch;
    size_t chargedBytes = 0;
    size_t initializedStates = 0;
    size_t initializedMergedStates = 0;
    uint32_t stride;

    void releaseStates() {
      if (stateResource) {
        while (initializedStates != 0) state((int32_t)--initializedStates).destroy();
        while (initializedMergedStates != 0) {
          AggregateStateView(child.aggregate,
              mergedStates.data() + --initializedMergedStates * stride).destroy();
        }
      }
      initializedStates = 0;
      initializedMergedStates = 0;
      std::vector<std::byte>().swap(states);
      std::vector<std::byte>().swap(mergedStates);
      if (chargedBytes != 0) child.req.memoryTracker.release(chargedBytes);
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

      auto* info = source.segInfos.at((size_t)segnum);
      if (!info) return;
      const auto& parentInfo = *info;
      OrdColReader parentColumn(postings, parentInfo);
      auto mapping = source.ordMap.getSegToGlobal(segnum);
      StrFacetSelectedOrdMap selected(mapping, selectedBuckets);

      std::vector<BoundAggregateInput> bindings;
      bool bindingFailed = false;
      try {
        bindings = bindAggregateInputs(
            child.aggregate, poolGuard.pool(), segment, child.fields);
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
          if (!binding.scalar()) {
            for (size_t i = 0; i < docCount; i++) {
              auto owners = std::span<const int32_t>(ownerIds).subspan(
                  ownerOffsets[i], ownerOffsets[i + 1] - ownerOffsets[i]);
              try {
                binding.forEachValue(docs[i], [&](const ValueResult& value) {
                  for (int32_t owner : owners) state(owner).add(binding.producerIndex, value);
                });
              } catch (const ValueEvaluationError&) {
                failOwners(owners);
              }
            }
            continue;
          }
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
                } else if (binding.producer->input->root().type
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
            domain, parentColumn, maxDoc, selectedMap, child.req.memoryTracker, child.name,
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
                                SearchOp& sub) override {
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
        child.withOutput(child.flags, mr, [&](auto output) {
          for (const auto& bucket : buckets) {
            assert(bucket.owner.value >= 0);
            assert(bucket.output.value >= 0);
            writeOutputValue(const_cast<api::Val&>(arr.v[(size_t)bucket.output.value]),
                             results[(size_t)bucket.owner.value], output);
          }
        });
      }
    };

  public:
    ColumnReplayExecutor(ExprStatsOp& child, SearchOp::Calculator& parent,
                         const StringFacetColumnSource& source)
        : child(child), parent(parent), source(source),
          selectedBuckets(StrFacetSelectedOrdMap::selectedBuckets(source.buckets)),
          chargeDetail(fmt::format("facet '{}' metric '{}'",
                                   parent.getOp().name, child.name)),
          stride(AggregateStateView::bytes(child.aggregate)) {
      if (child.aggregate.hasIndirectStates) {
        stateResource.emplace(child.req.memoryTracker, chargeDetail, FACET_STATE_BREAKER);
      }
    }

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
      bool ordered = child.orderSensitive && source.domains.size() > 1;
      size_t arrays = ordered ? 2 : 1;
      if (numOwners > std::numeric_limits<size_t>::max() / stride / arrays) {
        child.req.memoryTracker.chargeOverflow(
            FACET_STATE_BREAKER, chargeDetail);
      }
      size_t bytes = numOwners * stride;
      if (bytes != 0) {
        chargedBytes = child.chargeFacetState(bytes * arrays, bytes * arrays, chargeDetail);
      }
      states.resize(bytes);
      if (ordered) mergedStates.resize(bytes);
      for (size_t owner = 0; owner < numOwners; owner++) {
        state((int32_t)owner).init(stateResource ? &*stateResource : nullptr);
        initializedStates++;
      }
      if (aggregateExecutionStatsForTests != nullptr) {
        aggregateExecutionStatsForTests->producerStates.fetch_add(
            numOwners * child.aggregate.producers.size(), std::memory_order_relaxed);
      }

      for (int32_t segnum = 0; segnum < (int32_t)source.domains.size();
           segnum++) {
        replaySegment(segnum);
        if (ordered) {
          if (segnum == 0) {
            states.swap(mergedStates);
            std::swap(initializedStates, initializedMergedStates);
          } else {
            for (size_t owner = 0; owner < numOwners; owner++) {
              AggregateStateView(child.aggregate, mergedStates.data() + owner * stride)
                  .merge(state((int32_t)owner));
            }
          }
          while (initializedStates != 0) state((int32_t)--initializedStates).destroy();
          if (segnum + 1 < (int32_t)source.domains.size()) {
            for (size_t owner = 0; owner < numOwners; owner++) {
              state((int32_t)owner).init(stateResource ? &*stateResource : nullptr);
              initializedStates++;
            }
            if (aggregateExecutionStatsForTests != nullptr) {
              aggregateExecutionStatsForTests->producerStates.fetch_add(
                  numOwners * child.aggregate.producers.size(), std::memory_order_relaxed);
            }
          }
        }
      }
      if (ordered) {
        states.swap(mergedStates);
        std::swap(initializedStates, initializedMergedStates);
      }

      std::vector<BucketScalar> results;
      results.reserve(numOwners);
      for (size_t owner = 0; owner < numOwners; owner++) {
        results.push_back(state((int32_t)owner).finish(scratch));
      }
      if (aggregateExecutionStatsForTests != nullptr) {
        aggregateExecutionStatsForTests->finishedStateBytes.fetch_add(
            stateResource ? stateResource->bytes() : 0, std::memory_order_relaxed);
      }
      ResultWriter(child, parent, (int64_t)numOwners)
          .write(results, source.buckets);
      releaseStates();
      if (aggregateExecutionStatsForTests != nullptr) {
        aggregateExecutionStatsForTests->retainedStateBytes.fetch_add(
            stateResource ? stateResource->bytes() : 0, std::memory_order_relaxed);
      }
    }
  };

  class PassiveCalc final : public Calculator {
  public:
    using Calculator::Calculator;
    api::Val* getTargetForSub(SearchResponse* resp, SearchOp& sub) override { return nullptr; }
    void calc(oneapi::tbb::task_group* tg, int32_t segnum, DomainHandle domain) override {}
    void calcAll(oneapi::tbb::task_group* tg, std::span<const DomainHandle> domains) override {}
    size_t facetBucketObservedBytes() const override { return 0; }
  };

  Calculator* createCalculator(
      Calculator* parent, int64_t slot = -1, int64_t numSlots = -1,
      const StructuralConstraint* structural = nullptr) override {
    if (passive) return new PassiveCalc(*this, parent, slot, numSlots, structural);
    if (orderSensitive) return new Calc<true>(*this, parent, slot, numSlots, structural);
    return new Calc<false>(*this, parent, slot, numSlots, structural);
  }

  InlineCalculator* createInlineCalculator(
      Calculator* parent, int64_t slot = -1,
      int64_t numSlots = -1) override {
    assert(canInline());
    return new InlineCalc(*this, parent, slot, numSlots);
  }

  bool canInline() override {
    return !aggregate.hasIndirectStates
        && std::ranges::all_of(aggregate.producers, [](const AggregateProducer& producer) {
      return producer.resolved.state.inlineCapable
          && producer.input->root().type != ValueType::COLUMN_ONLY
          && !valueArray(producer.input->root().type);
    });
  }

  // Every producer a bare scalar column with a keyed raw batch accumulator: the
  // inline calculator then reads its values straight from the column per batch.
  bool inlinesAsColumnBatch() const override {
    return !aggregate.hasIndirectStates
        && std::ranges::all_of(aggregate.producers, [](const AggregateProducer& producer) {
      const ValueNode& input = producer.input->root();
      return producer.resolved.state.inlineCapable
          && input.kind == ValueNodeKind::COLUMN && !valueArray(input.type)
          && input.type != ValueType::COLUMN_ONLY
          && producer.resolved.state.accumulateKeyedRawBatch != nullptr;
    });
  }

  uint32_t inlineEntryBytes() const override {
    return denseFacetState != nullptr ? denseFacetState->bytes
                                     : AggregateStateView::bytes(aggregate);
  }

  size_t facetBucketResidentBytes() const override {
    return passive ? 0 : AggregateStateView::bytes(aggregate);
  }

  bool facetBucketGrows() const override { return !passive && aggregate.hasIndirectStates; }

  FacetChildExecutor* bindFacetChild(
      const FacetChildContext& context) override {
    if (passive || !members.empty()) return nullptr;
    if (bucketDomain != nullptr || needsStructuralScope) return nullptr;
    if (context.stringColumn == nullptr) return nullptr;
    if (!std::ranges::all_of(aggregate.producers, [](const AggregateProducer& producer) {
          return producer.resolved.state.replayCapable;
        })) return nullptr;
    for (const auto& bucket : context.stringColumn->buckets) {
      if (!bucket.id.has_value()) return nullptr;
    }
    return new ColumnReplayExecutor(*this, context.parent,
                                    *context.stringColumn);
  }
};

} // namespace luxir
