// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// createWeight (createFilteredEnum for MultiTermQuery subclasses) for the
// queries without a .cpp of their own. Each is its class's key function; see
// docs/dev/codebase-map.md.

#include <format>

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
#include "luxir/query/PhrasePrefixQuery.h"
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

void FuzzyQuery::truncateCandidates(
    Context& context, std::vector<ExpansionCandidate>& candidates) const {
  int matched = (int)candidates.size();
  ExpansionLimit limit = expansionLimit(context.limits.fuzzyMaxExpansions);
  if (matched <= limit.limit) {
    std::sort(candidates.begin(), candidates.end(), betterExpansion);
    return;
  }
  std::partial_sort(candidates.begin(), candidates.begin() + limit.limit, candidates.end(),
                    betterExpansion);
  candidates.resize((size_t)limit.limit);
  if (limit.warn) {
    context.warn(FUZZY_TRUNCATED_WARNING, std::format(
        "fuzzy query on field {} matched {} terms; only the {} closest were kept ({})",
        field, matched, limit.limit, limit.reason));
  }
}

Query* FuzzyQuery::rewriteToDisjunction(Context& context) const {
  CachedFieldInfo* cachedFieldInfo = context.getCachedFieldInfo(getField());
  if (cachedFieldInfo == nullptr) return context.pool.make<MatchNoDocsQuery>();

  MemPool scratch;
  auto candidates = collectCandidates(context, scratch, *cachedFieldInfo);
  truncateCandidates(context, candidates);
  if (candidates.empty()) return context.pool.make<MatchNoDocsQuery>();

  if (rewrite == FuzzyRewrite::CONSTANT_SCORE) {
    std::sort(candidates.begin(), candidates.end(),
              [](const ExpansionCandidate& a, const ExpansionCandidate& b) {
                return a.term < b.term;
              });
    auto terms = context.pool.make_span<std::string_view>(candidates.size());
    for (size_t i = 0; i < candidates.size(); i++) {
      terms[i] = copyTerm(context.pool, candidates[i].term);
    }
    return context.pool.make<ConstantScoreQuery>(
        context.pool.make<TermInSetQuery>(getField(), terms), getBoost());
  }

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
    float clauseBoost = getBoost() * candidate.damp;
    switch (rewrite) {
      case FuzzyRewrite::TOP_TERMS_BLENDED_FREQS:
        clauses[i] = context.pool.make<TermQuery>(
            getField(), termCopy, blendedStats, clauseBoost);
        break;
      case FuzzyRewrite::TOP_TERMS:
        clauses[i] = context.pool.make<TermQuery>(
            getField(), termCopy, candidate.termStats, clauseBoost);
        break;
      case FuzzyRewrite::TOP_TERMS_BOOST:
      case FuzzyRewrite::CONSTANT_SCORE:
        clauses[i] = context.pool.make<ConstantScoreQuery>(
            context.pool.make<TermQuery>(getField(), termCopy), clauseBoost);
        break;
    }
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

std::vector<PhrasePrefixQuery::Expansion> PhrasePrefixQuery::expand(
    Context& context, MemPool& pool) const {
  std::vector<Expansion> expansions;
  CachedFieldInfo* fieldInfo = context.getCachedFieldInfo(field);
  if (fieldInfo == nullptr) return expansions;

  // Each segment's dictionary is sorted, so only its first maxExpansions
  // matches can be among the merged first maxExpansions.
  MemPool scratch;
  std::vector<Expansion> found;
  for (size_t s = 0; s < context.numSegments(); s++) {
    auto* segFieldInfo = fieldInfo->segInfos.at(s);
    if (segFieldInfo == nullptr) continue;
    auto& postingsReader = context.topReader.segments()[s].postingsReader();
    TermsEnum te(scratch, postingsReader, *segFieldInfo);
    PrefixTermsEnum prefixEnum(te, prefix);
    for (int32_t n = 0; n < maxExpansions && prefixEnum.next(); n++) {
      std::string_view term = prefixEnum.termView();
      char* bytes = scratch.alloc(term.size());
      std::copy(term.begin(), term.end(), bytes);
      found.push_back({{bytes, term.size()},
                       {(int64_t) te.docFreq(), (int64_t) te.totalTermFreq()}});
    }
  }

  std::sort(found.begin(), found.end(),
            [](const Expansion& a, const Expansion& b) { return a.term < b.term; });
  for (const Expansion& expansion : found) {
    if (!expansions.empty() && expansions.back().term == expansion.term) {
      expansions.back().termStats.add(expansion.termStats);
      continue;
    }
    if ((int32_t) expansions.size() == maxExpansions) break;
    char* bytes = pool.alloc(expansion.term.size());
    std::copy(expansion.term.begin(), expansion.term.end(), bytes);
    expansions.push_back({{bytes, expansion.term.size()}, expansion.termStats});
  }
  return expansions;
}

Query* PhrasePrefixQuery::rewrite(Context& context) const {
  std::vector<Expansion> expansions = expand(context, context.pool);
  if (expansions.empty()) return context.pool.make<MatchNoDocsQuery>();

  if (terms.empty()) {
    // A lone prefix: the expansions share one slot's stats, as they would
    // as the last slot of a phrase.
    Similarity::TermStats slotStats = {};
    for (const Expansion& expansion : expansions) slotStats.add(expansion.termStats);
    CachedFieldInfo* fieldInfo = context.getCachedFieldInfo(field);
    slotStats.docFreq = std::min(slotStats.docFreq, fieldInfo->fieldStats.docsWithField);
    auto clauses = context.pool.make_span<Query*>(expansions.size());
    for (size_t i = 0; i < expansions.size(); i++) {
      clauses[i] = context.pool.make<TermQuery>(field, expansions[i].term, slotStats);
    }
    return context.pool.make<BooleanQuery>(
        std::span<Query*>{}, clauses, std::span<Query*>{}, std::span<Query*>{});
  }

  auto slots = context.pool.make_span<std::span<std::string_view>>(terms.size() + 1);
  for (size_t i = 0; i < terms.size(); i++) slots[i] = terms.subspan(i, 1);
  auto last = context.pool.make_span<std::string_view>(expansions.size());
  for (size_t i = 0; i < expansions.size(); i++) last[i] = expansions[i].term;
  slots[terms.size()] = last;
  return context.pool.make<PhraseQuery>(context.pool, field, slots, positions, slop);
}

Query::Weight* PhrasePrefixQuery::createWeight(
    Context& context, int32_t flags, float multiplier) {
  Query* rewritten = rewrite(context);
#ifndef NDEBUG
  if (context.logicalValidationActive()) {
    rewritten->validateLogical(context.planningContext(), multiplier);
  }
#endif
  return rewritten->createWeight(context, flags, multiplier);
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
