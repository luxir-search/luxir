// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/QueryBuild.h"
#include "luxir/query/ImpactsIndex.h"
#include "luxir/reader/SkipStats.h"

namespace luxir::test {
namespace {

class JoinPruningTest : public LuxirTest {
protected:
  struct Run {
    std::vector<Doc> docs;
    std::vector<float> scores;
    int64_t count;
    int64_t decoded;
    int64_t skips;
    bool gte;
    bool estimated;
  };

  void index(CollectionHelper& helper, int32_t segments) {
    for (int32_t segment = 0; segment < segments; segment++) {
      CollectionHelper::UpdateBuilder input;
      for (int32_t root = 0; root < 2048; root++) {
        std::string json = "{\"id\":\"" + std::to_string(segment * 2048 + root)
            + "\",\"body_w\":\"root " + (root % 3 ? std::string("other") : std::string("extra"))
            + "\",\"chunks_nested\":[";
        int32_t children = root % 31 == 0 ? 17 : root % 4 + 1;
        for (int32_t child = 0; child < children; child++) {
          std::string text;
          // Strong regions separated by weak blocks exercise skips and resumes.
          bool strong = root < 128 || (root >= 1024 && root < 1040);
          int32_t repeats = strong ? 12 + (root + child) % 13 : 1;
          for (int32_t i = 0; i < repeats; i++) text += "alpha beta ";
          if (root % 11 == 0) text += "rare ";
          if (root % 127 == 0) text += "scarce ";
          for (int32_t i = 0; i < (strong ? root % 9 : 100 + root % 23); i++) text += "pad ";
          if (child) json += ',';
          json += "{\"text_w\":\"" + text + "\"}";
        }
        json += "]}";
        input.addRaw([&](auto& doc, auto& arena) { ASSERT_TRUE(api::read_json(doc, json, arena)); });
      }
      input.commit();
      ASSERT_TRUE(helper.submit(input).success);
    }
    auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
    ASSERT_EQ((size_t)segments, reader->segments().size());
  }

  Run run(CollectionHelper& helper, int32_t family, int32_t outer, bool reversed,
          float boost, int32_t k, bool count, int64_t countLimit = -1) {
    auto req = localReq(helper.getSearchEngine());
    req->collection(helper.collection().getId().name);
    auto& op = req->topDocs().fields({"id"}).getScores().limit(k);
    if (count) op.countLimit(countLimit);
    auto& mr = req->mr;
    auto a = qb::match(mr, "text_w", "alpha");
    api::Query inner = family == 0 ? a : family == 1 ? qb::boolean(mr, {}, {a, qb::match(mr, "text_w", "rare"), qb::match(mr, "text_w", "scarce")})
        : family == 2 ? qb::boolean(mr, {a, qb::match(mr, "text_w", "rare")}) : qb::phraseText(mr, "text_w", "alpha beta");
    auto join = qb::boost(mr, qb::toParent(mr, "chunks_nested", inner), boost);
    auto root = qb::match(mr, "body_w", "root");
    auto first = reversed ? root : join;
    auto second = reversed ? join : root;
    op.rawQuery() = outer == 0 ? join : outer == 1 ? qb::boolean(mr, {first, second})
        : qb::boolean(mr, {}, {first, second});
    SkipStatsScope stats;
    req->execute(1);
    EXPECT_OK(req);
    Run out{req->getDocs(), {}, count ? req->getMatchCount() : -1,
        SkipStats::docBlocksDecoded,
        SkipStats::conjRangeSkips + SkipStats::wandAdvancePrunes + SkipStats::wandCandidatePrunes
            + SkipStats::maxScoreDeadOuterJumps + SkipStats::maxScoreBufferCompactions
            + SkipStats::phraseImpactBlocksSkipped,
        req->docList() && req->docList()->found_gte, req->docList() && req->docList()->found_estimated};
    if (auto* docs = req->docList(); docs && docs->row_count) {
      auto scores = std::get<api::ColFloat>(docs->columns.find("_score_")->kind).v;
      out.scores.assign(scores.begin(), scores.end());
    }
    return out;
  }

  void compare(const Run& exhaustive, const Run& pruned) {
    ASSERT_EQ(exhaustive.docs.size(), pruned.docs.size());
    ASSERT_EQ(exhaustive.scores.size(), pruned.scores.size());
    for (size_t i = 0; i < exhaustive.docs.size(); i++) {
      EXPECT_TRUE(docEquals(exhaustive.docs[i], pruned.docs[i])) << i;
      EXPECT_FLOAT_EQ(exhaustive.scores[i], pruned.scores[i]) << i;
    }
  }
};

TEST_F(JoinPruningTest, maximumTopKMatchesExhaustiveWithBoostsDeletesAndBooleanClauses) {
  SearchOverridesGuard impacts(ImpactsIndex::forceEagerForTests);
  for (int32_t segments : {1, 3}) {
    ImpactsIndex::forceEagerForTests = segments == 1;
    CollectionHelper helper("join_pruning_" + std::to_string(segments));
    index(helper, segments);
    for (bool deleted : {false, true}) {
      if (deleted) {
        CollectionHelper::UpdateBuilder input;
        for (int32_t id : {0, 1, 7, 1025, 2050, 4100}) input.remove(std::to_string(id));
        input.commit();
        ASSERT_TRUE(helper.submit(input).success);
      }
      for (int32_t family = 0; family < 4; family++) {
        for (float boost : {1.0f, 0.3f, 2.5f, 0.0f}) {
          for (int32_t k : {5, 7000}) {
            SCOPED_TRACE(::testing::Message() << segments << '/' << deleted << '/' << family << '/' << boost << '/' << k);
            auto exhaustive = run(helper, family, 0, false, boost, k, true);
            auto pruned = run(helper, family, 0, false, boost, k, false);
            compare(exhaustive, pruned);
            int64_t matches = family == 2 ? segments * 187 - (deleted ? 1 : 0)
                : segments * 2048 - (deleted ? segments == 1 ? 4 : 6 : 0);
            EXPECT_EQ(matches, exhaustive.count);
            EXPECT_FALSE(exhaustive.gte);
            EXPECT_FALSE(exhaustive.estimated);
            if (k == 5 && boost > 0) {
              if (family < 2) { EXPECT_LT(pruned.decoded, exhaustive.decoded); }
              else { EXPECT_GT(pruned.skips, 0); }
            }
          }
        }
        auto exhaustive = run(helper, family, 0, false, 1.0f, 5, true);
        auto bounded = run(helper, family, 0, false, 1.0f, 5, true, 16);
        compare(exhaustive, bounded);
        EXPECT_GE(bounded.count, 16);
        EXPECT_TRUE(bounded.gte || bounded.estimated || bounded.count == exhaustive.count);
        for (int32_t outer : {1, 2}) {
          for (bool reversed : {false, true}) {
            auto exhaustive = run(helper, family, outer, reversed, 0.3f, 7, true);
            compare(exhaustive, run(helper, family, outer, reversed, 0.3f, 7, false));
          }
        }
      }
    }
  }
}

} // namespace
} // namespace luxir::test
