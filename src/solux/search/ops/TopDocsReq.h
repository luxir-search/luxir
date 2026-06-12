#pragma once

#include <atomic>
#include <cstring>
#include <functional>
#include "SearchOp.h"
#include "solux/query/AllQuery.h"
#include "solux/query/Query.h"
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

  const solux::proto::TopDocs& topDocsProto;  // the relevant part of the protobuf request
  Query::Context& qcontext;
  Query* query;
  Query::Weight* weight;
  int64_t topCount; // maximum number of docs to return.
  std::span<std::pair<std::string_view, Query*>> filters;
  std::span<Query::Weight*> filterWeights;
  std::vector<SortField> sortFields;
  bool useFieldSort = false;

  // Optional sink for the merged top-K collector.  If set, the Calc invokes
  // it instead of self-emitting via fillQueryTopNResponse, letting another
  // op (e.g. FusionOp) consume this TopDocsReq's ranking.  `mc` is null on
  // the empty-index path (no segments, no collector ever obtained); sinks
  // must handle that case.
  std::function<void(Calc&, MergeableCollector*)> rankingSink;


  class Calc : public SearchOp::Calculator {
  public:
    solux::proto::Val* getTargetForSub(solux::proto::SearchResponse* searchResponse, Calculator* sub) override {
      auto* ourVal = parent->getTargetForSub(searchResponse, this);
      // the Val should either be unset, or have a DocList
      assert(
        ourVal != nullptr && (ourVal->kind_case() == solux::proto::Val::kDocs
          || ourVal->kind_case() == solux::proto::Val::KIND_NOT_SET));
      return &(*ourVal->mutable_docs()->mutable_ops())[sub->getOp().name];
    }

    Calc(TopDocsReq& op, Calculator* parent) : SearchOp::Calculator(op, parent, -1, -1), collectorMerger(nullptr, nullptr) {

      collectorMerger.creator = [&op]() -> MergeableCollector* {
        return new MergeableCollector(op.topCount, op.useFieldSort, op.sortFields, op.req.reader.get());
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
    std::vector<std::unique_ptr<DocSet>> effectiveDomainsOwned;
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

    Query::Scorer* createMainScorer(MemPool& pool, IndexReader::Segment& seg) {
      auto& op = thisOp();
      Query::SegmentSource& source = preparedWeight != nullptr
        ? static_cast<Query::SegmentSource&>(*preparedWeight)
        : static_cast<Query::SegmentSource&>(*op.weight);
      return QueryPrep::createScorer(pool, seg, source);
    }

    std::unique_ptr<DocSet> buildEffectiveDomain(int32_t segnum) {
      auto& op = thisOp();
      auto* baseDomain = baseDomains[(size_t)segnum];
      if (op.filterWeights.empty()) return nullptr;
      auto& seg = op.req.reader->segments()[segnum];

      std::vector<std::unique_ptr<DocSet>> filters;
      std::vector<DocSet*> filterPtrs;
      filters.reserve(op.filterWeights.size());
      filterPtrs.reserve(op.filterWeights.size());
      for (size_t i = 0; i < op.filterWeights.size(); i++) {
        filters.push_back(QueryPrep::materialize(
          *op.filterWeights[i], preparedFilterWeights[i].get(), seg, baseDomain));
        filterPtrs.push_back(filters.back().get());
      }
      if (filterPtrs.empty()) return nullptr;
      if (filterPtrs.size() == 1) return std::move(filters[0]);
      return DocSet::intersect(filterPtrs);
    }

    void doPrepareDomain(oneapi::tbb::task_group* tg, int32_t segnum, solux::DocSet* domain) {
      auto& op = thisOp();
      if (segnum < 0) {
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
      for (size_t i = 0; i < op.filterWeights.size(); i++) {
        if (op.filterWeights[i]->needsPrepare()) {
          preparedFilterWeights[i] = op.filterWeights[i]->prepare(baseCtx);
        }
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

      // If we have subcalcs and if we determine that we are matching everything, then we can skip collecting
      // a new domain and just use the existing one.
      // TODO: put a type field on the query and replace this dynamic cast.
      bool matchEverything = (dynamic_cast<AllQuery*>(op.query) != nullptr) && thisOp().filters.empty();
      MergeableCollector* data = nullptr;
      int64_t numSegs = (int64_t)op.req.reader->segments().size();

      {
        auto poolGuard = MemPool::threadLocalPoolGuard();
        auto& seg = op.qcontext.topReader.segments()[segnum];
        auto* scorer = createMainScorer(poolGuard.pool(), seg);

        // Wait until last moment to obtain collector in hopes of reusing an existing one.
        data = collectorMerger.obtain();

        std::optional<DocSetBuilder> builder;
        if (output.size() > 0) {
          // check if the query is a match-all-docs query with a dynamic cast
          matchEverything = (dynamic_cast<AllQuery*>(op.query) != nullptr) && thisOp().filters.empty();
          if (!matchEverything) {
            builder.emplace(seg.maxDoc());
          }
        }


        if (scorer != nullptr) {
          DocSet* filter = domain;
          std::unique_ptr<DocSet> newDomain;
          if (!preparedMode && !thisOp().filterWeights.empty()) {
            std::vector<std::unique_ptr<DocSet>> filters;
            std::vector<DocSet*> filterPtrs;
            for (auto weight : thisOp().filterWeights) {
              filters.push_back(QueryPrep::materialize(*weight, nullptr, seg, nullptr));
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
          if (data->useFieldSort) {
            data->fieldCollector->setSegment(segnum, &seg.postingsReader());
            collectTopK(segnum, scorer, filter, builderPtr, *data->fieldCollector);
          } else {
            collectTopK(segnum, scorer, filter, builderPtr, *data->scoreCollector);
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
      auto count = collectorMerger.release(data);
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


  // Arena::Create registers ~TopDocsReq with the arena *before* the ctor body
  // runs.  Any throw mid-ctor leaves a half-constructed object scheduled for
  // cleanup that crashes at arena reset.  Keep this ctor nothrow: all
  // validation (sort field schema lookup, Weight construction, filter
  // weights) is done by ProtobufSearchParser before Arena::Create.
  TopDocsReq(SearchRequest& req, std::string_view name, const proto::TopDocs& topDocsProto,
    Query::Context& qcontext, Query* query, Query::Weight* weight, int64_t topCount,
    std::vector<SortField>&& sortFields, bool useFieldSort,
    std::span<std::pair<std::string_view, Query*>> filters,
    std::span<Query::Weight*> filterWeights)
    : SearchOp(req, name), topDocsProto(topDocsProto), qcontext(qcontext), query(query),
      weight(weight), topCount(topCount), filters(filters), filterWeights(filterWeights),
      sortFields(std::move(sortFields)), useFieldSort(useFieldSort) {
  }

  Calculator* createCalculator(Calculator* parent, int64_t slot = -1, int64_t numSlots = -1) override {
    return new Calc(*this, parent);
  }


  // Called after all segments have been collected for a TopN query to fill out
  // the DocList proto.  Hands off the merged collector's ranked output to the
  // shared streaming emitter (emitDocsResponse), which blocks until field
  // loading completes and sends multiple streaming responses (all but the
  // last).  The empty-index case (no segments -> no collector ever obtained)
  // writes a matches=0 DocList into the request's lastResponse; submitBody
  // then sends it.
  void fillQueryTopNResponse(TopDocsReq::Calc& calc) {
    auto& qr = *this;
    auto* mergeableCollector = calc.collectorMerger.getData();

    if (mergeableCollector == nullptr) {
      auto& searchResultProto = *calc.getTarget(nullptr);
      auto& docListProto = *searchResultProto.mutable_docs();
      docListProto.set_matches(0);
      return;
    }

    auto getDocList = [&calc](solux::proto::SearchResponse* resp) -> solux::proto::DocList& {
      auto& val = *calc.getTarget(resp);
      return *val.mutable_docs();
    };

    if (mergeableCollector->useFieldSort) {
      auto& collector = *mergeableCollector->fieldCollector;
      auto sortDocs = collector.sort();
      emitDocsResponse(qr.req, getDocList,
        (int64_t)sortDocs.size(),
        [sortDocs](int64_t i) { return sortDocs[i].doc; },
        [sortDocs](int64_t i) { return sortDocs[i].score; },
        collector.totalHits(),
        qr.topDocsProto.fields(),
        qr.topDocsProto.batch_size(),
        qr.topDocsProto.offset(),
        qr.topDocsProto.get_number(),
        qr.topDocsProto.get_scores());
    } else {
      auto& collector = *mergeableCollector->scoreCollector;
      auto scoreDocs = collector.sort();
      emitDocsResponse(qr.req, getDocList,
        (int64_t)scoreDocs.size(),
        [scoreDocs](int64_t i) { return scoreDocs[i].doc; },
        [scoreDocs](int64_t i) { return scoreDocs[i].score; },
        collector.totalHits(),
        qr.topDocsProto.fields(),
        qr.topDocsProto.batch_size(),
        qr.topDocsProto.offset(),
        qr.topDocsProto.get_number(),
        qr.topDocsProto.get_scores());
    }
  }

};


}
