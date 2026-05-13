#pragma once

#include <algorithm>
#include <atomic>
#include <cstring>
#include <vector>

#include <boost/unordered/unordered_flat_map.hpp>

#include "SearchOp.h"
#include "solux/query/Query.h"
#include "solux/search/Collector.h"
#include "solux/search/DocSet.h"
#include "solux/search/EmitDocs.h"
#include "solux/search/FieldSortCollector.h"
#include "solux/search/SortField.h"
#include "solux/util/AtomicMerger.h"
#include "solux/util/MemPool.h"
#include "solux/util/thread.h"

namespace solux {


// Hybrid-search fusion: runs N named source queries (each a full TopDocs spec)
// across all segments and merges their per-source ranked lists into one fused
// list.  V1 implements Reciprocal Rank Fusion: fused_score(d) = sum over
// sources s of 1 / (k + rank_s(d)), where rank_s(d) is d's 1-based rank in
// source s's ranked output (0 if d does not appear).  Standard k = 60.
//
// The Fusion-level filter is computed once per segment and shared across all
// sources.  Per-source filters (if any) are intersected with the shared
// filter at scoring time.
//
// V1 limitations: sub-ops (Fusion.ops) are not yet supported; the parser
// rejects them before constructing the FusionOp.
class FusionOp : public SearchOp {
public:
  // Per-source state populated from the source's TopDocs proto.
  struct Source {
    std::string_view name;
    const proto::TopDocs* sourceProto;
    Query* query;
    Query::Weight* weight = nullptr;
    int64_t topCount;
    std::vector<SortField> sortFields;
    bool useFieldSort = false;
    std::span<std::pair<std::string_view, Query*>> filters;
    std::span<Query::Weight*> filterWeights;
  };

  const proto::Fusion& fusionProto;
  Query::Context& qcontext;
  std::vector<Source> sources;
  int64_t topCount;
  std::span<std::pair<std::string_view, Query*>> filters;  // shared fusion-level filters
  std::span<Query::Weight*> filterWeights;
  int32_t rrfK = 60;

  // See TopDocsReq's ctor comment about the arena-cleanup hazard: keep this
  // ctor nothrow.  ProtobufSearchParser fully populates each Source (sort
  // fields, Weight, filter weights) and validates RRF parameters before
  // calling Arena::Create<FusionOp>.
  FusionOp(SearchRequest& req, std::string_view name, const proto::Fusion& fusionProto,
           Query::Context& qcontext, std::vector<Source>&& sources, int64_t topCount,
           std::span<std::pair<std::string_view, Query*>> filters,
           std::span<Query::Weight*> filterWeights, int32_t rrfK)
    : SearchOp(req, name), fusionProto(fusionProto), qcontext(qcontext),
      sources(std::move(sources)), topCount(topCount), filters(filters),
      filterWeights(filterWeights), rrfK(rrfK) {
  }

  // Resolve a single filter Weight to a DocSet for one segment.  Mirrors
  // TopDocsReq::getDocSet so behavior matches across hybrid + non-hybrid
  // requests.
  std::unique_ptr<DocSet> getDocSet(Query::Weight& weight, int32_t segnum) {
    auto poolGuard = MemPool::threadLocalPoolGuard();
    auto* scorer = weight.createScorer(poolGuard.pool(), req.reader->segments()[segnum]);
    DocSetBuilder builder(req.reader->segments()[segnum].maxDoc());
    if (scorer != nullptr) {
      for (;;) {
        auto doc = scorer->next();
        if (doc == PostingsReader::END) break;
        builder.add(doc);
      }
    }
    return builder.build();
  }


  class Calc : public SearchOp::Calculator {
  public:
    // Same shape as TopDocsReq::Calc::MergeableCollector.  Duplicated here
    // rather than shared because the type is small and the surface is
    // private to ops; we can lift it to its own header if a third user
    // appears.
    class MergeableCollector : public MergeableData {
    public:
      std::unique_ptr<TopDocsCollector> scoreCollector;
      std::unique_ptr<FieldSortCollector> fieldCollector;
      bool useFieldSort;

      MergeableCollector(size_t topCount, bool useFieldSort,
                         const std::vector<SortField>& sortFields,
                         IndexReader* reader = nullptr)
        : useFieldSort(useFieldSort) {
        if (!useFieldSort) {
          scoreCollector = std::make_unique<TopDocsCollector>(topCount);
        } else {
          std::unique_ptr<FieldComparator> comparator;
          if (sortFields.size() == 1) {
            comparator = sortFields[0].createComparator(topCount, reader);
          } else {
            comparator = std::make_unique<MultiFieldComparator>(sortFields, topCount, reader);
          }
          fieldCollector = std::make_unique<FieldSortCollector>(topCount, std::move(comparator));
        }
      }

      static MergeableCollector* merge(MergeableCollector* a, MergeableCollector* b) {
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

    // One merger per source.  unique_ptr because AtomicMerger holds an atomic
    // pointer and is non-movable.
    struct SourceState {
      AtomicMerger<MergeableCollector> merger;
      SourceState() : merger(nullptr, nullptr) {}
    };
    std::vector<std::unique_ptr<SourceState>> sourceStates;

    // Shared filter per segment, kept alive by Calc until fusion fires.
    // Indexed by segnum; only populated when filterWeights is non-empty.
    std::vector<std::unique_ptr<DocSet>> segFilters;

    // Counts source completions (each source finishing across all segments
    // increments this).  When it reaches numSources, fusion fires.
    std::atomic<int32_t> sourcesFinished{0};

    Calc(FusionOp& op, Calculator* parent) : SearchOp::Calculator(op, parent, -1, -1) {
      auto numSources = op.sources.size();
      auto numSegs = op.req.reader->segments().size();
      sourceStates.reserve(numSources);
      // op.sources is moved into FusionOp at construction and never resized
      // afterward, so the per-source pointer captured below stays valid for
      // the lifetime of Calc.
      for (size_t s = 0; s < numSources; s++) {
        auto state = std::make_unique<SourceState>();
        Source* srcPtr = &op.sources[s];
        IndexReader* reader = op.req.reader.get();
        state->merger.creator = [srcPtr, reader]() -> MergeableCollector* {
          return new MergeableCollector(srcPtr->topCount, srcPtr->useFieldSort,
                                        srcPtr->sortFields, reader);
        };
        state->merger.destroyer = [](MergeableCollector* data) {
          delete data;
        };
        sourceStates.emplace_back(std::move(state));
      }
      segFilters.resize(numSegs);
    }

    FusionOp& thisOp() {
      return static_cast<FusionOp&>(op);
    }

    solux::proto::Val* getTargetForSub(solux::proto::SearchResponse* searchResponse, Calculator* sub) override {
      auto* ourVal = parent->getTargetForSub(searchResponse, this);
      assert(ourVal != nullptr && (ourVal->kind_case() == solux::proto::Val::kDocs
        || ourVal->kind_case() == solux::proto::Val::KIND_NOT_SET));
      return &(*ourVal->mutable_docs()->mutable_ops())[sub->getOp().name];
    }

    void calc(oneapi::tbb::task_group* tg, int32_t segnum, solux::DocSet* domain) override {
      auto& op = thisOp();

      if (segnum < 0) {
        // Empty-index case: no segments, no per-source tasks will run.  Go
        // straight to emit; doFusion sees nullptr collectors for every source
        // and writes an empty DocList.
        task_group_run(tg, [this]() { doFusion(); });
        return;
      }

      // Compute the shared filter once for this segment.  Build per-segment
      // filter DocSets from the fusion-level filter weights and intersect
      // with the incoming domain (typically liveDocs from RootOp).
      DocSet* sharedFilter = domain;
      if (!op.filterWeights.empty()) {
        std::vector<std::unique_ptr<DocSet>> filterDocSets;
        std::vector<DocSet*> filterPtrs;
        filterDocSets.reserve(op.filterWeights.size());
        filterPtrs.reserve(op.filterWeights.size() + 1);
        for (auto* w : op.filterWeights) {
          filterDocSets.push_back(op.getDocSet(*w, segnum));
          filterPtrs.push_back(filterDocSets.back().get());
        }
        if (domain) filterPtrs.push_back(domain);
        if (filterPtrs.size() > 1) {
          segFilters[segnum] = DocSet::intersect(filterPtrs);
        } else {
          segFilters[segnum] = std::move(filterDocSets[0]);
        }
        sharedFilter = segFilters[segnum].get();
      }

      // Dispatch one task per source.  Each task collects top-K for its
      // source over this segment, then atomically signals fusion if it was
      // the last segment for this source AND every other source is also
      // done.
      auto numSegs = (int64_t)op.req.reader->segments().size();
      auto numSources = (int64_t)op.sources.size();
      for (size_t s = 0; s < op.sources.size(); s++) {
        task_group_run(tg, [this, s, segnum, sharedFilter, tg, numSegs, numSources]() {
          runSource(s, segnum, sharedFilter, tg, numSegs, numSources);
        });
      }
    }

    void runSource(size_t sourceIdx, int32_t segnum, DocSet* sharedFilter,
                   oneapi::tbb::task_group* tg, int64_t numSegs, int64_t numSources) {
      auto& op = thisOp();
      auto& src = op.sources[sourceIdx];
      auto& state = *sourceStates[sourceIdx];

      MergeableCollector* data = nullptr;
      {
        auto poolGuard = MemPool::threadLocalPoolGuard();
        auto& seg = op.qcontext.topReader.segments()[segnum];
        auto* scorer = src.weight->createScorer(poolGuard.pool(), seg);

        data = state.merger.obtain();

        if (scorer != nullptr) {
          DocSet* filter = sharedFilter;
          std::unique_ptr<DocSet> mergedFilter;
          if (!src.filterWeights.empty()) {
            std::vector<std::unique_ptr<DocSet>> perSrcFilters;
            std::vector<DocSet*> filterPtrs;
            perSrcFilters.reserve(src.filterWeights.size());
            filterPtrs.reserve(src.filterWeights.size() + 1);
            for (auto* w : src.filterWeights) {
              perSrcFilters.push_back(op.getDocSet(*w, segnum));
              filterPtrs.push_back(perSrcFilters.back().get());
            }
            if (sharedFilter) filterPtrs.push_back(sharedFilter);
            if (filterPtrs.size() > 1) {
              mergedFilter = DocSet::intersect(filterPtrs);
              filter = mergedFilter.get();
            } else {
              mergedFilter = std::move(perSrcFilters[0]);
              filter = mergedFilter.get();
            }
          }

          if (data->useFieldSort) {
            data->fieldCollector->setSegment(segnum, &seg.postingsReader());
            collectTopK(segnum, scorer, filter, nullptr, *data->fieldCollector);
          } else {
            collectTopK(segnum, scorer, filter, nullptr, *data->scoreCollector);
          }
        }
      }

      auto count = state.merger.release(data);
      if (count == numSegs) {
        // This source has finished all segments.  Bump the global source
        // counter; the source that arrives last fires fusion.
        auto finished = sourcesFinished.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (finished == numSources) {
          task_group_run(tg, [this]() { doFusion(); });
        }
      }
    }

    struct SegdocHash {
      size_t operator()(const segdoc& d) const noexcept {
        return std::hash<int64_t>{}(std::bit_cast<int64_t>(d));
      }
    };

    // RRF merge across the per-source ranked lists, then emit a fused
    // DocList via the shared streaming emitter.
    void doFusion() {
      auto& op = thisOp();
      int32_t k = op.rrfK;

      // Per-source ranked doc lists.  Holding them as vectors of segdoc
      // keeps the inner loops branch-free and avoids carrying the
      // SortDoc/ScoreDoc shape further.
      std::vector<std::vector<segdoc>> rankedPerSource;
      rankedPerSource.reserve(op.sources.size());
      for (size_t s = 0; s < op.sources.size(); s++) {
        auto* mc = sourceStates[s]->merger.getData();
        std::vector<segdoc> docs;
        if (mc != nullptr) {
          if (mc->useFieldSort) {
            auto sortDocs = mc->fieldCollector->sort();
            docs.reserve(sortDocs.size());
            for (auto& sd : sortDocs) docs.push_back(sd.doc);
          } else {
            auto scoreDocs = mc->scoreCollector->sort();
            docs.reserve(scoreDocs.size());
            for (auto& sd : scoreDocs) docs.push_back(sd.doc);
          }
        }
        rankedPerSource.emplace_back(std::move(docs));
      }

      // Accumulate fused scores by segdoc.
      boost::unordered_flat_map<segdoc, float, SegdocHash> fused;
      for (auto& ranked : rankedPerSource) {
        for (int32_t rank = 0; rank < (int32_t)ranked.size(); rank++) {
          // 1-based rank; RRF: 1 / (k + rank).
          float contrib = 1.0f / (float)(k + rank + 1);
          fused[ranked[rank]] += contrib;
        }
      }

      // Sort by fused score desc, segdoc asc as a stable tiebreaker.
      std::vector<std::pair<segdoc, float>> fusedList;
      fusedList.reserve(fused.size());
      for (auto& [d, score] : fused) fusedList.emplace_back(d, score);
      std::sort(fusedList.begin(), fusedList.end(),
                [](const auto& a, const auto& b) {
                  if (a.second != b.second) return a.second > b.second;
                  return a.first < b.first;
                });

      int64_t numCollected = std::min((int64_t)fusedList.size(), op.topCount);
      int64_t totalHits = (int64_t)fused.size();

      auto getDocList = [this](solux::proto::SearchResponse* resp) -> solux::proto::DocList& {
        auto& val = *getTarget(resp);
        return *val.mutable_docs();
      };

      emitDocsResponse(op.req, getDocList,
        numCollected,
        [&fusedList](int64_t i) { return fusedList[i].first; },
        [&fusedList](int64_t i) { return fusedList[i].second; },
        totalHits,
        op.fusionProto.fields(),
        op.fusionProto.batch_size(),
        op.fusionProto.offset(),
        op.fusionProto.get_number(),
        op.fusionProto.get_scores());
    }
  };


  Calculator* createCalculator(Calculator* parent, int64_t slot = -1, int64_t numSlots = -1) override {
    return new Calc(*this, parent);
  }
};


}
