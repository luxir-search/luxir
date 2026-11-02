// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>
#include <chrono>
#include <random>
#include <format>
#include "luxir/query/KnnPlanner.h"

using namespace luxir;

namespace {
KnnPlanner::Segment segment(int64_t eligible = 1000) {
  return {.vectors = 100000, .liveVectors = 100000, .docsWithValue = 100000,
      .eligibleDocs = eligible, .eligibleVectors = eligible, .domainSize = eligible,
      .dims = 128, .nlist = 256, .pqM = 32, .pqBits = 8,
      .codec = KnnPlanner::Codec::PQ, .domain = true, .unfiltered = false, .bounded = true};
}
// Enumerate configurations independently of the planner's running-sum updates.
double cost(std::span<const KnnPlanner::Segment> segments, const KnnPlanner::Request& req,
            const KnnPlanner::Costs& costs, uint64_t mask) {
  double exact = 0, linear = 0, fixed = 0, boundedDims = 0, intervalDims = 0, physicalDims = 0;
  int64_t live = 0, eligible = 0, docs = 0, exactDocs = 0, bounded = 0, boundedLive = 0, physicalDocs = 0, slots = 0;
  for (size_t i = 0; i < segments.size(); i++) {
    const auto& s = segments[i];
    auto p = KnnPlanner::price(s, req, costs);
    if (KnnPlanner::canIvf(s) && s.bounded) { physicalDims += (double)s.vectors * s.dims; physicalDocs += s.docsWithValue; }
    if ((mask >> i & 1) && KnnPlanner::canIvf(s)) {
      linear += p.a; fixed += p.b; live += s.liveVectors; slots++;
      eligible += s.eligibleVectors; docs += s.eligibleDocs;
      if (s.bounded) { bounded += s.eligibleDocs; boundedLive += s.liveVectors; boundedDims += (double)s.eligibleVectors * s.dims; }
      else intervalDims += (double)s.eligibleVectors * s.dims;
    } else { exact += p.exactCost; exactDocs += s.eligibleDocs; }
  }
  double p0 = live ? KnnPlanner::initialScanFraction(req, (double)eligible / live) : 0;
  double coverage = p0;
  if (live && !req.nprobe) coverage = std::min(1.0, std::max({coverage,
      bounded ? (double)std::min({req.boundedTarget, bounded, boundedLive}) / bounded : 0,
      (double)std::max((int64_t)0, req.k - exactDocs) / std::max((int64_t)1, docs)}));
  double reads = std::min(boundedDims, req.boundedTarget * physicalDims / std::max((int64_t)1, physicalDocs))
      + coverage * intervalDims * costs.intervalReadFraction;
  double rounds = slots == 0 ? 0 : coverage <= p0 ? 1 : 1 + std::ceil(std::log2(coverage / p0));
  return costs.requestCost + exact + coverage * linear + fixed + reads * costs.units[KnnPlanner::TERMINAL_DIM]
      + slots * rounds * costs.units[KnnPlanner::ROUND];
}
}

TEST(KnnPlannerTest, substitutionMustLowerWholeConfiguration) {
  // A local substitution raises coverage on the other slot. Keep all-IVF
  // when that increase costs more than the local saving.
  std::vector<KnnPlanner::Segment> segments{segment(500), segment(1000)};
  KnnPlanner::Request req{.k = 20, .boundedTarget = 300, .referenceBreadth = 1024, .alpha = 0};
  KnnPlanner::Costs costs;
  costs.units.fill(0);
  costs.units[KnnPlanner::EXACT_DIM] = 1;
  costs.units[KnnPlanner::ENTRY_NONE] = 0;
  costs.units[KnnPlanner::BIT_PROBE] = 3.3;
  costs.units[KnnPlanner::PQ_SUBCODE] = 1;
  auto plan = KnnPlanner::choose(segments, req, costs);
  double all = cost(segments, req, costs, 3);
  EXPECT_LT(plan.prices[0].exactCost, plan.allIvfCoverage * plan.prices[0].a + plan.prices[0].b);
  ASSERT_LT(all, cost(segments, req, costs, 0));
  ASSERT_LT(all, cost(segments, req, costs, 1));
  ASSERT_LT(all, cost(segments, req, costs, 2));
  EXPECT_EQ(plan.ivf, (std::vector<char>{true, true}));
  EXPECT_DOUBLE_EQ(plan.cost, all);
}

TEST(KnnPlannerTest, exhaustiveAndBaselineBounds) {
  std::mt19937 random(519);
  KnnPlanner::Costs costs;
  for (int family = 0; family < 3; family++) {
    std::array<int, 4> misses{};
    std::array<double, 4> worst{1, 1, 1, 1};
    for (int trial = 0; trial < 400; trial++) {
      bool pinned = family == 0, adversarial = family == 2;
      int n = (adversarial ? 2 : 1) + (int)(random() % (adversarial ? 6 : 7));
      int dims = adversarial ? (random() % 2 ? 128 : 768) : 32 << (random() % 5);
      int64_t k = 1 + random() % 500;
      int64_t target = k + 24 + (int64_t)std::ceil(15 * std::pow(k, 0.8));
      int small = adversarial ? 1 + (int)(random() % (n - 1)) : 0;
      std::vector<KnnPlanner::Segment> segments;
      for (int i = 0; i < n; i++) {
        int64_t vectors = adversarial ? i < small ? 1000 + random() % 4000 : 100000 + random() % 900000
            : std::array<int64_t, 3>{1000, 10000, 100000}[random() % 3];
        auto s = segment(1 + random() % (adversarial && i < small ? std::min(vectors, target) : vectors));
        s.vectors = s.liveVectors = s.docsWithValue = vectors;
        s.dims = dims; s.nlist = 16 << (random() % 5);
        s.arrayDomain = random() % 2; s.sparse = random() % 2;
        if (adversarial ? i >= small : random() % 4 == 0) {
          s.codec = KnnPlanner::Codec::RABITQ; s.bounded = false; s.rabitqBits = 2;
        }
        segments.push_back(s);
      }
      KnnPlanner::Request req{.k = k, .boundedTarget = target,
          .nprobe = pinned ? 16 : 0, .referenceBreadth = 1024};
      double best = INFINITY;
      for (uint64_t mask = 0; mask < (1ull << n); mask++) best = std::min(best, cost(segments, req, costs, mask));
      for (int variant = 0; variant < 4; variant++) {
        SCOPED_TRACE(std::format("family={} trial={} variant={} k={} target={}", family, trial, variant, k, target));
        auto plan = KnnPlanner::choose(segments, req, costs, KnnPlanner::Routing::AUTO, (KnnPlanner::Search)variant);
        double tolerance = 1e-7 + best * 1e-12;
        ASSERT_LE(plan.cost, std::min(plan.allIvfCost, plan.allExactCost) + tolerance);
        uint64_t chosen = 0;
        for (int i = 0; i < n; i++) chosen |= (uint64_t)plan.ivf[(size_t)i] << i;
        ASSERT_NEAR(plan.cost, cost(segments, req, costs, chosen), tolerance);
        misses[(size_t)variant] += plan.cost > best + tolerance;
        worst[(size_t)variant] = std::max(worst[(size_t)variant], plan.cost / best);
        if (pinned) { ASSERT_NEAR(plan.cost, best, tolerance); }
        else if (variant == (int)KnnPlanner::DEFAULT_SEARCH) { ASSERT_LE(plan.cost, best * 1.1 + tolerance); }
      }
    }
    for (int variant = 0; variant < 4; variant++) {
      RecordProperty(std::format("family{}_variant{}_misses", family, variant), misses[(size_t)variant]);
      RecordProperty(std::format("family{}_variant{}_ratio", family, variant), std::format("{:.6f}", worst[(size_t)variant]));
    }
  }
}

TEST(KnnPlannerTest, tiesAndMandatoryExact) {
  KnnPlanner::Costs costs;
  costs.units.fill(0);
  std::vector<KnnPlanner::Segment> segments{segment(), segment(), segment(0)};
  segments[1].codec = KnnPlanner::Codec::FLAT;
  auto plan = KnnPlanner::choose(segments, {}, costs);
  EXPECT_EQ(plan.ivf, (std::vector<char>{true, false, false}));
  EXPECT_EQ(KnnPlanner::choose(segments, {}, costs, KnnPlanner::Routing::FORCE_EXACT).ivf,
            (std::vector<char>{false, false, false}));
}

TEST(KnnPlannerTest, planningTime) {
  KnnPlanner::Request req{.k = 20, .boundedTarget = 240, .referenceBreadth = 1024};
  double checksum = 0;
  for (int n : {1, 4, 32, 256, 1000}) {
    std::vector<KnnPlanner::Segment> segments;
    for (int i = 0; i < n; i++) {
      auto s = segment(1 + (i * 719 + 300) % 100000);
      if (i % 2) { s.codec = KnnPlanner::Codec::RABITQ; s.bounded = false; s.rabitqBits = 2; }
      segments.push_back(s);
    }
    for (int variant = 0; variant < 4; variant++) {
      auto start = std::chrono::steady_clock::now();
      for (int i = 0; i < 100; i++) checksum += KnnPlanner::choose(segments, req, {},
          KnnPlanner::Routing::AUTO, (KnnPlanner::Search)variant).cost;
      double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / 100;
      RecordProperty(std::format("segments{}_variant{}_us", n, variant), std::format("{:.2f}", us));
    }
  }
  EXPECT_GT(checksum, 0);
}

TEST(KnnPlannerTest, sharedTerminalPoolAndCodecSetup) {
  KnnPlanner::Costs costs;
  KnnPlanner::Request req{.k = 20, .boundedTarget = 100};
  std::vector<KnnPlanner::Segment> segments{segment(), segment(), segment()};
  auto plan = KnnPlanner::choose(segments, req, costs, KnnPlanner::Routing::FORCE_IVF);
  auto totals = KnnPlanner::exactTotals(segments, plan.prices, req);
  for (int i = 0; i < 3; i++) KnnPlanner::addIvf(totals, segments[i], plan.prices[i], 1);
  EXPECT_EQ(KnnPlanner::terminalDims(totals, KnnPlanner::coverage(totals, req), costs), 100 * 128);
  EXPECT_EQ(plan.prices[0].linear[KnnPlanner::PQ_LIST_SETUP], 256);
  EXPECT_EQ(plan.prices[0].linear[KnnPlanner::RABITQ_LIST_DIM], 0);
  segments[0].codec = KnnPlanner::Codec::RABITQ;
  auto price = KnnPlanner::price(segments[0], req, costs);
  EXPECT_EQ(price.linear[KnnPlanner::RABITQ_LIST_DIM], 256 * 128);
  EXPECT_EQ(price.exact[KnnPlanner::ROUND], 0);
  VectorSearchWork work;
  work.exactVectors = 10; work.seedReads = 3; work.remainingReads = 2; work.rounds = 1;
  auto actual = KnnPlanner::actualWork(segments[0], work);
  EXPECT_EQ(actual[KnnPlanner::EXACT_DIM], 10 * 128);
  EXPECT_EQ(actual[KnnPlanner::TERMINAL_DIM], 5 * 128);
  EXPECT_EQ(actual[KnnPlanner::ROUND], 0);
}

TEST(KnnPlannerTest, predictedRoundGrowthAndPin) {
  KnnPlanner::Request req{.k = 20, .boundedTarget = 100, .referenceBreadth = 1024, .alpha = 0};
  KnnPlanner::Costs costs;
  std::vector<KnnPlanner::Segment> segments{segment()};
  auto p = KnnPlanner::price(segments[0], req, costs);
  auto totals = KnnPlanner::exactTotals(segments, std::span(&p, 1), req);
  KnnPlanner::addIvf(totals, segments[0], p, 1);
  auto effort = KnnPlanner::effort(totals, req);
  EXPECT_DOUBLE_EQ(effort.initial, 1.0 / 32);
  EXPECT_DOUBLE_EQ(effort.coverage, 0.1);
  EXPECT_EQ(effort.rounds, 3);
  req.nprobe = 16;
  effort = KnnPlanner::effort(totals, req);
  EXPECT_DOUBLE_EQ(effort.initial, effort.coverage);
  EXPECT_EQ(effort.rounds, 1);
}

TEST(KnnPlannerTest, pinnedSharedPoolWithUnequalMultiplicity) {
  KnnPlanner::Costs costs;
  KnnPlanner::Request req{.k = 20, .boundedTarget = 500, .nprobe = 64, .referenceBreadth = 1024};
  std::mt19937 random(125);
  for (int trial = 0; trial < 100; trial++) {
    std::vector<KnnPlanner::Segment> segments;
    for (int i = 0; i < 5; i++) {
      int mult = 1 + (int)(random() % 8);
      auto s = segment(1 + random() % (100000 / mult));
      s.docsWithValue /= mult; s.multi = true; s.eligibleVectors = s.eligibleDocs * mult;
      if (i % 2) { s.codec = KnnPlanner::Codec::RABITQ; s.bounded = false; s.rabitqBits = 2; }
      segments.push_back(s);
    }
    double best = INFINITY;
    for (uint64_t mask = 0; mask < 32; mask++) best = std::min(best, cost(segments, req, costs, mask));
    auto result = KnnPlanner::choose(segments, req, costs);
    ASSERT_NEAR(result.cost, best, best * 1e-12 + 1e-7) << trial;
  }
}
