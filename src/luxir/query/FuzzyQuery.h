// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <boost/unordered/unordered_flat_map.hpp>

#include "FittedTerms.h"
#include "MatchNoDocsQuery.h"
#include "Query.h"
#include "TermQuery.h"
#include "luxir/reader/FuzzySeekEnum.h"
#include "luxir/util/MemPool.h"

namespace luxir {

// How the expanded terms become the query that runs (Elasticsearch `rewrite`).
enum class FuzzyRewrite : uint8_t {
  // Disjunction of the closest terms sharing one blended TermStats (default).
  TOP_TERMS_BLENDED_FREQS,
  // Disjunction of the closest terms, each scored with its own stats.
  TOP_TERMS,
  // Disjunction of the closest terms, each a constant of boost * similarity.
  TOP_TERMS_BOOST,
  // Every term within the distance as one constant-score union: no closest-N
  // selection and no clause budget.
  CONSTANT_SCORE,
};

// Fuzzy term query over Levenshtein distance in code points, an adjacent
// transposition counting as one edit unless disabled (Damerau / optimal string
// alignment, as in Lucene and Elasticsearch). The top-terms rewrites keep a
// capped disjunction over the closest expanded terms (Lucene-style
// maxExpansions, default 50); CONSTANT_SCORE keeps every term. The match set is
// one property of the query - identical across scored, count-only, and filter
// use. Whether the rewritten clauses score is decided by the request's
// NEED_SCORES flag alone. A cap that drops terms the request did not leave to
// the implicit default warns (FUZZY_TRUNCATED_WARNING).
class FuzzyQuery final : public Query {
  std::string_view field;
  float boost;
  std::string_view term;
  int maxEdits;
  int prefixLength;   // code points, clamped to the term's
  size_t prefixBytes; // bytes of those prefixLength code points
  int maxExpansions;  // 0/unset means DEFAULT_MAX_EXPANSIONS; also clause-budget capped
  bool transpositions;
  FuzzyRewrite rewrite;
  int rewriteSize;    // top-terms modes: 0/unset means maxExpansions decides
  // The field's long-term policy when edits can reach a fitted (hashed or
  // truncated) long value, whose discarded bytes distance cannot see;
  // nullopt means no warning applies.
  std::optional<TermPolicy> fittedPolicy;

  struct ExpansionCandidate {
    std::string_view term;
    float damp = 1.0f;
    Similarity::TermStats termStats = {};
  };

  // What bounds the kept expansion count, and whether dropping terms there
  // is worth a warning (only the implicit default stays quiet, as in Lucene).
  struct ExpansionLimit {
    int limit;
    std::string_view reason;
    bool warn;
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
  // which docs can match. CONSTANT_SCORE keeps every term (operator limit
  // aside), as Lucene's constant-score rewrite does.
  ExpansionLimit expansionLimit(int32_t operatorExpansions) const {
    ExpansionLimit out{std::numeric_limits<int>::max(), {}, true};
    auto tighten = [&](int limit, std::string_view reason, bool warn) {
      if (limit < out.limit) out = {limit, reason, warn};
    };
    if (rewrite != FuzzyRewrite::CONSTANT_SCORE) {
      tighten(maxExpansions > 0 ? maxExpansions : DEFAULT_MAX_EXPANSIONS,
              "max_expansions", maxExpansions > 0);
      if (rewriteSize > 0) tighten(rewriteSize, "rewrite_size", true);
      tighten(FUZZY_CLAUSE_BUDGET, "the scoring clause budget", true);
    }
    if (operatorExpansions > 0) tighten(operatorExpansions, "the operator expansion limit", true);
    return out;
  }

public:
  bool recoverableAsPredicate() const noexcept override { return true; }

  // Default expansion cap when max_expansions is unset (Lucene parity).
  static constexpr int DEFAULT_MAX_EXPANSIONS = 50;

  // HARDWARE SENSITIVE: interim clause cap for the top-terms rewrites (applies
  // scored or not, so the match set stays uniform). Laptop-tuned per the
  // tuning registry convention; re-validate on target desktop/cloud/ARM.
  static constexpr int FUZZY_CLAUSE_BUDGET = 64;

  static constexpr std::string_view FUZZY_TRUNCATED_WARNING = "fuzzy_truncated";

  FuzzyQuery(std::string_view field, std::string_view term, int maxEdits,
             int prefixLength = 0, int maxExpansions = 0, float boost = 1.0f,
             std::optional<TermPolicy> fittedPolicy = std::nullopt,
             bool transpositions = true,
             FuzzyRewrite rewrite = FuzzyRewrite::TOP_TERMS_BLENDED_FREQS,
             int rewriteSize = 0)
    : Query(QueryKind::FUZZY), field(field), boost(boost), term(term),
      maxEdits(maxEdits),
      prefixLength(std::min(prefixLength, fuzzy_units::count(term))),
      prefixBytes(fuzzy_units::prefixBytes(term, prefixLength)),
      maxExpansions(maxExpansions), transpositions(transpositions), rewrite(rewrite),
      rewriteSize(rewriteSize), fittedPolicy(fittedPolicy) {}

  bool equalsSameKind(const Query& other) const override {
    const auto& rhs = static_cast<const FuzzyQuery&>(other);
    return field == rhs.field && term == rhs.term
        && maxEdits == rhs.maxEdits && prefixLength == rhs.prefixLength
        && maxExpansions == rhs.maxExpansions
        && transpositions == rhs.transpositions && rewrite == rhs.rewrite
        && rewriteSize == rhs.rewriteSize
        && std::bit_cast<uint32_t>(boost)
            == std::bit_cast<uint32_t>(rhs.boost);
  }

  uint64_t hashImpl() const override {
    uint64_t value = mixHash(Query::hashImpl(), field);
    value = mixHash(value, term);
    value = mixHash(value, maxEdits);
    value = mixHash(value, prefixLength);
    value = mixHash(value, maxExpansions);
    value = mixHash(value, (int)transpositions);
    value = mixHash(value, (int)rewrite);
    value = mixHash(value, rewriteSize);
    return mixHash(value, std::bit_cast<uint32_t>(boost));
  }

  std::string_view getField() const { return field; }
  float getBoost() const { return boost; }
  std::string_view getTerm() const { return term; }
  int getMaxEdits() const { return maxEdits; }
  int getPrefixLength() const { return prefixLength; }
  int getMaxExpansions() const { return maxExpansions; }
  bool getTranspositions() const { return transpositions; }
  FuzzyRewrite getRewrite() const { return rewrite; }
  int getRewriteSize() const { return rewriteSize; }

  void validateLogicalImpl(
      PlanningContext& context, float multiplier = 1.0f) const override {
    checkedBoostProduct(multiplier, boost);
    if (!fittedPolicy) return;
    warnIfFittedTermsReachable(context, field, *fittedPolicy, [] { return true; });
  }

  // Scoring-only choices (the top-terms flavors) stay out of the key; what
  // decides membership goes in.
  FilterKeyScope appendFilterKey(FilterKeyBuilder& out,
                                 const FilterKeyContext& ctx) const override {
    out.appendKind(kind);
    out.appendString(field);
    out.appendTerm(term);
    out.appendInt32(maxEdits);
    out.appendInt32(prefixLength);
    out.appendInt32(transpositions ? 1 : 0);
    out.appendInt32(rewrite == FuzzyRewrite::CONSTANT_SCORE ? 1 : 0);
    out.appendInt32(expansionLimit(ctx.fuzzyMaxExpansions).limit);
    return FilterKeyScope::CORE_STABLE;
  }

private:
  std::vector<ExpansionCandidate> collectCandidates(Context& context, MemPool& scratch,
                                                     CachedFieldInfo& fieldInfo) const {
    boost::unordered_flat_map<std::string_view, int32_t,
                              PackedTermHash, PackedTermEqual> termToIndex;
    std::vector<ExpansionCandidate> candidates;
    std::string_view prefix = term.substr(0, prefixBytes);
    std::string_view suffix = term.substr(prefixBytes);

    size_t nSegs = context.numSegments();
    for (size_t s = 0; s < nSegs; s++) {
      auto* segFieldInfo = fieldInfo.segInfos.at(s);
      if (segFieldInfo == nullptr) continue;
      auto& postingsReader = context.topReader.segments()[s].postingsReader();
      TermsEnum te(scratch, postingsReader, *segFieldInfo);
      FuzzySeekEnum fte(scratch, te, prefix, suffix, maxEdits, false, transpositions);
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

  // Keeps the closest terms the limit allows, best first.
  void truncateCandidates(Context& context,
                          std::vector<ExpansionCandidate>& candidates) const;

  // TOP_TERMS_BLENDED_FREQS shares one blended TermStats across every kept
  // clause: docFreq/ttf = max across the kept expanded terms (each already
  // summed across segments during collection). Per-term IDF would invert
  // relevance for on-by-default typo handling because misspellings are rare;
  // blending makes edit-distance damp the intended separator. TOP_TERMS opts
  // into exactly that per-term IDF. (Stats and damped boosts only matter when
  // the request needs scores; the clause set is the same either way.)
  Query* rewriteToDisjunction(Context& context) const;

public:
  // The expansion set is selected here, independent of `flags`: NEED_SCORES
  // decides only whether the kept clauses score, never which docs match.
  Query::Weight* createWeight(Context& context, int32_t flags,
                              float multiplier = 1.0f) override;
};

} // namespace luxir
