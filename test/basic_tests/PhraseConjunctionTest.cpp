#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "solux/query/BooleanQuery.h"
#include "solux/reader/SkipStats.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/QueryBuild.h"
#include "test/SoluxTest.h"
#include "test/TestUtils.h"

using namespace solux;
using namespace solux::test;

namespace api = solux::api;

namespace {

struct ApproxFlattenGuard {
  bool saved = BooleanQuery::ConjunctionScorer::disableApproxFlattenForTests;

  explicit ApproxFlattenGuard(bool disabled) {
    BooleanQuery::ConjunctionScorer::disableApproxFlattenForTests = disabled;
  }

  ~ApproxFlattenGuard() {
    BooleanQuery::ConjunctionScorer::disableApproxFlattenForTests = saved;
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

} // namespace

class PhraseConjunctionTest : public SoluxTest {
public:
  enum class Shape {
    PHRASE_AND_TERMS,
    TWO_PHRASES,
    DISJOINT_PHRASES
  };

  struct Run {
    int64_t count = 0;
    std::set<std::string> ids;
    std::map<std::string, float> scores;
    int64_t posSeeks = 0;
    int64_t phraseVerifies = 0;
  };

  CollectionHelper helper;

  Run run(Shape shape, bool disableFlatten, int32_t slop = 0) {
    ApproxFlattenGuard flattenGuard(disableFlatten);
    SkipStatsGuard statsGuard;
    int64_t posSeeksBefore = SkipStats::posSeeks;
    int64_t phraseVerifiesBefore = SkipStats::phraseVerifies;

    auto req = localReq(helper.getSearchEngine());
    req->collection("main");
    auto& cur = req->topDocs("q");
    cur.getNumber().getScores().limit(32).batchSize(33).fields({"id"});

    std::vector<api::Query> required;
    if (shape == Shape::DISJOINT_PHRASES) {
      required.push_back(qb::phraseWords(cur.mr(), "body_w", {"u", "v"}));
      required.push_back(qb::phraseWords(cur.mr(), "body_w", {"w", "x"}));
    } else {
      api::Query first = qb::phraseWords(cur.mr(), "body_w", {"a", "b"});
      std::get<api::PhraseQuery>(first.kind).slop = slop;
      required.push_back(first);
      if (shape == Shape::PHRASE_AND_TERMS) {
        required.push_back(qb::match(cur.mr(), "body_w", "c"));
        required.push_back(qb::match(cur.mr(), "body_w", "d"));
      } else {
        api::Query second = qb::phraseWords(cur.mr(), "body_w", {"c", "d"});
        std::get<api::PhraseQuery>(second.kind).slop = slop;
        required.push_back(second);
      }
    }
    cur.rawQuery() = qb::boolean(cur.mr(), required);
    req->execute(false);
    EXPECT_TRUE(req->ok()) << req->errorMsg();

    Run result;
    result.count = req->getMatchCount();
    const api::DocList* docs = req->docList();
    if (docs == nullptr && result.count != 0) {
      ADD_FAILURE() << "missing doc list";
    } else if (docs != nullptr && result.count != 0) {
      const api::Column* ids = docs->columns.find("id");
      const api::Column* scores = docs->columns.find("_score_");
      if (ids == nullptr || scores == nullptr) {
        ADD_FAILURE() << "missing id or score column";
      } else {
        const auto& idValues = std::get<api::ColStr>(ids->kind).v;
        const auto& scoreValues = std::get<api::ColFloat>(scores->kind).v;
        EXPECT_EQ(idValues.size(), scoreValues.size());
        for (size_t i = 0; i < std::min(idValues.size(), scoreValues.size()); i++) {
          std::string idValue(idValues[i]);
          result.ids.insert(idValue);
          result.scores.emplace(std::move(idValue), scoreValues[i]);
        }
      }
    }
    result.posSeeks = SkipStats::posSeeks - posSeeksBefore;
    result.phraseVerifies = SkipStats::phraseVerifies - phraseVerifiesBefore;
    return result;
  }

  void SetUp() override {
    SoluxTest::SetUp();
    helper.indexAll({
      flatdoc("id", "a0", "body_w", "a q b c q d"),
      flatdoc("id", "a1", "body_w", "a b c q d"),
      flatdoc("id", "a2", "body_w", "a b c d"),
      flatdoc("id", "a3", "body_w", "a q b c d"),
      flatdoc("id", "a4", "body_w", "a b q c d"),
      flatdoc("id", "a5", "body_w", "a b c"),
      flatdoc("id", "a6", "body_w", "c d a"),
      flatdoc("id", "b0", "body_w", "u v"),
      flatdoc("id", "b1", "body_w", "u q v"),
      flatdoc("id", "b2", "body_w", "w x"),
      flatdoc("id", "b3", "body_w", "w q x")
    }, UpdateMessage::COMMIT);
  }
};

TEST_F(PhraseConjunctionTest, flattenedEnumsPreserveResultsAndDeferVerification) {
  Run nestedSingle = run(Shape::PHRASE_AND_TERMS, true);
  Run flatSingle = run(Shape::PHRASE_AND_TERMS, false);
  EXPECT_EQ(nestedSingle.count, flatSingle.count);
  EXPECT_EQ(nestedSingle.ids, flatSingle.ids);
  EXPECT_EQ(nestedSingle.scores, flatSingle.scores);
  EXPECT_EQ((std::set<std::string>{"a1", "a2", "a4"}), flatSingle.ids);
  EXPECT_EQ(5, flatSingle.phraseVerifies);
  EXPECT_LE(flatSingle.posSeeks, nestedSingle.posSeeks);

  Run nestedPhrases = run(Shape::TWO_PHRASES, true);
  Run flatPhrases = run(Shape::TWO_PHRASES, false);
  EXPECT_EQ(nestedPhrases.count, flatPhrases.count);
  EXPECT_EQ(nestedPhrases.ids, flatPhrases.ids);
  EXPECT_EQ(nestedPhrases.scores, flatPhrases.scores);
  EXPECT_EQ((std::set<std::string>{"a2", "a4"}), flatPhrases.ids);
  EXPECT_EQ(8, flatPhrases.phraseVerifies);
  EXPECT_LE(flatPhrases.posSeeks, nestedPhrases.posSeeks);

  Run nestedSloppy = run(Shape::TWO_PHRASES, true, 1);
  Run flatSloppy = run(Shape::TWO_PHRASES, false, 1);
  EXPECT_EQ(nestedSloppy.count, flatSloppy.count);
  EXPECT_EQ(nestedSloppy.ids, flatSloppy.ids);
  EXPECT_EQ(nestedSloppy.scores, flatSloppy.scores);
  EXPECT_EQ(10, flatSloppy.phraseVerifies);

  Run nestedDisjoint = run(Shape::DISJOINT_PHRASES, true);
  Run flatDisjoint = run(Shape::DISJOINT_PHRASES, false);
  EXPECT_EQ(nestedDisjoint.count, flatDisjoint.count);
  EXPECT_EQ(nestedDisjoint.ids, flatDisjoint.ids);
  EXPECT_EQ(nestedDisjoint.scores, flatDisjoint.scores);
  EXPECT_EQ(0, flatDisjoint.phraseVerifies);
  EXPECT_LE(flatDisjoint.posSeeks, nestedDisjoint.posSeeks);
}
