// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/search/ops/TopDocsReq.h"

namespace luxir {

std::span<Query::Weight*> buildFilterWeights(
    std::span<ParsedFilter> filters,
    Query::Context& qcontext, int32_t requestFlags) {
  if (filters.empty()) return {};
  // Filters constrain matches but never contribute to score. Preserve any
  // future request flags, but clear NEED_SCORES.
  int32_t filterFlags = requestFlags
      & ~(Query::NEED_SCORES | Query::ALLOW_PRUNING);
  size_t sourceCount = 0;
  for (const auto& filter : filters) {
    sourceCount += filter.sourceCount();
  }
  auto weights = qcontext.pool.make_span<Query::Weight*>(sourceCount);
  size_t source = 0;
  for (const auto& filter : filters) {
    for (size_t i = 0; i < filter.sourceCount(); i++) {
      weights[source++] =
          qcontext.createRootWeight(*filter.source(i), filterFlags);
    }
  }
  assert(source == weights.size());
  return weights;
}

TopDocsReq::TopDocsReq(
    SearchRequest& req, const std::shared_ptr<IndexReader>& view,
    std::string_view name, const ReqTopDocs& topDocsProto,
    Query::PlanningContext& planning, Query* query, Query* resultQuery,
    std::span<ParsedFilter> filters, DomainVariantPlan&& domainVariants,
    CollectionRequirements requirements, int64_t topCount, SortPlan&& sortPlan,
    CollapsePlan* collapse, DiversifyPlan* diversify, std::optional<SortPlan>&& headSort,
    Query::ScoreProfile scoreProfile, bool requestNeedsScores,
    Preparation preparation)
  : SearchOp(req, view, name), preparation(preparation), topDocsProto(topDocsProto),
    planning(planning), query(query), resultQuery(resultQuery), filters(filters),
    domainVariants(std::move(domainVariants)), requirements(requirements),
    topCount(topCount), sortPlan(std::move(sortPlan)), collapse(collapse),
    diversify(diversify), headSort(std::move(headSort)), scoreProfile(scoreProfile),
    requestNeedsScores(requestNeedsScores) {}

void TopDocsReq::selectSources() {
  assert(!sourceSelection);
  int32_t requestFlags = preparation.weightFlags;
  if (requestNeedsScores) requestFlags |= Query::NEED_SCORES;
  bool exactCountTopK = preparation.exactCountTopKShape && !preparation.boundedCount;

  FilterCache::Use* cacheFirstMembershipUse = nullptr;
  auto* cache = view->filterCache();
  auto wholeCountWork = [&] {
    return !QueryPrep::disableCanonicalCountFactsForTests
        && planning.filterScope(*query) <= FilterKeyScope::CORE_STABLE
        ? FilterCache::Work::nanoseconds(0) : FilterCache::Work::assumed();
  };
  auto lookupWholeCount = [&](bool needsIds = false) {
    bool scalar = !needsIds && !QueryPrep::disableCanonicalCountFactsForTests
        && planning.filterScope(*query) <= FilterKeyScope::CORE_STABLE
        && std::any_of(view->segments().begin(), view->segments().end(),
                       [](auto& segment) { return !segment.hasDeletes(); });
    if (scalar && (preparation.placement != TopDocsPlacement::ROOT_OP
        || std::any_of(view->segments().begin(), view->segments().end(),
                       [](auto& segment) { return segment.hasDeletes(); }))) {
      return std::optional<FilterCache::ExistingCandidate>{};
    }
    return planning.lookupExistingFilterUse(
        *query, preparation.placement == TopDocsPlacement::ROOT_OP, scalar);
  };
  if (preparation.pureCountShape && canOmitWeightForCacheFirstMembership(*query)
      && !directCountAvailable(*query, *view)
      && cache != nullptr && cache->enabled()) {
    auto candidate = lookupWholeCount();
    if (candidate.has_value()) {
      cacheFirstMembershipUse = planning.acceptExistingFilterUse(
          std::move(*candidate), FilterCache::AdmissionLane::WHOLE, wholeCountWork());
    }
    if (cacheFirstMembershipUse != nullptr) {
      skipCount(SkipStats::cacheFirstMembershipWeightSkips);
    }
  }

  bool cacheFirstTopKCount = false;
  if (cacheFirstMembershipUse == nullptr
      && preparation.placement == TopDocsPlacement::ROOT_OP
      && !QueryPrep::disableWholeMembershipPlanForTests
      && preparation.exactCountTopKShape
      && (preparation.foldFilters || filters.empty())
      && canOmitWeightForCacheFirstMembership(*query)
      && !directCountAvailable(*query, *view)
      && cache != nullptr && cache->enabled()) {
    auto candidate = lookupWholeCount(scoreProfile.kind != Query::ScoreProfile::Kind::VARIABLE);
    if (candidate.has_value()) {
      cacheFirstMembershipUse = planning.acceptExistingFilterUse(
          std::move(*candidate), FilterCache::AdmissionLane::WHOLE, wholeCountWork());
    }
    cacheFirstTopKCount = cacheFirstMembershipUse != nullptr;
    if (cacheFirstTopKCount) {
      skipCount(SkipStats::cacheFirstTopKCountWeightSkips);
      if (scoreProfile.kind != Query::ScoreProfile::Kind::VARIABLE) {
        skipCount(SkipStats::cacheFirstConstantTopKWeightSkips);
      }
    }
  }

  std::span<FilterCache::Use*> residentExactDomainUses;
  bool cacheFirstExactDomain = false;
  bool constantExactDomainRanking = requirements.needRankedDocs
      && scoreProfile.kind != Query::ScoreProfile::Kind::VARIABLE;
  bool canOmitForExactDomain = collapse == nullptr && diversify == nullptr
      && !preparation.routedFilters
      && preparation.placement == TopDocsPlacement::ROOT_OP
      && requirements.needExactDomain && !sortPlan.useFieldSort
      && (!requirements.needRankedDocs || constantExactDomainRanking)
      && cache != nullptr && cache->enabled();
  if (canOmitForExactDomain) {
    bool domainIdentity = exactDomainIdentity(*preparation.domainQuery);
    auto sourceQueries = req.requestPool.make_span<Query*>(
        (size_t)!domainIdentity + filters.size());
    size_t sourceIndex = 0;
    bool allOmittable = true;
    if (!domainIdentity) {
      sourceQueries[sourceIndex++] = preparation.domainQuery;
      allOmittable =
          canOmitWeightForCacheFirstMembership(*preparation.domainQuery);
    }
    for (size_t i = 0; i < filters.size(); i++) {
      sourceQueries[sourceIndex++] = filters[i].query;
      allOmittable &=
          canOmitWeightForCacheFirstMembership(*filters[i].query);
    }
    if (allOmittable && !sourceQueries.empty()) {
      bool allowReaderStable = sourceQueries.size() == 1;
      residentExactDomainUses = planning.acceptExistingFilterUses(
          sourceQueries, allowReaderStable,
          FilterCache::AdmissionLane::CLAUSE);
      cacheFirstExactDomain = !residentExactDomainUses.empty();
    }
    if (cacheFirstExactDomain) {
      skipCount(SkipStats::cacheFirstExactDomainWeightSkips);
      if (constantExactDomainRanking) {
        skipCount(SkipStats::cacheFirstConstantTopKWeightSkips);
      }
    }
  }

  bool cacheFirstFieldSort = false;
  bool cacheFirstFieldSortShape =
      collapse == nullptr && diversify == nullptr
      && preparation.placement == TopDocsPlacement::ROOT_OP
      && !QueryPrep::disableWholeMembershipPlanForTests
      && requirements.needRankedDocs && sortPlan.useFieldSort
      && (requestFlags & Query::NEED_SCORES) == 0
      && !requirements.needExactDomain
      && (preparation.foldFilters || filters.empty())
      && canOmitWeightForCacheFirstMembership(*query)
      && cache != nullptr && cache->enabled();
  if (cacheFirstFieldSortShape) {
    cacheFirstMembershipUse = planCacheFirstFieldSortWholeMembership(
        *query, planning, true);
    cacheFirstFieldSort = cacheFirstMembershipUse != nullptr;
    if (cacheFirstFieldSort) {
      skipCount(SkipStats::cacheFirstFieldSortWeightSkips);
    }
  }

  bool omitMainWeight = (cacheFirstMembershipUse != nullptr
          && (!cacheFirstTopKCount
              || scoreProfile.kind
                  != Query::ScoreProfile::Kind::VARIABLE))
      || cacheFirstExactDomain || cacheFirstFieldSort;
  bool cacheFirstVariableRanking = cacheFirstTopKCount
      && scoreProfile.kind == Query::ScoreProfile::Kind::VARIABLE;
  int32_t mainWeightFlags = cacheFirstVariableRanking
      ? requestFlags | Query::NEED_SCORES | Query::ALLOW_PRUNING
      : requestFlags;
  sourceSelection = SourceSelection{
      .cache = cache,
      .cacheFirstMembershipUse = cacheFirstMembershipUse,
      .residentExactDomainUses = residentExactDomainUses,
      .requestFlags = requestFlags,
      .mainWeightFlags = mainWeightFlags,
      .exactCountTopK = exactCountTopK,
      .constantExactDomainRanking = constantExactDomainRanking,
      .cacheFirstTopKCount = cacheFirstTopKCount,
      .cacheFirstExactDomain = cacheFirstExactDomain,
      .cacheFirstFieldSort = cacheFirstFieldSort,
      .cacheFirstVariableRanking = cacheFirstVariableRanking,
      .omitMainWeight = omitMainWeight,
  };
}

void TopDocsReq::materialize() {
  if (!sourceSelection) selectSources();
  const auto& selected = *sourceSelection;
  auto* cache = selected.cache;
  auto* cacheFirstMembershipUse = selected.cacheFirstMembershipUse;
  auto residentExactDomainUses = selected.residentExactDomainUses;
  int32_t requestFlags = selected.requestFlags;
  int32_t mainWeightFlags = selected.mainWeightFlags;
  bool exactCountTopK = selected.exactCountTopK;
  bool constantExactDomainRanking = selected.constantExactDomainRanking;
  bool cacheFirstTopKCount = selected.cacheFirstTopKCount;
  bool cacheFirstExactDomain = selected.cacheFirstExactDomain;
  bool cacheFirstFieldSort = selected.cacheFirstFieldSort;
  bool cacheFirstVariableRanking = selected.cacheFirstVariableRanking;
  bool omitMainWeight = selected.omitMainWeight;
  Query::Context* qcontext = nullptr;
  if (omitMainWeight) {
    skipCount(SkipStats::cacheFirstQueryContextsOmitted);
  } else {
    qcontext = Query::Context::create(&req.arena, planning);
    skipCount(SkipStats::queryContextsCreated);
  }
  auto* weight = omitMainWeight
      ? nullptr : qcontext->createRootWeight(*query, mainWeightFlags);
  Query::Weight* variantMembershipWeight = nullptr;
  if (preparation.routedFilters) {
    assert(weight != nullptr);
    int32_t membershipFlags =
        requestFlags & ~(Query::NEED_SCORES | Query::ALLOW_PRUNING);
    variantMembershipWeight =
        !weight->needsScores() && !weight->allowsPruning()
            ? weight
            : qcontext->createRootWeight(*query, membershipFlags);
  }
  if (cacheFirstTopKCount && weight != nullptr) {
    skipCount(SkipStats::cacheFirstTopKRankingWeights);
  }
  bool sparseFilteredTopKReroute = weight != nullptr
      && (preparation.allowPruning || cacheFirstVariableRanking)
      && preparation.foldFilters
      && !requirements.needExactDomain && !weight->needsPrepare()
      && admitSparseFilteredTopK(
          *weight, *view, topCount);
  if (sparseFilteredTopKReroute) {
    bool unionFamily = weight->sparseFilteredTopKFamily()
        == Query::Weight::SparseFilteredTopKFamily::UNION;
    mainWeightFlags &= ~Query::ALLOW_PRUNING;
    weight = qcontext->createRootWeight(*query, mainWeightFlags);
    requestFlags &= ~Query::ALLOW_PRUNING;
    skipCount(SkipStats::sparseFilteredTopKReroutes);
    if (unionFamily) {
      skipCount(SkipStats::sparseFilteredTopKUnionReroutes);
    }
  }
  Query::Weight* countWeight = nullptr;
  Query::Weight* rankingWeight = nullptr;
  bool newComposition = exactCountTopK && weight != nullptr && !weight->canComposeExactCountTopK();
  bool legalComposition = newComposition && topCount <= kExactCountTopKMaxDepth
      && !weight->needsPrepare() && !weight->consumesDomain() && !weight->isConstantScoring();
  bool forceComposition = forceExactCountTopKCompositionForTests && newComposition;
  bool forcedComposition = forceComposition && legalComposition;
  if (forceComposition && !forcedComposition) skipCount(SkipStats::forcedExactCountTopKCapabilityRejects);
  std::span<uint8_t> provedSegments;
  if (legalComposition && !forcedComposition && !disableTopKCountComposition
      && (preparation.foldFilters || filters.empty())
      && !cacheFirstTopKCount && !disableProvedDensityCompositionForTests) {
    for (auto& segment : view->segments()) {
      auto bounds = weight->countShortcut(segment, nullptr);
      int32_t inverse = provedDensityCompositionInverseForTests;
      if (inverse <= 0 || bounds.minimum() <= 0
          || bounds.minimum() * inverse < segment.maxDoc()
          || !weight->mayRankScoresExactly(segment)) continue;
      if (provedSegments.empty()) {
        provedSegments = qcontext->pool.make_span<uint8_t>(view->segments().size());
        std::fill(provedSegments.begin(), provedSegments.end(), 0);
      }
      provedSegments[(size_t)segment.ord] = 1;
    }
  }
  if (weight != nullptr && !cacheFirstTopKCount
      && !disableTopKCountComposition && exactCountTopK
      && !weight->needsPrepare()
      && (weight->canComposeExactCountTopK() || forcedComposition || !provedSegments.empty())
      // A constant-score ranking pass is bounded only when no collector
      // filter can reject its first K matches. Variable-score composition
      // retains its existing filtered candidate route.
      && (filters.empty() || !weight->isConstantScoring())) {
    int32_t countFlags =
        requestFlags & ~(Query::NEED_SCORES | Query::ALLOW_PRUNING);
    int32_t rankingFlags =
        requestFlags | Query::NEED_SCORES | Query::ALLOW_PRUNING;
    countWeight = qcontext->createRootWeight(*query, countFlags);
    rankingWeight = qcontext->createRootWeight(*query, rankingFlags);
  }
  auto filterWeights = preparation.foldFilters || cacheFirstExactDomain || filters.empty()
    ? std::span<Query::Weight*>{}
    : buildFilterWeights(filters, *qcontext, requestFlags);
  if (domainVariants.sourceCount() > 0) {
    domainVariants.buildWeights(
        *qcontext, planning, requestFlags);
  }
  Query::Weight* domainQueryWeight = nullptr;
  std::span<Query::Weight*> domainFilterWeights;
  if (!preparation.routedFilters && !cacheFirstExactDomain
      && requirements.needExactDomain
      && (!requirements.needRankedDocs
          || (constantExactDomainRanking
              && !sortPlan.useFieldSort))) {
    if (preparation.foldFilters || requirements.needRankedDocs) {
      int32_t domainFlags =
          requestFlags & ~(Query::NEED_SCORES | Query::ALLOW_PRUNING);
      domainQueryWeight =
          qcontext->createRootWeight(*preparation.domainQuery, domainFlags);
      domainFilterWeights =
          buildFilterWeights(filters, *qcontext, domainFlags);
    } else {
      domainQueryWeight = weight;
      domainFilterWeights = filterWeights;
    }
  }

  Query::Weight* wholeMembershipWeight = nullptr;
  Query::Weight* wholeRankingWeight = nullptr;
  FilterCache::Use* wholeMembershipUse = nullptr;
  bool wholeConstantRanking = false;
  bool pureCount = preparation.pureCountShape && filterWeights.empty();
  bool wholeTopKCount = !QueryPrep::disableWholeMembershipPlanForTests
      && preparation.exactCountTopKShape
      && (cacheFirstTopKCount
          || (weight != nullptr && !weight->needsPrepare()))
      && filterWeights.empty();
  bool wholeFieldSort = !QueryPrep::disableWholeMembershipPlanForTests
      && requirements.needRankedDocs && sortPlan.useFieldSort
      && (cacheFirstFieldSort
          || (weight != nullptr && !weight->needsScores()))
      && !requirements.needExactDomain && filterWeights.empty();
  // Nested TopDocs under a match-all parent can inherit the canonical
  // reader domain. Preserve the landed probe there and let
  // probeReaderStable enforce the runtime domain gate. Fusion sources have
  // a different ownership shape and remain excluded.
  bool preparedWhole = weight != nullptr && weight->needsPrepare()
      && preparation.placement != TopDocsPlacement::FUSION_SOURCE
      && (pureCount || wholeFieldSort);
  if (weight != nullptr && weight->needsPrepare() && !preparedWhole) {
    pureCount = false;
    wholeFieldSort = false;
  }
  if (pureCount || wholeTopKCount || wholeFieldSort) {
    if (cacheFirstMembershipUse != nullptr) {
      wholeMembershipUse = cacheFirstMembershipUse;
    } else if (wholeTopKCount && countWeight != nullptr
        && !countWeight->needsScores()
        && !countWeight->allowsPruning()) {
      wholeMembershipWeight = countWeight;
    } else if (pureCount || wholeFieldSort) {
      wholeMembershipWeight = weight;
    } else {
      int32_t membershipFlags =
          requestFlags & ~(Query::NEED_SCORES | Query::ALLOW_PRUNING);
      wholeMembershipWeight =
          qcontext->createRootWeight(*query, membershipFlags);
    }
    if (wholeMembershipWeight != nullptr) {
      assert(!wholeMembershipWeight->needsScores());
      assert(!wholeMembershipWeight->allowsPruning());

      bool everySegmentConstant = wholeMembershipWeight->matchesAllDocs();
      if (!everySegmentConstant) {
        everySegmentConstant = true;
        bool everySegmentEmpty = !view->segments().empty();
        for (auto& segment : view->segments()) {
          DocSet* rootDomain = segment.liveDocSet();
          auto shortcut = wholeMembershipWeight->countShortcut(segment, rootDomain);
          everySegmentEmpty &= shortcut.how == Query::CountHow::METADATA
              && shortcut.maxMatches == 0;
          if (shortcut.how > (preparation.boundedCount
                  ? Query::CountHow::METADATA : Query::COUNT_PROBE_LIMIT)) {
            everySegmentConstant = false;
            break;
          }
        }
        if (!preparation.boundedCount && everySegmentEmpty && hasRequiredConjunction(*query)) {
          everySegmentConstant = false;
        }
      }
      if (!preparation.boundedCount && !everySegmentConstant && cache != nullptr && cache->enabled()) {
        bool acquireUse = !wholeFieldSort || preparedWhole
            || QueryPrep::WholeMembershipPlan::observesFieldSort(
                QueryPrep::WholeMembershipConsumer::FIELD_SORT,
                planning.filterScope(*query), wholeMembershipWeight);
        // An accepted reader-stable value replaces the ANN preparation pass.
        // Its bounded kNN membership is also a valid ladder fallback, while
        // the numeric exact-domain driver still prices exact cardinality per
        // segment. Query-canonical prepared sources may publish raw segment
        // membership through the same lane; preparation alone says nothing
        // about key lifetime.
        if (acquireUse) {
          wholeMembershipUse = qcontext->getFilterUse(
              *query, FilterCache::AdmissionLane::WHOLE);
        }
      }
      // Field-sort constants are useful per segment even without a cache.
      // Prepared membership still requires a cache source and its runtime provenance check.
      if (!everySegmentConstant && wholeMembershipUse == nullptr
          && !(wholeFieldSort && !wholeMembershipWeight->needsPrepare())) {
        wholeMembershipWeight = nullptr;
      }
    }

    if (cacheFirstTopKCount && wholeTopKCount
        && scoreProfile.kind == Query::ScoreProfile::Kind::VARIABLE) {
      assert(weight != nullptr);
      wholeRankingWeight = weight;
    } else if (weight != nullptr && wholeTopKCount
        && wholeMembershipWeight != nullptr
        && !weight->isConstantScoring()) {
      Query::Weight* countFreeRankingWeight = preparation.boundedCount ? weight : rankingWeight;
      if (countFreeRankingWeight == nullptr) {
        int32_t rankingFlags =
            requestFlags | Query::NEED_SCORES | Query::ALLOW_PRUNING;
        countFreeRankingWeight =
            qcontext->createRootWeight(*query, rankingFlags);
      }
      bool reroute = !preparation.boundedCount && preparation.foldFilters
          && !countFreeRankingWeight->needsPrepare()
          && admitSparseFilteredTopK(
              *countFreeRankingWeight, *view, topCount);
      if (reroute) {
        bool unionFamily = countFreeRankingWeight->sparseFilteredTopKFamily()
            == Query::Weight::SparseFilteredTopKFamily::UNION;
        wholeRankingWeight = weight;
        skipCount(SkipStats::sparseFilteredTopKReroutes);
        if (unionFamily) {
          skipCount(SkipStats::sparseFilteredTopKUnionReroutes);
        }
      } else {
        wholeRankingWeight = countFreeRankingWeight;
      }
    }
    wholeConstantRanking = wholeTopKCount && !cacheFirstVariableRanking
        && (weight == nullptr
                ? scoreProfile.kind
                    != Query::ScoreProfile::Kind::VARIABLE
                : weight->isConstantScoring());
  }

  weightContext = qcontext;
  this->weight = weight;
  this->variantMembershipWeight = variantMembershipWeight;
  this->countWeight = countWeight;
  this->rankingWeight = rankingWeight;
  this->wholeRankingWeight = wholeRankingWeight;
  this->wholeConstantRanking = wholeConstantRanking;
  this->filterWeights = filterWeights;
  sortValues = sortValueEnvelope(sortPlan, query, filters);

  auto wholeConsumer = sortPlan.useFieldSort
      ? QueryPrep::WholeMembershipConsumer::FIELD_SORT
      : requirements.needRankedDocs
            ? wholeConstantRanking ? QueryPrep::WholeMembershipConsumer::TOP_K_FIRST_IDS
                  : requirements.countLimit >= 0
                        ? QueryPrep::WholeMembershipConsumer::TOP_K_BOUNDED_COUNT
                        : QueryPrep::WholeMembershipConsumer::TOP_K_COUNT
            : QueryPrep::WholeMembershipConsumer::COUNT;
  if (wholeMembershipWeight != nullptr) {
    wholeMembershipPlan = QueryPrep::WholeMembershipPlan(
        *wholeMembershipWeight, wholeMembershipUse,
        planning.filterUses,
        PreparedDomainDependence::QUERY_CANONICAL,
        wholeConsumer);
  } else if (wholeMembershipUse != nullptr) {
    wholeMembershipPlan = QueryPrep::WholeMembershipPlan::acceptedExisting(
        *wholeMembershipUse, planning.filterUses, wholeConsumer);
  }
  if (weight == nullptr) {
    bool acceptedWhole = wholeMembershipUse != nullptr
        && !requirements.needExactDomain
        && ((sortPlan.useFieldSort
                && requirements.needRankedDocs && !requestNeedsScores)
            || (requirements.needsCount()
                && (!requirements.needRankedDocs
                    || scoreProfile.kind
                        != Query::ScoreProfile::Kind::VARIABLE)));
    bool residentExact = !residentExactDomainUses.empty()
        && requirements.needExactDomain && !sortPlan.useFieldSort
        && (!requirements.needRankedDocs
            || scoreProfile.kind != Query::ScoreProfile::Kind::VARIABLE);
    if ((!acceptedWhole && !residentExact) || !filterWeights.empty()) {
      throw std::logic_error(
          "Weight-free TopDocs requires a complete cached source");
    }
  }
  if (!filterWeights.empty()) {
    filterUses = planning.pool.make_span<FilterCache::Use*>(
        filterWeights.size());
    size_t source = 0;
    for (const auto& filter : filters) {
      for (size_t i = 0; i < filter.sourceCount(); i++) {
        filterUses[source++] = planning.getFilterUse(*filter.source(i));
      }
    }
    assert(source == filterWeights.size());
  }
  if (!residentExactDomainUses.empty()) {
    for (auto* use : residentExactDomainUses) {
      exactDomainPlan.addAcceptedExisting(*use);
    }
  } else if (domainQueryWeight != nullptr) {
    if (!domainQueryWeight->matchesAllDocs()) {
      exactDomainPlan.add(
          *domainQueryWeight, planning.getFilterUse(*preparation.domainQuery));
    }
    size_t source = 0;
    for (const auto& filter : filters) {
      assert(!filter.composesUnion());
      for (size_t i = 0; i < filter.sourceCount(); i++) {
        exactDomainPlan.add(
            *domainFilterWeights[source++],
            planning.getFilterUse(*filter.source(i)));
      }
    }
    assert(source == domainFilterWeights.size());
  }

  forcedExactCountTopKComposition = forcedComposition && countWeight != nullptr;
  provedDensitySegments = provedSegments;
  boundedCount = preparation.boundedCount && wholeMembershipWeight == nullptr
      && wholeMembershipUse == nullptr;
  if (boundedCount && filterWeights.empty()) {
    int64_t minMatches = 0;
    for (auto& segment : view->segments()) {
      minMatches += weight->countShortcut(segment, nullptr).minimum();
    }
    countLimitProven = minMatches >= requirements.countLimit;
  }
  estimateCount = preparation.estimateCount && boundedCount;
  // A bounded count that metadata does not settle is counted apart from
  // ranking: an unscored pass stops once the total passes the limit, and
  // ranking prunes from its first page instead of holding its threshold
  // until the limit is reached. Constant scoring keeps its windowed drain.
  if (boundedCount && !countLimitProven && !estimateCount
      && !disableBoundedCountSplit && weight != nullptr && !weight->needsPrepare()
      && weight->allowsPruning() && !weight->isConstantScoring()) {
    int64_t remaining = requirements.countLimit;
    for (auto& segment : view->segments()) {
      // Metadata bounds may be loose, but a count that cannot pass the
      // limit would only duplicate the held ranking pass.
      auto bound = weight->countShortcut(segment, nullptr).maxMatches;
      if (bound > remaining) {
        boundedCountWeight = qcontext->createRootWeight(
            *query, requestFlags & ~(Query::NEED_SCORES | Query::ALLOW_PRUNING));
        break;
      }
      remaining -= bound;
    }
  }
}

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
void TopDocsReq::CalcBase::prepareAndDispatch(FanoutScope* tg) {
  {
    auto release = scope_guard([&]() noexcept {
      for (auto& segment : thisOp().view->segments()) releaseSegmentState(segment);
    });
    prepareDomains<Structural>(tg);
  }
  dispatch<Structural, MetricGroups, Admission>(tg, thisOp().domainVariants.empty()
      ? std::span<const DomainHandle>(effectiveDomains) : std::span<const DomainHandle>(inputDomains));
}

template <bool Structural>
void TopDocsReq::CalcBase::prepareDomains(FanoutScope* tg) {
  auto& op = thisOp();
  auto baseDomains = std::span<DocSet* const>(inputDomainViews.data(), inputDomainViews.size());
  Query::Weight::PrepareContext baseCtx{*op.view, baseDomains, tg, &op.req};
  bool routed = !op.domainVariants.empty();
  bool mainNeedsPrepare = op.weight != nullptr && (op.wholeMembershipPlan.preparesMainWeight(*op.weight) ||
                                                   op.weight->needsPrepare());
  if (routed) {
    size_t segmentCount = op.view->segments().size();
    preparedRoutedReservations.resize(segmentCount);
    if (mainNeedsPrepare) {
      preparedRoutedFilters.resize(segmentCount);
    }
    for (size_t i = 0; i < segmentCount; i++) {
      auto& seg = op.view->segments()[i];
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
    liveDomains.reserve(op.view->segments().size());
    for (auto& segment : op.view->segments()) {
      liveDomains.push_back(resetBase<Structural>(segment.ord));
    }
    Query::Weight::PrepareContext liveCtx{*op.view,
                                          std::span<DocSet* const>(liveDomains.data(), liveDomains.size()),
                                          tg, &op.req};
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
      for (size_t i = 0; i < op.view->segments().size(); i++) {
        preparedRoutedFilters[i] =
            materializeRoutedFilters<Structural>((int32_t)i, baseDomains[i], preparedRoutedReservations[i],
                                                 Structural ? &inputDomains[i] : nullptr);
        effectiveDomains[i] = op.domainVariants.defaultDomain<Structural>(
            inputDomains[i], preparedRoutedFilters[i], Structural ? &op.req.memoryTracker : nullptr);
        assert(effectiveDomains[i].isDeliverable());
        effectiveDomainViews[i] = effectiveDomains[i].get();
      }
      Query::Weight::PrepareContext queryCtx{
          *op.view, std::span<DocSet* const>(effectiveDomainViews.data(), effectiveDomainViews.size()),
          tg, &op.req};
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

  for (size_t i = 0; i < op.view->segments().size(); i++) {
    if (op.filterWeights.empty()) {
      effectiveDomains[i] = inputDomains[i];
    } else {
      effectiveDomains[i] = buildEffectiveDomain<Structural>((int32_t)i, baseDomains[i]);
    }
    assert(effectiveDomains[i].isDeliverable());
    effectiveDomainViews[i] = effectiveDomains[i].get();
  }

  Query::Weight::PrepareContext queryCtx{
      *op.view, std::span<DocSet* const>(effectiveDomainViews.data(), effectiveDomainViews.size()),
      tg, &op.req};
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
void TopDocsReq::CalcBase::prepareMainVariants(FanoutScope* tg) {
  auto& op = thisOp();
  if (preparedWeight == nullptr || !op.domainVariants.needsInheritProduction() ||
      preparedWeight->domainDependence() == PreparedDomainDependence::QUERY_CANONICAL) {
    return;
  }

  auto segments = op.view->segments();
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
      Query::Weight::PrepareContext context{*op.view,
                                            std::span<DocSet* const>(domainViews.data(), domainViews.size()),
                                            tg, &op.req};
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
      matches = std::move(matches).scoped(segments[segnum]);
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
                                                      FanoutScope* tg) {
  auto& op = thisOp();
  size_t sourceCount = op.domainVariants.sourceCount();
  size_t segmentCount = op.view->segments().size();
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
  Query::Weight::PrepareContext liveContext{*op.view, liveDomains, tg, &op.req};
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
    auto& segment = op.view->segments()[segnum];
    for (size_t source : resetExtraSources) {
      DomainHandle set;
      if constexpr (Structural) {
        DomainVariantPlan::SegmentSources sources{*op.view, op.planning, segment, op.req.memoryTracker};
        set = sources.resolve(*op.domainVariants.sourceWeight(source), op.domainVariants.segmentSourceUse(source),
                              resetBaseHandle((int32_t)segnum), &preparedDomainSources[source]);
      } else {
        set = QueryPrep::materializeEffectiveFilter(preparedDomainSources[source], *op.view, segment,
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
                                   : DomainHandle::pinned(liveDomains[segnum], op.view).scoped(op.view->segments()[segnum]);
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
        preparedDomainSources, eligibility, liveDomains, *op.view, tg, &op.req,
        [&](size_t variant, size_t segnum) {
          auto& segment = op.view->segments()[segnum];
          return reserveVariantPreparation<Structural>(
              segment.maxDoc(), "top_docs '" + std::string(op.name) + "' variant " + std::to_string(variant) +
                                    " segment " + std::to_string(segnum));
        });
  } else {
    preparedDomainVariants = op.domainVariants.prepareResetVariants(
        preparedDomainSources, eligibility, liveDomains, *op.view, tg, &op.req,
        [&](size_t variant, size_t segnum) {
          auto& segment = op.view->segments()[segnum];
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

template void TopDocsReq::CalcBase::prepareAndDispatch<false, false>(FanoutScope*);
template void TopDocsReq::CalcBase::prepareAndDispatch<true, false>(FanoutScope*);
template void TopDocsReq::CalcBase::prepareAndDispatch<false, true>(FanoutScope*);
template void TopDocsReq::CalcBase::prepareAndDispatch<true, true>(FanoutScope*);
template void TopDocsReq::CalcBase::prepareAndDispatch<false, false, DiversifyAdmission>(
    FanoutScope*);
template void TopDocsReq::CalcBase::prepareAndDispatch<true, false, DiversifyAdmission>(
    FanoutScope*);
template void TopDocsReq::CalcBase::prepareDomains<false>(FanoutScope*);
template void TopDocsReq::CalcBase::prepareDomains<true>(FanoutScope*);
template void TopDocsReq::CalcBase::prepareMainVariants<false>(FanoutScope*);
template void TopDocsReq::CalcBase::prepareMainVariants<true>(FanoutScope*);
template void TopDocsReq::CalcBase::prepareDomainQueryVariants<false>(std::span<DocSet* const>,
                                                                      FanoutScope*);
template void TopDocsReq::CalcBase::prepareDomainQueryVariants<true>(std::span<DocSet* const>,
                                                                     FanoutScope*);

}  // namespace luxir
