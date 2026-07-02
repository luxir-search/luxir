#pragma once

#include <algorithm>
#include <cstring>
#include <string_view>
#include <utility>
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
// the constant-score MultiTermQuery path; scoring use cases rewrite to boosted
// TermQuery clauses capped by maxExpansions.
class FuzzyQuery final : public MultiTermQuery {
  std::string_view term;
  int maxEdits;
  int prefixLength;   // clamped to <= term length
  int maxExpansions;

  // Copy transient term bytes (FuzzySeekEnum reuses its buffer across terms)
  // into `dst` so a string_view can outlive the enum's next advance.
  static std::string_view copyTerm(MemPool& dst, std::string_view t) {
    if (t.empty()) return {};
    char* d = dst.alloc(t.size());
    memcpy(d, t.data(), t.size());
    return {d, t.size()};
  }

  // Build a scoring disjunction of the closest matched terms.
  Query* rewriteToBooleanDisjunction(Context& context) {
    auto* cachedFieldInfo = context.getCachedFieldInfo(field);
    if (cachedFieldInfo == nullptr) return nullptr;  // field absent everywhere
    MemPool& pool = context.pool;

    std::string_view prefix = term.substr(0, prefixLength);
    std::string_view suffix = term.substr(prefixLength);

    // Keep broad fuzzy scans out of the request pool until the cap is applied.
    MemPool scratch;
    // Distinct matched terms -> edit-distance boost.
    boost::unordered_flat_map<std::string_view, float> termBoosts;
    size_t nSegs = context.numSegments();
    for (size_t s = 0; s < nSegs; s++) {
      auto* segFieldInfo = cachedFieldInfo->segInfos[s];
      if (segFieldInfo == nullptr) continue;
      auto& postingsReader = context.topReader.segments()[s].postingsReader();
      TermsEnum te(scratch, postingsReader, *segFieldInfo);
      FuzzySeekEnum fte(scratch, te, prefix, suffix, maxEdits);
      while (fte.next()) {
        std::string_view t = fte.termView();
        if (!termBoosts.contains(t)) {
          termBoosts.emplace(copyTerm(scratch, t), fte.currentScore());
        }
      }
    }
    if (termBoosts.empty()) return nullptr;

    // Keep the closest terms.
    std::vector<std::pair<std::string_view, float>> kept(termBoosts.begin(), termBoosts.end());
    if ((int)kept.size() > maxExpansions) {
      std::partial_sort(kept.begin(), kept.begin() + maxExpansions, kept.end(),
                        [](const auto& a, const auto& b) { return a.second > b.second; });
      kept.resize(maxExpansions);
    }

    // Copy survivors into the request pool for the rewritten clauses.
    auto clauses = pool.make_span<Query*>(kept.size());
    for (size_t i = 0; i < kept.size(); i++) {
      std::string_view termCopy = copyTerm(pool, kept[i].first);
      clauses[i] = pool.make<TermQuery>(field, termCopy, getBoost() * kept[i].second);
    }
    std::span<Query*> none{};
    return pool.make<BooleanQuery>(none, clauses, none, none);  // optional-only = scoring disjunction
  }

public:
  FuzzyQuery(std::string_view field, std::string_view term, int maxEdits,
             int prefixLength = 0, int maxExpansions = 50, float boost = 1.0f)
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

  Query::Weight* createWeight(Context& context, int32_t flags) override {
    if ((flags & NEED_SCORES) == 0) {
      return MultiTermQuery::createWeight(context, flags);
    }
    Query* rewritten = rewriteToBooleanDisjunction(context);
    if (rewritten == nullptr) {
      rewritten = context.pool.make<MatchNoDocsQuery>();
    }
    return rewritten->createWeight(context, flags);
  }
};

} // namespace solux
