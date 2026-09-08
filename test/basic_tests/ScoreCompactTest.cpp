// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/query/ScoreCompact.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

#include "gtest/gtest.h"

using namespace luxir;

namespace {

struct ThresholdCase {
  float minCompetitiveScore;
  double scoreBoundFactor;
  double bound;
};

void addThresholdNeighborhood(std::vector<float>& values, float threshold) {
  float lowTarget = -std::numeric_limits<float>::infinity();
  float highTarget = std::numeric_limits<float>::infinity();
  if (threshold == lowTarget) {
    float value = std::numeric_limits<float>::lowest();
    for (int32_t i = 0; i < 12; i++) {
      values.push_back(value);
      value = std::nextafter(value, highTarget);
    }
    return;
  }

  values.push_back(threshold);
  float down = threshold;
  for (int32_t i = 0; i < 12; i++) {
    down = std::nextafter(down, lowTarget);
    values.push_back(down);
  }
  float up = threshold;
  for (int32_t i = 0; i < 12; i++) {
    up = std::nextafter(up, highTarget);
    values.push_back(up);
  }
}

std::vector<float> thresholdSamples(float threshold) {
  std::vector<float> values = {
    std::numeric_limits<float>::lowest(),
    std::nextafter(std::numeric_limits<float>::lowest(),
                   std::numeric_limits<float>::infinity()),
    -1.0e30f,
    -1.0e10f,
    -128.0f,
    -1.0f,
    -0.0f,
    0.0f,
    std::numeric_limits<float>::denorm_min(),
    std::numeric_limits<float>::min(),
    1.0f,
    128.0f,
    1.0e10f,
    1.0e30f,
    std::numeric_limits<float>::max(),
    std::numeric_limits<float>::infinity(),
    std::numeric_limits<float>::quiet_NaN()
  };
  addThresholdNeighborhood(values, threshold);
  return values;
}

std::vector<float> makeScores(int32_t size, int32_t mode, float threshold) {
  std::vector<float> scores;
  scores.reserve((size_t) size);
  for (int32_t i = 0; i < size; i++) {
    switch (mode) {
      case 0:
        scores.push_back(threshold + 2.0f);
        break;
      case 1:
        scores.push_back(threshold - 2.0f);
        break;
      case 2:
        scores.push_back((i & 1) == 0 ? threshold + 1.0f : threshold - 1.0f);
        break;
      case 3:
        scores.push_back(threshold);
        break;
      default:
        if ((i % 5) == 0) {
          scores.push_back(std::numeric_limits<float>::quiet_NaN());
        } else if ((i % 5) == 1) {
          scores.push_back(threshold);
        } else if ((i % 5) == 2) {
          scores.push_back(threshold - 1.0f);
        } else {
          scores.push_back(threshold + 1.0f);
        }
        break;
    }
  }
  return scores;
}

std::vector<int32_t> makeDocs(int32_t size) {
  std::vector<int32_t> docs;
  docs.reserve((size_t) size);
  for (int32_t i = 0; i < size; i++) {
    docs.push_back(10 + i * 3);
  }
  return docs;
}

template <typename Keep>
void referenceCompact(const std::vector<int32_t>& docs, const std::vector<float>& scores,
                      float threshold, Keep keep, std::vector<int32_t>& outDocs,
                      std::vector<float>& outScores) {
  for (int32_t i = 0; i < (int32_t) docs.size(); i++) {
    if (keep(scores[(size_t) i], threshold)) {
      outDocs.push_back(docs[(size_t) i]);
      outScores.push_back(scores[(size_t) i]);
    }
  }
}

void expectCompactResult(const std::vector<int32_t>& docs, const std::vector<float>& scores,
                         int32_t size, const std::vector<int32_t>& expectedDocs,
                         const std::vector<float>& expectedScores) {
  ASSERT_EQ(size, (int32_t) expectedDocs.size());
  for (int32_t i = 0; i < size; i++) {
    EXPECT_EQ(docs[(size_t) i], expectedDocs[(size_t) i]) << "i=" << i;
    EXPECT_EQ(std::bit_cast<uint32_t>(scores[(size_t) i]),
              std::bit_cast<uint32_t>(expectedScores[(size_t) i])) << "i=" << i;
    if (i > 0) {
      EXPECT_LT(docs[(size_t) i - 1], docs[(size_t) i]) << "i=" << i;
    }
  }
}

} // namespace

TEST(ScoreCompactTest, CompetitiveThresholdMatchesCanReach) {
  double nearMaxFactor = 1.0 + (double) 13 * 0x1p-24;
  std::vector<ThresholdCase> cases = {
    {0.0f, 1.0, 0.0},
    {1.0f, 1.0, 0.0},
    {1.0f, 2.0, 0.0},
    {1.0f, 1.0, -1.0},
    {std::numeric_limits<float>::lowest(), 1.0, 0.0},
    {std::numeric_limits<float>::max(), 1.0, -(double) std::numeric_limits<float>::max()},
    {std::numeric_limits<float>::min(), 1.0, 0.0},
    {1.0e-30f, 1.0 + (double) 5 * 0x1p-24, -1.0e-30},
    {1.0e30f, 1.0 + (double) 7 * 0x1p-24, 0.0},
    {std::numeric_limits<float>::max(), nearMaxFactor,
     (double) std::numeric_limits<float>::max() / nearMaxFactor - 1.0e30},
    {1.0f, 1.0, 1.0e20},
    {1.0f, 1.0, -1.0e20}
  };

  for (const auto& testCase : cases) {
    SCOPED_TRACE(::testing::Message()
                 << "mcs=" << testCase.minCompetitiveScore
                 << " factor=" << testCase.scoreBoundFactor
                 << " bound=" << testCase.bound);
    float threshold = competitiveScoreThreshold(testCase.minCompetitiveScore,
                                                testCase.scoreBoundFactor,
                                                testCase.bound);
    if (scoreCanReach(std::numeric_limits<float>::lowest(), testCase.bound,
                      testCase.minCompetitiveScore, testCase.scoreBoundFactor)) {
      EXPECT_EQ(threshold, -std::numeric_limits<float>::infinity());
    } else if (!scoreCanReach(std::numeric_limits<float>::max(), testCase.bound,
                              testCase.minCompetitiveScore, testCase.scoreBoundFactor)) {
      EXPECT_EQ(threshold, std::numeric_limits<float>::infinity());
    }

    for (float score : thresholdSamples(threshold)) {
      bool expected = scoreCanReach(score, testCase.bound, testCase.minCompetitiveScore,
                                    testCase.scoreBoundFactor);
      bool actual = score >= threshold;
      EXPECT_EQ(actual, expected) << "score=" << score
                                  << " threshold=" << threshold;
    }
  }
}

TEST(ScoreCompactTest, CompetitiveThresholdSeedOverloadMatchesReference) {
  double nearFactor = 1.0 + (double) 7 * 0x1p-24;
  std::vector<ThresholdCase> cases = {
    {0.0f, 1.0, 0.0},
    {1.0f, 1.0, 0.0},
    {-1.0f, 1.0, 0.0},
    {std::numeric_limits<float>::infinity(), 1.0, 0.0},
    {-std::numeric_limits<float>::infinity(), 1.0, 0.0},
    {42.0f, nearFactor, 42.0 / nearFactor},
    {42.0f, nearFactor, 42.0 / nearFactor - 0x1p-40},
    {42.0f, nearFactor, 42.0 / nearFactor + 0x1p-40},
    {std::numeric_limits<float>::max(), nearFactor,
     (double) std::numeric_limits<float>::max() / nearFactor - 1.0e30},
    {1.0f, 1.0, 1.0e20},
    {1.0f, 1.0, -1.0e20}
  };

  for (const auto& testCase : cases) {
    float expected = competitiveScoreThresholdReference(testCase.minCompetitiveScore,
                                                        testCase.scoreBoundFactor,
                                                        testCase.bound);
    std::vector<float> seeds = {
      -std::numeric_limits<float>::infinity(),
      std::numeric_limits<float>::infinity(),
      std::numeric_limits<float>::lowest(),
      std::numeric_limits<float>::max(),
      -128.0f,
      0.0f,
      128.0f,
      expected
    };
    if (std::isfinite(expected)) {
      seeds.push_back(std::nextafter(expected, -std::numeric_limits<float>::infinity()));
      seeds.push_back(std::nextafter(expected, std::numeric_limits<float>::infinity()));
      seeds.push_back(expected - 16.0f);
      seeds.push_back(expected + 16.0f);
    }

    for (float seed : seeds) {
      SCOPED_TRACE(::testing::Message()
                   << "mcs=" << testCase.minCompetitiveScore
                   << " factor=" << testCase.scoreBoundFactor
                   << " bound=" << testCase.bound
                   << " seed=" << seed);
      float actual = competitiveScoreThreshold(testCase.minCompetitiveScore,
                                               testCase.scoreBoundFactor,
                                               testCase.bound, seed);
      EXPECT_EQ(std::bit_cast<uint32_t>(actual), std::bit_cast<uint32_t>(expected));
    }
  }
}

TEST(ScoreCompactTest, CompactByScoreThresholdMatchesReference) {
  float thresholds[] = {
    1.0f,
    -std::numeric_limits<float>::infinity()
  };
  for (float threshold : thresholds) {
    for (int32_t size = 0; size <= 40; size++) {
      for (int32_t mode = 0; mode < 5; mode++) {
        SCOPED_TRACE(::testing::Message()
                     << "threshold=" << threshold << " size=" << size
                     << " mode=" << mode);
        auto docs = makeDocs(size);
        auto scores = makeScores(size, mode, threshold);
        std::vector<int32_t> expectedDocs;
        std::vector<float> expectedScores;
        referenceCompact(docs, scores, threshold,
                         [](float score, float t) { return score >= t; },
                         expectedDocs, expectedScores);

        int32_t kept = compactByScoreThreshold(docs.data(), scores.data(), size, threshold);
        expectCompactResult(docs, scores, kept, expectedDocs, expectedScores);
      }
    }
  }
}

TEST(ScoreCompactTest, CompactByScoreNotLessThanThresholdMatchesReference) {
  float threshold = 1.0f;
  for (int32_t size = 0; size <= 40; size++) {
    for (int32_t mode = 0; mode < 5; mode++) {
      SCOPED_TRACE(::testing::Message()
                   << "size=" << size << " mode=" << mode);
      auto docs = makeDocs(size);
      auto scores = makeScores(size, mode, threshold);
      std::vector<int32_t> expectedDocs;
      std::vector<float> expectedScores;
      referenceCompact(docs, scores, threshold,
                       [](float score, float t) { return !(score < t); },
                       expectedDocs, expectedScores);

      int32_t kept = compactByScoreNotLessThanThreshold(docs.data(), scores.data(),
                                                        size, threshold);
      expectCompactResult(docs, scores, kept, expectedDocs, expectedScores);
    }
  }
}
