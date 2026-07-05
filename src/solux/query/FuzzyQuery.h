#pragma once

#include <algorithm>
#include <cstring>
#include <format>
#include <limits>
#include <span>
#include <string_view>
#include <vector>

#include <boost/unordered/unordered_flat_map.hpp>

#include "BooleanQuery.h"
#include "MatchNoDocsQuery.h"
#include "MultiTermQuery.h"
#include "TermQuery.h"
#include "solux/reader/FuzzySeekEnum.h"
#include "solux/util/MemPool.h"

namespace solux {

// Fuzzy term query over byte-wise Levenshtein distance. Filter use cases take
// the constant-score MultiTermQuery path; scoring use cases blend expansion
// stats and rewrite to a prunable disjunction of damped TermQuery clauses.
class FuzzyQuery final : public MultiTermQuery {
  std::string_view term;
  int maxEdits;
  int prefixLength;   // clamped to <= term length
  int maxExpansions;  // 0 means complete matching; scored path also has a clause budget

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

  int scoringClauseLimit(Context& context) const {
    int userLimit = maxExpansions > 0 ? maxExpansions : std::numeric_limits<int>::max();
    int operatorLimit = context.limits.fuzzyMaxExpansions > 0
        ? context.limits.fuzzyMaxExpansions
        : std::numeric_limits<int>::max();
    return std::min({userLimit, operatorLimit, FUZZY_SCORING_CLAUSE_BUDGET});
  }

public:
  // HARDWARE SENSITIVE: interim scored-fuzzy clause cap. Laptop-tuned per the
  // tuning registry convention; re-validate on target desktop/cloud/ARM.
  static constexpr int FUZZY_SCORING_CLAUSE_BUDGET = 64;

  FuzzyQuery(std::string_view field, std::string_view term, int maxEdits,
             int prefixLength = 0, int maxExpansions = 0, float boost = 1.0f)
    : MultiTermQuery(field, boost), term(term), maxEdits(maxEdits),
      prefixLength(std::min(prefixLength, (int)term.size())), maxExpansions(maxExpansions) {}

  std::string_view getTerm() const { return term; }
  int getMaxEdits() const { return maxEdits; }
  int getPrefixLength() const { return prefixLength; }
  int getMaxExpansions() const { return maxExpansions; }

  // Filter path: MultiTermQuery builds the constant-score bitset from this.
  FilteredTermsEnum* createFilteredEnum(MemPool& pool, TermsEnum& te) override {
    std::string_view prefix = term.substr(0, prefixLength);
    std::string_view suffix = term.substr(prefixLength);
    return pool.make<FuzzySeekEnum>(pool, te, prefix, suffix, maxEdits);
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

  void truncateScoringCandidates(Context& context,
                                 std::vector<ExpansionCandidate>& candidates) const {
    int matched = (int)candidates.size();
    int userLimit = maxExpansions > 0 ? maxExpansions : std::numeric_limits<int>::max();
    int operatorLimit = context.limits.fuzzyMaxExpansions > 0
        ? context.limits.fuzzyMaxExpansions
        : std::numeric_limits<int>::max();
    int limit = scoringClauseLimit(context);

    if (matched > limit) {
      std::partial_sort(candidates.begin(), candidates.begin() + limit, candidates.end(),
                        betterExpansion);
      candidates.resize((size_t)limit);
    } else {
      std::sort(candidates.begin(), candidates.end(), betterExpansion);
    }

    if (matched > operatorLimit && operatorLimit < userLimit) {
      context.warn("fuzzy_clamped",
                   std::format("fuzzy expansion matched {} terms for field '{}' term '{}'; "
                               "operator limit {} kept closest terms",
                               matched, getField(), getTerm(), operatorLimit));
    }
    if (matched > FUZZY_SCORING_CLAUSE_BUDGET
        && userLimit >= FUZZY_SCORING_CLAUSE_BUDGET
        && operatorLimit >= FUZZY_SCORING_CLAUSE_BUDGET) {
      context.warn("fuzzy_scoring_truncated",
                   std::format("fuzzy expansion matched {} terms for field '{}' term '{}'; "
                               "scoring top {} by damp with term-order tie-break",
                               matched, getField(), getTerm(), FUZZY_SCORING_CLAUSE_BUDGET));
    }
  }

  // The scored rewrite shares one blended TermStats across every kept clause:
  // docFreq/ttf = max across the kept expanded terms (each already summed
  // across segments during collection). Per-term IDF would invert relevance for
  // on-by-default typo handling because misspellings are rare; blending makes
  // edit-distance damp the intended separator.
  Query* rewriteToScoringDisjunction(Context& context) const {
    CachedFieldInfo* cachedFieldInfo = context.getCachedFieldInfo(getField());
    if (cachedFieldInfo == nullptr) return context.pool.make<MatchNoDocsQuery>();

    MemPool scratch;
    auto candidates = collectCandidates(context, scratch, *cachedFieldInfo);
    truncateScoringCandidates(context, candidates);
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
  Query::Weight* createWeight(Context& context, int32_t flags) override {
    if ((flags & NEED_SCORES) == 0) {
      return MultiTermQuery::createWeight(context, flags);
    }
    return rewriteToScoringDisjunction(context)->createWeight(context, flags);
  }
};

} // namespace solux
