#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "solux/query/BooleanQuery.h"
#include "solux/reader/DocsEnum.h"
#include "solux/reader/SkipStats.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/QueryBuild.h"
#include "test/SoluxTest.h"
#include "test/TestUtils.h"

using namespace solux;
using namespace solux::test;

namespace {

struct NegatedCountGuard {
  bool saved =
      BooleanQuery::ConjunctionBulkScorer::disableNegatedCountForTests;

  explicit NegatedCountGuard(bool disabled) {
    BooleanQuery::ConjunctionBulkScorer::disableNegatedCountForTests = disabled;
  }

  ~NegatedCountGuard() {
    BooleanQuery::ConjunctionBulkScorer::disableNegatedCountForTests = saved;
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

enum class PhraseShape {
  ORDINARY,
  REPEATED
};

void addAdmittedSegment(CollectionHelper& helper, std::string_view prefix) {
  constexpr int32_t kDocs = DocsEnumMeta::L1_DOCS + 257;
  std::vector<Doc> docs;
  docs.reserve(kDocs);
  for (int32_t doc = 0; doc < kDocs; doc++) {
    std::string body = "positive filler";
    if ((doc % 17) == 0) {
      body += (doc % 34) == 0 ? " exa exb" : " exa gap exb";
    }
    if ((doc % 19) == 0) {
      body += " termex";
    }
    if ((doc % 23) == 0) {
      body += (doc % 46) == 0 ? " rep middle rep"
                              : " rep rep middle";
    }
    docs.push_back(flatdoc(
        "id", std::string(prefix) + std::to_string(doc),
        "body_w", body));
  }
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
}

void addRejectedSegment(CollectionHelper& helper, std::string_view prefix) {
  constexpr int32_t kDocs = DocsEnumMeta::L1_DOCS + 257;
  std::vector<Doc> docs;
  docs.reserve(kDocs);
  for (int32_t doc = 0; doc < kDocs; doc++) {
    std::string body = (doc & 1) == 0
        ? "common_a common_b filler"
        : "common_b common_a filler";
    if ((doc % 4) == 1) {
      body += " positive";
    }
    docs.push_back(flatdoc(
        "id", std::string(prefix) + std::to_string(doc),
        "body_w", body));
  }
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
}

int64_t runCount(CollectionHelper& helper, bool disableWindowPath,
                 PhraseShape shape, bool withTermExclusion = false,
                 bool rejectedShape = false) {
  NegatedCountGuard guard(disableWindowPath);
  auto req = localReq(helper.getSearchEngine());
  req->collection("main");
  auto& top = req->topDocs("q").getNumber().limit(0);
  std::vector<api::Query> prohibited;
  if (rejectedShape) {
    prohibited.push_back(
        qb::phraseWords(top.mr(), "body_w", {"common_a", "common_b"}));
  } else if (shape == PhraseShape::REPEATED) {
    prohibited.push_back(
        qb::phraseWords(top.mr(), "body_w", {"rep", "middle", "rep"}));
  } else {
    prohibited.push_back(
        qb::phraseWords(top.mr(), "body_w", {"exa", "exb"}));
  }
  if (withTermExclusion) {
    prohibited.push_back(qb::match(top.mr(), "body_w", "termex"));
  }
  top.rawQuery() = qb::boolean(
      top.mr(), {qb::match(top.mr(), "body_w", "positive")}, {},
      prohibited);
  req->execute(false);
  EXPECT_TRUE(req->ok()) << req->errorMsg();
  return req->getMatchCount();
}

}  // namespace

class PhraseExclusionTest : public SoluxTest {
public:
  CollectionHelper helper;
};

TEST_F(PhraseExclusionTest, admittedCountMatchesDisabledOracleAcrossSegments) {
  addAdmittedSegment(helper, "a_");
  addAdmittedSegment(helper, "b_");
  int64_t expected = runCount(
      helper, true, PhraseShape::ORDINARY);

  SkipStatsGuard stats;
  int64_t actual = runCount(
      helper, false, PhraseShape::ORDINARY);
  EXPECT_EQ(actual, expected);
  EXPECT_GT(SkipStats::phraseExclusionWindowAdmits, 0);
  EXPECT_EQ(SkipStats::phraseExclusionWindowRejects, 0);
  EXPECT_GT(SkipStats::negatedCountWindows, 0);
  EXPECT_GT(SkipStats::negatedCountExclFills, 0);
}

TEST_F(PhraseExclusionTest, rejectedShapeDeclinesBulkProbeBeforeConstruction) {
  addRejectedSegment(helper, "r_");
  int64_t expected = runCount(
      helper, true, PhraseShape::ORDINARY, false, true);

  SkipStatsGuard stats;
  int64_t actual = runCount(
      helper, false, PhraseShape::ORDINARY, false, true);
  EXPECT_EQ(actual, expected);
  EXPECT_EQ(SkipStats::phraseExclusionWindowAdmits, 0);
  EXPECT_EQ(SkipStats::phraseExclusionWindowRejects, 0);
  EXPECT_EQ(SkipStats::conjPlanUnknownIslandPhrase, 0);
  EXPECT_EQ(SkipStats::negatedCountWindows, 0);
  EXPECT_EQ(SkipStats::negatedCountExclFills, 0);
}

TEST_F(PhraseExclusionTest, combinesPhraseAndTermExclusions) {
  addAdmittedSegment(helper, "m_");
  int64_t expected = runCount(
      helper, true, PhraseShape::ORDINARY, true);

  SkipStatsGuard stats;
  int64_t actual = runCount(
      helper, false, PhraseShape::ORDINARY, true);
  EXPECT_EQ(actual, expected);
  EXPECT_GT(SkipStats::phraseExclusionWindowAdmits, 0);
  EXPECT_GT(SkipStats::negatedCountExclFills, 0);
}

TEST_F(PhraseExclusionTest, repeatedTermPhraseMatchesDisabledOracle) {
  addAdmittedSegment(helper, "p_");
  int64_t expected = runCount(
      helper, true, PhraseShape::REPEATED);

  SkipStatsGuard stats;
  int64_t actual = runCount(
      helper, false, PhraseShape::REPEATED);
  EXPECT_EQ(actual, expected);
  EXPECT_GT(SkipStats::phraseExclusionWindowAdmits, 0);
  EXPECT_GT(SkipStats::negatedCountWindows, 0);
}
