// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "SearchOp.h"
#include "luxir/query/QueryPrep.h"
#include "luxir/search/DomainVariantPlan.h"

namespace luxir {

class RootOp : public SearchOp {
public:
  Query::PlanningContext* planning = nullptr;
  Query::Context* weightContext = nullptr;
  DomainVariantPlan domainVariants;

  RootOp(SearchRequest& req) : SearchOp(req, "root") {
  }


  class Calc final : public SearchOp::Calculator {
    std::vector<std::unique_ptr<Calculator>> subCalcs;
    std::vector<size_t> subCalcVariants;

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
      auto& root = static_cast<RootOp&>(op);
      assert(subCalcs.empty());
      subCalcs.reserve(op.subOps.size());
      if (!root.domainVariants.empty()) {
        subCalcVariants.reserve(op.subOps.size());
      }
      for (auto [key, subOp] : op.subOps) {
        auto* subCalc = subOp->createCalculator(this, -1);
        subCalcs.emplace_back(subCalc);
        if (!root.domainVariants.empty()) {
          subCalcVariants.push_back(
              root.domainVariants.variantForChild(key));
        }
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

      if (root.domainVariants.empty()) {
        for (auto& subCalc : subCalcs) {
          subCalc->calcAll(tg, domains);
        }
        return;
      }

      assert(root.planning != nullptr);
      std::vector<Query::Weight*> weights(root.domainVariants.sourceCount());
      std::vector<FilterCache::Use*> uses(root.domainVariants.sourceCount());
      for (size_t source = 0; source < weights.size(); source++) {
        weights[source] = root.domainVariants.sourceWeight(source);
        uses[source] = root.domainVariants.sourceUse(source);
      }
      std::vector<DocSet*> liveViews;
      liveViews.reserve(domains.size());
      for (const auto& domain : domains) liveViews.push_back(domain.get());
      Query::Weight::PrepareContext prepareContext{
        *op.req.reader,
        std::span<DocSet* const>(liveViews.data(), liveViews.size()),
        tg != nullptr
      };
      std::vector<QueryPrep::PreparedSource> prepared(weights.size());
      std::vector<size_t> extraSources;
      std::vector<size_t> resetExtraSources;
      std::vector<Query::Weight*> extraWeights;
      std::vector<FilterCache::Use*> extraUses;
      for (size_t source = 0; source < weights.size(); source++) {
        prepared[source].weight = weights[source];
        prepared[source].cacheUse = uses[source];
        if (root.domainVariants.sourceIsExtra(source)) {
          extraSources.push_back(source);
          extraWeights.push_back(weights[source]);
          extraUses.push_back(uses[source]);
        }
        if (root.domainVariants.sourceIsResetExtra(source)) {
          resetExtraSources.push_back(source);
        }
      }
      auto preparedExtras = QueryPrep::prepareFilterSources(
          extraWeights, extraUses, prepareContext);
      for (size_t i = 0; i < extraSources.size(); i++) {
        prepared[extraSources[i]] = std::move(preparedExtras[i]);
      }

      std::vector<std::shared_ptr<DomainVariantPlan::Reservation>>
          reservations(domains.size());
      std::vector<std::vector<DomainHandle>> extraSets(
          domains.size(), std::vector<DomainHandle>(weights.size()));
      for (size_t segnum = 0; segnum < domains.size(); segnum++) {
        auto& segment = op.req.reader->segments()[segnum];
        std::string detail = "root segment " + std::to_string(segnum);
        reservations[segnum] = root.domainVariants.reserveProduction(
            op.req.memoryTracker, segment.maxDoc(), detail);
        root.planning->filterUses->enableRoutedAccounting(
            segnum, op.req.memoryTracker, detail);
        for (size_t source : resetExtraSources) {
          DomainHandle set = QueryPrep::materializeEffectiveFilter(
              prepared[source], *op.req.reader, segment,
              domains[segnum].get());
          if (set.get() != nullptr && set.isDeliverable()) {
            reservations[segnum]->grow(set.get()->ramBytesUsed());
          }
          extraSets[segnum][source] =
              std::move(set).pinnedWith(root.planning->filterUses);
        }
      }

      size_t variantCount = root.domainVariants.variantCount();
      std::vector<std::vector<DomainHandle>> eligibility(
          domains.size(), std::vector<DomainHandle>(variantCount));
      for (size_t segnum = 0; segnum < domains.size(); segnum++) {
        for (size_t variant = 1; variant < variantCount; variant++) {
          if (root.domainVariants.frame(variant)
              != DomainVariantPlan::Frame::RESET) {
            continue;
          }
          std::vector<DomainHandle> parts;
          parts.push_back(domains[segnum]);
          for (size_t source : root.domainVariants.extraSources(variant)) {
            parts.push_back(extraSets[segnum][source]);
          }
          eligibility[segnum][variant] = DomainVariantPlan::compose(
              std::move(parts));
        }
      }

      auto preparedVariants = root.domainVariants.prepareResetVariants(
          prepared, eligibility,
          std::span<DocSet* const>(liveViews.data(), liveViews.size()),
          *op.req.reader, tg != nullptr,
          [&](size_t variant, size_t segnum) {
            auto& segment = op.req.reader->segments()[segnum];
            return root.domainVariants.reserveVariantPreparation(
                op.req.memoryTracker, segment.maxDoc(),
                "root variant " + std::to_string(variant)
                    + " segment " + std::to_string(segnum));
          });

      std::vector<std::vector<DomainHandle>> produced;
      produced.reserve(domains.size());
      for (size_t segnum = 0; segnum < domains.size(); segnum++) {
        auto& segment = op.req.reader->segments()[segnum];
        auto reservation = reservations[segnum];

        auto materialize = [&](size_t source) {
          if (root.domainVariants.sourceIsResetExtra(source)) {
            return extraSets[segnum][source];
          }
          DomainHandle set = QueryPrep::materializeEffectiveFilter(
              prepared[source], *op.req.reader, segment,
              domains[segnum].get());
          if (set.get() != nullptr && set.isDeliverable()) {
            reservation->grow(set.get()->ramBytesUsed());
          }
          return std::move(set).pinnedWith(root.planning->filterUses);
        };

        std::vector<DomainHandle> variants(
            root.domainVariants.variantCount());
        variants[0] = domains[segnum];
        for (size_t variant = 1; variant < variants.size(); variant++) {
          if (preparedVariants.ready[variant] != 0) {
            variants[variant] =
                preparedVariants.domains[segnum][variant];
            continue;
          }
          std::vector<DomainHandle> parts;
          if (root.domainVariants.frame(variant)
              == DomainVariantPlan::Frame::INHERIT) {
            parts.push_back(domains[segnum]);
          } else {
            parts.push_back(materialize(
                root.domainVariants.resetSource(variant)));
          }
          for (size_t source : root.domainVariants.extraSources(variant)) {
            parts.push_back(materialize(source));
          }
          variants[variant] = DomainVariantPlan::compose(
              std::move(parts));
        }
        root.domainVariants.retainReservation(variants, reservation);
        produced.push_back(std::move(variants));
      }

      assert(subCalcs.size() == subCalcVariants.size());
      std::vector<DomainHandle> childDomains(domains.size());
      for (size_t child = 0; child < subCalcs.size(); child++) {
        size_t variant = subCalcVariants[child];
        for (size_t segnum = 0; segnum < domains.size(); segnum++) {
          childDomains[segnum] = produced[segnum][variant];
        }
        subCalcs[child]->calcAll(tg, childDomains);
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
