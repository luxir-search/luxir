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
#include "solux/query/BoostQuery.h"
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

TEST_F(BooleanQueryDedupTest, wrappedDuplicateTermsCombineWrapperFactors) {
  TestIndex testIndex;
  const std::string_view bodies[] = {"a", "a b", "b", "a a"};
  buildBodyIndex(testIndex, bodies);

  TermQuery a1("body_w", "a");
  TermQuery a2("body_w", "a");
  BoostQuery boostedA1(&a1, 2.0f);
  BoostQuery boostedA2(&a2, 3.0f);
  std::vector<Query*> duplicates = {&boostedA1, &boostedA2};
  BooleanQuery duplicate({}, duplicates, {}, {});

  TermQuery oracleA("body_w", "a", 5.0f);
  assertSameScoresExact(collectHits(*testIndex.reader, oracleA),
                        collectHits(*testIndex.reader, duplicate));
}

TEST_F(BooleanQueryDedupTest, wrappedDuplicateTermsPreserveMinMatch) {
  TestIndex testIndex;
  const std::string_view bodies[] = {"a", "b", "a b", "c"};
  buildBodyIndex(testIndex, bodies);

  TermQuery a1("body_w", "a");
  TermQuery a2("body_w", "a");
  TermQuery b1("body_w", "b");
  BoostQuery boostedA1(&a1, 2.0f);
  BoostQuery boostedA2(&a2, 3.0f);
  std::vector<Query*> duplicates = {&boostedA1, &boostedA2, &b1};
  BooleanQuery duplicate({}, duplicates, {}, {}, 2);

  TermQuery oracleA("body_w", "a", 5.0f);
  TermQuery oracleB("body_w", "b");
  std::vector<Query*> oracleClauses = {&oracleA, &oracleB};
  BooleanQuery oracle({}, oracleClauses, {}, {}, 1);
  assertSameScoresExact(collectHits(*testIndex.reader, oracle),
                        collectHits(*testIndex.reader, duplicate));
}

// Solux-defined semantics, split by intent: min_match above half the
// clauses is a miss budget (each removed duplicate decrements it, floor 1);
// min_match at or below half is an absolute distinct-word count (kept,
// capped at the deduped clause count).
TEST_F(BooleanQueryDedupTest, minShouldMatchAdjustsForRemovedDuplicatesByIntent) {
  TestIndex testIndex;
  const std::string_view bodies[] = {"a", "b", "a b", "c", "b c"};
  buildBodyIndex(testIndex, bodies);

  // Miss budget (2 of 3 > half): "a a b" mm=2 allows one absence -> merged
  // "a^2 b" mm=1. Docs with a or b match; "c" does not.
  TermQuery a1("body_w", "a");
  TermQuery a2("body_w", "a");
  TermQuery b("body_w", "b");
  std::vector<Query*> optionalClauses = {&a1, &a2, &b};
  BooleanQuery query({}, optionalClauses, {}, {}, 2);
  auto hits = collectHits(*testIndex.reader, query);
  ASSERT_EQ(4u, hits.size());
  EXPECT_EQ(0, hits[0].doc.docId());
  EXPECT_EQ(1, hits[1].doc.docId());
  EXPECT_EQ(2, hits[2].doc.docId());
  EXPECT_EQ(4, hits[3].doc.docId());

  // Absolute count (2 of 5 <= half): "a a a b c" mm=2 means two distinct
  // words -> merged "a^3 b c" keeps mm=2. Only docs with two of {a,b,c}.
  TermQuery w1("body_w", "a");
  TermQuery w2("body_w", "a");
  TermQuery w3("body_w", "a");
  TermQuery wb("body_w", "b");
  TermQuery wc("body_w", "c");
  std::vector<Query*> absClauses = {&w1, &w2, &w3, &wb, &wc};
  BooleanQuery absolute({}, absClauses, {}, {}, 2);
  auto absHits = collectHits(*testIndex.reader, absolute);
  ASSERT_EQ(2u, absHits.size());
  EXPECT_EQ(2, absHits[0].doc.docId());
  EXPECT_EQ(4, absHits[1].doc.docId());

  // Absolute-count cap: "a a a a" mm=2 (2 of 4 <= half) dedups to one
  // clause; mm caps at the clause count so the query stays satisfiable.
  TermQuery c1("body_w", "a");
  TermQuery c2("body_w", "a");
  TermQuery c3("body_w", "a");
  TermQuery c4("body_w", "a");
  std::vector<Query*> capClauses = {&c1, &c2, &c3, &c4};
  BooleanQuery capped({}, capClauses, {}, {}, 2);
  auto capHits = collectHits(*testIndex.reader, capped);
  ASSERT_EQ(2u, capHits.size());
  EXPECT_EQ(0, capHits[0].doc.docId());
  EXPECT_EQ(2, capHits[1].doc.docId());

  // Miss-budget floor: "a a" mm=2 -> mm=1 still matches "a" docs.
  TermQuery f1("body_w", "a");
  TermQuery f2("body_w", "a");
  std::vector<Query*> floorClauses = {&f1, &f2};
  BooleanQuery floored({}, floorClauses, {}, {}, 2);
  auto floorHits = collectHits(*testIndex.reader, floored);
  ASSERT_EQ(2u, floorHits.size());
  EXPECT_EQ(0, floorHits[0].doc.docId());
  EXPECT_EQ(2, floorHits[1].doc.docId());

  // Miss budget, unsatisfiable stays unsatisfiable: "a a" mm=3 -> mm=2 over
  // one clause matches nothing (same as instance counting: max freq 2).
  TermQuery u1("body_w", "a");
  TermQuery u2("body_w", "a");
  std::vector<Query*> unsatClauses = {&u1, &u2};
  BooleanQuery unsat({}, unsatClauses, {}, {}, 3);
  EXPECT_EQ(0u, collectHits(*testIndex.reader, unsat).size());
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
// postings once for optional and mandatory clauses while a non-dedupable
// duplicate pair still walks twice.
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

  TermQuery mandatorySingleTerm("body_w", "a");
  std::vector<Query*> mandatorySingleClause = {&mandatorySingleTerm};
  BooleanQuery mandatorySingle(mandatorySingleClause, {}, {}, {});
  TermQuery mandatoryDup1("body_w", "a");
  TermQuery mandatoryDup2("body_w", "a");
  std::vector<Query*> mandatoryDupPair = {&mandatoryDup1, &mandatoryDup2};
  BooleanQuery mandatoryDup(mandatoryDupPair, {}, {}, {});
  TermQuery mandatoryInjected1("body_w", "a", stats);
  TermQuery mandatoryInjected2("body_w", "a", stats);
  std::vector<Query*> mandatoryInjectedPair = {&mandatoryInjected1, &mandatoryInjected2};
  BooleanQuery mandatoryInjected(mandatoryInjectedPair, {}, {}, {});

  int64_t mandatorySingleDecodes = countDecodes(mandatorySingle);
  ASSERT_GT(mandatorySingleDecodes, 0);
  EXPECT_EQ(countDecodes(mandatoryDup), mandatorySingleDecodes);
  EXPECT_EQ(countDecodes(mandatoryInjected), 2 * mandatorySingleDecodes);
}
