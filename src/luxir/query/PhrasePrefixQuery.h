// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

#include "Query.h"
#include "luxir/reader/FilteredTermsEnum.h"
#include "luxir/reader/TermsEnum.h"
#include "luxir/util/MemPool.h"

namespace luxir {

// A phrase whose last term is a prefix (Elasticsearch match_phrase_prefix,
// Lucene's MultiPhrasePrefixQuery). Weight creation expands the prefix over
// the whole index to the first maxExpansions terms in byte order and
// rewrites to a dis_max of one phrase per expansion, so a document scores as
// its best-matching completion. With no leading terms each expansion is a
// plain term query. The expansion set does not depend on scoring flags.
class PhrasePrefixQuery final : public Query {
  std::string_view field;
  std::span<std::string_view> terms;
  std::span<const int32_t> positions;
  std::string_view prefix;
  int32_t prefixPosition;
  int32_t slop;
  int32_t maxExpansions;

  static std::string_view copyTerm(MemPool& dst, std::string_view t) {
    if (t.empty()) return {};
    char* d = dst.alloc(t.size());
    memcpy(d, t.data(), t.size());
    return {d, t.size()};
  }

  // The first maxExpansions distinct terms in byte order across segments.
  // Each segment's prefix scan is sorted, so it never needs more than the cap.
  std::vector<std::string_view> expand(Context& context, MemPool& out) const {
    std::vector<std::string_view> found;
    CachedFieldInfo* info = context.getCachedFieldInfo(field);
    if (info == nullptr) return found;
    MemPool scratch;
    size_t nSegs = context.numSegments();
    for (size_t s = 0; s < nSegs; s++) {
      auto* segInfo = info->segInfos.at(s);
      if (segInfo == nullptr) continue;
      auto& postingsReader = context.topReader.segments()[s].postingsReader();
      TermsEnum te(scratch, postingsReader, *segInfo);
      PrefixTermsEnum pte(te, prefix);
      for (int32_t taken = 0; taken < maxExpansions && pte.next(); taken++) {
        found.push_back(copyTerm(out, pte.termView()));
      }
    }
    std::sort(found.begin(), found.end());
    found.erase(std::unique(found.begin(), found.end()), found.end());
    if (found.size() > (size_t)maxExpansions) found.resize((size_t)maxExpansions);
    return found;
  }

  Query* rewrite(Context& context) const;

public:
  static constexpr int32_t DEFAULT_MAX_EXPANSIONS = 50;

  // terms/positions are the leading phrase (positions normalized and
  // nondecreasing); the prefix sits at prefixPosition, at or after them.
  PhrasePrefixQuery(std::string_view field, std::span<std::string_view> terms,
                    std::span<const int32_t> positions, std::string_view prefix,
                    int32_t prefixPosition, int32_t slop, int32_t maxExpansions)
    : Query(QueryKind::PHRASE_PREFIX), field(field), terms(terms), positions(positions),
      prefix(prefix), prefixPosition(prefixPosition), slop(slop),
      maxExpansions(maxExpansions > 0 ? maxExpansions : DEFAULT_MAX_EXPANSIONS) {
    assert(terms.size() == positions.size());
    assert(slop >= 0);
  }

  bool recoverableAsPredicate() const noexcept override { return true; }

  std::string_view getField() const { return field; }
  std::string_view getPrefix() const { return prefix; }
  std::span<std::string_view> getTerms() const { return terms; }
  int32_t getMaxExpansions() const { return maxExpansions; }

  bool equalsSameKind(const Query& other) const override {
    const auto& rhs = static_cast<const PhrasePrefixQuery&>(other);
    return field == rhs.field && prefix == rhs.prefix
        && prefixPosition == rhs.prefixPosition && slop == rhs.slop
        && maxExpansions == rhs.maxExpansions
        && std::equal(terms.begin(), terms.end(), rhs.terms.begin(), rhs.terms.end())
        && std::equal(positions.begin(), positions.end(),
                      rhs.positions.begin(), rhs.positions.end());
  }

  uint64_t hashImpl() const override {
    uint64_t value = mixHash(mixHash(Query::hashImpl(), field), prefix);
    value = mixHash(mixHash(value, prefixPosition), slop);
    value = mixHash(value, maxExpansions);
    for (size_t i = 0; i < terms.size(); i++) {
      value = mixHash(mixHash(value, terms[i]), positions[i]);
    }
    return value;
  }

  void validateLogicalImpl(
      PlanningContext& context, float multiplier = 1.0f) const override {
    unused(context);
    checkedBoostProduct(multiplier, 1.0f);
  }

  // The expansion reads the whole index, like a fuzzy rewrite.
  FilterKeyScope appendFilterKey(FilterKeyBuilder& out,
                                 const FilterKeyContext& ctx) const override {
    unused(ctx);
    out.appendKind(kind);
    out.appendString(field);
    out.appendInt32((int32_t)terms.size());
    for (size_t i = 0; i < terms.size(); i++) {
      out.appendTerm(terms[i]);
      out.appendInt32(positions[i]);
    }
    out.appendTerm(prefix);
    out.appendInt32(prefixPosition);
    out.appendInt32(slop);
    out.appendInt32(maxExpansions);
    return FilterKeyScope::CORE_STABLE;
  }

  Query::Weight* createWeight(Context& context, int32_t flags,
                              float multiplier = 1.0f) override;
};

} // namespace luxir

#include "DisMaxQuery.h"
#include "MatchNoDocsQuery.h"
#include "PhraseQuery.h"
#include "TermQuery.h"

namespace luxir {

inline Query* PhrasePrefixQuery::rewrite(Context& context) const {
  MemPool& pool = context.pool;
  auto expansions = expand(context, pool);
  if (expansions.empty()) return pool.make<MatchNoDocsQuery>();
  auto clauses = pool.make_span<Query*>(expansions.size());
  size_t n = terms.size();
  for (size_t e = 0; e < expansions.size(); e++) {
    if (n == 0) {
      clauses[e] = pool.make<TermQuery>(field, expansions[e]);
      continue;
    }
    auto phraseTerms = pool.make_span<std::string_view>(n + 1);
    auto phrasePositions = pool.make_span<int32_t>(n + 1);
    for (size_t i = 0; i < n; i++) {
      phraseTerms[i] = terms[i];
      phrasePositions[i] = positions[i];
    }
    phraseTerms[n] = expansions[e];
    phrasePositions[n] = prefixPosition;
    clauses[e] = pool.make<PhraseQuery>(
        field, phraseTerms, std::span<const int32_t>(phrasePositions), slop);
  }
  return DisMaxQuery::make(pool, clauses, 0.0f);
}

inline Query::Weight* PhrasePrefixQuery::createWeight(
    Context& context, int32_t flags, float multiplier) {
  Query* rewritten = rewrite(context);
#ifndef NDEBUG
  if (context.logicalValidationActive()) {
    rewritten->validateLogical(context.planningContext(), multiplier);
  }
#endif
  return rewritten->createWeight(context, flags, multiplier);
}

} // namespace luxir
