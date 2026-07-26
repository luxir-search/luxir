#pragma once

#include <atomic>
#include <cstring>
#include <limits>
#include <functional>
#include "SearchOp.h"
#include "solux/query/Query.h"
#include "solux/search/SearchOverrides.h"
#include "solux/query/QueryPrep.h"
#include "solux/reader/IntColReader.h"
#include "solux/reader/StoredFieldsReader.h"
#include "solux/reader/StrColReader.h"
#include "solux/search/Collector.h"
#include "solux/search/EmitDocs.h"
#include "solux/search/FieldSortCollector.h"
#include "solux/search/MergeableCollector.h"
#include "solux/search/SortField.h"
#include "solux/search/SearchRequest.h"
#include "solux/util/AtomicMerger.h"

namespace solux {


class TopDocsReq : public SearchOp {
protected:

public:
  class Calc;

  const ReqTopDocs& topDocsProto;  // the relevant part of the protobuf request
  Query::Context& qcontext;
  Query* query;
  Query::Weight* weight;
  int64_t topCount; // maximum number of docs to return.
  std::span<std::pair<std::string_view, Query*>> filters;
  std::span<Query::Weight*> filterWeights;
  std::span<FilterCache::Use*> filterUses;
  SortPlan sortPlan;

  // Optional sink for the merged top-K collector.  If set, the Calc invokes
  // it instead of self-emitting via fillQueryTopNResponse, letting another
  // op (e.g. FusionOp) consume this TopDocsReq's ranking.  `mc` is null on
  // the empty-index path (no segments, no collector ever obtained); sinks
  // must handle that case.
  std::function<void(Calc&, MergeableCollector*)> rankingSink;


  class Calc : public SearchOp::Calculator {
  public:
    solux::api::Val* getTargetForSub(SearchResponse* resp, Calculator* sub) override {
      auto* ourVal = parent->getTargetForSub(resp, this);
      // the Val should either be unset, or have a DocList
      assert(
        ourVal != nullptr && (std::holds_alternative<solux::api::DocList>(ourVal->kind)
          || std::holds_alternative<std::monostate>(ourVal->kind)));
      auto& dl = oneofMut<solux::api::DocList>(*ourVal);
      return build::opsSlot(dl.ops, op.subOps.size(), sub->getOp().name, resp->mr);
    }

    Calc(TopDocsReq& op, Calculator* parent) : SearchOp::Calculator(op, parent, -1, -1), collectorMerger(nullptr, nullptr) {

      collectorMerger.creator = [&op]() -> MergeableCollector* {
        return new MergeableCollector(
            op.topCount, op.sortPlan, op.req.reader.get(), op.weight->needsScores());
      };
      collectorMerger.destroyer = [](MergeableCollector* data) {
        delete data;
      };

      if (op.subOps.size() > 0) {
        output.resize(op.req.reader->segments().size());
        subCalcs.reserve(op.subOps.size());
        for (auto& [key, subOp] : op.subOps) {
          auto* subCalc = subOp->createCalculator(this, -1);
          subCalcs.emplace_back(subCalc);
        }
      }
      needsPrepare = op.weight->needsPrepare();
      for (auto* weight : op.filterWeights) {
        if (weight->needsPrepare()) {
          needsPrepare = true;
          break;
        }
      }
      if (needsPrepare) {
        auto numSegs = op.req.reader->segments().size();
        baseDomains.assign(numSegs, nullptr);
        effectiveDomains.assign(numSegs, nullptr);
        effectiveDomainsOwned.resize(numSegs);
        preparedFilterWeights.resize(op.filterWeights.size());
      }
    }

    TopDocsReq& thisOp() {
      return static_cast<TopDocsReq&>(op);
    }



    AtomicMerger<MergeableCollector> collectorMerger;
    MaxScoreAccumulator scoreAccumulator;


    // produced domains for subOps.
    // TODO: how to avoid having 2 allocations per set, one for the DocSet and one for the memory in the DocSet?
    // Arena allocate?
    // We could have std::variant of DocSet.
    std::vector<std::unique_ptr<DocSet>> output;
    std::vector<std::unique_ptr<Calculator>> subCalcs;

    // Prepared path state. Used only when the main query or one of the
    // TopDocs filters needs all segment domains before scorer creation.
    //
    // Each segment task records its incoming base domain, then the last
    // arriving segment prepares any global weights, materializes effective
    // per-segment domains, and dispatches prepared collection tasks.
    bool needsPrepare = false;
    std::atomic<int32_t> preparedDomainsSeen{0};
    std::vector<DocSet*> baseDomains;
    std::vector<DocSet*> effectiveDomains;
    std::vector<QueryPrep::MaterializedFilter> effectiveDomainsOwned;
    std::vector<std::unique_ptr<Query::Weight::PreparedWeight>> preparedFilterWeights;
    std::unique_ptr<Query::Weight::PreparedWeight> preparedWeight;

    void calc(oneapi::tbb::task_group* tg, int32_t segnum, solux::DocSet* domain) override {
      // TODO: there is a redundant task launch here since RootOp creates tasks for each segment already.
      // We could think about rootOp calling us just once per index and we could launce a task per segment - but,
      // if we are a sub-op of another operation, it's possible we might already be running in a per-segment mode?
      if (needsPrepare) {
        task_group_run(tg, [this, tg, segnum, domain]() {
          doPrepareDomain(tg, segnum, domain);
        });
      } else {
        task_group_run(tg, [this, tg, segnum, domain]() {
          doCalc(tg, segnum, domain);
        });
      }
    }

    Query::ScorerSupplier* mainScorerSupplier(MemPool& pool, IndexReader::Segment& seg) {
      auto& op = thisOp();
      Query::SegmentSource& source = preparedWeight != nullptr
        ? static_cast<Query::SegmentSource&>(*preparedWeight)
        : static_cast<Query::SegmentSource&>(*op.weight);
      return source.scorerSupplier(pool, seg);
    }

    Query::Scorer* createMainScorer(MemPool& pool, IndexReader::Segment& seg) {
      auto* supplier = mainScorerSupplier(pool, seg);
      if (supplier == nullptr) return nullptr;
      return supplier->get(pool, std::numeric_limits<int64_t>::max());
    }

    QueryPrep::MaterializedFilter buildEffectiveDomain(int32_t segnum) {
      auto& op = thisOp();
      auto* baseDomain = baseDomains[(size_t)segnum];
      if (op.filterWeights.empty()) return {};
      auto& seg = op.req.reader->segments()[segnum];

      std::vector<QueryPrep::MaterializedFilter> filters;
      std::vector<DocSet*> filterPtrs;
      filters.reserve(op.filterWeights.size());
      filterPtrs.reserve(op.filterWeights.size());
      for (size_t i = 0; i < op.filterWeights.size(); i++) {
        filters.push_back(QueryPrep::materializeEffectiveFilter(
          *op.filterWeights[i], preparedFilterWeights[i].get(),
          op.filterUses[i], *op.req.reader, seg, baseDomain));
        filterPtrs.push_back(filters.back().get());
      }
      if (filterPtrs.empty()) return {};
      if (filterPtrs.size() == 1) return std::move(filters[0]);
      return QueryPrep::MaterializedFilter(DocSet::intersect(filterPtrs));
    }

    void doPrepareDomain(oneapi::tbb::task_group* tg, int32_t segnum, solux::DocSet* domain) {
      auto& op = thisOp();
      if (segnum < 0) {
        // Empty index: forward to sub-calculators so nested ops still emit a
        // result, mirroring the empty-index path in doCollect.  Without this,
        // a prepare-requiring query (force_prepare, KNN) silently drops nested
        // facet/avg ops on an empty index.
        for (auto& subCalc : subCalcs) {
          subCalc->calc(tg, -1, nullptr);
        }
        doneCollecting();
        return;
      }
      baseDomains[(size_t)segnum] = domain;
      auto count = preparedDomainsSeen.fetch_add(1, std::memory_order_acq_rel) + 1;
      if (count != (int32_t)op.req.reader->segments().size()) return;

      Query::Weight::PrepareContext baseCtx{
        *op.req.reader,
        std::span<DocSet* const>(baseDomains.data(), baseDomains.size()),
        tg != nullptr
      };
      auto preparedFilters = QueryPrep::prepareFilterSources(
          op.filterWeights, op.filterUses, baseCtx);
      for (size_t i = 0; i < preparedFilters.size(); i++) {
        preparedFilterWeights[i] = std::move(preparedFilters[i].prepared);
      }

      for (size_t i = 0; i < op.req.reader->segments().size(); i++) {
        if (op.filterWeights.empty()) {
          effectiveDomains[i] = baseDomains[i];
        } else {
          effectiveDomainsOwned[i] = buildEffectiveDomain((int32_t)i);
          effectiveDomains[i] = effectiveDomainsOwned[i].get();
        }
      }

      Query::Weight::PrepareContext queryCtx{
        *op.req.reader,
        std::span<DocSet* const>(effectiveDomains.data(), effectiveDomains.size()),
        tg != nullptr
      };
      if (op.weight->needsPrepare()) {
        preparedWeight = op.weight->prepare(queryCtx);
      }

      for (int32_t i = 0; i < (int32_t)op.req.reader->segments().size(); i++) {
        task_group_run(tg, [this, tg, i]() {
          doPreparedCollect(tg, i);
        });
      }
    }

    void doPreparedCollect(oneapi::tbb::task_group* tg, int32_t segnum) {
      doCollect(tg, segnum, effectiveDomains[(size_t)segnum], true);
    }

    void doCalc(oneapi::tbb::task_group* tg, int32_t segnum, solux::DocSet* domain) {
      doCollect(tg, segnum, domain, false);
    }

    void doCollect(oneapi::tbb::task_group* tg, int32_t segnum, solux::DocSet* domain,
                   bool preparedMode) {
      auto& op = thisOp();

      if (segnum < 0) {
        // special case for empty index reader, we are done.
        for (auto& subCalc : subCalcs) {
          subCalc->calc(tg, -1, nullptr);
        }
        doneCollecting();
        return;
      }

      // A constant-scoring match-all is the identity on its domain: every doc
      // in the domain matches, all with the same score.  So the domain IS the
      // result set - its cardinality is the exact hit count, doc order is the
      // ranking, and sub-ops inherit it unchanged.  None of that needs a
      // scorer, so the whole collection ladder below is skipped.  (Scores must
      // be constant for doc order to be the ranking; a rescore over a
      // match-all matches everything but reorders it.)
      bool matchEverything = op.weight->matchesAllDocs()
        && op.weight->isConstantScoring() && op.filterWeights.empty();
      std::unique_ptr<MergeableCollector> data;
      int64_t numSegs = (int64_t)op.req.reader->segments().size();

      {
        auto poolGuard = MemPool::threadLocalPoolGuard();
        auto& seg = op.qcontext.topReader.segments()[segnum];

        // Wait until last moment to obtain collector in hopes of reusing an existing one.
        // Keep ownership until release so a scoring error cannot orphan it.
        data.reset(collectorMerger.obtain());

        std::optional<DocSetBuilder> builder;
        if (output.size() > 0 && !matchEverything) {
          builder.emplace(seg.maxDoc());
        }

        // Field-sorted ranking is the one thing a match-all still has to
        // iterate for: it orders by column values the domain says nothing
        // about.  A count-only field sort (limit 0) reads nothing back, so it
        // takes the domain answer like the score-ranked case.
        bool rankFromDocOrder = !data->useFieldSort;
        if (matchEverything && (rankFromDocOrder || data->topCount() == 0)) {
          int64_t total = domain == nullptr ? seg.maxDoc() : domain->card();
          int64_t ranked = 0;
          if (rankFromDocOrder && data->topCount() > 0) {
            // Equal scores reduce ranking to doc order, so the domain's first
            // K docs are its top K.
            auto* supplier = mainScorerSupplier(poolGuard.pool(), seg);
            auto* scorer = supplier == nullptr ? nullptr
              : supplier->get(poolGuard.pool(), std::numeric_limits<int64_t>::max());
            if (scorer != nullptr) {
              ranked = collectFirstKConstant(segnum, scorer, domain,
                                             *data->scoreCollector, data->topCount());
            }
          }
          assert(total >= ranked);
          data->addHits(total - ranked);
        } else if (auto* supplier = mainScorerSupplier(poolGuard.pool(), seg);
                   supplier != nullptr) {
          DocSet* filter = domain;
          std::unique_ptr<DocSet> newDomain;
          // Keeps owned sets or request-pinned cache borrows alive; `filter`
          // may alias one directly, so the handles outlive collection below.
          std::vector<QueryPrep::MaterializedFilter> filters;
          if (!preparedMode && !thisOp().filterWeights.empty()) {
            std::vector<DocSet*> filterPtrs;
            for (size_t i = 0; i < thisOp().filterWeights.size(); i++) {
              filters.push_back(QueryPrep::materializeEffectiveFilter(
                  *thisOp().filterWeights[i], nullptr,
                  thisOp().filterUses[i], *op.req.reader, seg, nullptr));
              filterPtrs.push_back(filters.back().get());
            }
            if (domain) {
              filterPtrs.emplace_back(domain);
            }
            if (filterPtrs.size() == 1) {
              filter = filterPtrs[0];
            } else {
              newDomain = DocSet::intersect(filterPtrs);
              filter = newDomain.get();
            }
          }
          DocSetBuilder* builderPtr = builder.has_value() ? &*builder : nullptr;
          bool sourcePreparedAgainstFilter =
            preparedMode && preparedWeight != nullptr && filter == domain;
          DocSet* collectorFilter =
            sourcePreparedAgainstFilter && preparedWeight->outputIsSubsetOfDomain()
              ? nullptr
              : filter;
          // Exact-count shortcut: a count-only collection (limit 0) over the
          // raw query - no filters, no deletes (null filter per the domain
          // contract), no sub-op domain to build - can often read the count
          // straight from index stats (a term's docFreq) without iterating.
          bool counted = false;
          if (!preparedMode && builderPtr == nullptr && filter == nullptr
              && !data->useFieldSort && data->scoreCollector->topCount == 0) {
            int64_t exact = op.weight->count(seg);
            if (exact >= 0) {
              data->scoreCollector->hitCount += exact;
              counted = true;
            }
          }
          if (counted) {
            // fall through to the sub-calc/merge tail below
          } else if (data->useFieldSort) {
            bool usedBulk = false;
            if (!disableFieldSortBulk && !data->fieldCollector->needsScores) {
              auto* bulk = supplier->bulkScorer(poolGuard.pool());
              if (bulk != nullptr && bulk->supportsMatchWindows()) {
                data->fieldCollector->setSegment(segnum, &seg.postingsReader());
                std::optional<FieldSortCollector::ExpressionBindings> expressionBindings;
                if (data->fieldCollector->hasExpr && data->fieldCollector->topCount > 0) {
                  expressionBindings.emplace(
                      *data->fieldCollector, poolGuard.pool(), seg);
                }
                collectTopKMatchWindowed(
                    segnum, bulk, collectorFilter, builderPtr,
                    *data->fieldCollector, seg.maxDoc());
                usedBulk = true;
              }
            }
            if (!usedBulk) {
              auto* scorer = supplier->get(
                  poolGuard.pool(), std::numeric_limits<int64_t>::max());
              if (scorer != nullptr) {
                data->fieldCollector->setSegment(segnum, &seg.postingsReader());
                std::optional<FieldSortCollector::ExpressionBindings> expressionBindings;
                if (data->fieldCollector->hasExpr && data->fieldCollector->topCount > 0) {
                  expressionBindings.emplace(
                      *data->fieldCollector, poolGuard.pool(), seg);
                }
                collectTopK(segnum, scorer, collectorFilter, builderPtr, *data->fieldCollector);
              }
            }
          } else {
            // Pruning is enabled only when an exact count can either be omitted
            // or supplied by an exhaustive count/domain pass or an already-known
            // domain. Without pruning, windowed scoring still beats the
            // doc-at-a-time heap disjunction by using per-clause window drives.
            bool allowPruning = op.weight->allowsPruning();
            BulkScorer* bulk = nullptr;
            bool useSparseConstantPull =
                builderPtr != nullptr
                && data->scoreCollector->topCount > 0
                && op.weight->isConstantScoring()
                && op.weight->prefersPullForSparseArrayDomain()
                && supplier->cost() <= DocSetBuilder::arrayLimitFor(seg.maxDoc());
            if (useSparseConstantPull) {
              auto* scorer = supplier->get(
                  poolGuard.pool(), std::numeric_limits<int64_t>::max());
              if (scorer != nullptr) {
                collectConstantTopKAndDomain(
                    segnum, scorer, collectorFilter, *builder,
                    *data->scoreCollector);
              }
            } else if ((bulk = supplier->bulkScorer(poolGuard.pool())) != nullptr) {
              if (data->scoreCollector->topCount == 0) {
                // limit 0: the collector keeps nothing but the total, so count
                // windows without materializing docs or scores.
                collectCountWindowed(bulk, collectorFilter, builderPtr, *data->scoreCollector,
                                     seg.maxDoc());
              } else if (op.weight->isConstantScoring()) {
                // Equal scores reduce ranking to doc order. The bulk scorer
                // drives the exhaustive count/domain while an independent
                // scorer visits only this segment's first K matches.
                auto* captureSupplier = mainScorerSupplier(poolGuard.pool(), seg);
                int64_t captured = 0;
                if (captureSupplier != nullptr) {
                  auto* captureScorer = captureSupplier->get(
                      poolGuard.pool(), std::numeric_limits<int64_t>::max());
                  if (captureScorer != nullptr) {
                    captured = collectFirstKConstant(
                        segnum, captureScorer, collectorFilter,
                        *data->scoreCollector, data->scoreCollector->topCount);
                  }
                }
                int64_t count = countMatchesWindowed(
                    bulk, collectorFilter, builderPtr, seg.maxDoc());
                assert(count >= captured);
                data->scoreCollector->hitCount += count - captured;
              } else if (builderPtr != nullptr) {
                int64_t count = countMatchesWindowed(
                    bulk, collectorFilter, builderPtr, seg.maxDoc());
                int64_t before = data->scoreCollector->totalHits();
                auto* rankingSupplier = mainScorerSupplier(poolGuard.pool(), seg);
                if (rankingSupplier != nullptr) {
                  auto* rankingBulk = rankingSupplier->bulkScorer(poolGuard.pool());
                  if (rankingBulk != nullptr) {
                    collectTopKWindowed(
                        segnum, rankingBulk, collectorFilter, *data->scoreCollector,
                        allowPruning ? &scoreAccumulator : nullptr,
                        seg.maxDoc(), allowPruning);
                  } else {
                    auto* rankingScorer = rankingSupplier->get(
                        poolGuard.pool(), std::numeric_limits<int64_t>::max());
                    if (rankingScorer != nullptr) {
                      collectTopK(
                          segnum, rankingScorer, collectorFilter, nullptr,
                          *data->scoreCollector, allowPruning,
                          allowPruning ? &scoreAccumulator : nullptr);
                    }
                  }
                }
                int64_t after = data->scoreCollector->totalHits();
                assert(count >= after - before);
                data->scoreCollector->hitCount += count - (after - before);
              } else {
                collectTopKWindowed(
                    segnum, bulk, collectorFilter, *data->scoreCollector,
                    allowPruning ? &scoreAccumulator : nullptr,
                    seg.maxDoc(), allowPruning);
              }
            } else {
              auto* scorer = supplier->get(poolGuard.pool(), std::numeric_limits<int64_t>::max());
              if (scorer != nullptr) {
                collectTopK(segnum, scorer, collectorFilter, builderPtr, *data->scoreCollector,
                            allowPruning, &scoreAccumulator);
              }
            }
          }
        }
        if (builder.has_value()) {
          output[segnum] = std::move(builder->build());
        }
      }
      // For maximum parallelism, we want to launch sub-tasks that depend on matching documents
      // as soon as we have that set.  Releasing the collector below could end up
      // doing a substantial amount of work.

      // since tasks are executed on a stack, push sub-calculators in reverse order.
      // directly calling a sub-calculator would be beneficial since the domain
      // we just calculated will still be in cache.
      // The downside is that it could delay loading stored fields.
      // We could launch fillQueryTopNResponse as a sub-task and then
      // launch the sub-calculators in parallel after that.
      for (int i = subCalcs.size() - 1; i >= 0; i--) {
        // TODO: launch sub-calculators in parallel (except for the first one).
        auto* newDomain = matchEverything ? domain : output[segnum].get();
        subCalcs[i]->calc(tg, segnum, newDomain);
      }

      // Releasing the collector as soon as possible can save merging work.
      // On the other hand, it could delay the sub-calculators and spoil
      // the domain (which should be in the CPU cache).
      auto count = collectorMerger.release(data.release());
      if (count == numSegs) {
        // we are done, so we can call the callback
        // we could use a nested task_group to wait until we are done here as well.

        // If there are sub-calculators that can run, lanch a separate task
        // for field loading.
        auto* tgFieldLoad = subCalcs.size() > 0 ? tg : nullptr;
        task_group_run(tgFieldLoad, [this]() {doneCollecting();});
      }




    }

    void doneCollecting() {
      auto& op = thisOp();
      if (op.rankingSink) {
        // The sink is responsible for everything downstream of "ranking is
        // ready": emit, fuse, etc.  We pass the merged collector (possibly
        // null on the empty-index path) so the sink can sort() and act on
        // the ranked output.
        op.rankingSink(*this, collectorMerger.getData());
      } else {
        op.fillQueryTopNResponse(*this);
      }
      // TODO: figure out what state we can dump before and after this call.
    };
  };


  // ProtobufSearchParser resolves everything that can fail (sort field schema
  // lookup, Weight construction, filter weights) and passes the results in, so
  // this ctor just binds members.  (arenaCreate registers ~TopDocsReq only
  // after construction succeeds, so a throwing arena ctor is safe now - the
  // parser split is parse-phase structure, not a nothrow requirement.)
  TopDocsReq(SearchRequest& req, std::string_view name, const ReqTopDocs& topDocsProto,
    Query::Context& qcontext, Query* query, Query::Weight* weight, int64_t topCount,
    SortPlan&& sortPlan,
    std::span<std::pair<std::string_view, Query*>> filters,
    std::span<Query::Weight*> filterWeights)
    : SearchOp(req, name), topDocsProto(topDocsProto), qcontext(qcontext), query(query),
      weight(weight), topCount(topCount), filters(filters), filterWeights(filterWeights),
      sortPlan(std::move(sortPlan)) {
    if (!filterWeights.empty()) {
      assert(filterWeights.size() == filters.size());
      filterUses = qcontext.pool.make_span<FilterCache::Use*>(filters.size());
      for (size_t i = 0; i < filters.size(); i++) {
        filterUses[i] = qcontext.getFilterUse(*filters[i].second);
      }
    }
  }

  Calculator* createCalculator(Calculator* parent, int64_t slot = -1, int64_t numSlots = -1) override {
    return new Calc(*this, parent);
  }


  // Called after all segments have been collected for a TopN query to fill out
  // the DocList proto.  Hands off the merged collector's ranked output to the
  // shared streaming emitter (emitDocsResponse), which blocks until field
  // loading completes and sends multiple streaming responses (all but the
  // last).  The empty-index case (no segments -> no collector ever obtained)
  // writes an empty DocList into the request's lastResponse (with found=0 only
  // when the count was requested); submitBody then sends it.
  void fillQueryTopNResponse(TopDocsReq::Calc& calc) {
    auto& qr = *this;
    auto* mergeableCollector = calc.collectorMerger.getData();

    if (mergeableCollector == nullptr) {
      auto& searchResultProto = *calc.getTarget(nullptr);
      auto& docListProto = oneofMut<solux::api::DocList>(searchResultProto);
      // found is opt-in (see emitDocsResponse): only populate it when the
      // count was requested, so an empty index matches the non-empty contract.
      if (qr.topDocsProto.get_number) docListProto.found = 0;
      return;
    }

    auto getDocList = [&calc](SearchResponse* resp) -> solux::api::DocList& {
      auto& val = *calc.getTarget(resp);
      return oneofMut<solux::api::DocList>(val);
    };

    if (mergeableCollector->useFieldSort) {
      auto& collector = *mergeableCollector->fieldCollector;
      auto sortDocs = collector.sort();
      emitDocsResponse(qr.req, getDocList,
        (int64_t)sortDocs.size(),
        [sortDocs](int64_t i) { return sortDocs[i].doc; },
        [sortDocs](int64_t i) { return sortDocs[i].score; },
        collector.totalHits(),
        qr.topDocsProto.fields,
        qr.topDocsProto.batch_size,
        qr.topDocsProto.offset,
        qr.topDocsProto.get_number,
        qr.topDocsProto.get_scores,
        qr.topDocsProto.document_format);
    } else {
      auto& collector = *mergeableCollector->scoreCollector;
      auto scoreDocs = collector.sort();
      emitDocsResponse(qr.req, getDocList,
        (int64_t)scoreDocs.size(),
        [scoreDocs](int64_t i) { return scoreDocs[i].doc; },
        [scoreDocs](int64_t i) { return scoreDocs[i].score; },
        collector.totalHits(),
        qr.topDocsProto.fields,
        qr.topDocsProto.batch_size,
        qr.topDocsProto.offset,
        qr.topDocsProto.get_number,
        qr.topDocsProto.get_scores,
        qr.topDocsProto.document_format);
    }
  }

};


}
