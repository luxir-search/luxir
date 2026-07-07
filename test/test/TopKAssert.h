#pragma once

#include <algorithm>
#include <cmath>
#include <set>
#include <span>

#include "gtest/gtest.h"
#include "solux/search/Collector.h"

namespace solux::test {

// Cross-execution-path top-k equivalence under the accepted score policy:
// clause sums may differ in low bits between execution paths (FP summation
// order is not pinned), so ranks within a score tie-group can permute and the
// group cut by the k boundary may keep different members.  Assert per-rank
// score closeness and per-tie-group doc-set equality, comparing scores only
// for the group the k boundary cuts.  The tolerance is ~100x the achievable
// summation drift, and real tie-group gaps exceed it, so low-bit flips cannot
// cross groups.
inline void assertTopKEquivalent(std::span<const TopDocsCollector::ScoreDoc> expected,
                                 std::span<const TopDocsCollector::ScoreDoc> actual,
                                 float relTol = 1e-5f) {
  ASSERT_EQ(actual.size(), expected.size());
  auto near = [&](float a, float b) {
    float scale = std::max({std::fabs(a), std::fabs(b), 1e-30f});
    return std::fabs(a - b) <= relTol * scale;
  };
  size_t i = 0;
  while (i < expected.size()) {
    size_t j = i + 1;  // tie group [i, j): chained closeness over expected scores
    while (j < expected.size() && near(expected[j].score, expected[j - 1].score)) {
      j++;
    }
    for (size_t r = i; r < j; r++) {
      EXPECT_TRUE(near(actual[r].score, expected[r].score))
          << "rank " << r << ": score " << actual[r].score << " vs expected "
          << expected[r].score;
    }
    // Doc identity is skipped ONLY for a multi-element tie group cut by the k
    // boundary: its members may legitimately swap with near-tied docs beyond
    // k.  A singleton final group has no observed tie, so its doc must match
    // (a genuine unseen near-tie partner just past k would make this flaky in
    // theory; test corpora keep boundary scores separated).
    bool cutMultiGroup = (j == expected.size()) && (j - i > 1);
    if (!cutMultiGroup) {
      std::multiset<segdoc> expectedDocs;
      std::multiset<segdoc> actualDocs;
      for (size_t r = i; r < j; r++) {
        expectedDocs.insert(expected[r].doc);
        actualDocs.insert(actual[r].doc);
      }
      EXPECT_EQ(actualDocs, expectedDocs) << "tie group at ranks [" << i << "," << j << ")";
    }
    i = j;
  }
}

} // namespace solux::test
