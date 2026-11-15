// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "OwnershipRelation.h"
#include "DomainVariantPlan.h"
#include "luxir/query/QueryPrep.h"

namespace luxir {

class OwnershipRelation::Scorer final : public Query::Scorer {
  Query::Scorer& child;
  OwnershipMapper::Cursor cursor;
  QueryPrep::DocSetScorer* domain;
  int32_t ownerCount;
  bool maximum;
  float multiplier;
  int32_t childDoc;
  int32_t current = -1;
  float value = 0;

public:
  Scorer(MemPool& pool, Query::Scorer& child, const OwnershipMapper& mapper,
         DocSet* domain, int32_t ownerCount, bool maximum, float multiplier)
      : child(child), cursor(mapper, pool),
        domain(domain ? pool.make<QueryPrep::DocSetScorer>(domain, ownerCount) : nullptr),
        ownerCount(ownerCount), maximum(maximum), multiplier(multiplier), childDoc(child.docId()) {}

  int32_t docId() override { return current; }
  float score() override { return value; }

  void setMinCompetitiveScore(float floor) override {
    if (maximum && multiplier > 0.0f) {
      child.setMinCompetitiveScore(Query::unscaleScoreFloor(floor, multiplier));
    }
  }

  int32_t next() override {
    return current == PostingsReader::END ? current : advance(current + 1);
  }

  int32_t advance(int32_t target) override {
    assert(target > current);
    if (domain) {
      target = domain->docId() < target ? domain->advance(target) : domain->docId();
    }
    if (target >= ownerCount) return current = PostingsReader::END;
    auto family = cursor.seek(target);
    int32_t doc = childDoc;
    if (doc < family.first) doc = child.advance(family.first);
    while (doc != PostingsReader::END) {
      // A child inside the sought family needs no lookup.
      if (doc >= family.end) family = cursor.owner(doc);
      int32_t owner = family.owner;
      if (domain) {
        int32_t eligible = domain->docId() < owner ? domain->advance(owner) : domain->docId();
        if (eligible != owner) {
          if (eligible >= ownerCount) return current = PostingsReader::END;
          family = cursor.seek(eligible);
          doc = child.advance(family.first);
          continue;
        }
      }
      value = maximum ? child.score() : 1.0f;
      if (maximum) {
        while ((doc = child.next()) < family.end) value = std::max(value, child.score());
      }
      value *= multiplier;
      // A competitive next() can finish without moving the postings cursor.
      childDoc = doc;
      return current = owner;
    }
    return current = PostingsReader::END;
  }
};

class OwnershipRelation::Supplier final : public Query::ScorerSupplier {
  Query::ScorerSupplier& child;
  const OwnershipMapper& mapper;
  DocSet* domain;
  int32_t ownerCount;
  bool maximum;
  float multiplier;

  class Plan final : public Query::ScorerPlan {
    Supplier& supplier;
    Query::ScorerPlan& child;

    Query::Scorer* buildScorer(MemPool& pool) override {
      auto* scorer = child.build(pool);
      return scorer ? pool.make<Scorer>(pool, *scorer, supplier.mapper, supplier.domain,
          supplier.ownerCount, supplier.maximum, supplier.multiplier) : nullptr;
    }

  public:
    Plan(Supplier& supplier, const Query::PlanContext& context, Query::ScorerPlan& child)
        : Query::ScorerPlan(context, supplier.describeScorer(context), supplier.cost()),
          supplier(supplier), child(child) {}
  };

public:
  Supplier(Query::ScorerSupplier& child, const OwnershipMapper& mapper,
           DocSet* domain, int32_t ownerCount, bool maximum, float multiplier)
      : child(child), mapper(mapper), domain(domain), ownerCount(ownerCount),
        maximum(maximum), multiplier(multiplier) {}

  int64_t cost() override { return std::min(child.cost(), (int64_t)ownerCount); }
  Query::ScorerShape describeScorer(const Query::PlanContext& context) const override {
    Query::ScorerShape shape;
    shape.reportedTwoPhase = Query::ReportedTwoPhase::NO;
    shape.membershipConsumption = context.membershipConsumption == Query::MembershipConsumption::EITHER
        ? Query::MembershipConsumption::WINDOWS : context.membershipConsumption;
    return shape;
  }
  Query::ScorerPlan* resolve(MemPool& pool, const Query::PlanContext& context) override {
    auto childContext = context;
    childContext.membershipConsumption = Query::MembershipConsumption::WINDOWS;
    auto* plan = child.resolve(pool, childContext);
    return pool.make<Plan>(*this, context, *plan);
  }
};

OwnershipRelation::OwnershipRelation(IndexReader& many, IndexReader& one, RequestMemTracker& memory)
    : RowRelation(many, one, memory) {
  auto path = many.path();
  if (path == one.path()) throw RequestError("domain transform requires different spaces");
  while (!path.empty() && path != one.path()) path = spaceParentPath(path);
  if (path != one.path()) throw RequestError("domain transform requires ancestor and descendant paths");
  mappers.reserve(many.segments().size());
  for (size_t leaf = 0; leaf < many.segments().size(); leaf++) {
    auto& child = many.segments()[leaf];
    assert(&child.segmentCore() == &one.segments()[leaf].segmentCore());
    if (child.hasSpace()) mappers.emplace_back(std::in_place, child.segmentCore(), one.path(), many.path());
    else mappers.emplace_back(std::nullopt);
  }
}

Query::ScorerSupplier* OwnershipRelation::reduce(MemPool& pool, size_t leaf,
    Query::ScorerSupplier& input, DocSet* domain, bool maximum, float multiplier) {
  return mappers[leaf] ? pool.make<Supplier>(input, *mappers[leaf], domain,
      one.segments()[leaf].maxDoc(), maximum, multiplier) : nullptr;
}

FilterKeyScope OwnershipRelation::appendFilterKey(FilterKeyBuilder& out, const FilterKeyContext&) const {
  out.appendByte(0); // ownership relation
  out.appendString(many.path());
  out.appendString(one.path());
  return FilterKeyScope::SEGMENT_STABLE;
}

DomainHandle OwnershipRelation::expand(size_t leaf, DomainHandle domain) {
  auto& source = one.segments()[leaf];
  auto& destination = many.segments()[leaf];
  domain.assertScope(source);
  const auto& mapper = mappers[leaf];
  if (mapper) {
    if (!domain.get()) return DomainHandle().scoped(destination);
    if (domain.get() == source.liveDocSet()) {
      return DomainHandle::pinned(destination.liveDocSet(), destination.liveDocsShared()).scoped(destination);
    }
  }
  DocSetBuilder builder(destination.maxDoc());
  if (mapper && domain.get()) {
    domain.get()->forEach([&](int32_t row) {
      auto [first, end] = mapper->range(row);
      builder.addRange(first, end);
    });
  }
  return DomainVariantPlan::account(DomainHandle(builder.build()).scoped(destination), memory, "mapped domain");
}

DomainHandle OwnershipRelation::project(size_t leaf, DomainHandle domain) {
  auto& source = many.segments()[leaf];
  auto& destination = one.segments()[leaf];
  domain.assertScope(source);
  const auto& mapper = mappers[leaf];
  DocSetBuilder builder(destination.maxDoc());
  if (mapper && source.maxDoc()) {
    MemPool pool;
    OwnershipMapper::Cursor cursor(*mapper, pool);
    if (domain.get()) {
      int32_t lastOwner = -1;
      domain.get()->forEach([&](int32_t row) {
        int32_t owner = cursor.owner(row).owner;
        if (owner != lastOwner) builder.add(owner);
        lastOwner = owner;
      });
    } else {
      // All children still project to owners with nonempty families only.
      for (int32_t child = 0; child < source.maxDoc();) {
        auto family = cursor.owner(child);
        builder.add(family.owner);
        child = family.end;
      }
    }
  }
  return DomainVariantPlan::account(DomainHandle(builder.build()).scoped(destination), memory, "mapped domain");
}

} // namespace luxir
