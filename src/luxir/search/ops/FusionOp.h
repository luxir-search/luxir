// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <atomic>
#include <cstring>
#include <vector>

#include <boost/unordered/unordered_flat_map.hpp>

#include "SearchOp.h"
#include "TopDocsReq.h"
#include "luxir/query/Query.h"
#include "luxir/query/QueryPrep.h"
#include "luxir/search/DocSet.h"
#include "luxir/search/EmitDocs.h"
#include "luxir/search/MergeableCollector.h"
#include "luxir/util/MemPool.h"
#include "luxir/util/thread.h"

namespace luxir {


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
// Sub-ops run over the fused candidate set: every doc in any source ranked
// list after shared and per-source filters, exactly the set found counts.
// Fusion limit and offset do not affect this domain; source ops are rejected.
//
// As a facet bucket child, one independent fusion runs per bucket over that
// bucket's domain and its DocList lands in the bucket's slot (see
// canEmitAsBucketChild).
class FusionOp : public SearchOp {
public:
  const ReqFusion& fusionProto;
  // Each source is a full TopDocsReq parented to this FusionOp.  Their
  // rankingSinks are set by the parser to deliver to FusionOp::Calc.
  std::vector<TopDocsReq*> sources;
  int64_t topCount; // Collection depth (offset + page size, capped by maxDoc).
  std::span<ParsedFilter> filters;  // shared fusion-level filters
  std::span<Query::Weight*> filterWeights;
  std::span<FilterCache::Use*> filterUses;
  int32_t rrfK = 60;

  // ProtobufSearchParser builds each source TopDocsReq (with rankingSink wired)
  // and the filter weights, then passes them in (see TopDocsReq's ctor comment).
  FusionOp(SearchRequest& req, std::string_view name, const ReqFusion& fusionProto,
           std::vector<TopDocsReq*>&& sources, int64_t topCount,
           std::span<ParsedFilter> filters,
           std::span<Query::Weight*> filterWeights, int32_t rrfK)
    : SearchOp(req, name), fusionProto(fusionProto),
      sources(std::move(sources)), topCount(topCount), filters(filters),
      filterWeights(filterWeights), rrfK(rrfK) {
    if (!filterWeights.empty()) {
      assert(!this->sources.empty());
      assert(filterWeights.size() == filters.size());
      auto& context = this->sources.front()->planning;
      filterUses = context.pool.make_span<FilterCache::Use*>(filters.size());
      for (size_t i = 0; i < filters.size(); i++) {
        filterUses[i] = context.getFilterUse(*filters[i].query);
      }
    }
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
    // RRF-merged (segdoc, fused score) list, sorted; backs the emitter's
    // getDoc/getScore callbacks, which can outlive doFusion (paused emitter).
    std::vector<std::pair<segdoc, float>> fusedList;

    // Shared filter DocSet per segment, kept alive until fusion emits.
    std::vector<DomainHandle> segFilters;
    std::vector<std::unique_ptr<SearchOp::Calculator>> subCalcs;
    std::vector<DomainHandle> fusedDomains;
    oneapi::tbb::task_group* taskGroup = nullptr;

    bool needsPrepare = false;
    std::atomic<int32_t> preparedDomainsSeen{0};
    std::vector<DomainHandle> baseDomains;
    std::vector<DocSet*> baseDomainViews;
    std::vector<QueryPrep::PreparedSource> preparedFilterSources;

    // Number of sources that have delivered their ranking.  When this hits
    // op.sources.size(), the last delivery launches doFusion.
    std::atomic<int32_t> sourcesDelivered{0};

    Calc(FusionOp& op, Calculator* parent, int64_t slot, int64_t numSlots)
      : SearchOp::Calculator(op, parent, slot, numSlots) {
      auto numSources = op.sources.size();
      auto numSegs = op.req.reader->segments().size();
      sourceCalcs.reserve(numSources);
      deliveredCollectors.assign(numSources, nullptr);
      for (auto* src : op.sources) {
        sourceCalcs.emplace_back(src->createCalculator(this, -1));
      }
      subCalcs.reserve(op.subOps.size());
      for (auto& [key, subOp] : op.subOps) {
        subCalcs.emplace_back(subOp->createCalculator(this, -1));
      }
      segFilters.resize(numSegs);
      for (auto* weight : op.filterWeights) {
        if (weight->needsPrepare()) {
          needsPrepare = true;
          break;
        }
      }
      if (needsPrepare) {
        baseDomains.resize(numSegs);
        baseDomainViews.resize(numSegs);
      }
    }

    FusionOp& thisOp() {
      return static_cast<FusionOp&>(op);
    }

    luxir::api::Val* getTargetForSub(SearchResponse* resp, Calculator* sub) override {
      auto* ourVal = parent->getTargetForSub(resp, this);
      assert(ourVal != nullptr);
      auto& target = slotTarget<luxir::api::ArrVal>(*ourVal, resp->mr);
      assert(std::holds_alternative<luxir::api::DocList>(target.kind)
             || std::holds_alternative<std::monostate>(target.kind));
      auto& dl = oneofMut<luxir::api::DocList>(target);
      return build::opsSlot(dl.ops, op.subOps.size(), sub->getOp().name, resp->mr);
    }

    void calc(oneapi::tbb::task_group* tg, int32_t segnum,
              DomainHandle domain) override {
      assert(domain.isDeliverable());
      // Every segment shares the request task group; only the segment-0 (or
      // empty-index) dispatch writes it so there is a single writer.  That
      // dispatch precedes source completion, whose acq_rel counters order the
      // write before doFusion reads it.
      if (segnum <= 0) taskGroup = tg;
      if (needsPrepare) {
        task_group_run(tg, [this, tg, segnum, domain]() {
          doPrepareDomain(tg, segnum, domain);
        });
        return;
      }
      doCalc(tg, segnum, domain);
    }

    DomainHandle buildSharedFilter(int32_t segnum, DocSet* domain) {
      auto& op = thisOp();
      if (op.filterWeights.empty()) return {};

      std::vector<DomainHandle> filterDocSets;
      std::vector<DocSet*> filterPtrs;
      filterDocSets.reserve(op.filterWeights.size());
      filterPtrs.reserve(op.filterWeights.size());
      auto& seg = op.req.reader->segments()[segnum];
      for (size_t i = 0; i < op.filterWeights.size(); i++) {
        if (i < preparedFilterSources.size()) {
          filterDocSets.push_back(QueryPrep::materializeEffectiveFilter(
              preparedFilterSources[i], *op.req.reader, seg, domain));
        } else {
          filterDocSets.push_back(QueryPrep::materializeEffectiveFilter(
              *op.filterWeights[i], op.filterUses[i],
              *op.req.reader, seg, domain));
        }
        filterPtrs.push_back(filterDocSets.back().get());
      }
      if (filterPtrs.empty()) return {};
      if (filterPtrs.size() == 1) {
        auto result = std::move(filterDocSets[0]);
        assert(!op.sources.empty());
        return std::move(result).pinnedWith(
            op.sources.front()->planning.filterUses);
      }
      return DomainHandle(DocSet::intersect(filterPtrs));
    }

    void doPrepareDomain(oneapi::tbb::task_group* tg, int32_t segnum,
                         DomainHandle domain) {
      auto& op = thisOp();
      if (segnum < 0) {
        doCalc(tg, segnum, std::move(domain));
        return;
      }

      baseDomains[(size_t)segnum] = std::move(domain);
      baseDomainViews[(size_t)segnum] = baseDomains[(size_t)segnum].get();
      auto count = preparedDomainsSeen.fetch_add(1, std::memory_order_acq_rel) + 1;
      if (count != (int32_t)op.req.reader->segments().size()) return;

      Query::Weight::PrepareContext baseCtx{
        *op.req.reader,
        std::span<DocSet* const>(baseDomainViews.data(), baseDomainViews.size()),
        tg != nullptr
      };
      preparedFilterSources = QueryPrep::prepareFilterSources(
          op.filterWeights, op.filterUses, baseCtx);

      for (int32_t i = 0; i < (int32_t)op.req.reader->segments().size(); i++) {
        segFilters[(size_t)i] = buildSharedFilter(i, baseDomainViews[(size_t)i]);
      }

      for (int32_t i = 0; i < (int32_t)op.req.reader->segments().size(); i++) {
        task_group_run(tg, [this, tg, i]() {
          dispatchSources(tg, i, segFilters[(size_t)i]);
        });
      }
    }

    void doCalc(oneapi::tbb::task_group* tg, int32_t segnum,
                DomainHandle domain) {
      auto& op = thisOp();
      if (segnum < 0) {
        // Empty-index case: hand each source a -1 dispatch so they fire
        // their rankingSink with a null collector.  Once all sources have
        // delivered, doFusion writes an empty DocList.
        dispatchSources(tg, -1, {});
        return;
      }

      DomainHandle sharedFilter = std::move(domain);
      if (!op.filterWeights.empty()) {
        segFilters[(size_t)segnum] =
            buildSharedFilter(segnum, sharedFilter.get());
        sharedFilter = segFilters[segnum];
      }

      dispatchSources(tg, segnum, std::move(sharedFilter));
    }

    void dispatchSources(oneapi::tbb::task_group* tg, int32_t segnum,
                         DomainHandle sharedFilter) {
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
          float contrib = 1.0f / (float)((int64_t)k + rank + 1);
          fused[ranked[rank]] += contrib;
        }
      }

      // Sort by fused score desc, segdoc asc as a stable tiebreaker.  A Calc
      // member (not a local): the emitter's getDoc/getScore callbacks read it,
      // and a flow-controlled emitter can outlive this function.
      fusedList.reserve(fused.size());
      for (auto& [d, score] : fused) fusedList.emplace_back(d, score);
      std::sort(fusedList.begin(), fusedList.end(),
                [](const auto& a, const auto& b) {
                  if (a.second != b.second) return a.second > b.second;
                  return a.first < b.first;
                });

      if (!subCalcs.empty()) {
        std::vector<segdoc> candidates;
        candidates.reserve(fusedList.size());
        for (auto& [doc, score] : fusedList) candidates.push_back(doc);
        std::sort(candidates.begin(), candidates.end());
        const auto& segments = op.req.reader->segments();
        fusedDomains.reserve(segments.size());
        auto doc = candidates.begin();
        for (int32_t segnum = 0; segnum < (int32_t)segments.size(); segnum++) {
          DocSetBuilder builder(segments[segnum].maxDoc());
          while (doc != candidates.end() && doc->segment() == segnum) {
            builder.add(doc->docId());
            ++doc;
          }
          // An empty segment needs an empty set: a null domain means all docs.
          fusedDomains.emplace_back(builder.build());
        }
        for (auto& sub : subCalcs) sub->calcAll(taskGroup, fusedDomains);
      }

      int64_t numCollected = std::min((int64_t)fusedList.size(), op.topCount);
      int64_t totalHits = (int64_t)fused.size();

      auto getDocList = [this](SearchResponse* resp) -> luxir::api::DocList& {
        return *slotArm<luxir::api::DocList>(resp);
      };
      DocEmission emission = underBucketSlot()
          ? DocEmission::FINAL_RESPONSE : DocEmission::STREAM;

      emitDocsResponse(op.req, getDocList,
        numCollected,
        [this](int64_t i) { return fusedList[i].first; },
        [this](int64_t i) { return fusedList[i].second; },
        totalHits,
        op.fusionProto.fields,
        op.fusionProto.batch_size,
        op.fusionProto.offset,
        op.fusionProto.get_number,
        op.fusionProto.get_scores,
        op.fusionProto.document_format,
        emission);
    }
  };


  Calculator* createCalculator(Calculator* parent, int64_t slot = -1, int64_t numSlots = -1) override {
    return new Calc(*this, parent, slot, numSlots);
  }

  bool canEmitAsBucketChild() const override { return true; }

  // Sources plus the fusion state retained until the bucket's list is emitted:
  // the fused (segdoc, score) list can hold the union of every source window.
  size_t facetBucketResidentBytes() const override {
    size_t bytes = saturatingAdd(
        sizeof(Calc),
        req.reader->segments().size() * 2 * sizeof(DomainHandle));
    for (auto* source : sources) {
      bytes = saturatingAdd(bytes, source->facetBucketResidentBytes());
      bytes = saturatingAdd(
          bytes, (size_t)source->topCount * sizeof(std::pair<segdoc, float>));
    }
    for (const auto& [name, child] : subOps) {
      unused(name);
      bytes = saturatingAdd(bytes, child->facetBucketResidentBytes());
    }
    return bytes;
  }
};


}
