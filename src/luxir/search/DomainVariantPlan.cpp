// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "DomainVariantPlan.h"

namespace luxir {

bool DomainVariantPlan::segmentLocal(bool selectionInPath) const {
  if (selectionInPath) return false;
  for (const auto& source : querySources) {
    if (!source.query->recoverableAsPredicate() || source.weight->needsPrepare()) return false;
  }
  return true;
}

DomainHandle DomainVariantPlan::account(DomainHandle domain, RequestMemTracker& tracker,
                                        std::string_view detail) {
  if (!domain.get() || domain.isCharged()) return domain;
  auto charge = std::make_shared<DomainCharge>(tracker, domain.get()->ramBytesUsed(), detail);
  return freeze(std::move(domain).chargedWith(std::move(charge)));
}

DomainHandle DomainVariantPlan::composeTracked(std::vector<DomainHandle> parts, RequestMemTracker& tracker) {
  std::vector<DomainHandle> distinct;
  for (auto& part : parts) {
    if (!part.get()) continue;
    if (std::ranges::any_of(distinct, [&](const auto& old) { return old.get() == part.get(); })) continue;
    distinct.push_back(std::move(part));
  }
  if (distinct.size() <= 1) return freeze(distinct.empty() ? DomainHandle{} : distinct.front());
  return account(compose(std::move(distinct)), tracker, "composed segment domain");
}

std::vector<DomainHandle> DomainVariantPlan::produceSegment(SegmentSources& sources, DomainHandle inherited,
                                                            DomainHandle resetBase,
                                                            RequestMemTracker& tracker,
                                                            std::span<const DomainHandle> variantBases,
                                                            std::optional<Frame> onlyFrame) const {
  assert(segmentLocal());
  std::vector<std::optional<DomainHandle>> resolved(sourceCount() * 2);
  auto resolve = [&](size_t source, DomainHandle base, Frame kind) {
    auto& value = resolved[source * 2 + (kind == Frame::RESET)];
    if (!value) value = sources.resolve(source, std::move(base));
    return *value;
  };
  // A parent-filtered reset can have different bases per variant.
  if (!variantBases.empty()) {
    auto scoped = [&](size_t source, DomainHandle base, Frame) {
      return sources.resolve(source, std::move(base));
    };
    return produceSegment(std::move(inherited), std::move(resetBase), scoped, tracker, {}, {}, variantBases,
                          onlyFrame);
  }
  return produceSegment(std::move(inherited), std::move(resetBase), resolve, tracker, {}, {}, {}, onlyFrame);
}

DomainVariantPlan::SegmentSources::SegmentSources(const DomainVariantPlan& plan, IndexReader& reader,
                                                  Query::PlanningContext& planning, int32_t segment,
                                                  RequestMemTracker& tracker)
    : reader(reader),
      planning(planning),
      segment(segment),
      tracker(tracker),
      resource(tracker, "segment domain sources", "domain variants"),
      values(plan.sourceCount(), &resource) {
  for (size_t source = 0; source < values.size(); source++) {
    init(source, plan.sourceWeight(source),
         plan.querySources[source].query->recoverableAsPredicate() ? plan.sourceUse(source) : nullptr);
  }
}

DomainVariantPlan::SegmentSources::SegmentSources(std::span<Query::Weight* const> weights,
                                                  std::span<FilterCache::Use* const> uses,
                                                  IndexReader& reader, Query::PlanningContext& planning,
                                                  int32_t segment, RequestMemTracker& tracker)
    : reader(reader),
      planning(planning),
      segment(segment),
      tracker(tracker),
      resource(tracker, "segment filter sources", "domain variants"),
      values(weights.size(), &resource) {
  assert(weights.size() == uses.size());
  for (size_t source = 0; source < values.size(); source++) init(source, weights[source], uses[source]);
}

void DomainVariantPlan::SegmentSources::init(size_t source, Query::Weight* weight, FilterCache::Use* use) {
  auto& value = values[source];
  value.weight = weight;
  value.use = use;
  if (weight->matchesAllDocs() || weight->needsPrepare() || !use) return;
  use->enableRoutedAccounting((size_t)segment, tracker, "segment domain source");
  auto probe = use->probe((size_t)segment);
  if (probe.kind() == FilterCache::Probe::Kind::BYPASS) {
    value.bypassed = true;
    return;
  }
  auto& seg = reader.segments()[(size_t)segment];
  value.stable = materialize(source, seg.liveDocs() ? &seg.liveDocs()->docset() : nullptr, std::move(probe));
}

DomainHandle DomainVariantPlan::SegmentSources::resolve(size_t source, DomainHandle base,
                                                        const QueryPrep::PreparedSource* prepared) {
  auto& value = values[source];
  if (value.weight->matchesAllDocs()) return freeze(std::move(base));
  DomainHandle result;
  if ((prepared && prepared->prepared) || value.weight->needsPrepare() || value.bypassed) {
    // Admission is per source/segment, never per bucket. A bypass stays
    // scoped, while prepared selections remain request-local.
    result = account(
        DomainHandle(QueryPrep::materialize(*value.weight, prepared ? prepared->prepared.get() : nullptr,
                                            reader.segments()[(size_t)segment], base.get())),
        tracker, "scoped segment source");
  } else if (!value.stable) {
    result = materialize(source, base.get());
  } else {
    auto& domain = *value.stable;
    auto& seg = reader.segments()[(size_t)segment];
    DocSet* live = seg.liveDocs() ? &seg.liveDocs()->docset() : nullptr;
    if (!domain.get())
      result = base;
    else if (!base.get() || base.get() == live || base.get() == domain.get())
      result = domain;
    else {
      std::array<DocSet*, 2> parts{base.get(), domain.get()};
      result = account(DomainHandle(DocSet::intersect(parts)), tracker, "scoped segment source");
    }
  }
  assert(result.isDeliverable());
  assert(!base.get() || result.get());
  return freeze(std::move(result));
}

std::vector<std::vector<DomainHandle>> DomainVariantPlan::produceBucketDomains(
    IndexReader& reader, Query::PlanningContext& planning, std::span<const DomainHandle> inherited,
    std::span<const DomainHandle> resetBase, bool parallel, ExecutionWarnings* warnings,
    RequestMemTracker& tracker, Sources sources, const Finish& finish) const {
  BucketProduction production{tracker, sources};
  return producePreparedDomains(reader, planning, inherited, resetBase, parallel, warnings, production,
                                finish);
}

std::vector<std::vector<DomainHandle>> DomainVariantPlan::producePreparedDomains(
    IndexReader& reader, Query::PlanningContext& planning, std::span<const DomainHandle> inherited,
    std::span<const DomainHandle> resetBase, bool parallel, ExecutionWarnings* warnings,
    BucketProduction& production, const Finish& finish) const {
  assert(inherited.size() == resetBase.size());
  std::vector<Query::Weight*> weights(this->sourceCount());
  std::vector<FilterCache::Use*> uses(this->sourceCount());
  for (size_t source = 0; source < weights.size(); source++) {
    weights[source] = this->sourceWeight(source);
    uses[source] = this->sourceUse(source);
  }
  std::vector<DocSet*> views;
  auto preparationBase = hasResetVariants() ? resetBase : inherited;
  for (const auto& domain : preparationBase) views.push_back(domain.get());
  Query::Weight::PrepareContext prepareContext{reader, std::span<DocSet* const>(views.data(), views.size()),
                                               parallel, warnings};
  std::vector<QueryPrep::PreparedSource> prepared(weights.size());
  std::vector<QueryPrep::PreparedSource> inheritedExtras;
  auto release = scope_guard([&]() noexcept {
    for (auto& segment : reader.segments()) {
      for (const auto& source : prepared) production.release(source, segment);
      for (const auto& source : inheritedExtras) production.release(source, segment);
    }
  });
  std::vector<size_t> extraSources;
  std::vector<size_t> resetExtraSources;
  std::vector<Query::Weight*> extraWeights;
  std::vector<FilterCache::Use*> extraUses;
  for (size_t source = 0; source < weights.size(); source++) {
    prepared[source].weight = weights[source];
    prepared[source].cacheUse = uses[source];
    if (this->sourceIsExtra(source)) {
      extraSources.push_back(source);
      extraWeights.push_back(weights[source]);
      extraUses.push_back(uses[source]);
    }
    if (this->sourceIsResetExtra(source)) {
      resetExtraSources.push_back(source);
    }
  }
  auto preparedExtras = QueryPrep::prepareFilterSources(extraWeights, extraUses, prepareContext);
  for (size_t i = 0; i < extraSources.size(); i++) {
    prepared[extraSources[i]] = std::move(preparedExtras[i]);
  }

  // An extra source may occur in both frames. Input-dependent selections
  // must prepare against each frame's input, never reuse the wider reset
  // window for an inheriting sibling. Predicate sources need no extra work.
  if (hasResetVariants()) {
    bool different = false;
    for (size_t segment = 0; segment < inherited.size(); segment++) {
      different |= !sameEffectiveDomain(inherited[segment].get(), resetBase[segment].get(),
                                        reader.segments()[segment].maxDoc());
    }
    std::vector<size_t> sources;
    std::vector<Query::Weight*> localWeights;
    std::vector<FilterCache::Use*> localUses;
    if (different)
      for (size_t variant = 1; variant < variantCount(); variant++) {
        if (frame(variant) != Frame::INHERIT) continue;
        for (size_t source : this->extraSources(variant)) {
          if (prepared[source].domainDependence == PreparedDomainDependence::QUERY_CANONICAL ||
              std::ranges::find(sources, source) != sources.end())
            continue;
          sources.push_back(source);
          localWeights.push_back(weights[source]);
          localUses.push_back(uses[source]);
        }
      }
    if (!sources.empty()) {
      std::vector<DocSet*> localViews;
      for (const auto& domain : inherited) localViews.push_back(domain.get());
      Query::Weight::PrepareContext context{
          reader, std::span<DocSet* const>(localViews.data(), localViews.size()), parallel, warnings};
      auto local = QueryPrep::prepareFilterSources(localWeights, localUses, context);
      inheritedExtras.resize(weights.size());
      for (size_t i = 0; i < sources.size(); i++) inheritedExtras[sources[i]] = std::move(local[i]);
    }
  }

  std::vector<std::shared_ptr<Reservation>> preparedCharges;
  for (const auto* sources : {&prepared, &inheritedExtras}) {
    for (const auto& source : *sources)
      if (source.prepared) {
        if (auto charge = production.chargePrepared(*source.prepared)) {
          preparedCharges.push_back(std::move(charge));
        }
      }
  }

  std::vector<std::shared_ptr<DomainVariantPlan::Reservation>> reservations(inherited.size());
  std::vector<std::vector<DomainHandle>> extraSets(inherited.size(),
                                                   std::vector<DomainHandle>(weights.size()));
  for (size_t segnum = 0; segnum < inherited.size(); segnum++) {
    std::string detail = "reset segment " + std::to_string(segnum);
    reservations[segnum] = production.reserve(segnum, detail);
    planning.filterUses->enableRoutedAccounting(segnum, production.tracker, detail);
    for (size_t source : resetExtraSources) {
      if (sourceWeight(source)->matchesAllDocs()) continue;
      DomainHandle set =
          production.resolve(source, prepared[source], segnum, resetBase[segnum], *reservations[segnum]);
      extraSets[segnum][source] = std::move(set).pinnedWith(planning.filterUses);
    }
  }

  size_t variantCount = this->variantCount();
  std::vector<std::vector<DomainHandle>> eligibility(inherited.size(),
                                                     std::vector<DomainHandle>(variantCount));
  for (size_t segnum = 0; segnum < inherited.size(); segnum++) {
    for (size_t variant = 1; variant < variantCount; variant++) {
      if (this->frame(variant) != DomainVariantPlan::Frame::RESET) {
        continue;
      }
      std::vector<DomainHandle> parts;
      for (size_t source : this->extraSources(variant)) {
        if (!sourceWeight(source)->matchesAllDocs()) parts.push_back(extraSets[segnum][source]);
      }
      // Every effective extra is already restricted to resetBase.
      if (parts.empty()) parts.push_back(resetBase[segnum]);
      eligibility[segnum][variant] = production.composeResult(std::move(parts));
    }
  }

  auto preparedVariants = this->prepareResetVariants(
      prepared, eligibility, std::span<DocSet* const>(views.data(), views.size()), reader, parallel, warnings,
      [&](size_t variant, size_t segnum) {
        std::string detail =
            "reset variant " + std::to_string(variant) + " segment " + std::to_string(segnum);
        return production.reserveVariant(segnum, std::move(detail));
      },
      PreparedAccounting{production.tracker,
          [&](auto charge) { preparedCharges.push_back(std::move(charge)); }}, finish);

  std::vector<std::vector<DomainHandle>> produced;
  produced.reserve(inherited.size());
  for (size_t segnum = 0; segnum < inherited.size(); segnum++) {
    auto& segment = reader.segments()[segnum];
    auto releaseSegment = scope_guard([&]() noexcept {
      for (const auto& source : prepared) production.release(source, segment);
    });
    auto reservation = reservations[segnum];

    auto materialize = [&](size_t source, DomainHandle base, Frame frame) {
      if (this->sourceIsResetExtra(source) && base.get() == resetBase[segnum].get() &&
          (frame == Frame::RESET || inheritedExtras.empty() || !inheritedExtras[source].weight)) {
        return extraSets[segnum][source];
      }
      const auto& inputSource =
          frame == Frame::INHERIT && !inheritedExtras.empty() && inheritedExtras[source].weight
              ? inheritedExtras[source]
              : prepared[source];
      return production.resolve(source, inputSource, segnum, std::move(base), *reservation);
    };

    auto variants = composeVariants(
        inherited[segnum], resetBase[segnum], materialize,
        [&](auto parts) { return production.composeResult(std::move(parts)); },
        preparedVariants.domains[segnum], preparedVariants.ready);
    for (auto& domain : variants) {
      domain = domain.isCharged() ? std::move(domain).chargedWith(reservation)
                                  : std::move(domain).retainedWith(reservation);
    }
    produced.push_back(std::move(variants));
  }

  return produced;
}

DomainHandle DomainVariantPlan::SegmentSources::materialize(size_t source, DocSet* base) {
  auto& value = values[source];
  QueryPrep::PreparedSource input;
  input.weight = value.weight;
  input.cacheUse = value.use;
  auto domain =
      QueryPrep::materializeEffectiveFilter(input, reader, reader.segments()[(size_t)segment], base);
  bool borrowed =
      value.use && domain.get() && domain.get() == value.use->effectiveDocSet((size_t)segment, reader);
  domain = std::move(domain).pinnedWith(planning.filterUses);
  return borrowed ? std::move(domain) : account(std::move(domain), tracker, "segment source domain");
}

DomainHandle DomainVariantPlan::SegmentSources::materialize(size_t source, DocSet* base,
                                                            FilterCache::Probe acquired) {
  auto& value = values[source];
  QueryPrep::PreparedSource input;
  input.weight = value.weight;
  input.cacheUse = value.use;
  auto domain = QueryPrep::materializeEffectiveFilter(input, reader, reader.segments()[(size_t)segment], base,
                                                      std::move(acquired));
  bool borrowed =
      value.use && domain.get() && domain.get() == value.use->effectiveDocSet((size_t)segment, reader);
  domain = std::move(domain).pinnedWith(planning.filterUses);
  return borrowed ? std::move(domain) : account(std::move(domain), tracker, "segment source domain");
}

bool DomainVariantPlan::addChild(std::string_view key, std::function_ref<bool(size_t)> excluded,
                                 const QuerySpec* resetQuery, bool applyParentFilters,
                                 std::span<const QuerySpec> extraFilters) {
  Frame frame = resetQuery == nullptr ? Frame::INHERIT : Frame::RESET;
  std::vector<uint64_t> included((filterCount + 63) >> 6, 0);
  if (frame == Frame::INHERIT || applyParentFilters) {
    for (size_t i = 0; i < filterCount; i++) {
      if (!excluded(i)) included[i >> 6] |= 1ULL << (i & 63);
    }
  }

  Variant candidate;
  candidate.frame = frame;
  candidate.included = std::move(included);
  if (frame == Frame::INHERIT) {
    candidate.inheritBase = internInheritBase(candidate.included);
  } else {
    candidate.resetSource = internSource(*resetQuery);
  }
  candidate.extraSources.reserve(extraFilters.size());
  for (const auto& filter : extraFilters) {
    candidate.extraSources.push_back(internSource(filter));
  }

  if (candidate.frame == Frame::INHERIT && candidate.inheritBase == 0 && candidate.extraSources.empty()) {
    childRoutes.push_back({key, 0});
    return false;
  }

  auto found = std::find_if(variants.begin(), variants.end(), [&](const Variant& variant) {
    return variant.frame == candidate.frame && variant.included == candidate.included &&
           variant.resetSource == candidate.resetSource && variant.extraSources == candidate.extraSources;
  });
  size_t variant;
  bool created = found == variants.end();
  if (created) {
    if (variants.size() >= MAX_NON_DEFAULT_VARIANTS) {
      bool domainOverride = resetQuery != nullptr || !extraFilters.empty();
      throw std::runtime_error("op '" + std::string(key) +
                               (domainOverride ? "': domain variant cap 64 exceeded"
                                               : "': routed filter variant cap 64 exceeded"));
    }
    variants.push_back(std::move(candidate));
    if (resetQuery != nullptr || !extraFilters.empty()) {
      domainOverrideCount++;
    }
    variant = variants.size();
  } else {
    variant = (size_t)(found - variants.begin()) + 1;
  }
  childRoutes.push_back({key, (uint8_t)variant});
  return created;
}

void DomainVariantPlan::buildWeights(Query::Context& context, Query::PlanningContext& planning,
                                     int32_t requestFlags) {
  int32_t filterFlags = requestFlags & ~(Query::NEED_SCORES | Query::ALLOW_PRUNING);
  for (auto& source : querySources) {
    source.weight = source.query->createWeight(context, filterFlags);
    source.use = planning.getFilterUse(*source.query);
  }
}

}  // namespace luxir
