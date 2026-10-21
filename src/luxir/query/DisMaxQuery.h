// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <bit>
#include <cmath>
#include <span>
#include <stdexcept>

#include "Query.h"
#include "BoostQuery.h"

namespace luxir {

// Disjunction max (Lucene's DisjunctionMaxQuery, Elasticsearch dis_max): a
// document matches when any disjunct matches, and scores
//
//   max_i s_i + tie * (sum_i s_i - max_i s_i)
//
// over the disjuncts that match it. tie 0 takes the best disjunct
// alone; 1 adds them up like a disjunction.
//
// Membership is exactly the disjunction of the disjuncts, so the query holds
// that BooleanQuery: unscored use and the filter key go through it and share
// the boolean's cache entries. Scored use shares BooleanQuery's pull scorers.
class DisMaxQuery final : public Query {
  std::span<Query*> disjuncts;
  float tie;
  Query* membership;

public:
  // membership must be the SHOULD-only BooleanQuery over the same disjuncts;
  // make() builds both.
  DisMaxQuery(std::span<Query*> disjuncts, float tie, Query* membership)
    : Query(QueryKind::DIS_MAX), disjuncts(disjuncts), tie(tie),
      membership(membership) {
    if (!std::isfinite(tie) || tie < 0.0f || tie > 1.0f) {
      throw std::invalid_argument("dis_max tie must be finite and in [0, 1]");
    }
    assert(disjuncts.size() >= 2);
  }

  // A single disjunct is returned as is: its max is its own score.
  static Query* make(MemPool& pool, std::span<Query*> disjuncts, float tie);

  std::span<Query*> getDisjuncts() const { return disjuncts; }
  float getTie() const { return tie; }
  Query* getMembership() const { return membership; }

  bool recoverableAsPredicate() const noexcept override {
    return membership->recoverableAsPredicate();
  }

  bool equalsSameKind(const Query& other) const override {
    const auto& rhs = static_cast<const DisMaxQuery&>(other);
    if (std::bit_cast<uint32_t>(tie) != std::bit_cast<uint32_t>(rhs.tie)
        || disjuncts.size() != rhs.disjuncts.size()) {
      return false;
    }
    for (size_t i = 0; i < disjuncts.size(); i++) {
      if (!sameScoringClause(disjuncts[i], rhs.disjuncts[i])) return false;
    }
    return true;
  }

  uint64_t hashImpl() const override {
    uint64_t value = mixHash(Query::hashImpl(), std::bit_cast<uint32_t>(tie));
    for (auto* disjunct : disjuncts) value = mixHash(value, scoringClauseHash(disjunct));
    return value;
  }

  void validateLogicalImpl(
      PlanningContext& context, float multiplier = 1.0f) const override {
    float product = checkedBoostProduct(multiplier, 1.0f);
    for (auto* disjunct : disjuncts) disjunct->validateLogical(context, product);
  }

  // Scores never change which documents match.
  FilterKeyScope appendFilterKey(FilterKeyBuilder& out,
                                 const FilterKeyContext& ctx) const override {
    return membership->appendFilterKey(out, ctx);
  }

  Query::Weight* createWeight(Context& context, int32_t flags,
                              float multiplier = 1.0f) override;
};

} // namespace luxir

#include "BooleanQuery.h"

namespace luxir {

inline Query::Weight* DisMaxQuery::createWeight(
    Context& context, int32_t flags, float multiplier) {
  if ((flags & NEED_SCORES) == 0) {
#ifndef NDEBUG
    if (context.logicalValidationActive()) {
      membership->validateLogical(context.planningContext(), multiplier);
    }
#endif
    return membership->createWeight(context, flags, multiplier);
  }
  return BooleanQuery::makeDisjunctionWeight(
      context, *this, disjuncts, {BooleanQuery::ScorePolicy::DISMAX, tie},
      flags, multiplier);
}

inline Query* DisMaxQuery::make(MemPool& pool, std::span<Query*> disjuncts, float tie) {
  assert(!disjuncts.empty());
  if (disjuncts.size() == 1) return disjuncts[0];
  std::span<Query*> none{};
  auto* membership = pool.make<BooleanQuery>(none, disjuncts, none, none);
  // tie 1 adds every disjunct: that is the disjunction itself.
  if (tie == 1.0f) return membership;
  return pool.make<DisMaxQuery>(disjuncts, tie, membership);
}

} // namespace luxir
