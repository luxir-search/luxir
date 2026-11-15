// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "QueryPrep.h"
#include "luxir/search/RowRelation.h"

namespace luxir {

class ToParentQuery final : public Query {
  Query* child;
  PlanningContext& source;
  RowRelation& relation;
  bool maximum;

public:
  ToParentQuery(Query* child, PlanningContext& source, RowRelation& relation, bool maximum)
      : Query(QueryKind::TO_PARENT), child(child), source(source), relation(relation), maximum(maximum) {}

  Query* getChild() const { return child; }
  bool takesMaximum() const { return maximum; }
  bool recoverableAsPredicate() const noexcept override { return child->recoverableAsPredicate(); }

  bool equalsSameKind(const Query& other) const override {
    const auto& rhs = (const ToParentQuery&)other;
    return maximum == rhs.maximum && source.topReader.path() == rhs.source.topReader.path()
        && child->equals(*rhs.child);
  }
  uint64_t hashImpl() const override {
    return mixHash(mixHash(Query::hashImpl(), child->hash()),
        mixHash(Hash::hash(source.topReader.path().data(), source.topReader.path().size()), maximum));
  }
  void validateLogicalImpl(PlanningContext&, float multiplier) const override {
    child->validateLogical(source, maximum ? multiplier : 1.0f);
  }
  FilterKeyScope appendFilterKey(FilterKeyBuilder& out, const FilterKeyContext& context) const override {
    out.appendKind(getKind());
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
    RowRelation& relation;
    bool maximum;
    float multiplier;

    Query::ScorerSupplier* supplier(MemPool& pool, IndexReader::Segment& segment,
        Query::SegmentSource& childSource, DocSet* domain, Query::SupplierExecutionMode mode) {
      if (mode != Query::SupplierExecutionMode::RAW_MEMBERSHIP && domain && domain->card() == 0) return nullptr;
      auto& leaf = source.segments()[(size_t)segment.ord];
      auto childMode = child->consumesDomain() ? mode : Query::SupplierExecutionMode::RAW_MEMBERSHIP;
      auto* input = childSource.scorerSupplier(pool, leaf, childMode);
      if (!input) return nullptr;
      return relation.reduce(pool, (size_t)segment.ord, *input,
          mode == Query::SupplierExecutionMode::RAW_MEMBERSHIP ? nullptr : domain,
          maximum && needsScores(), multiplier);
    }

    class Prepared final : public Query::Weight::PreparedWeight {
      Weight& weight;
      std::vector<DomainHandle> eligibility;
      QueryPrep::PreparedSource child;
      std::vector<DocSet*> domains;

    public:
      Prepared(Weight& weight, QueryPrep::PreparedSource child, std::span<DocSet* const> domains,
               std::vector<DomainHandle> eligibility)
          : weight(weight), eligibility(std::move(eligibility)), child(std::move(child)),
            domains(domains.begin(), domains.end()) {}
      Query::ScorerSupplier* scorerSupplierImpl(MemPool& pool, IndexReader::Segment& segment,
          Query::SupplierExecutionMode mode) override {
        return weight.supplier(pool, segment, child.segmentSource(),
            mode == Query::SupplierExecutionMode::RAW_MEMBERSHIP ? nullptr
                : domains.empty() ? segment.liveDocSet() : domains[(size_t)segment.ord], mode);
      }
      Query::Scorer* createScorer(MemPool& pool, IndexReader::Segment& segment) override {
        return QueryPrep::createScorer(pool, segment, *this);
      }
      PreparedDomainDependence domainDependence() const noexcept override {
        // Predicate sources were prepared without destination eligibility;
        // raw output remains canonical even when the ordinary scorer is scoped.
        return weight.child->consumesDomain() ? child.domainDependence
            : PreparedDomainDependence::QUERY_CANONICAL;
      }
      size_t retainedBytes() const noexcept override {
        return sizeof(*this) + eligibility.capacity() * sizeof(DomainHandle)
            + domains.capacity() * sizeof(DocSet*)
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
          maximum(query.maximum), multiplier(constantWhenScored(flags, multiplier)) {
      auto* childContext = Query::Context::create(&context.arena(), query.source);
      child = query.child->createWeight(*childContext, maximum ? flags : flags & ~(ALLOW_PRUNING | NEED_SCORES), 1.0f);
      traits = NEEDS_PREPARE | (child->getFlags() & CONSUMES_DOMAIN);
      if (!maximum || !needsScores()) traits |= IS_CONSTANT_SCORING;
      assert(&context.topReader == &relation.one);
      assert(&source == &relation.many);
    }
    std::unique_ptr<Query::Weight::PreparedWeight> prepare(Query::Weight::PrepareContext& prepareContext) override {
      QueryPrep::PreparedSource input;
      input.weight = child;
      std::vector<DomainHandle> eligibility;
      std::vector<DocSet*> views;
      if (child->needsPrepare()) {
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
        Query::Weight::PrepareContext childContext{source, views, prepareContext.parallel, prepareContext.warnings,
            child->consumesDomain() ? Query::SupplierExecutionMode::ORDINARY
                : Query::SupplierExecutionMode::RAW_MEMBERSHIP};
        input.setPrepared(child->prepare(childContext));
      }
      return std::make_unique<Prepared>(*this, std::move(input), prepareContext.domainPerSeg, std::move(eligibility));
    }
    Query::ScorerSupplier* scorerSupplierImpl(MemPool& pool, IndexReader::Segment& segment,
        Query::SupplierExecutionMode mode) override {
      return supplier(pool, segment, *child,
          mode == Query::SupplierExecutionMode::RAW_MEMBERSHIP ? nullptr : segment.liveDocSet(), mode);
    }
    Query::Scorer* createScorer(MemPool& pool, IndexReader::Segment& segment) override {
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
