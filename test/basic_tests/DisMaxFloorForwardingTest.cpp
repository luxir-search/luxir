// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <deque>
#include <limits>
#include <random>
#include <string>
#include <vector>

#include "luxir/query/BooleanWeight.h"
#include "luxir/query/DisMaxQuery.h"
#include "luxir/query/TermQuery.h"
#include "luxir/search/Collector.h"
#include "test/CollectionHelper.h"
#include "test/LuxirTest.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

namespace {

using Hits = std::vector<TopDocsCollector::DocScore>;

constexpr int kVocab = 200;

struct TopK {
  Hits hits;
  int64_t visited = 0;
};

class DisMaxFloorForwardingTest : public LuxirTest {
public:
  CollectionHelper helper;
  decltype(helper.getIndexWriter()->snapshots.readers.getReader()) reader;
  std::mt19937 rng{20261109};
  std::deque<std::string> strings;

  std::string token() {
    double u = std::uniform_real_distribution<double>(0.0, 1.0)(rng);
    return "t" + std::to_string((int)std::pow((double)kVocab, u) - 1);
  }

  std::string text(int minTokens, int maxTokens) {
    int n = std::uniform_int_distribution<int>(minTokens, maxTokens)(rng);
    std::string out;
    for (int i = 0; i < n; i++) out += (i ? " " : "") + token();
    return out;
  }

  // Short title, long body, and a field many docs leave out.
  void indexSegment(int docs, int base) {
    std::vector<Doc> batch;
    for (int i = 0; i < docs; i++) {
      std::string id = std::to_string(base + i);
      std::string title = text(1, 4);
      std::string body = text(5, 300);
      bool hasTitle = rng() % 4 != 0;
      bool hasExtra = rng() % 3 == 0;
      if (hasTitle && hasExtra) {
        batch.push_back(flatdoc("id", id, "title_w", title, "body_w", body,
                                "extra_w", text(1, 20)));
      } else if (hasTitle) {
        batch.push_back(flatdoc("id", id, "title_w", title, "body_w", body));
      } else if (hasExtra) {
        batch.push_back(flatdoc("id", id, "body_w", body, "extra_w", text(1, 20)));
      } else {
        batch.push_back(flatdoc("id", id, "body_w", body));
      }
    }
    ASSERT_TRUE(helper.indexAll(batch, UpdateMessage::COMMIT).success);
  }

  void SetUp() override {
    LuxirTest::SetUp();
    int scale = (int)LuxirTest::scaleTestWork(1);
    indexSegment(8000 * scale, 0);
    indexSegment(3000 * scale, 100000);
    indexSegment(500 * scale, 200000);
    reader = helper.getIndexWriter()->snapshots.readers.getReader();
  }

  Query* termQuery(MemPool& pool, const char* field) {
    return pool.make<TermQuery>(field, strings.emplace_back(token()));
  }

  // A random per-field clause: single term, OR of terms, minimum_should_match,
  // conjunction, or required plus optional.
  Query* fieldClause(MemPool& pool, const char* field) {
    auto clause = [&](int n) {
      auto clauses = pool.make_span<Query*>(n);
      for (auto& c : clauses) c = termQuery(pool, field);
      return clauses;
    };
    switch (rng() % 7) {
      case 0: return termQuery(pool, field);
      case 1: return pool.make<BooleanQuery>(std::span<Query*>{}, clause(2),
                                             std::span<Query*>{}, std::span<Query*>{});
      case 2: return pool.make<BooleanQuery>(std::span<Query*>{}, clause(3),
                                             std::span<Query*>{}, std::span<Query*>{});
      case 3: return pool.make<BooleanQuery>(std::span<Query*>{}, clause(4),
                                             std::span<Query*>{}, std::span<Query*>{}, 2);
      case 4: return pool.make<BooleanQuery>(clause(2), std::span<Query*>{},
                                             std::span<Query*>{}, std::span<Query*>{});
      case 5: return pool.make<BooleanQuery>(clause(1), clause(2),
                                             std::span<Query*>{}, std::span<Query*>{});
      default: return pool.make<BooleanQuery>(clause(1), clause(1),
                                              std::span<Query*>{}, std::span<Query*>{});
    }
  }

  Query* randomDisMax(MemPool& pool, float tie) {
    static const char* fields[] = {"title_w", "body_w", "extra_w", "missing_w"};
    int disjuncts = 1 + (int)(rng() % 4);
    auto parts = pool.make_span<Query*>(disjuncts);
    for (int i = 0; i < disjuncts; i++) {
      parts[i] = fieldClause(pool, fields[rng() % 4]);
    }
    return DisMaxQuery::make(pool, parts, tie);
  }

  TopK run(Query& query, int32_t k, bool pruning) {
    MemPool pool;
    Query::Context context(pool, *reader);
    auto* weight = query.createWeight(context,
        Query::NEED_SCORES | (pruning ? Query::ALLOW_PRUNING : 0));
    TopDocsCollector collector(k);
    TopK out;
    auto segments = context.topReader.segments();
    for (int32_t segnum = 0; segnum < (int32_t)segments.size(); segnum++) {
      auto& segment = segments[segnum];
      auto* scorer = weight->createScorer(pool, segment);
      if (scorer == nullptr) continue;
      DocSet* live = segment.liveDocs() ? &segment.liveDocs()->docset() : nullptr;
      collectTopK(segnum, scorer, live, nullptr, collector, pruning);
      if (auto* dismax = dynamic_cast<BooleanQuery::MaxScoreDisjunctionScorerT<true>*>(scorer)) {
        out.visited += dismax->visited();
      }
    }
    auto sorted = collector.sort();
    out.hits = Hits(sorted.begin(), sorted.end());
    return out;
  }
};

TEST_F(DisMaxFloorForwardingTest, prunedTopKMatchesExhaustive) {
  int iterations = (int)std::min<int64_t>(600, LuxirTest::scaleTestWork(600));
  for (int it = 0; it < iterations; it++) {
    MemPool pool;
    float tie = std::array{0.0f, 0.0f, 0.1f, 0.3f, 0.5f, 0.9f}[rng() % 6];
    Query* query = randomDisMax(pool, tie);
    int32_t k = std::array{1, 3, 10, 50}[rng() % 4];
    TopK exhaustive = run(*query, k, false);
    TopK pruned = run(*query, k, true);
    ASSERT_EQ(exhaustive.hits.size(), pruned.hits.size()) << "iteration " << it;
    for (size_t r = 0; r < pruned.hits.size(); r++) {
      ASSERT_EQ(exhaustive.hits[r].doc, pruned.hits[r].doc)
          << "iteration " << it << " tie " << tie << " rank " << r;
      ASSERT_EQ(std::bit_cast<uint32_t>(exhaustive.hits[r].score),
                std::bit_cast<uint32_t>(pruned.hits[r].score))
          << "iteration " << it << " tie " << tie << " rank " << r
          << " " << exhaustive.hits[r].score << " " << pruned.hits[r].score;
    }
  }
}

// The floor has to reach the per-field disjunctions, whose own MaxScore split
// removes docs that a dis_max split over whole fields cannot.
TEST_F(DisMaxFloorForwardingTest, floorReachesPerFieldDisjuncts) {
  using DisMaxScorer = BooleanQuery::MaxScoreDisjunctionScorerT<true>;
  MemPool pool;
  auto fieldQuery = [&](const char* field) -> Query* {
    auto terms = pool.make_span<Query*>(2);
    terms[0] = pool.make<TermQuery>(field, "t0");
    terms[1] = pool.make<TermQuery>(field, "t1");
    return pool.make<BooleanQuery>(std::span<Query*>{}, terms,
                                   std::span<Query*>{}, std::span<Query*>{});
  };
  Query* parts[] = {fieldQuery("title_w"), fieldQuery("body_w")};
  auto* dismax = DisMaxQuery::make(pool, parts, 0.0f);
  DisMaxScorer::disableDisMaxFloorForwardingForTests = true;
  TopK off = run(*dismax, 1, true);
  DisMaxScorer::disableDisMaxFloorForwardingForTests = false;
  TopK on = run(*dismax, 1, true);
  ASSERT_EQ(off.hits.size(), on.hits.size());
  EXPECT_EQ(off.hits[0].doc, on.hits[0].doc);
  EXPECT_LT(on.visited, off.visited);
}

TEST(DisMaxClauseFloor, edgeCases) {
  constexpr float inf = std::numeric_limits<float>::infinity();
  double factor = 1.0 + 6.0 * 0x1p-24;
  auto none = [](float floor) { return floor == -inf; };
  EXPECT_TRUE(none(disMaxClauseFloor(inf, 1.0f, 1.0, 0.5f, factor)));
  EXPECT_TRUE(none(disMaxClauseFloor(std::nanf(""), 1.0f, 1.0, 0.5f, factor)));
  EXPECT_TRUE(none(disMaxClauseFloor(10.0f, inf, 1.0, 0.5f, factor)));
  // A doc missing this disjunct can reach theta: no floor.
  EXPECT_TRUE(none(disMaxClauseFloor(1.0f, 2.0f, 2.0, 0.3f, factor)));
  // theta -> required, floor = theta - tie * others.
  float required = disMaxClauseFloor(5.0f, 2.0f, 2.0, 0.5f, factor);
  EXPECT_LT((double)required, 4.0);
  EXPECT_GT((double)required, 3.99);
  // theta above the others' combination but within reach of best: exact root
  // (theta - without) / tie.
  float root = disMaxClauseFloor(4.0f, 3.0f, 3.5, 0.5f, factor);
  EXPECT_LT((double)root, (4.0 - 0.5 * 3.0 - 0.5 * 3.5) / 0.5);
  EXPECT_GT((double)root, (4.0 - 0.5 * 3.0 - 0.5 * 3.5) / 0.5 * 0.999);
}

// An upper-bound check against the dis_max score function: every score below the
// floor, with the others at their maxima, falls short of theta.
TEST(DisMaxClauseFloor, floorIsExactForTheScoreFunction) {
  auto score = [](float s, float best, float sum, float tie) {
    float b = std::max(s, best);
    return (1.0f - tie) * b + tie * (s + sum);
  };
  double factor = 1.0 + 6.0 * 0x1p-24;
  for (float tie : {0.01f, 0.1f, 0.3f, 0.5f, 0.9f, 0.99f}) {
    for (float theta : {0.5f, 1.0f, 3.7f, 12.0f}) {
      for (float best : {0.0f, 0.2f, 1.0f, 3.0f, 8.0f}) {
        for (float sum : {best, best * 1.5f, best * 3.0f}) {
          float floor = disMaxClauseFloor(theta, best, (double)sum, tie, factor);
          if (floor == -std::numeric_limits<float>::infinity()) continue;
          float below = std::nextafter(floor, 0.0f);
          ASSERT_LT(score(below, best, sum, tie), theta)
              << tie << " " << theta << " " << best << " " << sum;
          // Docs missing the disjunct cannot compete either.
          ASSERT_LT(score(0.0f, best, sum, tie), theta);
        }
      }
    }
  }
}

}  // namespace
