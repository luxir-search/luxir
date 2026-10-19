// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// createWeight (createFilteredEnum for MultiTermQuery subclasses) for the
// queries without a .cpp of their own. Each is its class's key function; see
// docs/dev/codebase-map.md.

#include "luxir/query/AllQuery.h"
#include "luxir/query/AutomatonQuery.h"
#include "luxir/query/BooleanQuery.h"
#include "luxir/query/BoostQuery.h"
#include "luxir/query/ConstantScoreQuery.h"
#include "luxir/query/ExistsQuery.h"
#include "luxir/query/ForcePrepareQuery.h"
#include "luxir/query/FuzzyQuery.h"
#include "luxir/query/GeoBoxQuery.h"
#include "luxir/query/GeoDistanceQuery.h"
#include "luxir/query/MatchNoDocsQuery.h"
#include "luxir/query/MinScoreQuery.h"
#include "luxir/query/MultiTermQuery.h"
#include "luxir/query/NumericPredicateQuery.h"
#include "luxir/query/PhraseQuery.h"
#include "luxir/query/PrefixQuery.h"
#include "luxir/query/RescoreQuery.h"
#include "luxir/query/TermInSetQuery.h"
#include "luxir/query/TermQuery.h"
#include "luxir/query/TermRangeQuery.h"

namespace luxir {

AllQuery::Weight* AllQuery::createWeight(Context& context, int32_t flags,
                                         float multiplier) {
  AllQuery::Weight* weight = context.pool.make<AllQuery::Weight>(
      context, *this, flags, constantWhenScored(flags, multiplier));
  return weight;
}

FilteredTermsEnum* AutomatonQuery::createFilteredEnum(MemPool& pool, TermsEnum& te) {
  switch (classification) {
    case automaton::ByteDfaKind::SINGLE:
      return pool.make<ExactTermTermsEnum>(te, exactOrPrefix);
    case automaton::ByteDfaKind::PREFIX:
    case automaton::ByteDfaKind::ALL:
      return pool.make<PrefixTermsEnum>(te, exactOrPrefix);
    case automaton::ByteDfaKind::NORMAL:
      return pool.make<DfaIntersectEnum>(pool, te, dfa, scanPlan);
    case automaton::ByteDfaKind::NONE:
      assert(false);
  }
  return nullptr;
}

Query::Weight* BoostQuery::createWeight(Context& context, int32_t flags,
                                        float multiplier) {
  return child->createWeight(context, flags, multiplier * boost);
}

Query::Weight* ConstantScoreQuery::createWeight(Context& context, int32_t flags,
                                                float multiplier) {
  return context.pool.make<ConstantScoreQuery::Weight>(context, *this, flags,
                                                       multiplier);
}

ExistsQuery::Weight* ExistsQuery::createWeight(
    Context& context, int32_t flags, float multiplier) {
  return context.pool.make<ExistsQuery::Weight>(
      context, *this, flags, Query::constantWhenScored(flags, multiplier));
}

Query::Weight* ForcePrepareQuery::createWeight(Context& context, int32_t flags,
                                               float multiplier) {
  return context.pool.make<ForcePrepareQuery::Weight>(context, *this, flags,
                                                      multiplier);
}

Query* FuzzyQuery::rewriteToDisjunction(Context& context) const {
  CachedFieldInfo* cachedFieldInfo = context.getCachedFieldInfo(getField());
  if (cachedFieldInfo == nullptr) return context.pool.make<MatchNoDocsQuery>();

  MemPool scratch;
  auto candidates = collectCandidates(context, scratch, *cachedFieldInfo);
  truncateCandidates(context, candidates);
  if (candidates.empty()) return context.pool.make<MatchNoDocsQuery>();

  Similarity::TermStats blendedStats = {};
  for (const ExpansionCandidate& candidate : candidates) {
    blendedStats.docFreq = std::max(
        blendedStats.docFreq, candidate.termStats.docFreq);
    blendedStats.totalTermFreq = std::max(
        blendedStats.totalTermFreq, candidate.termStats.totalTermFreq);
  }

  auto* clauses = context.pool.make_arr<Query*>(candidates.size());
  for (size_t i = 0; i < candidates.size(); i++) {
    const ExpansionCandidate& candidate = candidates[i];
    std::string_view termCopy = copyTerm(context.pool, candidate.term);
    clauses[i] = context.pool.make<TermQuery>(
        getField(), termCopy, blendedStats, getBoost() * candidate.damp);
  }

  return context.pool.make<BooleanQuery>(
      std::span<Query*>{}, std::span<Query*>(clauses, candidates.size()),
      std::span<Query*>{}, std::span<Query*>{});
}

Query::Weight* FuzzyQuery::createWeight(
    Context& context, int32_t flags, float multiplier) {
  Query* rewritten = rewriteToDisjunction(context);
#ifndef NDEBUG
  if (context.logicalValidationActive()) {
    rewritten->validateLogical(context.planningContext(), multiplier);
  }
#endif
  return rewritten->createWeight(context, flags, multiplier);
}

Query::Weight* GeoBoxQuery::createWeight(Context& context, int32_t flags,
                                         float multiplier) {
  float score = constantWhenScored(flags, multiplier);
  return context.pool.make<Weight>(context, *this, flags, score);
}

Query::Weight* GeoDistanceQuery::createWeight(Context& context, int32_t flags,
                                              float multiplier) {
  float score = constantWhenScored(flags, multiplier);
  return context.pool.make<Weight>(context, *this, flags, score);
}

Query::Weight* MatchNoDocsQuery::createWeight(Context& context, int32_t flags,
                                              float multiplier) {
  unused(multiplier);
  return context.pool.make<MatchNoDocsQuery::Weight>(context, *this, flags);
}

Query::Weight* MinScoreQuery::createWeight(Context& context, int32_t flags,
                                           float multiplier) {
  return context.pool.make<MinScoreQuery::Weight>(context, *this, flags,
                                                  multiplier);
}

Query::Weight* MultiTermQuery::createWeight(Context& context, int32_t flags,
                                            float multiplier) {
  return context.pool.make<MultiTermQuery::Weight>(context, *this, flags, multiplier);
}

Query::Weight* NumericPredicateQuery::createWeight(Context& context, int32_t flags,
                                                   float multiplier) {
  float score = constantWhenScored(flags, multiplier);
  return context.pool.make<Weight>(context, *this, flags, score);
}

Query::Weight* PhraseQuery::createWeight(Context& context, int32_t flags,
                                         float multiplier) {
  return context.pool.make<PhraseQuery::Weight>(context, *this, flags, multiplier);
}

FilteredTermsEnum* PrefixQuery::createFilteredEnum(MemPool& pool, TermsEnum& te) {
  return pool.make<PrefixTermsEnum>(te, prefix);
}

Query::Weight* RescoreQuery::createWeight(
    Context& context, int32_t flags, float multiplier) {
  return context.pool.make<RescoreQuery::Weight>(
      context, *this, flags, multiplier);
}

FilteredTermsEnum* TermInSetQuery::createFilteredEnum(MemPool& pool,
                                                      TermsEnum& te) {
  return pool.make<ExplicitTermsEnum>(te, terms);
}

Query::Weight* TermQuery::createWeight(Context& context, int32_t flags,
                                       float multiplier) {
  return context.pool.make<TermQuery::Weight>(context, *this, flags, multiplier);
}

BulkScorer* TermQuery::Weight::Supplier::buildBulk(
    MemPool& targetPool, const BulkPlan& plan) {
  using DirectTermScorer = TermQuery::Scorer;
  assert(plan.available == BulkAnswer::YES);
  assert(!plan.hasConstantCount());
  auto* scorer = dynamic_cast<DirectTermScorer*>(
      weight.createScorer(targetPool, segment));
  if (scorer == nullptr) {
    return nullptr;
  }
  return targetPool.make<TermQuery::TermBulkScorer>(targetPool, scorer, segment.maxDoc());
}

FilteredTermsEnum* TermRangeQuery::createFilteredEnum(MemPool& pool, TermsEnum& te) {
  return pool.make<RangeTermsEnum>(te, lower, includeLower, upper, includeUpper);
}

} // namespace luxir
