#pragma once

#include <algorithm>
#include <string_view>
#include <utility>

#include "FilteredTermsEnum.h"
#include "solux/util/MemPool.h"

namespace solux {

// Brute-force fuzzy term scan over a required exact prefix plus a byte-wise
// Levenshtein match on the remaining suffix.
class FuzzyTermsEnum final : public FilteredTermsEnum {
  std::string_view prefix;       // non-fuzzy leading bytes that must match exactly
  std::string_view suffix;       // query bytes after the prefix (the fuzzy part)
  int maxEdits;
  int n;                         // suffix.size()
  bool prefixMode;
  // Two reused rows of the edit-distance matrix, sized n+1, pool-owned.
  int* prev;
  int* cur;
  float score = 1.0f;            // similarity of the last accepted term
  int bestPrefixDist = 0;

  // Bounded Levenshtein distance between `suffix` and `t`.
  int editDistance(std::string_view t) {
    const int m = (int)t.size();
    bestPrefixDist = maxEdits + 1;
    if (!prefixMode && (m - n > maxEdits || n - m > maxEdits)) return maxEdits + 1;  // length filter

    for (int i = 0; i <= n; i++) prev[i] = i;
    if (prefixMode) bestPrefixDist = std::min(maxEdits + 1, prev[n]);
    const char* q = suffix.data();
    int* p = prev;
    int* d = cur;
    for (int j = 1; j <= m; j++) {
      const char tj = t[j - 1];
      d[0] = j;
      int best = j;  // cell(0,j); included so the column-min bound stays valid
      for (int i = 1; i <= n; i++) {
        int sub = p[i - 1] + (tj == q[i - 1] ? 0 : 1);
        int v = std::min({sub, p[i] + 1, d[i - 1] + 1});
        d[i] = v;
        best = std::min(best, v);
      }
      if (prefixMode) bestPrefixDist = std::min(bestPrefixDist, std::min(maxEdits + 1, d[n]));
      if (best > maxEdits) {
        return prefixMode && bestPrefixDist <= maxEdits ? bestPrefixDist : maxEdits + 1;
      }
      std::swap(p, d);
    }
    return prefixMode ? bestPrefixDist : p[n];  // p holds the last computed row after the final swap
  }

protected:
  bool seekStart() override { return te.seekCeil(prefix); }

  Status accept() override {
    std::string_view t = termView();
    if (!t.starts_with(prefix)) return Status::END;  // past the non-fuzzy prefix run
    std::string_view tsuffix = t.substr(prefix.size());
    int dist = editDistance(tsuffix);
    if (dist > maxEdits) return Status::REJECT;
    // Closer terms score higher; one edit matters more on shorter terms.
    int denom = prefixMode ? (int)prefix.size() + n
                           : (int)prefix.size() + std::min(n, (int)tsuffix.size());
    score = denom > 0 ? 1.0f - (float)dist / (float)denom : 1.0f;
    return Status::ACCEPT;
  }

public:
  // `prefix` is exact; `suffix` is fuzzy. Both views must outlive this enum.
  FuzzyTermsEnum(MemPool& pool, TermsEnum& te, std::string_view prefix,
                 std::string_view suffix, int maxEdits, bool prefixMode = false)
    : FilteredTermsEnum(te), prefix(prefix), suffix(suffix), maxEdits(maxEdits),
      n((int)suffix.size()), prefixMode(prefixMode) {
    prev = (int*)pool.alloc((n + 1) * sizeof(int), alignof(int));
    cur = (int*)pool.alloc((n + 1) * sizeof(int), alignof(int));
  }

  float currentScore() const override { return score; }
};

} // namespace solux
