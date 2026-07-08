#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "gtest/gtest.h"
#include "test/SoluxTest.h"
#include "test/TestIndex.h"
#include "solux/query/AllQuery.h"
#include "solux/query/BooleanQuery.h"
#include "solux/query/TermQuery.h"
#include "solux/reader/SkipStats.h"
#include "solux/search/Collector.h"

using namespace solux;
using namespace solux::test;

class BooleanQueryDedupTest : public SoluxTest {
protected:
  struct Hit {
    segdoc doc;
    float score;
  };

  static void buildBodyIndex(TestIndex& testIndex, std::span<const std::string_view> bodies) {
    TestField field(testIndex, "body_w");
    field.startIndexing();
    for (size_t i = 0; i < bodies.size(); i++) {
      field.add((int32_t) i, bodies[i]);
    }
    testIndex.flush();
    field.startReading();
  }

  static std::vector<Hit> collectHits(IndexReader& reader, Query& query) {
    MemPool pool;
    Query::Context context(pool, reader);
    auto* weight = query.createWeight(context, Query::NEED_SCORES);
    std::vector<Hit> hits;
    auto segments = context.topReader.segments();
    for (int32_t segnum = 0; segnum < (int32_t) segments.size(); segnum++) {
      auto* scorer = weight->createScorer(pool, segments[segnum]);
      if (scorer == nullptr) continue;
      for (int32_t doc = scorer->next(); doc != PostingsReader::END; doc = scorer->next()) {
        hits.push_back({segdoc(segnum, doc), scorer->score()});
      }
    }
    return hits;
  }

  static int64_t exactCount(IndexReader& reader, Query& query) {
    MemPool pool;
    Query::Context context(pool, reader);
    auto* weight = query.createWeight(context, 0);
    int64_t count = 0;
    auto segments = context.topReader.segments();
    for (int32_t segnum = 0; segnum < (int32_t) segments.size(); segnum++) {
      int64_t segCount = weight->count(segments[segnum]);
      if (segCount < 0) return -1;
      count += segCount;
    }
    return count;
  }

  static void assertSameDocs(const std::vector<Hit>& expected, const std::vector<Hit>& actual) {
    ASSERT_EQ(expected.size(), actual.size());
    for (size_t i = 0; i < expected.size(); i++) {
      EXPECT_EQ(expected[i].doc.segment(), actual[i].doc.segment()) << "rank " << i;
      EXPECT_EQ(expected[i].doc.docId(), actual[i].doc.docId()) << "rank " << i;
    }
  }

  static void assertSameScoresExact(const std::vector<Hit>& expected, const std::vector<Hit>& actual) {
    assertSameDocs(expected, actual);
    for (size_t i = 0; i < expected.size(); i++) {
      EXPECT_EQ(std::bit_cast<uint32_t>(expected[i].score),
                std::bit_cast<uint32_t>(actual[i].score)) << "rank " << i;
    }
  }

  static void assertSameScoresNear(const std::vector<Hit>& expected, const std::vector<Hit>& actual) {
    assertSameDocs(expected, actual);
    for (size_t i = 0; i < expected.size(); i++) {
      float scale = std::max({std::fabs(expected[i].score), std::fabs(actual[i].score), 1.0f});
      EXPECT_NEAR(expected[i].score, actual[i].score, 1e-6f * scale) << "rank " << i;
    }
  }
};

TEST_F(BooleanQueryDedupTest, optionalDuplicateScoresExactlyLikeBoostTwo) {
  TestIndex testIndex;
  const std::string_view bodies[] = {"a", "a b", "b", "a a"};
  buildBodyIndex(testIndex, bodies);

  std::string field1 = "body_w";
  std::string field2 = "body_w";
  std::string term1 = "a";
  std::string term2 = "a";
  TermQuery dupA1(field1, term1);
  TermQuery dupA2(field2, term2);
  std::vector<Query*> duplicateClauses = {&dupA1, &dupA2};
  BooleanQuery duplicate({}, duplicateClauses, {}, {});

  TermQuery singleA("body_w", "a");
  TermQuery boostTwoA("body_w", "a", 2.0f);

  auto duplicateHits = collectHits(*testIndex.reader, duplicate);
  auto singleHits = collectHits(*testIndex.reader, singleA);
  auto boostTwoHits = collectHits(*testIndex.reader, boostTwoA);

  assertSameScoresExact(boostTwoHits, duplicateHits);
  assertSameDocs(singleHits, duplicateHits);
  for (size_t i = 0; i < singleHits.size(); i++) {
    EXPECT_EQ(std::bit_cast<uint32_t>(singleHits[i].score * 2.0f),
              std::bit_cast<uint32_t>(duplicateHits[i].score)) << "rank " << i;
  }
}

TEST_F(BooleanQueryDedupTest, optionalTripleDuplicateScoresLikeBoostThreeWithinEpsilon) {
  TestIndex testIndex;
  const std::string_view bodies[] = {"a", "a b", "b", "a a"};
  buildBodyIndex(testIndex, bodies);

  TermQuery dupA1("body_w", "a");
  TermQuery dupA2("body_w", "a");
  TermQuery dupA3("body_w", "a");
  std::vector<Query*> duplicateClauses = {&dupA1, &dupA2, &dupA3};
  BooleanQuery duplicate({}, duplicateClauses, {}, {});

  TermQuery boostThreeA("body_w", "a", 3.0f);
  auto duplicateHits = collectHits(*testIndex.reader, duplicate);
  auto boostThreeHits = collectHits(*testIndex.reader, boostThreeA);

  assertSameScoresNear(boostThreeHits, duplicateHits);
}

TEST_F(BooleanQueryDedupTest, optionalDuplicatesPreserveMinShouldMatchSemantics) {
  TestIndex testIndex;
  const std::string_view bodies[] = {"a", "b", "a b", "c"};
  buildBodyIndex(testIndex, bodies);

  TermQuery a1("body_w", "a");
  TermQuery a2("body_w", "a");
  TermQuery b("body_w", "b");
  std::vector<Query*> optionalClauses = {&a1, &a2, &b};
  BooleanQuery query({}, optionalClauses, {}, {}, 2);

  auto hits = collectHits(*testIndex.reader, query);
  ASSERT_EQ(2u, hits.size());
  EXPECT_EQ(0, hits[0].doc.docId());
  EXPECT_EQ(2, hits[1].doc.docId());
}

TEST_F(BooleanQueryDedupTest, mandatoryDuplicateScoresLikeBoostedRequiredTerm) {
  TestIndex testIndex;
  const std::string_view bodies[] = {"a b", "a", "b", "a a b"};
  buildBodyIndex(testIndex, bodies);

  TermQuery dupA1("body_w", "a");
  TermQuery dupA2("body_w", "a");
  TermQuery dupB("body_w", "b");
  std::vector<Query*> duplicateRequired = {&dupA1, &dupA2, &dupB};
  BooleanQuery duplicate(duplicateRequired, {}, {}, {});

  TermQuery singleA("body_w", "a");
  TermQuery singleB("body_w", "b");
  std::vector<Query*> singleRequired = {&singleA, &singleB};
  BooleanQuery single(singleRequired, {}, {}, {});

  TermQuery boostA("body_w", "a", 2.0f);
  TermQuery boostB("body_w", "b");
  std::vector<Query*> boostedRequired = {&boostA, &boostB};
  BooleanQuery boosted(boostedRequired, {}, {}, {});

  auto duplicateHits = collectHits(*testIndex.reader, duplicate);
  auto singleHits = collectHits(*testIndex.reader, single);
  auto boostedHits = collectHits(*testIndex.reader, boosted);

  assertSameDocs(singleHits, duplicateHits);
  assertSameScoresExact(boostedHits, duplicateHits);
}

TEST_F(BooleanQueryDedupTest, prohibitedDuplicateDropsLikeSingleClause) {
  TestIndex testIndex;
  const std::string_view bodies[] = {"a b", "b", "a", "c"};
  buildBodyIndex(testIndex, bodies);

  TermQuery dupRequiredB("body_w", "b");
  TermQuery dupA1("body_w", "a");
  TermQuery dupA2("body_w", "a");
  std::vector<Query*> duplicateRequired = {&dupRequiredB};
  std::vector<Query*> duplicateProhibited = {&dupA1, &dupA2};
  BooleanQuery duplicate(duplicateRequired, {}, duplicateProhibited, {});

  TermQuery singleRequiredB("body_w", "b");
  TermQuery singleA("body_w", "a");
  std::vector<Query*> singleRequired = {&singleRequiredB};
  std::vector<Query*> singleProhibited = {&singleA};
  BooleanQuery single(singleRequired, {}, singleProhibited, {});

  auto duplicateHits = collectHits(*testIndex.reader, duplicate);
  auto singleHits = collectHits(*testIndex.reader, single);

  assertSameScoresExact(singleHits, duplicateHits);
}

TEST_F(BooleanQueryDedupTest, countParityForDuplicateUnionAndSingleTerm) {
  TestIndex testIndex;
  const std::string_view bodies[] = {"to", "to be", "be"};
  buildBodyIndex(testIndex, bodies);

  TermQuery dupTo1("body_w", "to");
  TermQuery dupTo2("body_w", "to");
  std::vector<Query*> duplicateClauses = {&dupTo1, &dupTo2};
  BooleanQuery duplicate({}, duplicateClauses, {}, {});

  TermQuery singleTo("body_w", "to");
  std::vector<Query*> singleClauses = {&singleTo};
  BooleanQuery single({}, singleClauses, {}, {});

  int64_t duplicateCount = exactCount(*testIndex.reader, duplicate);
  int64_t singleCount = exactCount(*testIndex.reader, single);

  EXPECT_EQ(singleCount, duplicateCount);
  EXPECT_EQ(2, duplicateCount);
}

TEST_F(BooleanQueryDedupTest, injectedTermStatsClauseNeverMerges) {
  TestIndex testIndex;
  const std::string_view bodies[] = {"a", "a a", "b"};
  buildBodyIndex(testIndex, bodies);

  Similarity::TermStats injectedStats;
  injectedStats.docFreq = 1;
  injectedStats.totalTermFreq = 1;

  TermQuery injected("body_w", "a", injectedStats);
  TermQuery normal("body_w", "a");
  std::vector<Query*> optionalClauses = {&injected, &normal};
  BooleanQuery duplicate({}, optionalClauses, {}, {});

  TermQuery injectedSolo("body_w", "a", injectedStats);
  TermQuery normalSolo("body_w", "a");
  auto duplicateHits = collectHits(*testIndex.reader, duplicate);
  auto injectedHits = collectHits(*testIndex.reader, injectedSolo);
  auto normalHits = collectHits(*testIndex.reader, normalSolo);

  assertSameDocs(injectedHits, duplicateHits);
  assertSameDocs(normalHits, duplicateHits);
  for (size_t i = 0; i < duplicateHits.size(); i++) {
    float expected = injectedHits[i].score + normalHits[i].score;
    float scale = std::max({std::fabs(expected), std::fabs(duplicateHits[i].score), 1.0f});
    EXPECT_NEAR(expected, duplicateHits[i].score, 1e-6f * scale) << "rank " << i;
  }
}

// Structural probes at weight creation: merging clones into the request pool
// (the query tree is never mutated), and a merged duplicate walks its
// postings once while a non-dedupable duplicate pair still walks twice.
// Score comparisons alone cannot prove the merge: an N=2 merge is
// bit-identical to the unmerged sum (s + s == 2 * s).
TEST_F(BooleanQueryDedupTest, weightDedupClonesAndWalksPostingsOnce) {
  TestIndex testIndex;
  std::vector<std::string> bodyStorage;
  std::vector<std::string_view> bodies;
  for (int32_t i = 0; i < 320; i++) {
    bodyStorage.push_back(i % 3 == 0 ? "a b" : "a");
  }
  for (auto& s : bodyStorage) {
    bodies.push_back(s);
  }
  buildBodyIndex(testIndex, bodies);

  TermQuery a1("body_w", "a", 1.5f);
  TermQuery a2("body_w", "a", 2.5f);
  TermQuery b("body_w", "b");
  std::vector<Query*> optionalClauses = {&a1, &a2, &b};
  BooleanQuery duplicate({}, optionalClauses, {}, {});

  TermQuery aBoosted("body_w", "a", 4.0f);
  TermQuery b2("body_w", "b");
  std::vector<Query*> oracleClauses = {&aBoosted, &b2};
  BooleanQuery oracle({}, oracleClauses, {}, {});

  auto duplicateHits = collectHits(*testIndex.reader, duplicate);
  auto oracleHits = collectHits(*testIndex.reader, oracle);
  assertSameScoresExact(oracleHits, duplicateHits);
  // The query tree survives weight creation untouched.
  EXPECT_EQ(a1.getBoost(), 1.5f);
  EXPECT_EQ(a2.getBoost(), 2.5f);
  EXPECT_EQ(b.getBoost(), 1.0f);

  auto countDecodes = [&](Query& q) {
    bool saved = SkipStats::enabled;
    SkipStats::enabled = true;
    SkipStats::reset();
    collectHits(*testIndex.reader, q);
    int64_t decodes = SkipStats::docBlocksDecoded;
    SkipStats::enabled = saved;
    return decodes;
  };
  TermQuery s1("body_w", "a");
  std::vector<Query*> singleClause = {&s1};
  BooleanQuery single({}, singleClause, {}, {});
  TermQuery d1("body_w", "a");
  TermQuery d2("body_w", "a");
  std::vector<Query*> dupPair = {&d1, &d2};
  BooleanQuery dup({}, dupPair, {}, {});
  Similarity::TermStats stats;
  stats.docFreq = 320;
  stats.totalTermFreq = 320;
  TermQuery i1("body_w", "a", stats);
  TermQuery i2("body_w", "a", stats);
  std::vector<Query*> injectedPair = {&i1, &i2};
  BooleanQuery injected({}, injectedPair, {}, {});

  int64_t singleDecodes = countDecodes(single);
  ASSERT_GT(singleDecodes, 0);
  EXPECT_EQ(countDecodes(dup), singleDecodes);
  EXPECT_EQ(countDecodes(injected), 2 * singleDecodes);
}
