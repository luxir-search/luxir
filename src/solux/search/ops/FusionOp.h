#pragma once

#include <algorithm>
#include <atomic>
#include <cstring>
#include <vector>

#include <boost/unordered/unordered_flat_map.hpp>

#include "SearchOp.h"
#include "TopDocsReq.h"
#include "solux/query/Query.h"
#include "solux/search/DocSet.h"
#include "solux/search/EmitDocs.h"
#include "solux/search/MergeableCollector.h"
#include "solux/util/MemPool.h"
#include "solux/util/thread.h"

namespace solux {


// Hybrid-search fusion: runs N named source queries (each a full TopDocs)
// across all segments and merges their per-source ranked lists into one
// fused list.  V1 implements Reciprocal Rank Fusion: fused_score(d) =
// sum over sources s of 1 / (k + rank_s(d)), where rank_s(d) is d's
// 1-based rank in source s's ranked output (0 if d does not appear).
// Standard k = 60.
//
// Each source is a child TopDocsReq with its rankingSink wired to deliver
// the merged top-K back here.  This means TopDocsReq's per-segment
// scoring/collection machinery is reused as-is (filters, sort modes,
// match-everything optimization) - FusionOp only orchestrates the shared
// filter and the cross-source fusion step.
//
// The Fusion-level filter is computed once per segment and passed to every
// source as its domain.  Per-source filters (if any) are intersected by
// the source's TopDocsReq with the incoming domain.
//
// V1 limitations: top-level Fusion sub-ops (over the union of source
// matches) are not yet wired; the parser rejects them.
class FusionOp : public SearchOp {
public:
  const proto::Fusion& fusionProto;
  // Each source is a full TopDocsReq parented to this FusionOp.  Their
  // rankingSinks are set by the parser to deliver to FusionOp::Calc.
  std::vector<TopDocsReq*> sources;
  int64_t topCount;
  std::span<std::pair<std::string_view, Query*>> filters;  // shared fusion-level filters
  std::span<Query::Weight*> filterWeights;
  int32_t rrfK = 60;

  // Nothrow ctor (Arena::Create hazard - see TopDocsReq's ctor comment).
  // ProtobufSearchParser builds each source TopDocsReq (with rankingSink
  // wired) and the filter weights before allocation.
  FusionOp(SearchRequest& req, std::string_view name, const proto::Fusion& fusionProto,
           std::vector<TopDocsReq*>&& sources, int64_t topCount,
           std::span<std::pair<std::string_view, Query*>> filters,
           std::span<Query::Weight*> filterWeights, int32_t rrfK)
    : SearchOp(req, name), fusionProto(fusionProto),
      sources(std::move(sources)), topCount(topCount), filters(filters),
      filterWeights(filterWeights), rrfK(rrfK) {
  }

  // Sources are not in `subOps` (different lifecycle: they deliver via
  // rankingSink rather than emit), so SearchOp::init() does not reach
  // them - walk them explicitly here.
  void init() override {
    SearchOp::init();
    for (auto* src : sources) {
      src->init();
    }
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
    // One sub-calc per source TopDocsReq.  Each is a TopDocsReq::Calc that
    // collects across all segments and ultimately fires its rankingSink,
    // delivering its merged collector to this Calc.
    std::vector<std::unique_ptr<SearchOp::Calculator>> sourceCalcs;

    // Per-source merged collectors, populated by acceptSourceRanking.
    // Index parallel to op.sources.  Null entries mean the empty-index
    // path or an all-segments-empty source.
    std::vector<MergeableCollector*> deliveredCollectors;

    // Shared filter DocSet per segment, kept alive until fusion emits.
    std::vector<std::unique_ptr<DocSet>> segFilters;

    // Number of sources that have delivered their ranking.  When this hits
    // op.sources.size(), the last delivery launches doFusion.
    std::atomic<int32_t> sourcesDelivered{0};

    Calc(FusionOp& op, Calculator* parent) : SearchOp::Calculator(op, parent, -1, -1) {
      auto numSources = op.sources.size();
      auto numSegs = op.req.reader->segments().size();
      sourceCalcs.reserve(numSources);
      deliveredCollectors.assign(numSources, nullptr);
      for (auto* src : op.sources) {
        sourceCalcs.emplace_back(src->createCalculator(this, -1));
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
        // Empty-index case: hand each source a -1 dispatch so they fire
        // their rankingSink with a null collector.  Once all sources have
        // delivered, doFusion writes an empty DocList.
        for (auto& sc : sourceCalcs) {
          sc->calc(tg, -1, nullptr);
        }
        return;
      }

      // Compute the shared filter once per segment.  Intersect the
      // fusion-level filter weights with the incoming domain (typically
      // liveDocs from RootOp) and stash the result so it stays alive
      // until every source has finished using it.
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

      // Dispatch each source's TopDocsReq::Calc::calc with the shared
      // filter as its domain.  The source will intersect with its own
      // filterWeights, collect top-K, and ultimately invoke rankingSink
      // which lands in acceptSourceRanking below.
      for (auto& sc : sourceCalcs) {
        sc->calc(tg, segnum, sharedFilter);
      }
    }

    // Called by each source TopDocsReq's rankingSink when its top-K is
    // merged across all segments.  `mc` may be null on the empty-index
    // path.  `idx` is the source's position in op.sources, captured by
    // the parser when wiring the rankingSink.
    void acceptSourceRanking(size_t idx, MergeableCollector* mc) {
      deliveredCollectors[idx] = mc;

      // Release side of this fetch_add publishes the deliveredCollectors[idx]
      // write above; the last thread's acquire side observes all prior
      // publishes, so the plain reads in doFusion see every slot.
      auto finished = sourcesDelivered.fetch_add(1, std::memory_order_acq_rel) + 1;
      if (finished == (int32_t)thisOp().sources.size()) {
        // Inline call matches TopDocsReq::doneCollecting -> fillQueryTopNResponse
        // pattern: emitDocsResponse manages its own task scheduling internally.
        doFusion();
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

      // Pull each source's ranked docs out of its delivered collector.
      std::vector<std::vector<segdoc>> rankedPerSource;
      rankedPerSource.reserve(op.sources.size());
      for (size_t s = 0; s < op.sources.size(); s++) {
        auto* mc = deliveredCollectors[s];
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
