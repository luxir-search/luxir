// Copyright 2026 Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "luxir/reader/NestedBlocks.h"
#include "luxir/search/EmitDocs.h"
#include "luxir/search/ops/SearchOp.h"

namespace luxir {

// nested / reverse_nested: re-points the incoming document domain at a nested
// path's child documents (nested) or back at their root documents
// (reverse_nested), then runs sub-operations over that domain. The result is a
// DocList with the domain's document count and the sub-operations in ops.
class NestedOp final : public SearchOp {
public:
  std::string_view path;  // empty for reverse_nested
  bool reverse;

  NestedOp(SearchRequest& req, std::string_view name, std::string_view path, bool reverse)
      : SearchOp(req, name), path(path), reverse(reverse) {}

  class Calc final : public Calculator {
    NestedOp& nestedOp;
    std::vector<DomainHandle> domains;
    std::vector<std::unique_ptr<Calculator>> subCalcs;
    std::atomic<int32_t> pending;
    std::atomic<int64_t> found{0};
    oneapi::tbb::task_group* taskGroup = nullptr;

    std::unique_ptr<DocSet> transform(IndexReader::Segment& segment, DocSet* in) {
      auto blocks = segment.postingsReader().nestedBlocks();
      DocSetBuilder out(segment.maxDoc());
      if (nestedOp.reverse) {
        // A child maps to the root that follows it; other documents are roots.
        if (in) {
          int32_t last = -1; // rootOf is monotonic, so duplicates are adjacent
          in->forEach([&](int32_t doc) {
            int32_t root = blocks && blocks->isChild(doc) ? blocks->rootOf(doc) : doc;
            if (root != last) out.add(root);
            last = root;
          });
        } else {
          for (int32_t doc = 0; doc < segment.maxDoc(); ++doc) {
            if (!blocks || !blocks->isChild(doc)) out.add(doc);
          }
        }
        return out.build();
      }
      if (!blocks) return out.build();
      auto found = blocks->paths.find(nestedOp.path);
      if (found == blocks->paths.end()) return out.build();
      auto& bits = found->second;
      for (int32_t doc = bits.nextSetBit(0); doc < segment.maxDoc(); doc = bits.nextSetBit(doc + 1)) {
        int32_t root = blocks->rootOf(doc);
        if (in ? in->get(root) : true) out.add(doc);
      }
      return out.build();
    }

  public:
    Calc(NestedOp& op, Calculator* parent, int64_t slot, int64_t numSlots, const StructuralConstraint* structural)
        : Calculator(op, parent, slot, numSlots, structural), nestedOp(op),
          domains(op.req.reader->segments().size()), pending((int32_t)op.req.reader->segments().size()) {
      for (auto& [key, sub] : op.subOps) subCalcs.emplace_back(sub->createCalculator(this, -1));
    }

    api::Val* getTargetForSub(SearchResponse* resp, SearchOp& sub) override {
      // Reached under a child's response lock; do not re-enter getTarget here.
      auto* ourVal = parent->getTargetForSub(resp, op);
      auto& target = slotTarget<api::ArrVal>(*ourVal, resp->mr);
      auto& list = oneofMut<api::DocList>(target);
      return api::build::opsSlot(list.ops, op.subOps.size(), sub.name, resp->mr);
    }

    void calc(oneapi::tbb::task_group* tg, int32_t segnum, DomainHandle domain) override {
      if (segnum < 0) { finish(tg); return; }
      if (segnum == 0) taskGroup = tg;
      auto& segment = op.req.reader->segments()[(size_t)segnum];
      // Filtering is against live documents: root domains are already live.
      auto* incoming = domain.get();
      auto produced = transform(segment, incoming ? incoming : segment.liveDocSet());
      found.fetch_add(produced->card(), std::memory_order_relaxed);
      domains[(size_t)segnum] = DomainHandle(std::move(produced));
      if (pending.fetch_sub(1, std::memory_order_acq_rel) == 1) finish(taskGroup);
    }

    void finish(oneapi::tbb::task_group* tg) {
      task_group_fanout(subCalcs.empty() ? nullptr : tg, subCalcs.size(), [&](size_t i) {
        auto* subCalc = subCalcs[i].get();
        return [this, subCalc, tg]() { subCalc->calcAll(tg, domains); };
      });
      int64_t total = found.load(std::memory_order_relaxed);
      emitDocsResponse(
          op.req, op.flags, [this](SearchResponse* r) -> api::DocList& { return *slotArm<api::DocList>(r); },
          0, [](int64_t) { return segdoc(0, 0); }, [](int64_t) { return 0.0f; }, total,
          std::span<const std::string_view>{}, 0, 0, true, false, api::DocFormat{},
          DocEmission::FINAL_RESPONSE);
    }
  };

  Calculator* createCalculator(Calculator* parent, int64_t slot, int64_t numSlots,
                               const StructuralConstraint* structural) override {
    return new Calc(*this, parent, slot, numSlots, structural);
  }
};

}  // namespace luxir
