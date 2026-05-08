#pragma once

#include <cstring>
#include "SearchOp.h"
#include "solux/query/AllQuery.h"
#include "solux/query/Query.h"
#include "solux/reader/IntColReader.h"
#include "solux/reader/StoredFieldsReader.h"
#include "solux/reader/StrColReader.h"
#include "solux/search/Collector.h"
#include "solux/search/EmitDocs.h"
#include "solux/search/FieldSortCollector.h"
#include "solux/search/SortField.h"
#include "solux/search/SearchRequest.h"
#include "solux/util/AtomicMerger.h"

namespace solux {


class TopDocsReq : public SearchOp {
protected:

public:
  const solux::proto::TopDocs& topDocsProto;  // the relevant part of the protobuf request
  Query::Context& qcontext;
  Query* query;
  Query::Weight* weight;
  int64_t topCount; // maximum number of docs to return.
  std::span<std::pair<std::string_view, Query*>> filters;
  std::span<Query::Weight*> filterWeights;
  std::vector<SortField> sortFields;
  bool useFieldSort = false;


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
    }

    TopDocsReq& thisOp() {
      return static_cast<TopDocsReq&>(op);
    }



    class MergeableCollector : public MergeableData {
    public:
      // QueryReq* queryReq;  // the query request that this collector is for
      std::unique_ptr<TopDocsCollector> scoreCollector;
      std::unique_ptr<FieldSortCollector> fieldCollector;
      bool useFieldSort;

      MergeableCollector(size_t topCount, bool useFieldSort, const std::vector<SortField>& sortFields, IndexReader* reader = nullptr) 
        : useFieldSort(useFieldSort) {
        if (!useFieldSort) {
          scoreCollector = std::make_unique<TopDocsCollector>(topCount);
        } else {
          std::unique_ptr<FieldComparator> comparator;
          if (sortFields.size() == 1) {
            // For single field sort, create the comparator directly
            comparator = sortFields[0].createComparator(topCount, reader);
          } else {
            // For multiple fields, use MultiFieldComparator
            comparator = std::make_unique<MultiFieldComparator>(sortFields, topCount, reader);
          }
          fieldCollector = std::make_unique<FieldSortCollector>(topCount, std::move(comparator));
        }
      }

      static MergeableCollector* merge(MergeableCollector* a, MergeableCollector* b) {
        // merge the smaller collector into the larger collector, or if both the same size, merge
        // the less competitive collector into the more competitive collector.
        if (a->useFieldSort) {
          if (a->fieldCollector->size() < b->fieldCollector->size()) {
            std::swap(a, b);
          }
          a->fieldCollector->merge(*b->fieldCollector);
        } else {
          if (a->scoreCollector->size() < b->scoreCollector->size()
            || a->scoreCollector->minCompetitiveVal < b->scoreCollector->minCompetitiveVal) {
            std::swap(a, b);
          }
          a->scoreCollector->merge(*b->scoreCollector);
        }
        return a;
      }
    };

    AtomicMerger<MergeableCollector> collectorMerger;


    // produced domains for subOps.
    // TODO: how to avoid having 2 allocations per set, one for the DocSet and one for the memory in the DocSet?
    // Arena allocate?
    // We could have std::variant of DocSet.
    std::vector<std::unique_ptr<DocSet>> output;
    std::vector<std::unique_ptr<Calculator>> subCalcs;

    void calc(oneapi::tbb::task_group* tg, int32_t segnum, solux::DocSet* domain) override {
      task_group_run(tg, [this, tg, segnum, domain]() {
        doCalc(tg, segnum, domain);
      });
    }

    void doCalc(oneapi::tbb::task_group* tg, int32_t segnum, solux::DocSet* domain) {
      auto& op = thisOp();

      if (segnum < 0) {
        // special case for empty index reader, we are done.
        // TODO: pass along to sub-calculators?
        doneCollecting();
        return;
      }

      // If we have subcalcs and if we determine that we are matching everything, then we can skip collecting
      // a new domain and just use the existing one.
      // TODO: put a type field on the query and replace this dynamic cast.
      bool matchEverything = (dynamic_cast<AllQuery*>(op.query) != nullptr) && thisOp().filters.empty();
;
      MergeableCollector* data = nullptr;
      int64_t numSegs = (int64_t)op.req.reader->segments().size();

      {
        auto poolGuard = MemPool::threadLocalPoolGuard();
        auto& seg = op.qcontext.topReader.segments()[segnum];
        auto* scorer = op.weight->createScorer(poolGuard.pool(), seg);

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
          std::unique_ptr<DocSet> newDomain;;
          if (!thisOp().filterWeights.empty()) {
            std::vector<std::unique_ptr<DocSet>> filters;
            std::vector<DocSet*> filterPtrs;
            for (auto weight : thisOp().filterWeights) {
              filters.push_back(thisOp().getDocSet(*weight, segnum));
              filterPtrs.push_back(filters.back().get());
            }
            if (domain) {
              filterPtrs.emplace_back(domain);
            }
            newDomain = DocSet::intersect(filterPtrs);
            filter = newDomain.get();
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
        auto* newDomain = matchEverything? domain : output[segnum].get();
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
      thisOp().fillQueryTopNResponse(*this);
      // TODO: figure out what state we can dump before and after this call.
    };
  };


  //  req, *qcontext, query, limit
  // NOTE: keep this constructor nothrow.  TopDocsReq is created via
  // google::protobuf::Arena::Create<TopDocsReq>, which registers
  // ~TopDocsReq() with the arena *before* the body runs.  A throw mid-ctor
  // leaves a half-constructed object scheduled for cleanup, and
  // ~TopDocsReq() crashes on uninitialized members during arena reset.
  // All work that can throw (Weight construction, schema lookups for sort
  // fields) goes in init(), which runs after the object is fully built.
  TopDocsReq(SearchRequest& req, std::string_view name, const proto::TopDocs& topDocsProto, Query::Context& qcontext, Query* query, int64_t topCount,
    std::span<std::pair<std::string_view, Query*>> filters)
    : SearchOp(req, name), topDocsProto(topDocsProto), qcontext(qcontext), query(query),
      weight(nullptr), topCount(topCount), filters(filters) {
  }

  // Build sort fields, the main weight, and the filter weights here rather
  // than in the ctor so that throwing paths (schema field-not-found, kNN
  // dim mismatch, etc.) propagate cleanly out of submitBody.  See ctor
  // comment for the arena-cleanup hazard.
  void init() override {
    SearchOp::init();

    // Sort fields can throw if the schema doesn't have the field.
    if (topDocsProto.sorts_size() > 0) {
      useFieldSort = true;
      // Static instances of special field types
      static ScoreFieldType scoreType;
      static DocFieldType docType;

      for (const auto& sortSpec : topDocsProto.sorts()) {
        SortField::SortOrder order = sortSpec.dir() == proto::SortSpec::DESC ?
          SortField::DESC : SortField::ASC;
        FieldComparator::MissingValue missing = FieldComparator::MISSING_LAST;

        if (sortSpec.field() == "_score_") {
          sortFields.emplace_back(sortSpec.field(), scoreType, order, missing);
        } else if (sortSpec.field() == "_docid_") {
          sortFields.emplace_back(sortSpec.field(), docType, order, missing);
        } else {
          auto fieldTypePtr = req.schema->getFieldTypeEx(sortSpec.field());
          if (!fieldTypePtr) {
            throw std::runtime_error(std::string("Field not found in schema: ") + std::string(sortSpec.field()));
          }
          sortFields.emplace_back(sortSpec.field(), *fieldTypePtr, order, missing);
        }
      }
    }

    // Weight ctors run validation that can throw (e.g. KnnQuery dim check).
    weight = query->createWeight(qcontext);
    if (filters.size() > 0) {
      filterWeights = req.requestPool.make_span<Query::Weight*>(filters.size());
      for (size_t i = 0; i < filters.size(); i++) {
        filterWeights[i] = filters[i].second->createWeight(qcontext);
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


  std::unique_ptr<DocSet> getDocSet(Query::Weight& weight, int32_t segnum) {
    auto poolGuard = MemPool::threadLocalPoolGuard();
    auto* scorer = weight.createScorer(poolGuard.pool(), req.reader->segments()[segnum]);
    DocSetBuilder builder(req.reader->segments()[segnum].maxDoc());
    if (scorer != nullptr) {
      for (;;) {
        auto doc = scorer->next();
        if (doc == PostingsReader::END) {
          break;
        }
        builder.add(doc);
      }
    }
    return builder.build();
  }

};


}
