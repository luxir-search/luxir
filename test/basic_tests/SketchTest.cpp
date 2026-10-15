// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

#include "luxir/util/HyperLogLog.h"
#include "luxir/util/TDigest.h"

namespace {

using luxir::HyperLogLog;
using luxir::TDigest;

TEST(HyperLogLog, PrecisionFollowsThreshold) {
  EXPECT_EQ(12, HyperLogLog::precisionForThreshold(3000));
  EXPECT_EQ(4, HyperLogLog::precisionForThreshold(0));
  EXPECT_EQ(4, HyperLogLog::precisionForThreshold(5));
  EXPECT_EQ(16, HyperLogLog::precisionForThreshold(40000));
}

TEST(HyperLogLog, ExactBelowThreshold) {
  HyperLogLog sketch(1000);
  EXPECT_EQ(0, sketch.estimate());
  for (int round = 0; round < 3; round++) {
    for (uint64_t i = 0; i < 1000; i++) sketch.add(HyperLogLog::mix(i));
  }
  EXPECT_FALSE(sketch.dense());
  EXPECT_EQ(1000, sketch.estimate());
  sketch.add(HyperLogLog::mix(1000));
  EXPECT_TRUE(sketch.dense());
}

TEST(HyperLogLog, DenseErrorWithinBounds) {
  // p = 12: standard error 1.04 / 64 = 1.6%. Allow about four sigma.
  for (uint64_t distinct : {3500ULL, 20000ULL, 300000ULL, 2000000ULL}) {
    HyperLogLog sketch(3000);
    for (uint64_t i = 0; i < distinct; i++) {
      sketch.add(HyperLogLog::mix(i * 0x9E3779B97F4A7C15ULL + 7));
    }
    double error = std::fabs((double)sketch.estimate() - (double)distinct)
        / (double)distinct;
    EXPECT_LT(error, 0.065) << distinct;
  }
}

TEST(HyperLogLog, SmallRangeWithTinyThreshold) {
  // Threshold 0 is dense from the first value; the estimator must stay
  // accurate where linear counting would be used by plain HyperLogLog.
  HyperLogLog sketch(0);
  ASSERT_EQ(4, sketch.precision);
  for (uint64_t i = 0; i < 3; i++) sketch.add(HyperLogLog::mix(i));
  EXPECT_NEAR(3.0, (double)sketch.estimate(), 1.0);
}

TEST(HyperLogLog, MergeMatchesSingleSketch) {
  std::mt19937_64 rng(42);
  std::vector<uint64_t> hashes(50000);
  for (auto& hash : hashes) hash = HyperLogLog::mix(rng() % 30000);
  HyperLogLog whole(1000);
  for (uint64_t hash : hashes) whole.add(hash);

  // Mixed exact and dense parts in both merge directions.
  HyperLogLog small(1000);
  HyperLogLog big(1000);
  for (size_t i = 0; i < hashes.size(); i++) {
    (i % 100 == 0 ? small : big).add(hashes[i]);
  }
  ASSERT_FALSE(small.dense());
  ASSERT_TRUE(big.dense());
  HyperLogLog left = small;
  left.merge(big);
  HyperLogLog right = big;
  right.merge(small);
  EXPECT_EQ(whole.estimate(), left.estimate());
  EXPECT_EQ(whole.estimate(), right.estimate());
  EXPECT_EQ(whole.registers, left.registers);

  // Exact parts whose union crosses the threshold become dense.
  HyperLogLog a(1000);
  HyperLogLog b(1000);
  for (uint64_t i = 0; i < 800; i++) a.add(HyperLogLog::mix(i));
  for (uint64_t i = 400; i < 1200; i++) b.add(HyperLogLog::mix(i));
  a.merge(b);
  EXPECT_TRUE(a.dense());
  EXPECT_NEAR(1200.0, (double)a.estimate(), 1200 * 0.065);
}

// Largest distance between q and the rank interval x occupies in the data.
double rankError(const std::vector<double>& sorted, double x, double q) {
  auto lo = std::lower_bound(sorted.begin(), sorted.end(), x);
  auto hi = std::upper_bound(sorted.begin(), sorted.end(), x);
  double loRank = (double)(lo - sorted.begin()) / (double)sorted.size();
  double hiRank = (double)(hi - sorted.begin()) / (double)sorted.size();
  if (q < loRank) return loRank - q;
  if (q > hiRank) return q - hiRank;
  return 0.0;
}

TEST(TDigest, ExactForSmallInputs) {
  TDigest digest;
  for (int i = 100; i >= 1; i--) digest.add(i);
  digest.finish();
  ASSERT_TRUE(digest.exact());
  EXPECT_DOUBLE_EQ(1.0, digest.quantile(0.0));
  EXPECT_DOUBLE_EQ(50.5, digest.quantile(0.5));
  EXPECT_DOUBLE_EQ(95.05, digest.quantile(0.95));
  EXPECT_DOUBLE_EQ(100.0, digest.quantile(1.0));
  EXPECT_DOUBLE_EQ(0.5, digest.cdf(50));
  EXPECT_DOUBLE_EQ(0.0, digest.cdf(0.5));
  EXPECT_DOUBLE_EQ(1.0, digest.cdf(100));

  TDigest one;
  one.add(7.5);
  one.finish();
  EXPECT_DOUBLE_EQ(7.5, one.quantile(0.01));
  EXPECT_DOUBLE_EQ(7.5, one.quantile(0.99));

  TDigest none;
  none.finish();
  EXPECT_TRUE(std::isnan(none.quantile(0.5)));
  EXPECT_TRUE(std::isnan(none.cdf(1.0)));
}

void expectAccurate(TDigest& digest, std::vector<double> data,
                    const char* label) {
  std::sort(data.begin(), data.end());
  digest.finish();
  ASSERT_FALSE(digest.exact());
  EXPECT_LE(digest.centroids.size(), (size_t)digest.compression) << label;
  EXPECT_DOUBLE_EQ(data.front(), digest.quantile(0.0)) << label;
  EXPECT_DOUBLE_EQ(data.back(), digest.quantile(1.0)) << label;
  for (double q : {0.001, 0.01, 0.05, 0.25, 0.5, 0.75, 0.95, 0.99, 0.999}) {
    // Tolerance scales with the centroid size the k1 scale allows at q.
    double tolerance = 0.002 + 3.0 * std::sqrt(q * (1.0 - q)) / digest.compression;
    EXPECT_LE(rankError(data, digest.quantile(q), q), tolerance)
        << label << " q=" << q;
  }
  for (double q : {0.01, 0.1, 0.5, 0.9, 0.99}) {
    double x = data[(size_t)(q * (double)(data.size() - 1))];
    // Any rank inside x's run of equal values is correct.
    EXPECT_LE(rankError(data, x, digest.cdf(x)), 0.01) << label << " q=" << q;
  }
}

TEST(TDigest, RankErrorOnRandomData) {
  std::mt19937_64 rng(7);
  std::uniform_real_distribution<double> uniform(-1000.0, 1000.0);
  std::exponential_distribution<double> skewed(0.01);
  std::uniform_int_distribution<int> fewValues(0, 9);

  std::vector<double> data(200000);
  TDigest a;
  for (auto& x : data) {
    x = uniform(rng);
    a.add(x);
  }
  expectAccurate(a, data, "uniform");

  TDigest b;
  for (auto& x : data) {
    x = skewed(rng);
    b.add(x);
  }
  expectAccurate(b, data, "exponential");

  // Heavy ties: any value inside a run of equal values is a correct answer.
  TDigest c;
  for (auto& x : data) {
    x = fewValues(rng);
    c.add(x);
  }
  expectAccurate(c, data, "ties");
}

TEST(TDigest, MergedPartsKeepAccuracy) {
  std::mt19937_64 rng(11);
  std::lognormal_distribution<double> latency(3.0, 1.0);
  std::vector<double> data;
  TDigest merged;
  // Parts of very different sizes, some still exact when merged.
  for (int part = 0; part < 40; part++) {
    TDigest digest;
    int size = part % 4 == 0 ? 50 : 5000 + part * 100;
    for (int i = 0; i < size; i++) {
      double x = latency(rng);
      data.push_back(x);
      digest.add(x);
    }
    merged.merge(digest);
  }
  EXPECT_DOUBLE_EQ((double)data.size(), merged.count());
  expectAccurate(merged, data, "merged");
}

TEST(TDigest, ExactPartsMergeExactly) {
  TDigest a;
  TDigest b;
  for (int i = 1; i <= 50; i++) a.add(i);
  for (int i = 51; i <= 100; i++) b.add(i);
  a.merge(b);
  a.finish();
  ASSERT_TRUE(a.exact());
  EXPECT_DOUBLE_EQ(50.5, a.quantile(0.5));
  EXPECT_DOUBLE_EQ(1.0, a.min);
  EXPECT_DOUBLE_EQ(100.0, a.max);
}

} // namespace
