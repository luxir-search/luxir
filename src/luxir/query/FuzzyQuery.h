#pragma once

#include <algorithm>
#include <cstring>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

#include <boost/unordered/unordered_flat_map.hpp>

#include "BooleanQuery.h"
#include "MatchNoDocsQuery.h"
#include "Query.h"
#include "TermQuery.h"
#include "luxir/reader/FuzzySeekEnum.h"
#include "luxir/util/MemPool.h"

namespace luxir {

// Fuzzy term query over byte-wise Levenshtein distance. Always rewrites to a
// capped disjunction over the closest expanded terms (Lucene-style
// maxExpansions, default 50), so the match set is one property of the query -
// identical across scored, count-only, and filter use. Whether the rewritten
// clauses score (blended stats, damped TermQuery boosts) is decided by the
// request's NEED_SCORES flag alone.
class FuzzyQuery final : public Query {
  std::string_view field;
  float boost;
  std::string_view term;
  int maxEdits;
  int prefixLength;   // clamped to <= term length
  int maxExpansions;  // 0/unset means DEFAULT_MAX_EXPANSIONS; also clause-budget capped

  struct ExpansionCandidate {
    std::string_view term;
    float damp = 1.0f;
    Similarity::TermStats termStats = {};
  };

  // Copy transient term bytes (FuzzySeekEnum reuses its buffer across terms)
  // into `dst` so a string_view can outlive the enum's next advance.
  static std::string_view copyTerm(MemPool& dst, std::string_view t) {
    if (t.empty()) return {};
    char* d = dst.alloc(t.size());
    memcpy(d, t.data(), t.size());
    return {d, t.size()};
  }

  static bool betterExpansion(const ExpansionCandidate& a, const ExpansionCandidate& b) {
    if (a.damp != b.damp) return a.damp > b.damp;
    return a.term < b.term;
  }

  // Explicit maxExpansions pins the cap; unset adopts the Lucene-compatible
  // default rather than complete matching, so an expansion set that outgrows
  // the corpus never silently changes query cost or (via the clause budget)
  // which docs can match.
  int userExpansionLimit() const {
    return maxExpansions > 0 ? maxExpansions : DEFAULT_MAX_EXPANSIONS;
  }

  int clauseLimit(Context& context) const {
    int operatorLimit = context.limits.fuzzyMaxExpansions > 0
        ? context.limits.fuzzyMaxExpansions
        : std::numeric_limits<int>::max();
    return std::min({userExpansionLimit(), operatorLimit, FUZZY_CLAUSE_BUDGET});
  }

public:
  // Default expansion cap when max_expansions is unset (Lucene parity).
  static constexpr int DEFAULT_MAX_EXPANSIONS = 50;

  // HARDWARE SENSITIVE: interim clause cap (applies scored or not, so the
  // match set stays uniform). Laptop-tuned per the tuning registry convention;
  // re-validate on target desktop/cloud/ARM.
  static constexpr int FUZZY_CLAUSE_BUDGET = 64;

  FuzzyQuery(std::string_view field, std::string_view term, int maxEdits,
             int prefixLength = 0, int maxExpansions = 0, float boost = 1.0f)
    : field(field), boost(boost), term(term), maxEdits(maxEdits),
      prefixLength(std::min(prefixLength, (int)term.size())), maxExpansions(maxExpansions) {}

  std::string_view getField() const { return field; }
  float getBoost() const { return boost; }
  std::string_view getTerm() const { return term; }
  int getMaxEdits() const { return maxEdits; }
  int getPrefixLength() const { return prefixLength; }
  int getMaxExpansions() const { return maxExpansions; }

  bool canOmitWeightForCacheFirstMembership() const override { return true; }

  void validateLogicalImpl(
      PlanningContext& context, float multiplier = 1.0f) const override {
    unused(context);
    checkedBoostProduct(multiplier, boost);
  }

  FilterKeyScope appendFilterKey(FilterKeyBuilder& out,
                                 const FilterKeyContext& ctx) const override {
    out.appendTag(FilterKeyTag::FUZZY);
    out.appendString(field);
    out.appendTerm(term);
    out.appendInt32(maxEdits);
    out.appendInt32(prefixLength);
    int operatorLimit = ctx.fuzzyMaxExpansions > 0
        ? ctx.fuzzyMaxExpansions
        : std::numeric_limits<int>::max();
    int effectiveLimit = std::min({userExpansionLimit(), operatorLimit,
                                   FUZZY_CLAUSE_BUDGET});
    out.appendInt32(effectiveLimit);
    return FilterKeyScope::CORE_STABLE;
  }

private:
  std::vector<ExpansionCandidate> collectCandidates(Context& context, MemPool& scratch,
                                                     CachedFieldInfo& fieldInfo) const {
    boost::unordered_flat_map<std::string_view, int32_t,
                              PackedTermHash, PackedTermEqual> termToIndex;
    std::vector<ExpansionCandidate> candidates;
    std::string_view prefix = term.substr(0, prefixLength);
    std::string_view suffix = term.substr(prefixLength);

    size_t nSegs = context.numSegments();
    for (size_t s = 0; s < nSegs; s++) {
      auto* segFieldInfo = fieldInfo.segInfos[s];
      if (segFieldInfo == nullptr) continue;
      auto& postingsReader = context.topReader.segments()[s].postingsReader();
      TermsEnum te(scratch, postingsReader, *segFieldInfo);
      FuzzySeekEnum fte(scratch, te, prefix, suffix, maxEdits);
      while (fte.next()) {
        std::string_view t = fte.termView();
        auto iter = termToIndex.find(t);
        if (iter == termToIndex.end()) {
          std::string_view termCopy = copyTerm(scratch, t);
          int32_t idx = (int32_t)candidates.size();
          termToIndex.emplace(termCopy, idx);
          candidates.push_back({termCopy, fte.currentScore(), {}});
          iter = termToIndex.find(termCopy);
        }
        ExpansionCandidate& candidate = candidates[(size_t)iter->second];
        candidate.termStats.docFreq += te.docFreq();
        candidate.termStats.totalTermFreq += te.totalTermFreq();
      }
    }
    return candidates;
  }

  void truncateCandidates(Context& context,
                          std::vector<ExpansionCandidate>& candidates) const {
    int matched = (int)candidates.size();
    int limit = clauseLimit(context);

    if (matched > limit) {
      std::partial_sort(candidates.begin(), candidates.begin() + limit, candidates.end(),
                        betterExpansion);
      candidates.resize((size_t)limit);
    } else {
      std::sort(candidates.begin(), candidates.end(), betterExpansion);
    }
  }

  // The rewrite shares one blended TermStats across every kept clause:
  // docFreq/ttf = max across the kept expanded terms (each already summed
  // across segments during collection). Per-term IDF would invert relevance for
  // on-by-default typo handling because misspellings are rare; blending makes
  // edit-distance damp the intended separator. (Stats and damped boosts only
  // matter when the request needs scores; the clause set is the same either way.)
  Query* rewriteToDisjunction(Context& context) const {
    CachedFieldInfo* cachedFieldInfo = context.getCachedFieldInfo(getField());
    if (cachedFieldInfo == nullptr) return context.pool.make<MatchNoDocsQuery>();

    MemPool scratch;
    auto candidates = collectCandidates(context, scratch, *cachedFieldInfo);
    truncateCandidates(context, candidates);
    if (candidates.empty()) return context.pool.make<MatchNoDocsQuery>();

    Similarity::TermStats blendedStats = {};
    for (const ExpansionCandidate& candidate : candidates) {
      blendedStats.docFreq = std::max(blendedStats.docFreq, candidate.termStats.docFreq);
      blendedStats.totalTermFreq = std::max(blendedStats.totalTermFreq,
                                            candidate.termStats.totalTermFreq);
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

public:
  // The expansion set is selected here, independent of `flags`: NEED_SCORES
  // decides only whether the kept clauses score, never which docs match.
  Query::Weight* createWeight(Context& context, int32_t flags,
                              float multiplier = 1.0f) override {
    Query* rewritten = rewriteToDisjunction(context);
#ifndef NDEBUG
    if (context.logicalValidationActive()) {
      rewritten->validateLogical(context.planningContext(), multiplier);
    }
#endif
    return rewritten->createWeight(context, flags, multiplier);
  }
};

} // namespace luxir
