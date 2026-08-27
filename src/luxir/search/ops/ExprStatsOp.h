#pragma once

#include <array>
#include <memory>
#include <vector>

#include <fmt/format.h>

#include "luxir/search/ops/SearchOp.h"
#include "luxir/search/ops/DomainIter.h"
#include "luxir/util/SegmentMergeDriver.h"
#include "luxir/value/AggregateExpr.h"

namespace luxir {

// Expression aggregate over an incoming domain. Facet inline and replay
// bindings are added separately; this calculator is the bucket-domain/root
// execution primitive used by both paths.
class ExprStatsOp : public SearchOp {
  AggregateProgram& aggregate;

public:
  ExprStatsOp(SearchRequest& req, std::string_view name,
              AggregateProgram& aggregate)
      : SearchOp(req, name), aggregate(aggregate) {}

  bool canEmitAsBucketChild() const override { return true; }

  class Calc : public Calculator {
    static constexpr size_t BATCH_SIZE = 256;

    SegmentMergeDriver<AggregateAccumulator> driver;

    ExprStatsOp& thisOp() { return (ExprStatsOp&)getOp(); }

    void emitResult(const AggregateAccumulator& accumulator) {
      BucketScalar result = accumulator.finish();
      if (result.failure != AggregateFailure::NONE) {
        op.req.warnOnce("aggregate_eval_failed", fmt::format(
            "aggregate op '{}' emitted null: {}", thisOp().name,
            aggregateFailureReason(result.failure)));
      }
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
          std::vector<BoundValueProgram*> bindings;
          bindings.reserve(thisOp().aggregate.leaves.size());
          for (const AggregateLeaf& leaf : thisOp().aggregate.leaves) {
            bindings.push_back(leaf.input->bind(pool, segment));
          }

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

  Calculator* createCalculator(Calculator* parent, int64_t slot = -1,
                               int64_t numSlots = -1) override {
    return new Calc(*this, parent, slot, numSlots);
  }
};

} // namespace luxir
