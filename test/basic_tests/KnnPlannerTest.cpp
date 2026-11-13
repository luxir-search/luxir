// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>
#include <random>
#include <format>
#include "luxir/query/KnnPlanner.h"

using namespace luxir;

namespace {
KnnPlanner::Segment segment(int64_t eligible = 1000) {
  return {.vectors = 100000, .liveVectors = 100000, .docsWithValue = 100000,
      .eligibleDocs = eligible, .eligibleVectors = eligible, .domainSize = eligible, .maxDoc = 100000,
      .dims = 128, .nlist = 256, .pqM = 32, .pqBits = 8,
      .codec = KnnPlanner::Codec::PQ, .l2 = true, .domain = true, .unfiltered = false, .bounded = true};
}
// Enumerate configurations independently of the planner's running-sum updates.
double cost(std::span<const KnnPlanner::Segment> segments, const KnnPlanner::Request& req,
            const KnnPlanner::Costs& costs, uint64_t mask) {
  double exact = 0, linear = 0, fixed = 0, boundedDims = 0, intervalDims = 0, physicalDims = 0;
  double boundedReads = 0, intervalReads = 0, physicalReads = 0, intervalVectors = 0;
  int64_t live = 0, eligible = 0, docs = 0, exactDocs = 0, bounded = 0, boundedLive = 0, physicalDocs = 0, slots = 0;
  for (size_t i = 0; i < segments.size(); i++) {
    const auto& s = segments[i];
    auto p = KnnPlanner::price(s, req, costs);
    if (KnnPlanner::canIvf(s) && s.bounded) {
      physicalDims += (double)s.vectors * s.dims; physicalReads += (double)s.vectors; physicalDocs += s.docsWithValue;
    }
    if ((mask >> i & 1) && KnnPlanner::canIvf(s)) {
      linear += p.a; fixed += p.b; live += s.liveVectors; slots++;
      eligible += s.eligibleVectors; docs += s.eligibleDocs;
      if (s.bounded) {
        bounded += s.eligibleDocs; boundedLive += s.liveVectors;
        boundedDims += (double)s.eligibleVectors * s.dims; boundedReads += (double)s.eligibleVectors;
      } else {
        intervalVectors += s.eligibleVectors;
        intervalDims += (double)s.eligibleVectors * s.dims * p.intervalReadFraction;
        intervalReads += (double)s.eligibleVectors * p.intervalReadFraction;
      }
    } else { exact += p.exactCost; exactDocs += s.eligibleDocs; }
  }
  double p0 = live ? KnnPlanner::initialScanFraction(req, (double)eligible / live) : 0;
  double coverage = p0;
  if (live && !req.nprobe) coverage = std::min(1.0, std::max({coverage,
      bounded ? (double)std::min({req.boundedTarget, bounded, boundedLive}) / bounded : 0,
      (double)std::max((int64_t)0, req.k - exactDocs) / std::max((int64_t)1, docs)}));
  double interval = std::max(coverage * intervalReads, std::min((double)req.k, coverage * intervalVectors));
  double dims = std::min(boundedDims, req.boundedTarget * physicalDims / std::max((int64_t)1, physicalDocs))
      + (intervalReads > 0 ? interval * intervalDims / intervalReads : 0);
  double reads = std::min(boundedReads, req.boundedTarget * physicalReads / std::max((int64_t)1, physicalDocs))
      + interval;
  double rounds = slots == 0 ? 0 : coverage <= p0 ? 1 : 1 + std::ceil(std::log2(coverage / p0));
  double selectors = 0;
  for (size_t i = 0; i < segments.size(); i++) if ((mask >> i & 1) && KnnPlanner::canIvf(segments[i])) {
    const auto& s = segments[i];
    if (s.unfiltered) continue;
    double fold = costs.price(KnnPlanner::foldWork(s));
    bool folded = KnnPlanner::foldFirst(s, p0, req.selector, costs);
    if (coverage > p0 && KnnPlanner::borrowDomain(s) && s.bounded)
      selectors += ((s.vectors + 63) / 64) * costs.units[KnnPlanner::FOLD_WORD];
    selectors += (folded || coverage > p0 ? fold : 0)
        + (folded ? 0 : KnnPlanner::predictedChecks(s, p0) * (KnnPlanner::resolveCost(s, costs) - costs.units[KnnPlanner::ENTRY_BITMAP]));
  }
  return costs.requestCost + selectors + exact + coverage * linear + fixed + dims * costs.units[KnnPlanner::TERMINAL_DIM]
      + reads * costs.units[KnnPlanner::TERMINAL_READ] + slots * rounds * costs.units[KnnPlanner::ROUND]
      + (slots ? costs.units[KnnPlanner::IVF_REQUEST] : 0);
}
}

TEST(KnnPlannerTest, substitutionMustLowerWholeConfiguration) {
  // A local substitution raises coverage on the other slot. Keep all-IVF
  // when that increase costs more than the local saving.
  std::vector<KnnPlanner::Segment> segments{segment(500), segment(1000)};
  KnnPlanner::Request req{.k = 20, .boundedTarget = 300, .referenceBreadth = 1024, .alpha = 0, .selector = KnnPlanner::Selector::RESOLVE};
  KnnPlanner::Costs costs;
  costs.units.fill(0);
  costs.units[KnnPlanner::EXACT_DIM] = 1;
  costs.units[KnnPlanner::ENTRY_NONE] = 0;
  costs.units[KnnPlanner::ENTRY_BITMAP] = 3.3;
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
    int misses = 0;
    double worst = 1;
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
        s.vectors = s.liveVectors = s.docsWithValue = s.maxDoc = vectors;
        s.domainSize = s.eligibleDocs;
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
      {
        SCOPED_TRACE(std::format("family={} trial={} k={} target={}", family, trial, k, target));
        auto plan = KnnPlanner::choose(segments, req, costs);
        double tolerance = 1e-7 + best * 1e-12;
        ASSERT_LE(plan.cost, std::min(plan.allIvfCost, plan.allExactCost) + tolerance);
        uint64_t chosen = 0;
        for (int i = 0; i < n; i++) chosen |= (uint64_t)plan.ivf[(size_t)i] << i;
        ASSERT_NEAR(plan.cost, cost(segments, req, costs, chosen), tolerance);
        misses += plan.cost > best + tolerance;
        worst = std::max(worst, plan.cost / best);
        // Pinned search is exact without the interval read floor, which adds
        // at most k terminal reads to any configuration.
        double floor = (double)k * (costs.units[KnnPlanner::TERMINAL_READ] + dims * costs.units[KnnPlanner::TERMINAL_DIM]);
        if (pinned) { ASSERT_LE(plan.cost, best + floor + tolerance); }
        else { ASSERT_LE(plan.cost, best * 1.1 + tolerance); }
      }
    }
    RecordProperty(std::format("family{}_misses", family), misses);
    RecordProperty(std::format("family{}_ratio", family), std::format("{:.6f}", worst));
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

TEST(KnnPlannerTest, sharedTerminalPoolAndCodecSetup) {
  KnnPlanner::Costs costs;
  KnnPlanner::Request req{.k = 20, .boundedTarget = 100};
  std::vector<KnnPlanner::Segment> segments{segment(), segment(), segment()};
  auto plan = KnnPlanner::choose(segments, req, costs, KnnPlanner::Routing::FORCE_IVF);
  auto totals = KnnPlanner::exactTotals(segments, plan.prices, req);
  for (int i = 0; i < 3; i++) KnnPlanner::addIvf(totals, segments[i], plan.prices[i], 1);
  EXPECT_EQ(KnnPlanner::terminalDims(totals, KnnPlanner::coverage(totals, req), req.k), 100 * 128);
  EXPECT_EQ(KnnPlanner::terminalReads(totals, KnnPlanner::coverage(totals, req), req.k), 100);
  EXPECT_EQ(plan.prices[0].linear[KnnPlanner::PQ_LIST_SETUP], 256 * 32);
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
  EXPECT_EQ(actual[KnnPlanner::TERMINAL_READ], 5);
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

TEST(KnnPlannerTest, selectorCrossoverAndDeepenCost) {
  auto s = segment(25000);
  s.sparse = true;
  KnnPlanner::Costs costs;
  KnnPlanner::Request req;
  auto price = KnnPlanner::price(s, req, costs);
  KnnPlanner::SelectorSums sums(std::span(&price, 1));
  sums.add(0, price, 1);
  ASSERT_GT(price.foldThreshold, 0);
  ASSERT_LT(price.foldThreshold, 1);
  for (double p : {price.foldThreshold * 0.5, price.foldThreshold, price.foldThreshold * 2}) {
    EXPECT_EQ(p > price.foldThreshold, KnnPlanner::foldFirst(s, p, req.selector, costs));
    for (bool deepen : {false, true}) {
      KnnPlanner::Effort effort{p, deepen ? 1 : p, 1};
      EXPECT_NEAR(KnnPlanner::selectorCost(price, p, deepen), sums.cost(effort), 1e-7);
    }
  }
  s.sparse = false;
  EXPECT_TRUE(KnnPlanner::foldFirst(s, 0, req.selector, costs));
  EXPECT_EQ(0, costs.price(KnnPlanner::foldWork(s)));
  s.multi = true; s.vectorsEstimated = true;
  EXPECT_EQ(-1, KnnPlanner::foldThreshold(s, KnnPlanner::Selector::AUTO, costs));
  EXPECT_EQ(INFINITY, KnnPlanner::foldThreshold(s, KnnPlanner::Selector::RESOLVE, costs));
  s.domainSize = 99999;
  EXPECT_EQ(1, KnnPlanner::foldWork(s)[KnnPlanner::FOLD_DOC]);
  s.arrayDomain = true;
  EXPECT_EQ(99999, KnnPlanner::foldWork(s)[KnnPlanner::FOLD_DOC]);
}

TEST(KnnPlannerTest, sharedIvfRequestCanFavorExact) {
  auto s = segment(5000);
  s.vectors = s.liveVectors = s.docsWithValue = s.maxDoc = 5000;
  s.nlist = 16; s.domain = false; s.unfiltered = true;
  std::array segments{s};
  KnnPlanner::Costs costs;
  double requestCost = costs.units[KnnPlanner::IVF_REQUEST];
  for (int nprobe : {0, 16}) {
    KnnPlanner::Request req{.k = 3, .boundedTarget = 100, .nprobe = nprobe, .referenceBreadth = 256};
    costs.units[KnnPlanner::IVF_REQUEST] = 0;
    auto without = KnnPlanner::choose(segments, req, costs);
    ASSERT_TRUE(without.ivf[0]);
    costs.units[KnnPlanner::IVF_REQUEST] = requestCost;
    auto with = KnnPlanner::choose(segments, req, costs);
    EXPECT_FALSE(with.ivf[0]);
    EXPECT_DOUBLE_EQ(without.allIvfCost + requestCost, with.allIvfCost);
    EXPECT_DOUBLE_EQ(without.allExactCost, with.allExactCost);
    EXPECT_DOUBLE_EQ(with.allExactCost, with.cost);
    auto forced = KnnPlanner::choose(segments, req, costs, KnnPlanner::Routing::FORCE_EXACT);
    EXPECT_DOUBLE_EQ(without.allExactCost, forced.cost);
    std::array multiple{s, s, s};
    auto all = KnnPlanner::choose(multiple, req, costs, KnnPlanner::Routing::FORCE_IVF);
    costs.units[KnnPlanner::IVF_REQUEST] = 0;
    auto noCharge = KnnPlanner::choose(multiple, req, costs, KnnPlanner::Routing::FORCE_IVF);
    EXPECT_DOUBLE_EQ(noCharge.cost + requestCost, all.cost);
  }
}

// pq4 runs the kernel over every physical code: its units count entries, not
// eligible vectors, and its filter test pays neither the virtual selector nor
// a misprediction per code.
TEST(KnnPlannerTest, pq4PricesPhysicalCodes) {
  auto s = segment(10000);
  s.l2 = false; s.codec = KnnPlanner::Codec::PQ4; s.pqM = 64; s.pqBits = 4;
  auto price = KnnPlanner::price(s, {}, {});
  EXPECT_EQ(0, price.linear[KnnPlanner::ENTRY_PQ]);
  EXPECT_EQ(0, price.linear[KnnPlanner::ENTRY_MISPREDICT]);
  EXPECT_EQ(0, price.linear[KnnPlanner::PQ_SUBCODE]);
  EXPECT_EQ(0, price.linear[KnnPlanner::ONE_BIT_DIM]);
  EXPECT_EQ(0, price.linear[KnnPlanner::RABITQ_LIST_DIM]);
  EXPECT_NEAR(100000, price.linear[KnnPlanner::PQ4_CODE], 1);
  EXPECT_EQ(price.linear[KnnPlanner::PQ4_CODE] * 64, price.linear[KnnPlanner::PQ4_SUBCODE]);
  EXPECT_EQ(256, price.linear[KnnPlanner::PQ4_LIST]);
  EXPECT_EQ(16 * 128, price.fixed[KnnPlanner::PQ_TABLE_DIM]);
  EXPECT_EQ(100000, price.linear[KnnPlanner::ENTRY_BITMAP]);
  // Chunks with no eligible code skip the kernel.
  auto selective = segment(10);
  selective.codec = KnnPlanner::Codec::PQ4; selective.pqM = 64; selective.pqBits = 4;
  EXPECT_LT(KnnPlanner::price(selective, {}, {}).linear[KnnPlanner::PQ4_CODE], 3000);
  VectorSearchWork work;
  work.listSetups = 256; work.physicalEntries = 100000; work.fastScanCodes = 100032; work.selectorPasses = 10000;
  work.selectorBitmap = 100000;
  auto actual = KnnPlanner::actualWork(s, work);
  EXPECT_EQ(100032, actual[KnnPlanner::PQ4_CODE]);
  EXPECT_EQ(100032 * 64, actual[KnnPlanner::PQ4_SUBCODE]);
  EXPECT_EQ(256, actual[KnnPlanner::PQ4_LIST]);
  EXPECT_EQ(16 * 128, actual[KnnPlanner::PQ_TABLE_DIM]);
  EXPECT_EQ(0, actual[KnnPlanner::ENTRY_PQ]);
  EXPECT_EQ(0, actual[KnnPlanner::ENTRY_MISPREDICT]);
  // At equal code bytes (m doubled) the unfiltered scan prices below 8-bit PQ.
  auto pq = s;
  pq.codec = KnnPlanner::Codec::PQ; pq.pqM = 32; pq.pqBits = 8; pq.eligibleVectors = pq.vectors; pq.unfiltered = true; pq.domain = false;
  auto unfiltered = s;
  unfiltered.eligibleVectors = unfiltered.vectors; unfiltered.unfiltered = true; unfiltered.domain = false;
  EXPECT_LT(KnnPlanner::price(unfiltered, {}, {}).a, KnnPlanner::price(pq, {}, {}).a);
}

TEST(KnnPlannerTest, selectorMetricAndTerminalFeatures) {
  for (auto codec : {KnnPlanner::Codec::PQ, KnnPlanner::Codec::RABITQ})
    for (bool l2 : {false, true}) for (int selector : {0, 1, 2}) {
      SCOPED_TRACE(::testing::Message() << (int)codec << '/' << l2 << '/' << selector);
      auto s = segment(selector ? 10 : 100);
      s.vectors = s.liveVectors = s.docsWithValue = s.maxDoc = 100;
      s.nlist = 3; s.pqM = 4; s.codec = codec; s.l2 = l2;
      s.domain = selector != 0; s.unfiltered = selector == 0;
      auto price = KnnPlanner::price(s, {}, {});
      VectorSearchWork work;
      work.listSetups = 3; work.physicalEntries = 100; work.selectorPasses = s.eligibleVectors;
      work.selectorNone = selector == 0 ? 100 : 0;
      work.selectorBitmap = selector == 1 ? 100 : 0;
      work.resolveIdentity = selector == 2 ? 100 : 0;
      work.seedReads = 2; work.remainingReads = 3; work.ivfRounds = 1;
      auto actual = KnnPlanner::actualWork(s, work);
      for (const auto& features : {price.linear, actual}) {
        EXPECT_EQ(codec == KnnPlanner::Codec::PQ && selector ? 100 : 0, features[KnnPlanner::ENTRY_PQ]);
        EXPECT_DOUBLE_EQ(selector ? 9 : 0, features[KnnPlanner::ENTRY_MISPREDICT]);
        EXPECT_EQ(codec == KnnPlanner::Codec::PQ && l2 ? 12 : 0, features[KnnPlanner::PQ_LIST_SETUP]);
        EXPECT_EQ(0, features[KnnPlanner::IVF_REQUEST]); // Shared charge stays out of completion.
      }
      EXPECT_EQ(5, actual[KnnPlanner::TERMINAL_READ]);
      EXPECT_EQ(5 * 128, actual[KnnPlanner::TERMINAL_DIM]);
      EXPECT_EQ(0, KnnPlanner::actualWork(s, {})[KnnPlanner::ENTRY_MISPREDICT]);
    }
}

TEST(KnnPlannerTest, pqPerListTablePricing) {
  auto s = segment();
  KnnPlanner::Costs costs;
  auto precomputed = KnnPlanner::price(s, {}, costs);
  s.pqPerListTable = true;
  auto perList = KnnPlanner::price(s, {}, costs);
  double tableDims = (double)s.nlist * (1 << s.pqBits) * s.dims;
  EXPECT_EQ(tableDims, perList.linear[KnnPlanner::PQ_TABLE_DIM]);
  EXPECT_EQ(0, perList.linear[KnnPlanner::PQ_LIST_SETUP]);
  EXPECT_NEAR(perList.a - precomputed.a,
      tableDims * costs.units[KnnPlanner::PQ_TABLE_DIM]
          - s.nlist * s.pqM * costs.units[KnnPlanner::PQ_LIST_SETUP], 1e-7);
  EXPECT_GT(perList.a, precomputed.a);
  VectorSearchWork work;
  work.listSetups = s.nlist;
  auto actual = KnnPlanner::actualWork(s, work);
  EXPECT_EQ(tableDims, actual[KnnPlanner::PQ_TABLE_DIM]);
  EXPECT_EQ(0, actual[KnnPlanner::PQ_LIST_SETUP]);
}

TEST(KnnPlannerTest, intervalTerminalReadsHaveSharedKFloor) {
  auto s = segment(200);
  s.vectors = s.liveVectors = s.docsWithValue = s.maxDoc = 200;
  s.codec = KnnPlanner::Codec::RABITQ; s.bounded = false; s.rabitqBits = 1;
  auto second = s;
  second.dims = 768;
  std::array segments{s, second};
  KnnPlanner::Request req{.k = 100};
  auto plan = KnnPlanner::choose(segments, req, {}, KnnPlanner::Routing::FORCE_IVF);
  auto totals = KnnPlanner::exactTotals(segments, plan.prices, req);
  for (size_t i = 0; i < segments.size(); i++) KnnPlanner::addIvf(totals, segments[i], plan.prices[i], 1);
  for (double coverage : {0.1, 0.5, 1.0}) {
    double reads = std::min((double)req.k, coverage * 400);
    EXPECT_DOUBLE_EQ(reads, KnnPlanner::terminalReads(totals, coverage, req.k));
    EXPECT_DOUBLE_EQ(reads * totals.intervalDims / totals.intervalReads,
        KnnPlanner::terminalDims(totals, coverage, req.k));
  }
  KnnPlanner::addIvf(totals, second, plan.prices[1], -1);
  EXPECT_EQ(100, KnnPlanner::terminalReads(totals, 1, req.k));
  KnnPlanner::addIvf(totals, s, plan.prices[0], -1);
  EXPECT_EQ(0, totals.intervalVectors);
}
