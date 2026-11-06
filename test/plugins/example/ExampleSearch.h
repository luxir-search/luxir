// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "ExampleNodeState.h"
#include "luxir/api/build.h"
#include "luxir/query/BooleanQuery.h"
#include "luxir/query/QueryRegistry.h"
#include "luxir/query/ParseContext.h"
#include "luxir/reader/DocsEnum.h"
#include "luxir/reader/TermsEnum.h"
#include "luxir/search/DocSet.h"
#include "luxir/search/EmitDocs.h"
#include "luxir/search/SearchOpRegistry.h"
#include "luxir/search/ops/SearchOp.h"
#include "luxir/util/thread.h"

namespace example {

// doc_ids: {"doc_ids": {"values": ["a", "b"], "within": <query>}} selects the
// documents with those ids, optionally restricted to a nested query. The
// expression form is doc_ids([a, b], within=cat_s:x).
inline luxir::Query* makeDocIds(luxir::PluginQueryContext& ctx) {
  namespace api = luxir::api;
  if (auto* node = ctx.node()) node->pluginState<NodeState>("example")->queries++;
  auto& pool = ctx.parse.pool;
  auto values = ctx.params.getStringList("values");
  const api::Val* within = ctx.params.get("within");
  ctx.params.rejectUnknown();
  auto ids = pool.make_span<std::string_view>(values.size());
  std::copy(values.begin(), values.end(), ids.begin());
  auto* val = pool.make<api::Val>();
  val->kind = api::ArrStr{ids};
  api::Query anyOf;
  anyOf.kind = api::AnyOfQuery{"id", val};
  luxir::Query* selected = ctx.parseQuery(anyOf);
  if (within == nullptr) return selected;
  auto mandatory = pool.make_span<luxir::Query*>(1);
  auto filter = pool.make_span<luxir::Query*>(1);
  mandatory[0] = selected;
  filter[0] = ctx.parseQuery(*within);
  return pool.make<luxir::BooleanQuery>(mandatory, std::span<luxir::Query*>{}, std::span<luxir::Query*>{}, filter);
}

inline luxir::PluginQueryDef docIdsDef() {
  using Kind = luxir::PluginQueryArg::Kind;
  return {makeDocIds, {{"values", Kind::STRING_LIST}, {"within", Kind::QUERY}}, "values"};
}

// sample_docs: {"sample_docs": {"ids": [...], "fields": [...], "ops": {...}}}
// returns the live documents with those ids in the order given, each with a
// synthetic _rank column, and runs its nested ops over exactly those
// documents. It exercises plugin ops, extra columns and op-owned domains.
class SampleDocsOp : public luxir::SearchOp {
public:
  std::vector<std::string_view> ids;
  std::vector<std::string_view> fields;
  std::vector<std::string_view> rankNames{"_rank"};

  SampleDocsOp(luxir::SearchRequest& req, std::string_view name) : SearchOp(req, name) {}

  class Calc : public Calculator {
  public:
    // Per segment: (index into ids, doc).
    std::vector<std::vector<std::pair<size_t, int32_t>>> found;
    std::atomic<int32_t> pending;
    std::vector<std::unique_ptr<Calculator>> subCalcs;
    std::vector<luxir::segdoc> ranked;
    std::vector<int64_t> ranks;
    std::vector<luxir::DomainHandle> domains;
    oneapi::tbb::task_group* taskGroup = nullptr;

    Calc(SampleDocsOp& op, Calculator* parent, int64_t slot, int64_t numSlots,
         const luxir::StructuralConstraint* structural)
      : Calculator(op, parent, slot, numSlots, structural),
        found(op.req.reader->segments().size()),
        pending((int32_t)op.req.reader->segments().size()) {
      for (auto& [key, sub] : op.subOps) subCalcs.emplace_back(sub->createCalculator(this, -1));
    }

    SampleDocsOp& sampleOp() { return static_cast<SampleDocsOp&>(op); }

    luxir::api::Val* getTargetForSub(luxir::SearchResponse* resp, SearchOp& sub) override {
      auto* ourVal = parent->getTargetForSub(resp, op);
      auto& target = slotTarget<luxir::api::ArrVal>(*ourVal, resp->mr);
      auto& docList = luxir::oneofMut<luxir::api::DocList>(target);
      return luxir::api::build::opsSlot(docList.ops, op.subOps.size(), sub.name, resp->mr);
    }

    void calc(oneapi::tbb::task_group* tg, int32_t segnum, luxir::DomainHandle domain) override {
      if (segnum < 0) {
        finish(tg);
        return;
      }
      if (segnum == 0) taskGroup = tg;
      auto& reader = *op.req.reader;
      auto& segment = reader.segments()[(size_t)segnum];
      if (const auto* info = reader.field("id").at(segment)) {
        auto poolGuard = luxir::MemPool::threadLocalPoolGuard();
        luxir::TermsEnum terms(poolGuard.pool(), segment.postingsReader(), *info);
        for (size_t i = 0; i < sampleOp().ids.size(); i++) {
          if (!terms.seek(sampleOp().ids[i])) continue;
          luxir::DocsOnlyEnum docs(terms);
          for (int32_t doc = docs.next(); doc != luxir::DocsEnumMeta::END; doc = docs.next()) {
            if (domain.get() == nullptr || domain.get()->get(doc)) found[(size_t)segnum].emplace_back(i, doc);
          }
        }
      }
      if (pending.fetch_sub(1, std::memory_order_acq_rel) == 1) finish(taskGroup);
    }

    void finish(oneapi::tbb::task_group* tg) {
      std::vector<std::pair<size_t, luxir::segdoc>> hits;
      for (size_t s = 0; s < found.size(); s++) {
        for (auto [i, doc] : found[s]) hits.emplace_back(i, luxir::segdoc((int32_t)s, doc));
      }
      std::ranges::sort(hits);
      for (auto& [i, doc] : hits) {
        ranked.push_back(doc);
        ranks.push_back((int64_t)ranked.size());
      }
      if (!subCalcs.empty()) {
        std::vector<luxir::segdoc> sorted = ranked;
        std::ranges::sort(sorted);
        auto doc = sorted.begin();
        for (int32_t s = 0; s < (int32_t)op.req.reader->segments().size(); s++) {
          luxir::DocSetBuilder builder(op.req.reader->segments()[(size_t)s].maxDoc());
          for (; doc != sorted.end() && doc->segment() == s; ++doc) builder.add(doc->docId());
          domains.emplace_back(builder.build());  // an empty set, never null (null = all docs)
        }
        luxir::task_group_fanout(domains.empty() ? nullptr : tg, subCalcs.size(), [&](size_t i) {
          auto* subCalc = subCalcs[i].get();
          return [this, subCalc, tg]() { subCalc->calcAll(tg, domains); };
        });
      }
      std::vector<luxir::ExtraColumn> extraColumns;
      auto& sop = sampleOp();
      for (auto name : sop.rankNames) {
        extraColumns.push_back({name, [this](int64_t i, luxir::api::Val& out, std::pmr::memory_resource&) {
          out.kind = ranks[(size_t)i];
        }});
      }
      luxir::emitDocsResponse(
          op.req, op.flags, [this](luxir::SearchResponse* r) -> luxir::api::DocList& {
            return *slotArm<luxir::api::DocList>(r);
          },
          (int64_t)ranked.size(), [this](int64_t i) { return ranked[(size_t)i]; },
          [](int64_t) { return 0.0f; }, (int64_t)ranked.size(), sop.fields, 0, 0, true, false,
          luxir::api::DocFormat{}, underBucketSlot() ? luxir::DocEmission::FINAL_RESPONSE : luxir::DocEmission::STREAM,
          {}, false, extraColumns);
    }
  };

  Calculator* createCalculator(Calculator* parent, int64_t slot, int64_t numSlots,
                               const luxir::StructuralConstraint* structural) override {
    return new Calc(*this, parent, slot, numSlots, structural);
  }
};

inline luxir::SearchOp* makeSampleDocs(luxir::PluginOpContext& ctx) {
  ctx.node().pluginState<NodeState>("example")->ops++;
  auto* op = luxir::arenaCreate<SampleDocsOp>(ctx.req.arena, ctx.req, ctx.name);
  op->ids = ctx.params.getStringList("ids");
  op->fields = ctx.params.getStringList("fields");
  if (ctx.params.has("rank_names")) op->rankNames = ctx.params.getStringList("rank_names");
  ctx.params.rejectUnknown();
  ctx.addSubs(*op);
  return op;
}

}  // namespace example
