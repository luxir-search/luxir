#include <array>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "luxir/query/BooleanQuery.h"
#include "luxir/reader/SkipStats.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/QueryBuild.h"
#include "test/LuxirTest.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

namespace {

struct BulkExclusionGuard {
  bool saved = BooleanQuery::disableBulkExclusionForTests;

  explicit BulkExclusionGuard(bool disabled) {
    BooleanQuery::disableBulkExclusionForTests = disabled;
  }

  ~BulkExclusionGuard() {
    BooleanQuery::disableBulkExclusionForTests = saved;
  }
};

struct ProhibitedCacheGuard {
  bool saved = QueryPrep::disableProhibitedCacheForTests;

  explicit ProhibitedCacheGuard(bool disabled) {
    QueryPrep::disableProhibitedCacheForTests = disabled;
  }

  ~ProhibitedCacheGuard() {
    QueryPrep::disableProhibitedCacheForTests = saved;
  }
};

struct WholeMembershipGuard {
  bool saved = QueryPrep::disableWholeMembershipPlanForTests;

  explicit WholeMembershipGuard(bool disabled) {
    QueryPrep::disableWholeMembershipPlanForTests = disabled;
  }

  ~WholeMembershipGuard() {
    QueryPrep::disableWholeMembershipPlanForTests = saved;
  }
};

struct ProhibitedVerificationCacheGuard {
  bool saved = BooleanQuery::disableProhibitedVerificationCacheForTests;

  explicit ProhibitedVerificationCacheGuard(bool disabled) {
    BooleanQuery::disableProhibitedVerificationCacheForTests = disabled;
  }

  ~ProhibitedVerificationCacheGuard() {
    BooleanQuery::disableProhibitedVerificationCacheForTests = saved;
  }
};

struct SkipStatsGuard {
  bool saved = SkipStats::enabled;

  SkipStatsGuard() {
    SkipStats::enabled = true;
    SkipStats::reset();
  }

  ~SkipStatsGuard() {
    SkipStats::enabled = saved;
    SkipStats::reset();
  }
};

struct Counters {
  int64_t engagements;
  int64_t windows;
  int64_t fills;
  int64_t disabledFallbacks;
  int64_t shapeFallbacks;
  int64_t positiveSegmentFallbacks;
  int64_t unsupportedFallbacks;
  int64_t phraseAdmits;
  int64_t phraseRejects;
  int64_t maxScoreWindows;
  int64_t cachePullHits;
  int64_t cachePullBuilds;
  int64_t cachePullBypasses;
  int64_t cachePullRoutingBypasses;
  int64_t cacheDenseCountHits;
  int64_t cacheDenseCountBuilds;
  int64_t cacheDenseCountBypasses;
  int64_t cacheDenseCountRoutingBypasses;
  int64_t cacheMaxScoreHits;
  int64_t cacheMaxScoreBuilds;
  int64_t cacheMaxScoreBypasses;
  int64_t cacheMaxScoreRoutingBypasses;
  int64_t cacheCandidateHits;
  int64_t cacheCandidateBuilds;
  int64_t cacheCandidateBypasses;
  int64_t cacheCandidateRoutingBypasses;
};

struct TopRun {
  std::vector<std::string> ids;
  std::vector<float> scores;
  Counters counters;
};

TopRun collectTopRun(const LocalReq& req) {
  TopRun result;
  const auto* docs = req.docList("q");
  if (docs != nullptr) {
    const auto* ids = docs->columns.find("id");
    const auto* scores = docs->columns.find("_score_");
    if (ids != nullptr && scores != nullptr) {
      const auto& idValues = std::get<api::ColStr>(ids->kind).v;
      const auto& scoreValues = std::get<api::ColFloat>(scores->kind).v;
      for (std::string_view id : idValues) {
        result.ids.emplace_back(id);
      }
      result.scores.assign(scoreValues.begin(), scoreValues.end());
    }
  }
  result.counters = {
      .engagements = SkipStats::bulkExclusionEngagements,
      .windows = SkipStats::bulkExclusionWindows,
      .fills = SkipStats::bulkExclusionFills,
      .disabledFallbacks = SkipStats::bulkExclusionDisabledFallbacks,
      .shapeFallbacks = SkipStats::bulkExclusionShapeFallbacks,
      .positiveSegmentFallbacks =
          SkipStats::bulkExclusionPositiveSegmentFallbacks,
      .unsupportedFallbacks = SkipStats::bulkExclusionUnsupportedFallbacks,
      .phraseAdmits = SkipStats::phraseExclusionWindowAdmits,
      .phraseRejects = SkipStats::phraseExclusionWindowRejects,
      .maxScoreWindows = SkipStats::maxScoreInnerWindows,
      .cachePullHits = SkipStats::prohibitedCachePullHits,
      .cachePullBuilds = SkipStats::prohibitedCachePullBuilds,
      .cachePullBypasses = SkipStats::prohibitedCachePullBypasses,
      .cachePullRoutingBypasses =
          SkipStats::prohibitedCachePullRoutingBypasses,
      .cacheDenseCountHits = SkipStats::prohibitedCacheDenseCountHits,
      .cacheDenseCountBuilds = SkipStats::prohibitedCacheDenseCountBuilds,
      .cacheDenseCountBypasses = SkipStats::prohibitedCacheDenseCountBypasses,
      .cacheDenseCountRoutingBypasses =
          SkipStats::prohibitedCacheDenseCountRoutingBypasses,
      .cacheMaxScoreHits = SkipStats::prohibitedCacheMaxScoreHits,
      .cacheMaxScoreBuilds = SkipStats::prohibitedCacheMaxScoreBuilds,
      .cacheMaxScoreBypasses = SkipStats::prohibitedCacheMaxScoreBypasses,
      .cacheMaxScoreRoutingBypasses =
          SkipStats::prohibitedCacheMaxScoreRoutingBypasses,
      .cacheCandidateHits = SkipStats::prohibitedCacheCandidateHits,
      .cacheCandidateBuilds = SkipStats::prohibitedCacheCandidateBuilds,
      .cacheCandidateBypasses = SkipStats::prohibitedCacheCandidateBypasses,
      .cacheCandidateRoutingBypasses =
          SkipStats::prohibitedCacheCandidateRoutingBypasses,
  };
  return result;
}

api::Query scoredDisjunction(
    std::pmr::memory_resource& mr, std::string_view left,
    std::string_view right, std::span<const api::Query> prohibited) {
  std::array<api::Query, 2> optional = {
      qb::match(mr, "body_w", left),
      qb::match(mr, "body_w", right)};
  return qb::boolean(mr, {}, optional, prohibited, {}, 1);
}

TopRun runTop(CollectionHelper& helper, bool disabled,
              std::string_view left, std::string_view right,
              std::string_view exclusion = "ex0",
              bool phraseExclusion = false, bool disjunctiveExclusion = false,
              int32_t topK = 10, bool disableProhibitedCache = false) {
  BulkExclusionGuard bulkGuard(disabled);
  ProhibitedCacheGuard cacheGuard(disableProhibitedCache);
  SkipStatsGuard statsGuard;
  auto req = localReq(helper.getSearchEngine());
  req->collection("main");
  auto& top = req->topDocs("q");
  top.getScores().fields({"id"}).limit(topK);

  std::vector<api::Query> prohibited;
  if (phraseExclusion) {
    prohibited.push_back(
        qb::phraseWords(top.mr(), "body_w", {"exclude", "phrase"}));
  } else if (disjunctiveExclusion) {
    std::array<api::Query, 2> terms = {
        qb::match(top.mr(), "body_w", "ex0"),
        qb::match(top.mr(), "body_w", "ex1")};
    prohibited.push_back(qb::boolean(top.mr(), {}, terms));
  } else {
    prohibited.push_back(qb::match(top.mr(), "body_w", exclusion));
  }
  top.rawQuery() = scoredDisjunction(
      top.mr(), left, right, prohibited);
  req->execute(false);
  EXPECT_TRUE(req->ok()) << req->errorMsg();

  return collectTopRun(*req);
}

TopRun runPullTop(CollectionHelper& helper, std::string_view exclusion,
                  bool disableProhibitedCache = false) {
  ProhibitedCacheGuard cacheGuard(disableProhibitedCache);
  SkipStatsGuard statsGuard;
  auto req = localReq(helper.getSearchEngine());
  req->collection("main");
  auto& top = req->topDocs("q");
  top.getScores().fields({"id"}).limit(20);
  top.rawQuery() = qb::boolean(
      top.mr(), {qb::match(top.mr(), "body_w", "left")}, {},
      {qb::match(top.mr(), "body_w", exclusion)});
  req->execute(false);
  EXPECT_TRUE(req->ok()) << req->errorMsg();
  return collectTopRun(*req);
}

TopRun runCandidateTop(CollectionHelper& helper,
                       std::string_view exclusion = "ex0",
                       bool disableProhibitedCache = false) {
  ProhibitedCacheGuard cacheGuard(disableProhibitedCache);
  SkipStatsGuard statsGuard;
  auto req = localReq(helper.getSearchEngine());
  req->collection("main");
  auto& top = req->topDocs("q");
  top.matchFilter("selection", "body_w", "ex1");
  top.getScores().fields({"id"}).limit(20);
  top.rawQuery() = qb::boolean(
      top.mr(), {qb::match(top.mr(), "body_w", "left")}, {},
      {qb::match(top.mr(), "body_w", exclusion)});
  req->execute(false);
  EXPECT_TRUE(req->ok()) << req->errorMsg();
  return collectTopRun(*req);
}

enum class ExclusionShape : uint8_t {
  MULTIPLE,
  NESTED_OR,
  EMPTY,
  MATCH_ALL,
};

TopRun runExclusionShape(CollectionHelper& helper, ExclusionShape shape,
                         bool disableProhibitedCache = false) {
  ProhibitedCacheGuard cacheGuard(disableProhibitedCache);
  SkipStatsGuard statsGuard;
  auto req = localReq(helper.getSearchEngine());
  req->collection("main");
  auto& top = req->topDocs("q");
  top.getScores().fields({"id"}).limit(20);
  std::vector<api::Query> prohibited;
  if (shape == ExclusionShape::MULTIPLE) {
    prohibited.push_back(qb::match(top.mr(), "body_w", "ex0"));
    prohibited.push_back(qb::match(top.mr(), "body_w", "ex1"));
  } else if (shape == ExclusionShape::NESTED_OR) {
    std::array<api::Query, 2> members = {
        qb::match(top.mr(), "body_w", "ex0"),
        qb::match(top.mr(), "body_w", "ex1")};
    prohibited.push_back(qb::boolean(top.mr(), {}, members));
  } else if (shape == ExclusionShape::EMPTY) {
    prohibited.push_back(qb::match(top.mr(), "body_w", "missing_ex"));
  } else {
    prohibited.push_back(qb::all());
  }
  top.rawQuery() = qb::boolean(
      top.mr(), {qb::match(top.mr(), "body_w", "left")}, {}, prohibited);
  req->execute(false);
  EXPECT_TRUE(req->ok()) << req->errorMsg();
  return collectTopRun(*req);
}

struct CountRun {
  int64_t count = 0;
  int64_t hits = 0;
  int64_t builds = 0;
  int64_t bypasses = 0;
  int64_t routingBypasses = 0;
};

CountRun runCachedCount(CollectionHelper& helper,
                        bool disableProhibitedCache = false) {
  ProhibitedCacheGuard cacheGuard(disableProhibitedCache);
  WholeMembershipGuard wholeGuard(true);
  SkipStatsGuard statsGuard;
  auto req = localReq(helper.getSearchEngine());
  req->collection("main");
  auto& top = req->topDocs("q").getNumber().limit(0);
  top.rawQuery() = qb::boolean(
      top.mr(),
      {qb::match(top.mr(), "body_w", "left"),
       qb::match(top.mr(), "body_w", "right")}, {},
      {qb::match(top.mr(), "body_w", "ex0")});
  req->execute(false);
  EXPECT_TRUE(req->ok()) << req->errorMsg();
  return {
      req->getMatchCount(),
      SkipStats::prohibitedCacheDenseCountHits,
      SkipStats::prohibitedCacheDenseCountBuilds,
      SkipStats::prohibitedCacheDenseCountBypasses,
      SkipStats::prohibitedCacheDenseCountRoutingBypasses,
  };
}

int64_t runCount(CollectionHelper& helper, bool disabled,
                 std::string_view left, std::string_view right,
                 std::string_view exclusion) {
  BulkExclusionGuard bulkGuard(disabled);
  auto req = localReq(helper.getSearchEngine());
  req->collection("main");
  auto& top = req->topDocs("q").getNumber().limit(0);
  std::array<api::Query, 1> prohibited = {
      qb::match(top.mr(), "body_w", exclusion)};
  top.rawQuery() = scoredDisjunction(
      top.mr(), left, right, prohibited);
  req->execute(false);
  EXPECT_TRUE(req->ok()) << req->errorMsg();
  return req->getMatchCount();
}

void expectSameTop(const TopRun& expected, const TopRun& actual) {
  EXPECT_EQ(expected.ids, actual.ids);
  ASSERT_EQ(expected.scores.size(), actual.scores.size());
  for (size_t i = 0; i < expected.scores.size(); i++) {
    EXPECT_FLOAT_EQ(expected.scores[i], actual.scores[i]) << "hit=" << i;
  }
}

void addSegment(CollectionHelper& helper, std::string_view prefix,
                int32_t count, bool includeRight, bool includeExclusion) {
  std::vector<Doc> docs;
  docs.reserve((size_t) count);
  for (int32_t doc = 0; doc < count; doc++) {
    std::string body = "filler left";
    if (includeRight && (doc % 3) != 0) {
      body += " right";
    }
    if ((doc % 5) == 0) {
      body += " left left";
    }
    if (includeExclusion && (doc % 7) == 0) {
      body += " ex0";
    }
    if ((doc % 11) == 0) {
      body += " ex1";
    }
    if ((doc % 13) == 0) {
      body += " exclude phrase";
    }
    if ((doc % 100) == 0) {
      body += " array_ex";
    }
    if (doc == 1) {
      body += " sparse_ex";
    }
    docs.push_back(flatdoc(
        "id", std::string(prefix) + std::to_string(doc),
        "body_w", body));
  }
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
}

}  // namespace

class BulkExclusionTest : public LuxirTest {
public:
  CollectionHelper helper;
};

TEST_F(BulkExclusionTest, scoredDisjunctionEngagesAndMatchesDisabledOracle) {
  addSegment(helper, "a_", 3000, true, true);
  addSegment(helper, "b_", 3000, true, true);

  TopRun bulk = runTop(helper, false, "left", "right");
  TopRun pull = runTop(helper, true, "left", "right");
  expectSameTop(pull, bulk);
  EXPECT_GT(bulk.counters.engagements, 0);
  EXPECT_GT(bulk.counters.windows, 0);
  EXPECT_GT(bulk.counters.fills, 0);
  EXPECT_GT(bulk.counters.maxScoreWindows, 0);
  EXPECT_GT(pull.counters.disabledFallbacks, 0);
  EXPECT_EQ(pull.counters.engagements, 0);
}

TEST_F(BulkExclusionTest, phraseExclusionEngagesAndMatchesDisabledOracle) {
  addSegment(helper, "p_", 1000, true, true);
  addSegment(helper, "q_", 1000, true, true);
  TopRun bulk = runTop(
      helper, false, "left", "right", "ex0", /*phraseExclusion=*/true);
  TopRun pull = runTop(
      helper, true, "left", "right", "ex0", /*phraseExclusion=*/true);
  expectSameTop(pull, bulk);
  EXPECT_GT(bulk.counters.phraseAdmits, 0);
  EXPECT_EQ(bulk.counters.phraseRejects, 0);
  EXPECT_EQ(bulk.counters.unsupportedFallbacks, 0);
  EXPECT_GT(bulk.counters.engagements, 0);
  EXPECT_GT(bulk.counters.windows, 0);
  EXPECT_GT(bulk.counters.fills, 0);
  EXPECT_GT(bulk.counters.maxScoreWindows, 0);
  EXPECT_GT(pull.counters.disabledFallbacks, 0);
}

TEST_F(BulkExclusionTest, singleTermPositiveKeepsPullPath) {
  addSegment(helper, "s_", 1000, true, true);
  BulkExclusionGuard bulkGuard(false);
  SkipStatsGuard statsGuard;
  auto req = localReq(helper.getSearchEngine());
  req->collection("main");
  auto& top = req->topDocs("q");
  top.getScores().fields({"id"}).limit(10);
  top.rawQuery() = qb::boolean(
      top.mr(), {qb::match(top.mr(), "body_w", "left")}, {},
      {qb::match(top.mr(), "body_w", "ex0")});
  req->execute(false);
  ASSERT_OK(req);
  EXPECT_GT(SkipStats::bulkExclusionShapeFallbacks, 0);
  EXPECT_EQ(SkipStats::bulkExclusionEngagements, 0);
  EXPECT_EQ(SkipStats::maxScoreInnerWindows, 0);
}

TEST_F(BulkExclusionTest, segmentAdmissionIsIndependent) {
  addSegment(helper, "full_", 800, true, true);
  addSegment(helper, "one_", 800, false, true);
  addSegment(helper, "noex_", 800, true, false);

  TopRun bulk = runTop(
      helper, false, "left", "right", "ex0", false,
      /*disjunctiveExclusion=*/true, 100);
  TopRun pull = runTop(
      helper, true, "left", "right", "ex0", false,
      /*disjunctiveExclusion=*/true, 100);
  expectSameTop(pull, bulk);
  EXPECT_GT(bulk.counters.engagements, 0);
  EXPECT_GT(bulk.counters.positiveSegmentFallbacks, 0);
  EXPECT_GT(bulk.counters.fills, bulk.counters.windows);
}

TEST_F(BulkExclusionTest, cachedMaxScoreExclusionBuildsThenHits) {
  addSegment(helper, "m0_", 5000, true, true);
  addSegment(helper, "m1_", 5000, true, true);

  TopRun oracle = runTop(
      helper, false, "left", "right", "ex0", false, false, 100, true);
  TopRun bypass = runTop(
      helper, false, "left", "right", "ex0", false, false, 100);
  TopRun build = runTop(
      helper, false, "left", "right", "ex0", false, false, 100);
  TopRun hit = runTop(
      helper, false, "left", "right", "ex0", false, false, 100);

  expectSameTop(oracle, bypass);
  expectSameTop(oracle, build);
  expectSameTop(oracle, hit);
  EXPECT_GT(bypass.counters.cacheMaxScoreBypasses, 0);
  EXPECT_GT(build.counters.cacheMaxScoreBuilds, 0);
  EXPECT_GT(hit.counters.cacheMaxScoreHits, 0);
  EXPECT_GT(hit.counters.engagements, 0);
}

TEST_F(BulkExclusionTest, verificationRoutedPhraseExclusionBuildsThenHits) {
  ProhibitedVerificationCacheGuard verificationGuard(false);
  addSegment(helper, "ph_", 5000, true, true);

  TopRun oracle = runTop(
      helper, false, "left", "right", "ex0", true, false, 100, true);
  TopRun bypass = runTop(
      helper, false, "left", "right", "ex0", true, false, 100);
  TopRun build = runTop(
      helper, false, "left", "right", "ex0", true, false, 100);
  TopRun hit = runTop(
      helper, false, "left", "right", "ex0", true, false, 100);
  expectSameTop(oracle, bypass);
  expectSameTop(oracle, build);
  expectSameTop(oracle, hit);
  EXPECT_GT(bypass.counters.cacheMaxScoreBypasses, 0);
  EXPECT_GT(build.counters.cacheMaxScoreBuilds, 0);
  EXPECT_GT(hit.counters.cacheMaxScoreHits, 0);
  EXPECT_EQ(0, hit.counters.phraseAdmits);
}

TEST_F(BulkExclusionTest, pullConsumesCachedBitsetAndArrayExclusions) {
  addSegment(helper, "p_", 10000, true, true);

  TopRun denseOracle = runPullTop(helper, "ex0", true);
  runPullTop(helper, "ex0");
  runPullTop(helper, "ex0");
  TopRun denseHit = runPullTop(helper, "ex0");
  expectSameTop(denseOracle, denseHit);
  EXPECT_GT(denseHit.counters.cachePullHits, 0);

  TopRun arrayOracle = runPullTop(helper, "array_ex", true);
  runPullTop(helper, "array_ex");
  runPullTop(helper, "array_ex");
  TopRun arrayHit = runPullTop(helper, "array_ex");
  expectSameTop(arrayOracle, arrayHit);
  EXPECT_GT(arrayHit.counters.cachePullHits, 0);
}

TEST_F(BulkExclusionTest, denseCountConsumesCachedExclusion) {
  addSegment(helper, "c0_", 5000, true, true);
  addSegment(helper, "c1_", 5000, true, true);

  CountRun oracle = runCachedCount(helper, true);
  CountRun bypass = runCachedCount(helper);
  CountRun build = runCachedCount(helper);
  CountRun hit = runCachedCount(helper);
  EXPECT_EQ(oracle.count, bypass.count);
  EXPECT_EQ(oracle.count, build.count);
  EXPECT_EQ(oracle.count, hit.count);
  EXPECT_GT(bypass.bypasses, 0);
  EXPECT_GT(build.builds, 0);
  EXPECT_GT(hit.hits, 0);
}

TEST_F(BulkExclusionTest, candidateConjunctionProbesCachedExclusion) {
  addSegment(helper, "b_", 10000, true, true);

  TopRun oracle = runCandidateTop(helper, "ex0", true);
  TopRun bypass = runCandidateTop(helper);
  TopRun build = runCandidateTop(helper);
  TopRun hit = runCandidateTop(helper);
  expectSameTop(oracle, bypass);
  expectSameTop(oracle, build);
  expectSameTop(oracle, hit);
  EXPECT_GT(bypass.counters.cacheCandidateBypasses, 0);
  EXPECT_GT(build.counters.cacheCandidateBuilds, 0);
  EXPECT_GT(hit.counters.cacheCandidateHits, 0);

  TopRun arrayOracle = runCandidateTop(helper, "array_ex", true);
  runCandidateTop(helper, "array_ex");
  runCandidateTop(helper, "array_ex");
  TopRun arrayHit = runCandidateTop(helper, "array_ex");
  expectSameTop(arrayOracle, arrayHit);
  EXPECT_GT(arrayHit.counters.cacheCandidateHits, 0);
}

TEST_F(BulkExclusionTest, sparseExclusionBypassesBeforeSighting) {
  addSegment(helper, "s_", 10000, true, true);
  auto reader = helper.getIndexWriter()->getIndexReader();
  auto* cache = reader->filterCache();
  ASSERT_NE(nullptr, cache);
  size_t entriesBefore = cache->entryCountForTest();

  TopRun first = runPullTop(helper, "sparse_ex");
  TopRun second = runPullTop(helper, "sparse_ex");
  TopRun third = runPullTop(helper, "sparse_ex");
  EXPECT_GT(first.counters.cachePullRoutingBypasses, 0);
  EXPECT_GT(second.counters.cachePullRoutingBypasses, 0);
  EXPECT_GT(third.counters.cachePullRoutingBypasses, 0);
  EXPECT_EQ(0, third.counters.cachePullHits);
  EXPECT_EQ(0, third.counters.cachePullBuilds);
  EXPECT_EQ(0, third.counters.cachePullBypasses);
  EXPECT_EQ(entriesBefore, cache->entryCountForTest());
}

TEST_F(BulkExclusionTest, multipleNestedEmptyAndMatchAllExclusions) {
  addSegment(helper, "n_", 10000, true, true);

  TopRun multipleOracle = runExclusionShape(
      helper, ExclusionShape::MULTIPLE, true);
  runExclusionShape(helper, ExclusionShape::MULTIPLE);
  runExclusionShape(helper, ExclusionShape::MULTIPLE);
  TopRun multipleHit = runExclusionShape(helper, ExclusionShape::MULTIPLE);
  expectSameTop(multipleOracle, multipleHit);
  EXPECT_GE(multipleHit.counters.cachePullHits, 2);

  TopRun nestedOracle = runExclusionShape(
      helper, ExclusionShape::NESTED_OR, true);
  runExclusionShape(helper, ExclusionShape::NESTED_OR);
  runExclusionShape(helper, ExclusionShape::NESTED_OR);
  TopRun nestedHit = runExclusionShape(helper, ExclusionShape::NESTED_OR);
  expectSameTop(nestedOracle, nestedHit);
  EXPECT_GT(nestedHit.counters.cachePullHits, 0);
  EXPECT_EQ(multipleHit.ids, nestedHit.ids);

  auto reader = helper.getIndexWriter()->getIndexReader();
  auto* cache = reader->filterCache();
  ASSERT_NE(nullptr, cache);
  size_t entriesBeforeConstants = cache->entryCountForTest();
  TopRun emptyOracle = runExclusionShape(
      helper, ExclusionShape::EMPTY, true);
  TopRun empty = runExclusionShape(helper, ExclusionShape::EMPTY);
  expectSameTop(emptyOracle, empty);
  TopRun all = runExclusionShape(helper, ExclusionShape::MATCH_ALL);
  EXPECT_TRUE(all.ids.empty());
  EXPECT_EQ(entriesBeforeConstants, cache->entryCountForTest());
}

TEST_F(BulkExclusionTest, cachedExclusionComposesDeleteLiveness) {
  addSegment(helper, "d_", 10000, true, true);
  runPullTop(helper, "ex0");
  runPullTop(helper, "ex0");
  TopRun beforeDelete = runPullTop(helper, "ex0");
  EXPECT_GT(beforeDelete.counters.cachePullHits, 0);

  ASSERT_TRUE(helper.deleteById("d_0", UpdateMessage::COMMIT).success);
  TopRun oracle = runPullTop(helper, "ex0", true);
  TopRun hit = runPullTop(helper, "ex0");
  expectSameTop(oracle, hit);
  EXPECT_GT(hit.counters.cachePullHits, 0);
}

TEST_F(BulkExclusionTest, randomizedCountAndScoreMatchDisabledOracle) {
  uint32_t state = 0x6d2b79f5u;
  auto random = [&]() {
    state = state * 1664525u + 1013904223u;
    return state;
  };
  constexpr std::array<std::string_view, 5> positives = {
      "p0", "p1", "p2", "p3", "p4"};
  constexpr std::array<std::string_view, 4> exclusions = {
      "x0", "x1", "x2", "x3"};

  for (int32_t segment = 0; segment < 3; segment++) {
    std::vector<Doc> docs;
    for (int32_t doc = 0; doc < 900; doc++) {
      std::string body = "filler";
      for (std::string_view term : positives) {
        if ((random() % 100) < 55) {
          body += " ";
          body += term;
          if ((random() % 5) == 0) {
            body += " ";
            body += term;
          }
        }
      }
      for (std::string_view term : exclusions) {
        if ((random() % 100) < 23) {
          body += " ";
          body += term;
        }
      }
      docs.push_back(flatdoc(
          "id", "r_" + std::to_string(segment) + "_"
              + std::to_string(doc),
          "body_w", body));
    }
    ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
  }

  for (int32_t iter = 0; iter < 12; iter++) {
    size_t leftIndex = (size_t) (random() % positives.size());
    size_t rightIndex = (size_t) (random() % positives.size());
    if (leftIndex == rightIndex) {
      rightIndex = (rightIndex + 1) % positives.size();
    }
    std::string_view left = positives[leftIndex];
    std::string_view right = positives[rightIndex];
    std::string_view exclusion =
        exclusions[(size_t) (random() % exclusions.size())];

    TopRun bulk = runTop(
        helper, false, left, right, exclusion, false, false, 50);
    TopRun pull = runTop(
        helper, true, left, right, exclusion, false, false, 50);
    expectSameTop(pull, bulk);
    EXPECT_EQ(runCount(helper, false, left, right, exclusion),
              runCount(helper, true, left, right, exclusion));
  }
}
