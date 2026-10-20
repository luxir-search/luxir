// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <string_view>
#include <utility>

#include "FilteredTermsEnum.h"
#include "LevenshteinAutomaton.h"
#include "luxir/util/MemPool.h"

namespace luxir {

// Brute-force fuzzy term scan over a required exact prefix plus a full-matrix
// Levenshtein (optionally optimal string alignment) match on the remaining
// suffix, both measured in code-point units (fuzzy_units). The reference the
// automaton-driven FuzzySeekEnum is tested against.
class FuzzyTermsEnum final : public ScanTermsEnum {
  std::string_view prefix;       // non-fuzzy leading bytes that must match exactly
  uint32_t* query;               // suffix units (the fuzzy part)
  uint32_t* term;                // the current term's suffix units
  int maxEdits;
  int n;                         // query suffix units
  int prefixUnits;
  bool prefixMode;
  bool transpositions;
  // Three reused rows of the edit-distance matrix, sized n+1, pool-owned.
  int* prev2;
  int* prev;
  int* cur;
  float score = 1.0f;            // similarity of the last accepted term
  int bestPrefixDist = 0;

  // Bounded distance between the query suffix and t[0..m).
  int editDistance(const uint32_t* t, int m) {
    bestPrefixDist = maxEdits + 1;
    if (!prefixMode && (m - n > maxEdits || n - m > maxEdits)) return maxEdits + 1;  // length filter

    for (int i = 0; i <= n; i++) prev[i] = i;
    if (prefixMode) bestPrefixDist = std::min(maxEdits + 1, prev[n]);
    const uint32_t* q = query;
    int* p2 = prev2;
    int* p = prev;
    int* d = cur;
    for (int j = 1; j <= m; j++) {
      const uint32_t tj = t[j - 1];
      d[0] = j;
      int best = j;  // cell(0,j); included so the column-min bound stays valid
      for (int i = 1; i <= n; i++) {
        int sub = p[i - 1] + (tj == q[i - 1] ? 0 : 1);
        int v = std::min({sub, p[i] + 1, d[i - 1] + 1});
        if (transpositions && i > 1 && j > 1 && tj == q[i - 2] && t[j - 2] == q[i - 1]) {
          v = std::min(v, p2[i - 2] + 1);
        }
        d[i] = v;
        best = std::min(best, v);
      }
      if (prefixMode) bestPrefixDist = std::min(bestPrefixDist, std::min(maxEdits + 1, d[n]));
      if (best > maxEdits) {
        return prefixMode && bestPrefixDist <= maxEdits ? bestPrefixDist : maxEdits + 1;
      }
      int* oldest = p2;
      p2 = p;
      p = d;
      d = oldest;
    }
    return prefixMode ? bestPrefixDist : p[n];  // p holds the last computed row
  }

protected:
  bool seekStart() override { return te.seekCeil(prefix); }

  Status accept() override {
    std::string_view t = termView();
    if (!t.starts_with(prefix)) return Status::END;  // past the non-fuzzy prefix run
    int m = fuzzy_units::decode(t.substr(prefix.size()), term);
    int dist = editDistance(term, m);
    if (dist > maxEdits) return Status::REJECT;
    // Closer terms score higher; one edit matters more on shorter terms.
    int denom = prefixMode ? prefixUnits + n : prefixUnits + std::min(n, m);
    score = denom > 0 ? 1.0f - (float)dist / (float)denom : 1.0f;
    return Status::ACCEPT;
  }

public:
  // `prefix` is exact; `suffix` is fuzzy. `prefix` must outlive this enum.
  FuzzyTermsEnum(MemPool& pool, TermsEnum& te, std::string_view prefix,
                 std::string_view suffix, int maxEdits, bool prefixMode = false,
                 bool transpositions = true)
    : ScanTermsEnum(te), prefix(prefix), maxEdits(maxEdits),
      prefixUnits(fuzzy_units::count(prefix)), prefixMode(prefixMode),
      transpositions(transpositions) {
    query = (uint32_t*)pool.alloc((suffix.size() + 1) * sizeof(uint32_t), alignof(uint32_t));
    n = fuzzy_units::decode(suffix, query);
    term = (uint32_t*)pool.alloc((PackedTerm::MAX_LEN + 1) * sizeof(uint32_t),
                                 alignof(uint32_t));
    prev2 = (int*)pool.alloc((n + 1) * sizeof(int), alignof(int));
    prev = (int*)pool.alloc((n + 1) * sizeof(int), alignof(int));
    cur = (int*)pool.alloc((n + 1) * sizeof(int), alignof(int));
  }

  float currentScore() const override { return score; }
};

} // namespace luxir
