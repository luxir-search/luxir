// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/search/ops/TopDocsReq.h"

namespace luxir {

namespace {
void retainPreparedWindow(RequestMemTracker& tracker,
                          std::pmr::vector<std::shared_ptr<DomainVariantPlan::Reservation>>& charges,
                          const Query::Weight::PreparedWeight* window) {
  if (window == nullptr || window->retainedBytes() == 0) return;
  charges.push_back(std::make_shared<DomainVariantPlan::Reservation>(tracker, window->retainedBytes(),
                                                                     "top_docs prepared reset scope"));
}
}  // namespace

template <bool Structural, bool MetricGroups, typename Admission>
void TopDocsReq::CalcBase::prepareAndDispatch(oneapi::tbb::task_group* tg) {
  {
    auto release = scope_guard([&]() noexcept {
      for (auto& segment : thisOp().req.reader->segments()) releaseSegmentState(segment);
    });
    prepareDomains<Structural>(tg);
  }
  dispatch<Structural, MetricGroups, Admission>(tg, thisOp().domainVariants.empty()
      ? std::span<const DomainHandle>(effectiveDomains) : std::span<const DomainHandle>(inputDomains));
}

template <bool Structural>
void TopDocsReq::CalcBase::prepareDomains(oneapi::tbb::task_group* tg) {
  auto& op = thisOp();
  auto baseDomains = std::span<DocSet* const>(inputDomainViews.data(), inputDomainViews.size());
  Query::Weight::PrepareContext baseCtx{*op.req.reader, baseDomains, tg != nullptr, &op.req};
  bool routed = !op.domainVariants.empty();
  bool mainNeedsPrepare = op.weight != nullptr && (op.wholeMembershipPlan.preparesMainWeight(*op.weight) ||
                                                   op.weight->needsPrepare());
  if (routed) {
    size_t segmentCount = op.req.reader->segments().size();
    preparedRoutedReservations.resize(segmentCount);
    if (mainNeedsPrepare) {
      preparedRoutedFilters.resize(segmentCount);
    }
    for (size_t i = 0; i < segmentCount; i++) {
      auto& seg = op.req.reader->segments()[i];
      std::string detail = "top_docs '" + std::string(op.name) + "'";
      if (mainNeedsPrepare) detail += " variant 0";
      detail += " segment " + std::to_string(i);
      preparedRoutedReservations[i] = reserveProduction<Structural>(seg.maxDoc(), detail);
      op.planning.filterUses->enableRoutedAccounting(i, op.req.memoryTracker, detail);
    }
  }
  preparedFilterSources = QueryPrep::prepareFilterSources(op.filterWeights, op.filterUses, baseCtx);
  if constexpr (Structural)
    for (const auto& source : preparedFilterSources) {
      retainPreparedWindow(op.req.memoryTracker, structuralReset->preparedCharges, source.prepared.get());
    }

  if (routed && (op.domainVariants.sourceCount() > 0 || op.domainVariants.needsResetParentFilters())) {
    std::vector<DocSet*> liveDomains;
    liveDomains.reserve(op.req.reader->segments().size());
    for (auto& segment : op.req.reader->segments()) {
      liveDomains.push_back(resetBase<Structural>(segment.ord));
    }
    Query::Weight::PrepareContext liveCtx{*op.req.reader,
                                          std::span<DocSet* const>(liveDomains.data(), liveDomains.size()),
                                          tg != nullptr, &op.req};
    if (op.domainVariants.needsResetParentFilters()) {
      preparedResetFilterSources = QueryPrep::prepareFilterSources(op.filterWeights, op.filterUses, liveCtx);
      if constexpr (Structural)
        for (const auto& source : preparedResetFilterSources) {
          retainPreparedWindow(op.req.memoryTracker, structuralReset->preparedCharges, source.prepared.get());
        }
    }
    prepareDomainQueryVariants<Structural>(std::span<DocSet* const>(liveDomains.data(), liveDomains.size()),
                                           tg);
  }

  if (routed) {
    if (mainNeedsPrepare) {
      for (size_t i = 0; i < op.req.reader->segments().size(); i++) {
        preparedRoutedFilters[i] =
            materializeRoutedFilters<Structural>((int32_t)i, baseDomains[i], preparedRoutedReservations[i],
                                                 Structural ? &inputDomains[i] : nullptr);
        effectiveDomains[i] = op.domainVariants.defaultDomain<Structural>(
            inputDomains[i], preparedRoutedFilters[i], Structural ? &op.req.memoryTracker : nullptr);
        assert(effectiveDomains[i].isDeliverable());
        effectiveDomainViews[i] = effectiveDomains[i].get();
      }
      Query::Weight::PrepareContext queryCtx{
          *op.req.reader, std::span<DocSet* const>(effectiveDomainViews.data(), effectiveDomainViews.size()),
          tg != nullptr, &op.req};
      if (op.wholeMembershipPlan.preparesMainWeight(*op.weight)) {
        preparedWeight = op.wholeMembershipPlan.prepareMainWeight(queryCtx);
      } else {
        preparedWeight = op.weight->prepare(queryCtx);
      }
      if constexpr (Structural) {
        retainPreparedWindow(op.req.memoryTracker, structuralReset->preparedCharges, preparedWeight.get());
      }
      prepareMainVariants<Structural>(tg);
      effectiveDomainViews.clear();
      effectiveDomains.clear();
    }
    return;
  }

  for (size_t i = 0; i < op.req.reader->segments().size(); i++) {
    if (op.filterWeights.empty()) {
      effectiveDomains[i] = inputDomains[i];
    } else {
      effectiveDomains[i] = buildEffectiveDomain<Structural>((int32_t)i, baseDomains[i]);
    }
    assert(effectiveDomains[i].isDeliverable());
    effectiveDomainViews[i] = effectiveDomains[i].get();
  }

  Query::Weight::PrepareContext queryCtx{
      *op.req.reader, std::span<DocSet* const>(effectiveDomainViews.data(), effectiveDomainViews.size()),
      tg != nullptr, &op.req};
  if (op.weight != nullptr && op.wholeMembershipPlan.preparesMainWeight(*op.weight)) {
    preparedWeight = op.wholeMembershipPlan.prepareMainWeight(queryCtx);
  } else if (op.weight != nullptr && op.weight->needsPrepare()) {
    preparedWeight = op.weight->prepare(queryCtx);
  }
  if constexpr (Structural) {
    retainPreparedWindow(op.req.memoryTracker, structuralReset->preparedCharges, preparedWeight.get());
  }
}

template <bool Structural>
void TopDocsReq::CalcBase::prepareMainVariants(oneapi::tbb::task_group* tg) {
  auto& op = thisOp();
  if (preparedWeight == nullptr || !op.domainVariants.needsInheritProduction() ||
      preparedWeight->domainDependence() == PreparedDomainDependence::QUERY_CANONICAL) {
    return;
  }

  auto segments = op.req.reader->segments();
  size_t baseCount = op.domainVariants.inheritBaseCount();
  std::vector<std::vector<DomainHandle>> eligibility(segments.size());
  for (size_t segnum = 0; segnum < segments.size(); segnum++) {
    eligibility[segnum] = op.domainVariants.produceSharedBases<Structural>(
        inputDomains[segnum], preparedRoutedFilters[segnum], Structural ? &op.req.memoryTracker : nullptr);
    assert(eligibility[segnum].size() == baseCount);
  }

  std::vector<size_t> representatives;
  std::vector<size_t> groupForBase(baseCount);
  for (size_t base = 0; base < baseCount; base++) {
    auto found = std::ranges::find_if(representatives, [&](size_t representative) {
      return DomainVariantPlan::sameWholeReaderEffectiveDomain(eligibility, base, representative, segments);
    });
    if (found == representatives.end()) {
      groupForBase[base] = representatives.size();
      representatives.push_back(base);
    } else {
      groupForBase[base] = (size_t)(found - representatives.begin());
    }
  }
  assert(!representatives.empty() && representatives[0] == 0);

  preparedVariantInheritBases.assign(segments.size(), std::vector<DomainHandle>(baseCount));
  std::vector<std::vector<DomainHandle>> groupMatches(representatives.size(),
                                                      std::vector<DomainHandle>(segments.size()));
  for (size_t group = 0; group < representatives.size(); group++) {
    size_t representative = representatives[group];
    std::vector<DocSet*> domainViews(segments.size());
    for (size_t segnum = 0; segnum < segments.size(); segnum++) {
      domainViews[segnum] = eligibility[segnum][representative].get();
    }
    Query::Weight::PreparedWeight* variantWeight;
    std::unique_ptr<Query::Weight::PreparedWeight> ownedWeight;
    std::vector<std::shared_ptr<DomainVariantPlan::Reservation>> variantReservations;
    if (representative == 0) {
      variantWeight = preparedWeight.get();
    } else {
      size_t variant = op.domainVariants.representativeInheritVariant(representative);
      variantReservations.resize(segments.size());
      for (size_t segnum = 0; segnum < segments.size(); segnum++) {
        variantReservations[segnum] = reserveVariantPreparation<Structural>(
            segments[segnum].maxDoc(), "top_docs '" + std::string(op.name) + "' variant " +
                                           std::to_string(variant) + " segment " + std::to_string(segnum));
      }
      Query::Weight::PrepareContext context{*op.req.reader,
                                            std::span<DocSet* const>(domainViews.data(), domainViews.size()),
                                            tg != nullptr, &op.req};
      ownedWeight = op.weight->prepare(context);
      variantWeight = ownedWeight.get();
    }
    [[maybe_unused]] auto windowCharge = [&] {
      if constexpr (Structural) {
        return ownedWeight ? std::make_shared<DomainVariantPlan::Reservation>(
                                 op.req.memoryTracker, ownedWeight->retainedBytes(),
                                 "top_docs temporary variant window")
                           : nullptr;
      } else
        return nullptr;
    }();
    for (size_t segnum = 0; segnum < segments.size(); segnum++) {
      auto reservation =
          representative == 0 ? preparedRoutedReservations[segnum] : variantReservations[segnum];
      DomainHandle matches(QueryPrep::materialize(*op.weight, variantWeight, segments[segnum],
                                                  eligibility[segnum][representative].get()));
      if (matches.get() != nullptr) {
        reservation->grow(matches.get()->ramBytesUsed());
      }
      if constexpr (Structural)
        groupMatches[group][segnum] = std::move(matches).chargedWith(reservation);
      else
        groupMatches[group][segnum] = std::move(matches).retainedWith(reservation);
    }
  }
  for (size_t base = 0; base < baseCount; base++) {
    for (size_t segnum = 0; segnum < segments.size(); segnum++) {
      preparedVariantInheritBases[segnum][base] = groupMatches[groupForBase[base]][segnum];
    }
  }
}

template <bool Structural>
void TopDocsReq::CalcBase::prepareDomainQueryVariants(std::span<DocSet* const> liveDomains,
                                                      oneapi::tbb::task_group* tg) {
  auto& op = thisOp();
  size_t sourceCount = op.domainVariants.sourceCount();
  size_t segmentCount = op.req.reader->segments().size();
  preparedDomainSources.resize(sourceCount);
  std::vector<size_t> extraSources;
  std::vector<size_t> resetExtraSources;
  std::vector<Query::Weight*> extraWeights;
  std::vector<FilterCache::Use*> extraUses;
  for (size_t source = 0; source < sourceCount; source++) {
    preparedDomainSources[source].weight = op.domainVariants.sourceWeight(source);
    preparedDomainSources[source].cacheUse = op.domainVariants.sourceUse(source);
    if (op.domainVariants.sourceIsExtra(source)) {
      extraSources.push_back(source);
      extraWeights.push_back(op.domainVariants.sourceWeight(source));
      extraUses.push_back(op.domainVariants.sourceUse(source));
    }
    if (op.domainVariants.sourceIsResetExtra(source)) {
      resetExtraSources.push_back(source);
    }
  }
  Query::Weight::PrepareContext liveContext{*op.req.reader, liveDomains, tg != nullptr, &op.req};
  auto preparedExtras = QueryPrep::prepareFilterSources(extraWeights, extraUses, liveContext);
  if constexpr (Structural)
    for (const auto& source : preparedExtras) {
      retainPreparedWindow(op.req.memoryTracker, structuralReset->preparedCharges, source.prepared.get());
    }
  for (size_t i = 0; i < extraSources.size(); i++) {
    preparedDomainSources[extraSources[i]] = std::move(preparedExtras[i]);
  }

  preparedDomainExtraSets.assign(segmentCount, std::vector<DomainHandle>(sourceCount));
  if (op.domainVariants.needsResetParentFilters()) {
    preparedResetRoutedFilters.resize(segmentCount);
  }
  for (size_t segnum = 0; segnum < segmentCount; segnum++) {
    auto reservation = preparedRoutedReservations[segnum];
    if (op.domainVariants.needsResetParentFilters()) {
      preparedResetRoutedFilters[segnum] =
          materializeResetParentFilters<Structural>((int32_t)segnum, reservation);
    }
    auto& segment = op.req.reader->segments()[segnum];
    for (size_t source : resetExtraSources) {
      DomainHandle set;
      if constexpr (Structural) {
        auto& sources = structural->producer->segmentSources(op.domainVariants, *op.req.reader, op.planning,
                                                             (int32_t)segnum, op.req.memoryTracker);
        set = sources.resolve(source, resetBaseHandle((int32_t)segnum), &preparedDomainSources[source]);
      } else {
        set = QueryPrep::materializeEffectiveFilter(preparedDomainSources[source], *op.req.reader, segment,
                                                    liveDomains[segnum]);
        if (set.get() != nullptr && set.isDeliverable()) reservation->grow(set.get()->ramBytesUsed());
      }
      preparedDomainExtraSets[segnum][source] = std::move(set).pinnedWith(op.planning.filterUses);
    }
  }

  size_t variantCount = op.domainVariants.variantCount();
  std::vector<std::vector<DomainHandle>> eligibility(segmentCount, std::vector<DomainHandle>(variantCount));
  for (size_t segnum = 0; segnum < segmentCount; segnum++) {
    DomainHandle live = Structural ? resetBaseHandle((int32_t)segnum)
                                   : DomainHandle::pinned(liveDomains[segnum], op.req.reader);
    for (size_t variant = 1; variant < variantCount; variant++) {
      if (op.domainVariants.frame(variant) != DomainVariantPlan::Frame::RESET) {
        continue;
      }
      std::vector<DomainHandle> parts;
      parts.push_back(live);
      if (!preparedResetRoutedFilters.empty()) {
        for (size_t filter = 0; filter < preparedResetRoutedFilters[segnum].size(); filter++) {
          if (op.domainVariants.includesFilter(variant, filter)) {
            parts.push_back(preparedResetRoutedFilters[segnum][filter]);
          }
        }
      }
      for (size_t source : op.domainVariants.extraSources(variant)) {
        parts.push_back(preparedDomainExtraSets[segnum][source]);
      }
      eligibility[segnum][variant] =
          Structural ? DomainVariantPlan::composeTracked(std::move(parts), op.req.memoryTracker)
                     : DomainVariantPlan::compose(std::move(parts));
    }
  }

  if constexpr (!Structural) {
    preparedDomainVariants = op.domainVariants.prepareResetVariants(
        preparedDomainSources, eligibility, liveDomains, *op.req.reader, tg != nullptr, &op.req,
        [&](size_t variant, size_t segnum) {
          auto& segment = op.req.reader->segments()[segnum];
          return reserveVariantPreparation<Structural>(
              segment.maxDoc(), "top_docs '" + std::string(op.name) + "' variant " + std::to_string(variant) +
                                    " segment " + std::to_string(segnum));
        });
  } else {
    preparedDomainVariants = op.domainVariants.prepareResetVariants(
        preparedDomainSources, eligibility, liveDomains, *op.req.reader, tg != nullptr, &op.req,
        [&](size_t variant, size_t segnum) {
          auto& segment = op.req.reader->segments()[segnum];
          return reserveVariantPreparation<Structural>(
              segment.maxDoc(), "top_docs '" + std::string(op.name) + "' variant " + std::to_string(variant) +
                                    " segment " + std::to_string(segnum));
        },
        DomainVariantPlan::PreparedAccounting{
            op.req.memoryTracker,
            [&](auto charge) { structuralReset->preparedCharges.push_back(std::move(charge)); }},
        structuralFinish());
  }
}

template void TopDocsReq::CalcBase::prepareAndDispatch<false, false>(oneapi::tbb::task_group*);
template void TopDocsReq::CalcBase::prepareAndDispatch<true, false>(oneapi::tbb::task_group*);
template void TopDocsReq::CalcBase::prepareAndDispatch<false, true>(oneapi::tbb::task_group*);
template void TopDocsReq::CalcBase::prepareAndDispatch<true, true>(oneapi::tbb::task_group*);
template void TopDocsReq::CalcBase::prepareAndDispatch<false, false, DiversifyAdmission>(
    oneapi::tbb::task_group*);
template void TopDocsReq::CalcBase::prepareAndDispatch<true, false, DiversifyAdmission>(
    oneapi::tbb::task_group*);
template void TopDocsReq::CalcBase::prepareDomains<false>(oneapi::tbb::task_group*);
template void TopDocsReq::CalcBase::prepareDomains<true>(oneapi::tbb::task_group*);
template void TopDocsReq::CalcBase::prepareMainVariants<false>(oneapi::tbb::task_group*);
template void TopDocsReq::CalcBase::prepareMainVariants<true>(oneapi::tbb::task_group*);
template void TopDocsReq::CalcBase::prepareDomainQueryVariants<false>(std::span<DocSet* const>,
                                                                      oneapi::tbb::task_group*);
template void TopDocsReq::CalcBase::prepareDomainQueryVariants<true>(std::span<DocSet* const>,
                                                                     oneapi::tbb::task_group*);

}  // namespace luxir
