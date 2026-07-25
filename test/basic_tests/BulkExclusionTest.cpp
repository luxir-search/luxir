#include <array>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "solux/query/BooleanQuery.h"
#include "solux/reader/SkipStats.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/QueryBuild.h"
#include "test/SoluxTest.h"
#include "test/TestUtils.h"

using namespace solux;
using namespace solux::test;

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
  int64_t maxScoreWindows;
};

struct TopRun {
  std::vector<std::string> ids;
  std::vector<float> scores;
  Counters counters;
};

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
              int32_t topK = 10) {
  BulkExclusionGuard bulkGuard(disabled);
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

  TopRun result;
  const auto* docs = req->docList("q");
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
      .maxScoreWindows = SkipStats::maxScoreInnerWindows,
  };
  return result;
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
    docs.push_back(flatdoc(
        "id", std::string(prefix) + std::to_string(doc),
        "body_w", body));
  }
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
}

}  // namespace

class BulkExclusionTest : public SoluxTest {
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

TEST_F(BulkExclusionTest, phraseExclusionFallsBack) {
  addSegment(helper, "p_", 1000, true, true);
  TopRun run = runTop(
      helper, false, "left", "right", "ex0", /*phraseExclusion=*/true);
  EXPECT_GT(run.counters.unsupportedFallbacks, 0);
  EXPECT_EQ(run.counters.engagements, 0);
  EXPECT_EQ(run.counters.maxScoreWindows, 0);
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
