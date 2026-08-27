#pragma once

#include <algorithm>
#include <array>
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

  bool canEmitAsBucketChild() const override { return true; }

  class Calc : public Calculator {
    static constexpr size_t BATCH_SIZE = 256;

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

    void accumulateBatch(AggregateAccumulator& accumulator,
                         std::span<BoundValueProgram* const> bindings,
                         std::span<const int32_t> docs,
                         std::span<ValueResult> results) {
      for (size_t leaf = 0; leaf < bindings.size(); leaf++) {
        bindings[leaf]->evalBatch(docs, {}, results.first(docs.size()));
        for (size_t i = 0; i < docs.size(); i++) {
          accumulator.add((uint32_t)leaf, results[i]);
        }
      }
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
          std::vector<BoundValueProgram*> bindings = bindAggregateInputs(
              thisOp().aggregate, pool, segment);

          std::array<int32_t, BATCH_SIZE> docs;
          std::array<ValueResult, BATCH_SIZE> results;
          size_t count = 0;
          auto addDoc = [&](int32_t doc) {
            docs[count++] = doc;
            if (count != docs.size()) return;
            accumulateBatch(accumulator, bindings,
                            std::span<const int32_t>(docs.data(), count), results);
            count = 0;
          };

          int32_t maxDoc = segment.postingsReader().maxDoc();
          forEachDomainDoc(domainHandle.get(), maxDoc, addDoc);
          if (count != 0) {
            accumulateBatch(accumulator, bindings,
                            std::span<const int32_t>(docs.data(), count), results);
          }
        } catch (const ValueEvaluationError&) {
          accumulator.fail(AggregateFailure::VALUE_EVALUATION);
        }
      });
    }
  };

  class InlineCalc final : public InlineCalculator {
    static constexpr size_t RESERVATION_CHUNK_BYTES = 64 * 1024;

    std::vector<BoundValueProgram*> bindings;
    std::vector<BucketScalar> finalized;
    std::string chargeDetail;
    AggregateEvalScratch scratch;
    AggregateFailure segmentFailure = AggregateFailure::NONE;
    size_t chargedBytes = 0;
    size_t reservationRemainder = 0;
    uint32_t entryBytes;

    ExprStatsOp& thisOp() { return (ExprStatsOp&)getOp(); }

    AggregateStateView state(void* entry) {
      return AggregateStateView(thisOp().aggregate, entry);
    }

    void addDoc(void* entry, int32_t docid) {
      AggregateStateView aggregateState = state(entry);
      if (segmentFailure != AggregateFailure::NONE) {
        aggregateState.fail(segmentFailure);
        return;
      }
      try {
        for (size_t leaf = 0; leaf < bindings.size(); leaf++) {
          aggregateState.add((uint32_t)leaf,
                             bindings[leaf]->evalPoint(docid, 0.0f));
        }
      } catch (const ValueEvaluationError&) {
        aggregateState.fail(AggregateFailure::VALUE_EVALUATION);
      }
    }

    static int compareResults(const BucketScalar& a,
                              const BucketScalar& b) {
      if (!a.valid || !b.valid) return (int)a.valid - (int)b.valid;
      if (a.type == BucketValueType::DOUBLE) {
        return a.doubleValue < b.doubleValue ? -1
            : (a.doubleValue > b.doubleValue ? 1 : 0);
      }
      return a.intValue < b.intValue ? -1
          : (a.intValue > b.intValue ? 1 : 0);
    }

  public:
    InlineCalc(SearchOp& op, Calculator* parent, int64_t slot,
               int64_t numSlots)
        : InlineCalculator(op, parent, slot, numSlots),
          chargeDetail(fmt::format("facet '{}' metric '{}'",
                                   parent->getOp().name, op.name)),
          entryBytes(AggregateStateView::bytes(
              static_cast<ExprStatsOp&>(op).aggregate)) {}

    ~InlineCalc() override {
      if (chargedBytes != 0) {
        thisOp().req.memoryTracker.release(chargedBytes);
      }
    }

    int insert(void* entry, int32_t docid, int space) override {
      if (space < (int)entryBytes) return -(int)entryBytes;
      state(entry).init();
      addDoc(entry, docid);
      return (int)entryBytes;
    }

    int update(void* entry, int32_t docid) override {
      addDoc(entry, docid);
      return (int)entryBytes;
    }

    std::pair<int, int> merge(void* target, void* from) override {
      state(target).merge(state(from));
      return {(int)entryBytes, (int)entryBytes};
    }

    std::pair<int, int> mergeNew(void* target, void* from,
                                 int space) override {
      if (space < (int)entryBytes) {
        return {-(int)entryBytes, (int)entryBytes};
      }
      state(target).init();
      state(target).merge(state(from));
      return {(int)entryBytes, (int)entryBytes};
    }

    void beginFinalize(size_t entries) override {
      if (reservationRemainder != 0) {
        thisOp().req.memoryTracker.release(reservationRemainder);
        chargedBytes -= reservationRemainder;
        reservationRemainder = 0;
      }
      finalized.clear();
      finalized.reserve(entries);
    }

    int finalize(void* entry, int64_t count) override {
      unused(count);
      finalized.push_back(state(entry).finish(scratch));
      return (int)entryBytes;
    }

    int compare(size_t a, size_t b) override {
      return compareResults(finalized[a], finalized[b]);
    }

    bool isMissing(size_t entry) override {
      return !finalized[entry].valid;
    }

    void prepareEntry() override {
      if (reservationRemainder < entryBytes) {
        size_t minimum = entryBytes - reservationRemainder;
        size_t preferred = entryBytes >= RESERVATION_CHUNK_BYTES
            ? minimum : RESERVATION_CHUNK_BYTES - reservationRemainder;
        size_t reserved = thisOp().chargeFacetState(
            preferred, minimum, chargeDetail);
        chargedBytes += reserved;
        reservationRemainder += reserved;
      }
      reservationRemainder -= entryBytes;
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
                    std::span<const size_t> finalizedSlots) override {
      assert(entries.size() == finalizedSlots.size());
      for (size_t slot : finalizedSlots) {
        thisOp().warnFailure(finalized[slot]);
      }

      auto& mr = op.req.lastResponse->mr;
      auto* value = getTarget(nullptr, [&](api::Val& target) {
        auto& arr = oneofMut<api::ArrVal>(target);
        if (arr.v.empty()) build::allocArray(arr.v, entries.size(), mr);
      });
      auto& arr = oneofMut<api::ArrVal>(*value);
      for (size_t i = 0; i < entries.size(); i++) {
        writeAggregateValue(const_cast<api::Val&>(arr.v[i]),
                            finalized[finalizedSlots[i]]);
        entries[i] += entryBytes;
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

    void failOwners(std::span<const int32_t> owners) {
      for (int32_t owner : owners) {
        state(owner).fail(AggregateFailure::VALUE_EVALUATION);
      }
    }

    void addDoc(std::span<const int32_t> owners, int32_t docid,
                std::span<BoundValueProgram* const> bindings,
                std::span<ValueResult> values) {
      if (owners.empty()) return;
      try {
        for (size_t leaf = 0; leaf < bindings.size(); leaf++) {
          values[leaf] = bindings[leaf]->evalPoint(docid, 0.0f);
        }
      } catch (const ValueEvaluationError&) {
        failOwners(owners);
        return;
      }
      for (int32_t owner : owners) {
        AggregateStateView aggregateState = state(owner);
        for (size_t leaf = 0; leaf < values.size(); leaf++) {
          aggregateState.add((uint32_t)leaf, values[leaf]);
        }
      }
    }

    void replaySegment(int32_t segnum) {
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

      std::vector<BoundValueProgram*> bindings;
      bool bindingFailed = false;
      try {
        bindings = bindAggregateInputs(
            child.aggregate, poolGuard.pool(), segment);
      } catch (const ValueEvaluationError&) {
        bindings.clear();
        bindingFailed = true;
      }
      std::vector<ValueResult> values(child.aggregate.leaves.size());

      selected.visit([&](const auto& selectedMap) {
        DocSet* domain = source.domains[(size_t)segnum].get();
        forEachSelectedBucketDoc(
            domain, parentColumn, maxDoc, selectedMap,
            [&](int32_t docid,
                std::span<const int32_t> owners) LUXIR_INLINE {
              if (bindingFailed) {
                failOwners(owners);
              } else {
                addDoc(owners, docid, bindings, values);
              }
            });
      });
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
