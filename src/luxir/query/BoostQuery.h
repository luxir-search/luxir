#pragma once

#include <bit>
#include <cmath>
#include <stdexcept>

#include "Query.h"
#include "TermQuery.h"

namespace luxir {

// Query-tree-only score multiplier. Weight creation folds the factor into
// scoring leaves, so there is deliberately no BoostQuery execution object.
class BoostQuery final : public Query {
  Query* child;
  float boost;

public:
  BoostQuery(Query* child, float boost)
    : Query(QueryKind::BOOST), child(child), boost(boost) {
    if (!std::isfinite(boost) || boost < 0.0f) {
      throw std::invalid_argument("query boost must be finite and non-negative");
    }
  }

  bool equalsSameKind(const Query& other) const override {
    const auto& rhs = static_cast<const BoostQuery&>(other);
    return sameScoringClause(this, &rhs);
  }

  uint64_t hashImpl() const override {
    return mixHash(Query::hashImpl(), scoringClauseHash(this));
  }

  Query* getChild() const { return child; }
  float getBoost() const { return boost; }

  FieldSortConjunction fieldSortConjunction(
      PlanningContext& context) const override {
    return child->fieldSortConjunction(context);
  }

  void validateLogicalImpl(
      PlanningContext& context, float multiplier = 1.0f) const override {
    child->validateLogical(
        context, checkedBoostProduct(multiplier, boost));
  }

  FilterKeyScope appendFilterKey(FilterKeyBuilder& out,
                                 const FilterKeyContext& ctx) const override {
    return child->appendFilterKey(out, ctx);
  }

  Query::Weight* createWeight(Context& context, int32_t flags,
                              float multiplier = 1.0f) override {
    return child->createWeight(context, flags, multiplier * boost);
  }
};

inline const Query* peelBoost(const Query* query, float& boost) {
  if (query->getKind() == QueryKind::BOOST) {
    const auto* boostQuery = static_cast<const BoostQuery*>(query);
    boost = Query::checkedBoostProduct(boost, boostQuery->getBoost());
    return peelBoost(boostQuery->getChild(), boost);
  }
  if (query->getKind() == QueryKind::TERM) {
    const auto* termQuery = static_cast<const TermQuery*>(query);
    boost = Query::checkedBoostProduct(boost, termQuery->getBoost());
  }
  return query;
}

// The peeled core of a mutable clause stays mutable: merged clauses are
// rebuilt around it.
inline Query* peelBoost(Query* query, float& boost) {
  return const_cast<Query*>(peelBoost(static_cast<const Query*>(query), boost));
}

inline bool sameScoringClause(const Query* a, const Query* b) {
  float aBoost = 1.0f;
  float bBoost = 1.0f;
  const Query* aCore = peelBoost(a, aBoost);
  const Query* bCore = peelBoost(b, bBoost);
  return std::bit_cast<uint32_t>(aBoost) == std::bit_cast<uint32_t>(bBoost)
      && aCore->equals(*bCore);
}

inline uint64_t scoringClauseHash(const Query* query) {
  float boost = 1.0f;
  const Query* core = peelBoost(query, boost);
  uint32_t bits = std::bit_cast<uint32_t>(boost);
  return Hash::hash(&bits, sizeof(bits), core->hash());
}

} // namespace luxir
