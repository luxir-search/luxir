#pragma once

#include "SearchOp.h"
#include "luxir/search/DomainVariantPlan.h"

namespace luxir {

class RootOp : public SearchOp {
public:
  DomainVariantPlan domainVariants;

  RootOp(SearchRequest& req) : SearchOp(req, "root") {
  }


  class Calc final : public SearchOp::Calculator {
    std::vector<std::unique_ptr<Calculator>> subCalcs;
  public:
    Calc(RootOp& op, SearchOp::Calculator* parent, int64_t slot, int64_t numSlots) : SearchOp::Calculator(op, parent, slot, numSlots) {}

    luxir::api::Val* getTargetForSub(SearchResponse* resp, Calculator* sub) override {
      // Top-level ops map: slot array sized to the root op's subOps; one Val per sub.
      return build::opsSlot(resp->proto.ops, op.subOps.size(), sub->getOp().name, resp->mr);
    }

    void calc(
        oneapi::tbb::task_group* tg, int32_t segnum,
        DomainHandle domain) override {
      unused(tg);
      unused(segnum);
      unused(domain);
      assert(false);
      std::unreachable();
    }

    void calcAll(
        oneapi::tbb::task_group* tg,
        std::span<const DomainHandle> domains) override {
      unused(tg);
      unused(domains);
      assert(false);
      std::unreachable();
    }

    void start(oneapi::tbb::task_group* tg) {
      // Root-domain messages are not parsed yet, so this stage's root plan is
      // structurally inert. The same parent-owned seam is ready for that later
      // syntax without routing through child calculators.
      assert(static_cast<RootOp&>(op).domainVariants.empty());
      assert(subCalcs.empty());
      subCalcs.reserve(op.subOps.size());
      for (auto [key, subOp] : op.subOps) {
        auto* subCalc = subOp->createCalculator(this, -1);
        subCalcs.emplace_back(subCalc);
      }

      const auto& segs = op.req.reader->segments();
      std::vector<DomainHandle> domains;
      domains.reserve(segs.size());
      for (auto& seg : segs) {
        // Start with live docs. This establishes the PrepareContext domain
        // contract (Query.h): a segment's domain, when present, is
        // live-filtered and is the complete eligibility predicate; null means
        // no deletes and no filters.
        auto* liveDocs = seg.liveDocs() ? &seg.liveDocs()->docset() : nullptr;
        domains.push_back(DomainHandle::pinned(liveDocs, op.req.reader));
      }

      for (auto& subCalc : subCalcs) {
        subCalc->calcAll(tg, domains);
      }
    }
  };

  Calc* createCalculator(
      Calculator* parent, int64_t slot = -1,
      int64_t numSlots = -1) override {
    assert(parent == nullptr);
    return new Calc(*this, nullptr, slot, numSlots);
  }

};

}
