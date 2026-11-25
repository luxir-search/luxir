// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CatalogSummaryView.h"
#include "QueryPrep.h"
#include "luxir/search/RowRelation.h"

namespace luxir {

class ToParentQuery final : public Query {
  Query* child;
  PlanningContext& source;
  RowRelation& relation;
  RowReduction reduction;

public:
  // The score every match receives, when the tree alone fixes it exactly.
  static std::optional<float> uniformScore(Query& query, float multiplier = 1.0f);

  ToParentQuery(Query* child, PlanningContext& source, RowRelation& relation, RowReduction reduction)
      : Query(QueryKind::TO_PARENT), child(child), source(source), relation(relation), reduction(reduction) {}

  Query* getChild() const { return child; }
  std::string_view sourcePath() const { return source.topReader.path(); }
  const RowReduction& getReduction() const { return reduction; }
  bool recoverableAsPredicate() const noexcept override { return child->recoverableAsPredicate(); }

  // A parent matches only through at least one matching child row
  // (min_children >= 1), so a segment without the child space, or whose
  // child rows the child query cannot match, holds no match.
  MatchState mayMatch(const CatalogSummaryView& view) const override {
    auto rows = view.inSpace(source);
    if (!rows) {
      view.prove("nested space absent");
      return MatchState::EMPTY;
    }
    return child->mayMatch(*rows) == MatchState::EMPTY ? MatchState::EMPTY : MatchState::UNKNOWN;
  }

  bool equalsSameKind(const Query& other) const override {
    const auto& rhs = (const ToParentQuery&)other;
    return reduction == rhs.reduction && source.topReader.path() == rhs.source.topReader.path()
        && child->equals(*rhs.child);
  }
  uint64_t hashImpl() const override {
    return mixHash(mixHash(Query::hashImpl(), child->hash()),
        mixHash(Hash::hash(source.topReader.path().data(), source.topReader.path().size()),
            mixHash((int)reduction.kind, mixHash(reduction.min, reduction.max))));
  }
  void validateLogicalImpl(PlanningContext& context, float multiplier) const override {
    child->validateLogical(source, reduction.kind != RowReduction::NONE ? multiplier : 1.0f);
    if (child->mayRequirePreparation()) context.requirePreparation();
  }
  FilterKeyScope appendFilterKey(FilterKeyBuilder& out, const FilterKeyContext& context) const override {
    out.appendKind(getKind());
    out.appendUInt64(reduction.min);
    out.appendUInt64(reduction.max);
    auto scope = relation.appendFilterKey(out, context);
    FilterKeyBuilder childKey;
    auto childScope = child->appendFilterKey(childKey, source.filterKeyContext);
    auto identity = std::move(childKey).finishStructural(childScope, source.filterKeyContext);
    out.appendString(std::string_view((const char*)identity.bytes().data(), identity.bytes().size()));
    if (childScope == FilterKeyScope::READER_STABLE) out.appendUInt64(source.topReader.coreCommitTime());
    return strongestFilterKeyScope(scope, childScope);
  }

  class Weight final : public Query::Weight {
    IndexReader& source;
    Query::Weight* child;
    FilterCache::Use* childUse;
    RowRelation& relation;
    RowReduction reduction;
    float multiplier;

    Query::ScorerSupplier* supplier(MemPool& pool, IndexReader::Segment& segment,
        const QueryPrep::PreparedSource& input, DocSet* domain, Query::SupplierExecutionMode mode) {
      if (mode != Query::SupplierExecutionMode::RAW_MEMBERSHIP && domain && domain->card() == 0) return nullptr;
      auto& leaf = source.segments()[(size_t)segment.ord];
      auto childMode = child->consumesDomain() ? mode : Query::SupplierExecutionMode::RAW_MEMBERSHIP;
      skipCount(SkipStats::joinChildSuppliers);
      auto* supplier = childUse
          ? QueryPrep::filterSupplier(pool, input, source, leaf,
                allowsPruning() ? QueryPrep::FilterSupplierMode::DENSITY_ROUTED
                    : QueryPrep::FilterSupplierMode::EXHAUSTIVE_CLAUSE,
                0, childMode, childUse->work((size_t)leaf.ord).observedOnly())
          : input.segmentSource().scorerSupplier(pool, leaf, childMode);
      if (!supplier) return nullptr;
      return relation.reduce(pool, (size_t)segment.ord, *supplier,
          mode == Query::SupplierExecutionMode::RAW_MEMBERSHIP ? nullptr : domain,
          reduction, multiplier);
    }

    class Prepared final : public Query::Weight::PreparedWeight {
      Weight& weight;
      std::vector<DomainHandle> eligibility;
      QueryPrep::PreparedSource child;

    public:
      Prepared(Weight& weight, QueryPrep::PreparedSource child, std::vector<DomainHandle> eligibility)
          : weight(weight), eligibility(std::move(eligibility)), child(std::move(child)) {}
      // The prepare domain reaches a consuming child through its selection and
      // is not kept: it does not outlive preparation, and a canonical source
      // must answer for every domain.
      Query::ScorerSupplier* scorerSupplierImpl(MemPool& pool, IndexReader::Segment& segment,
          Query::SupplierExecutionMode mode) override {
        return weight.supplier(pool, segment, child,
            mode == Query::SupplierExecutionMode::RAW_MEMBERSHIP ? nullptr : segment.liveDocSet(), mode);
      }
      Query::Scorer* createScorerImpl(MemPool& pool, IndexReader::Segment& segment) override {
        return QueryPrep::createScorer(pool, segment, *this);
      }
      PreparedDomainDependence domainDependence() const noexcept override {
        // Predicate sources were prepared without destination eligibility.
        return weight.child->consumesDomain() ? child.domainDependence
            : PreparedDomainDependence::QUERY_CANONICAL;
      }
      size_t retainedBytes() const noexcept override {
        return sizeof(*this) + eligibility.capacity() * sizeof(DomainHandle)
            + (child.prepared ? child.prepared->retainedBytes() : 0);
      }
      void releaseSegmentState(IndexReader::Segment& segment) noexcept override {
        child.releaseSegmentState(weight.source.segments()[(size_t)segment.ord]);
      }
    };

  public:
    Weight(Query::Context& context, ToParentQuery& query, int32_t flags, float multiplier)
        : Query::Weight(context, query, flags), source(query.source.topReader),
          relation(query.relation),
          reduction(query.reduction), multiplier(constantWhenScored(flags, multiplier)) {
      auto* childContext = Query::Context::create(&context.arena(), query.source);
      if (!(flags & NEED_SCORES) || reduction.kind == RowReduction::NONE) {
        reduction.kind = RowReduction::FIXED;
        this->multiplier = 0;
      }
      auto constant = reduction.kind != RowReduction::FIXED
              && reduction.kind != RowReduction::SUM
          ? uniformScore(*query.child) : std::nullopt;
      if (constant) {
        // Uniform reductions retain the child's rounding before the multiplier.
        this->multiplier = *constant * this->multiplier;
        reduction.kind = RowReduction::FIXED;
      }
      int32_t childFlags = flags;
      if (reduction.kind == RowReduction::FIXED) childFlags &= ~(ALLOW_PRUNING | NEED_SCORES);
      else if (reduction.kind != RowReduction::MAXIMUM || reduction.bounded()) childFlags &= ~ALLOW_PRUNING;
      child = query.child->createWeight(*childContext, childFlags, 1.0f);
      if (reduction.kind != RowReduction::FIXED && reduction.kind != RowReduction::SUM
          && child->isConstantScoring()) reduction.kind = RowReduction::CONSTANT;
      // Every row of the path is not a set worth materializing or caching.
      childUse = !child->needsScores() && !child->allowsPruning() && !child->matchesAllDocs()
          ? childContext->getFilterUse(*query.child) : nullptr;
      if (childUse && childUse->scope() != FilterKeyScope::READER_STABLE) childUse->observe();
      traits = OBSERVED_FILTER_CACHE | (child->getFlags() & (NEEDS_PREPARE | CONSUMES_DOMAIN));
      if (child->consumesDomain()) traits |= NEEDS_PREPARE;
      if (reduction.kind == RowReduction::FIXED || reduction.kind == RowReduction::CONSTANT) {
        traits |= IS_CONSTANT_SCORING;
      }
      assert(&context.topReader == &relation.one);
      assert(&source == &relation.many);
    }
    std::optional<float> constantContribution() const override {
      return reduction.kind == RowReduction::FIXED
          ? std::optional<float>(multiplier) : std::nullopt;
    }
    std::unique_ptr<Query::Weight::PreparedWeight> prepareImpl(Query::Weight::PrepareContext& prepareContext) override {
      std::vector<DomainHandle> eligibility;
      std::vector<DocSet*> views;
      if (child->consumesDomain()) {
        eligibility.reserve(source.segments().size());
        views.reserve(source.segments().size());
        for (auto& leaf : source.segments()) {
          auto& owner = prepareContext.reader.segments()[(size_t)leaf.ord];
          auto* domain = prepareContext.domainPerSeg.empty() ? owner.liveDocSet()
              : prepareContext.domainPerSeg[(size_t)leaf.ord];
          eligibility.push_back(relation.expand((size_t)leaf.ord,
              DomainHandle::borrowed(domain).scoped(owner)));
          views.push_back(eligibility.back().get());
        }
      }
      Query::Weight::PrepareContext childContext{source, views, prepareContext.fanout, prepareContext.warnings,
          child->consumesDomain() ? Query::SupplierExecutionMode::ORDINARY
              : Query::SupplierExecutionMode::RAW_MEMBERSHIP};
      std::array<Query::Weight*, 1> weights{child};
      std::array<FilterCache::Use*, 1> uses{childUse};
      auto inputs = QueryPrep::prepareFilterSources(weights, uses, childContext);
      return std::make_unique<Prepared>(*this, std::move(inputs[0]), std::move(eligibility));
    }
    Query::ScorerSupplier* scorerSupplierImpl(MemPool& pool, IndexReader::Segment& segment,
        Query::SupplierExecutionMode mode) override {
      QueryPrep::PreparedSource input;
      input.weight = child;
      input.cacheUse = childUse;
      return supplier(pool, segment, input,
          mode == Query::SupplierExecutionMode::RAW_MEMBERSHIP ? nullptr : segment.liveDocSet(), mode);
    }
    Query::Scorer* createScorerImpl(MemPool& pool, IndexReader::Segment& segment) override {
      return QueryPrep::createScorer(pool, segment, *this);
    }
    void releaseSegmentState(IndexReader::Segment& segment) noexcept override {
      child->releaseSegmentState(source.segments()[(size_t)segment.ord]);
    }
    bool hasReleasableSegmentState() const noexcept override { return child->hasReleasableSegmentState(); }
  };

  Query::Weight* createWeight(Context& context, int32_t flags, float multiplier) override;
};

} // namespace luxir
