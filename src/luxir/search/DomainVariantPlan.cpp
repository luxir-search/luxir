// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/query/ReadDeclarer.h"
#include "DomainVariantPlan.h"
#include "luxir/search/SearchOverrides.h"

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
  DomainHandle identity;
  if (!parts.empty()) identity = DomainHandle().scopedLike(parts[0]);
  for (auto& part : parts) {
    part.assertSameScope(identity);
    if (!part.get()) continue;
    if (std::ranges::any_of(distinct, [&](const auto& old) { return old.get() == part.get(); })) continue;
    distinct.push_back(std::move(part));
  }
  if (distinct.size() <= 1) return freeze(distinct.empty() ? identity : distinct.front());
  return account(compose(std::move(distinct)), tracker, "composed segment domain");
}

std::vector<DomainHandle> DomainVariantPlan::produceSegment(const SegmentSources& sources,
                                                            DomainHandle inherited, DomainHandle resetBase,
                                                            std::span<const DomainHandle> variantBases,
                                                            std::optional<Frame> onlyFrame) const {
  assert(segmentLocal());
  auto scoped = [&](size_t source, DomainHandle base, Frame) {
    return sources.resolve(*sourceWeight(source), segmentSourceUse(source), std::move(base));
  };
  // A parent-filtered reset can have different bases per variant.
  if (!variantBases.empty()) {
    return produceSegment(std::move(inherited), std::move(resetBase), scoped, sources.tracker,
                          {}, {}, variantBases, onlyFrame);
  }
  std::vector<std::optional<DomainHandle>> resolved(sourceCount() * 2);
  auto resolve = [&](size_t source, DomainHandle base, Frame kind) {
    auto& value = resolved[source * 2 + (kind == Frame::RESET)];
    if (!value) value = scoped(source, std::move(base), kind);
    return *value;
  };
  return produceSegment(std::move(inherited), std::move(resetBase), resolve, sources.tracker, {}, {}, {}, onlyFrame);
}

DocSet* DomainVariantPlan::SegmentSources::membership(
    Query::Weight& weight, FilterCache::Use* use, const QueryPrep::PreparedSource* prepared) const {
  auto route = domainSourceRouteForTests;
  if (route == DomainSourceRoute::PER_DOMAIN || use == nullptr
      || use->scope() == FilterKeyScope::READER_STABLE
      || weight.needsPrepare() || weight.needsScores() || weight.allowsPruning()
      || (prepared && prepared->prepared)) {
    return nullptr;
  }

  assert(&planning.topReader == &reader);
  size_t ord = (size_t)segment.ord;
  use->enableRoutedAccounting(ord, tracker, "segment domain source");
  auto probe = use->probe(ord);
  using Kind = FilterCache::Probe::Kind;
  if (probe.kind() == Kind::HIT) {
    if (domainSourceStatsForTests) domainSourceStatsForTests->borrows++;
    return probe.docSet();
  }
  if (probe.kind() == Kind::BYPASS) {
    if (QueryPrep::disableFilterOwnForTests) return nullptr;
    if (route == DomainSourceRoute::AUTO && probe.rentDomainOnce()) {
      if (domainSourceStatsForTests) domainSourceStatsForTests->rents++;
      return nullptr;
    }
  }

  auto* raw = QueryPrep::populateRawFilter(weight, nullptr, segment, *use, probe);
  assert(raw);
  if (domainSourceStatsForTests) domainSourceStatsForTests->buys++;
  return raw;
}

DomainHandle DomainVariantPlan::SegmentSources::resolve(
    Query::Weight& weight, FilterCache::Use* use, DomainHandle base,
    const QueryPrep::PreparedSource* prepared) const {
  auto* domain = base.get(segment);
  if ((domain && domain->card() == 0) || weight.matchesAllDocs()) return freeze(std::move(base));

  DomainHandle result;
  if (membership(weight, use, prepared)) {
    result = use->effectiveDomain((size_t)segment.ord, reader, domain);
    // Owned intersections are charged here. The Use accounts for its own sets.
    if (result.isDeliverable()) result = account(std::move(result), tracker, "scoped segment source");
    else result = std::move(result).pinnedWith(planning.filterUses);
  } else {
    auto docs = QueryPrep::materialize(weight, prepared ? prepared->prepared.get() : nullptr,
                                      segment, domain ? domain : segment.liveDocSet());
    result = account(DomainHandle(std::move(docs)).scoped(segment), tracker, "scoped segment source");
  }
  assert(result.isDeliverable());
  assert(!domain || result.get());
  return freeze(std::move(result));
}

int64_t DomainVariantPlan::SegmentSources::count(
    Query::Weight& weight, FilterCache::Use* use, const DomainHandle& base,
    const QueryPrep::PreparedSource* prepared) const {
  auto* domain = base.get(segment);
  if (domain && domain->card() == 0) return 0;
  if (weight.matchesAllDocs()) return domain ? domain->card() : segment.numLive();
  if (!domain) domain = segment.liveDocSet();

  if (auto* raw = membership(weight, use, prepared)) {
    // Domains carry liveness, so raw membership counts without a live-composed set.
    assert(!domain || !segment.hasDeletes()
           || DocSet::intersectionCard(domain, segment.liveDocSet()) == domain->card());
    return domain ? DocSet::intersectionCard(raw, domain) : raw->card();
  }

  auto guard = MemPool::threadLocalPoolGuard();
  auto& source = prepared ? prepared->segmentSource() : static_cast<Query::SegmentSource&>(weight);
  auto* supplier = source.scorerSupplier(guard.pool(), segment);
  return QueryPrep::countSegment(guard.pool(), supplier, domain, segment.maxDoc());
}

std::vector<std::vector<DomainHandle>> DomainVariantPlan::produceBucketDomains(
    IndexReader& reader, Query::PlanningContext& planning, std::span<const DomainHandle> inherited,
    std::span<const DomainHandle> resetBase, FanoutScope* fanout, ExecutionWarnings* warnings,
    RequestMemTracker& tracker, const Finish& finish) const {
  BucketProduction production{tracker};
  return producePreparedDomains(reader, planning, inherited, resetBase, fanout, warnings, production,
                                finish);
}

std::vector<std::vector<DomainHandle>> DomainVariantPlan::producePreparedDomains(
    IndexReader& reader, Query::PlanningContext& planning, std::span<const DomainHandle> inherited,
    std::span<const DomainHandle> resetBase, FanoutScope* fanout, ExecutionWarnings* warnings,
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
                                               fanout, warnings};
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
          reader, std::span<DocSet* const>(localViews.data(), localViews.size()), fanout, warnings};
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
    SegmentSources sources{reader, planning, reader.segments()[segnum], production.tracker};
    for (size_t source : resetExtraSources) {
      if (sourceWeight(source)->matchesAllDocs()) continue;
      DomainHandle set =
          sources.resolve(*weights[source], segmentSourceUse(source), resetBase[segnum], &prepared[source]);
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
      prepared, eligibility, std::span<DocSet* const>(views.data(), views.size()), reader, fanout, warnings,
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
    SegmentSources sources{reader, planning, segment, production.tracker};

    auto materialize = [&](size_t source, DomainHandle base, Frame frame) {
      if (this->sourceIsResetExtra(source) && base.get() == resetBase[segnum].get() &&
          (frame == Frame::RESET || inheritedExtras.empty() || !inheritedExtras[source].weight)) {
        return extraSets[segnum][source];
      }
      const auto& inputSource =
          frame == Frame::INHERIT && !inheritedExtras.empty() && inheritedExtras[source].weight
              ? inheritedExtras[source]
              : prepared[source];
      return sources.resolve(*weights[source], segmentSourceUse(source), std::move(base), &inputSource);
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

void DomainVariantPlan::declareReads(ReadDeclarer& reads, Query::PlanningContext& planning,
                                     int32_t requestFlags) const {
  int32_t flags = Query::membershipWeightFlags(requestFlags);
  for (const auto& source : querySources) reads.root(*source.query, planning, flags);
}

void DomainVariantPlan::buildWeights(Query::Context& context, Query::PlanningContext& planning,
                                     int32_t requestFlags) {
  int32_t filterFlags = Query::membershipWeightFlags(requestFlags);
  for (auto& source : querySources) {
    source.weight = context.createRootWeight(*source.query, filterFlags);
    source.use = planning.getFilterUse(*source.query);
  }
}

}  // namespace luxir
