#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <string_view>

#include "solux/query/BooleanQuery.h"
#include "solux/query/PhraseQuery.h"
#include "solux/query/TermQuery.h"
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

struct DisjTwoPhaseGuard {
  bool saved = BooleanQuery::DisjunctionScorer::disableDisjTwoPhaseForTests;

  explicit DisjTwoPhaseGuard(bool disabled) {
    BooleanQuery::DisjunctionScorer::disableDisjTwoPhaseForTests = disabled;
  }

  ~DisjTwoPhaseGuard() {
    BooleanQuery::DisjunctionScorer::disableDisjTwoPhaseForTests = saved;
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

class DisjTwoPhaseTest : public SoluxTest {
public:
  struct DirectRun {
    std::map<int32_t, float> scores;
    int64_t phraseVerifies = 0;
  };

  struct RequestRun {
    int64_t count = 0;
    std::set<std::string> ids;
    std::map<std::string, float> scores;
    int64_t phraseVerifies = 0;
  };

  CollectionHelper helper;

  static std::set<int32_t> docsOf(const DirectRun& run) {
    std::set<int32_t> docs;
    for (const auto& [doc, score] : run.scores) {
      unused(score);
      docs.insert(doc);
    }
    return docs;
  }

  static RequestRun readRequestRun(LocalReq& req) {
    RequestRun run;
    run.count = req.getMatchCount();
    const api::DocList* docs = req.docList();
    if (docs == nullptr) {
      ADD_FAILURE() << "missing doc list";
      return run;
    }
    const api::Column* ids = docs->columns.find("id");
    const api::Column* scores = docs->columns.find("_score_");
    if (ids == nullptr || scores == nullptr) {
      ADD_FAILURE() << "missing id or score column";
      return run;
    }
    const auto& idValues = std::get<api::ColStr>(ids->kind).v;
    const auto& scoreValues = std::get<api::ColFloat>(scores->kind).v;
    EXPECT_EQ(idValues.size(), scoreValues.size());
    for (size_t i = 0; i < std::min(idValues.size(), scoreValues.size()); i++) {
      std::string id(idValues[i]);
      run.ids.insert(id);
      run.scores.emplace(std::move(id), scoreValues[i]);
    }
    run.phraseVerifies = SkipStats::phraseVerifies;
    return run;
  }

  DirectRun runStandalone(bool disabled) {
    DisjTwoPhaseGuard disjGuard(disabled);
    SkipStatsGuard statsGuard;
    auto reader = helper.getIndexWriter()->getIndexReader();
    MemPool pool;
    Query::Context context(pool, *reader);
    std::string_view phraseTerms[] = {"sa", "sb"};
    int32_t phrasePositions[] = {0, 1};
    PhraseQuery phrase("body_w", phraseTerms, phrasePositions);
    TermQuery term("body_w", "st");
    auto& segment = context.topReader.segments()[0];
    Query::Scorer* members[] = {
      phrase.createWeight(context, Query::NEED_SCORES)->createScorer(pool, segment),
      term.createWeight(context, Query::NEED_SCORES)->createScorer(pool, segment)};
    if (members[0] == nullptr || members[1] == nullptr) {
      ADD_FAILURE() << "missing standalone member scorer";
      return {};
    }
    auto* scorer = pool.make<BooleanQuery::DisjunctionScorer>(pool, members);
    EXPECT_EQ(!disabled, scorer->hasTwoPhase());

    DirectRun run;
    for (int32_t doc = scorer->next(); doc != PostingsReader::END; doc = scorer->next()) {
      run.scores.emplace(doc, scorer->score());
    }
    run.phraseVerifies = SkipStats::phraseVerifies;
    return run;
  }

  DirectRun runConjunction(bool disabled) {
    DisjTwoPhaseGuard disjGuard(disabled);
    SkipStatsGuard statsGuard;
    auto reader = helper.getIndexWriter()->getIndexReader();
    MemPool pool;
    Query::Context context(pool, *reader);
    std::string_view firstTerms[] = {"ca", "cb"};
    std::string_view secondTerms[] = {"cc", "cd"};
    int32_t positions[] = {0, 1};
    PhraseQuery first("body_w", firstTerms, positions);
    PhraseQuery second("body_w", secondTerms, positions);
    TermQuery required("body_w", "ce");
    auto& segment = context.topReader.segments()[0];
    Query::Scorer* groupMembers[] = {
      first.createWeight(context, Query::NEED_SCORES)->createScorer(pool, segment),
      second.createWeight(context, Query::NEED_SCORES)->createScorer(pool, segment)};
    auto* requiredScorer =
      required.createWeight(context, Query::NEED_SCORES)->createScorer(pool, segment);
    if (groupMembers[0] == nullptr || groupMembers[1] == nullptr
        || requiredScorer == nullptr) {
      ADD_FAILURE() << "missing conjunction member scorer";
      return {};
    }
    auto* group = pool.make<BooleanQuery::DisjunctionScorer>(pool, groupMembers);
    Query::Scorer* all[] = {group, requiredScorer};
    int64_t costs[] = {5, 4};
    Query::Scorer* scoring[] = {group, requiredScorer};
    auto* scorer = pool.make<BooleanQuery::ConjunctionScorer>(
      pool, all, costs, scoring, true);

    DirectRun run;
    for (int32_t doc = scorer->next(); doc != PostingsReader::END; doc = scorer->next()) {
      run.scores.emplace(doc, scorer->score());
    }
    run.phraseVerifies = SkipStats::phraseVerifies;
    return run;
  }

  RequestRun runScoredConjunction(bool disabled) {
    DisjTwoPhaseGuard disjGuard(disabled);
    SkipStatsGuard statsGuard;
    auto req = localReq(helper.getSearchEngine());
    req->collection("main");
    auto& cur = req->topDocs("q");
    cur.getNumber().getScores().limit(32).batchSize(33).fields({"id"});
    api::Query group = qb::boolean(
      cur.mr(), {},
      {qb::boost(cur.mr(),
                 qb::phraseWords(cur.mr(), "body_w", {"ca", "cb"}),
                 0x1p24f),
       qb::phraseWords(cur.mr(), "body_w", {"cc", "cd"}),
       qb::match(cur.mr(), "body_w", "ct")});
    cur.rawQuery() = qb::boolean(
      cur.mr(), {group, qb::match(cur.mr(), "body_w", "ce")});
    req->execute(false);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    return readRequestRun(*req);
  }

  RequestRun runLegacyMaxScoreConjunction() {
    SkipStatsGuard statsGuard;
    auto reader = helper.getIndexWriter()->getIndexReader();
    MemPool pool;
    Query::Context context(pool, *reader);
    std::string_view firstTerms[] = {"ca", "cb"};
    std::string_view secondTerms[] = {"cc", "cd"};
    int32_t positions[] = {0, 1};
    PhraseQuery first("body_w", firstTerms, positions);
    PhraseQuery second("body_w", secondTerms, positions);
    TermQuery term("body_w", "ct");
    TermQuery required("body_w", "ce");
    auto& segment = context.topReader.segments()[0];
    Query::Scorer* groupMembers[] = {
      first.createWeight(context, Query::NEED_SCORES, 0x1p24f)->createScorer(pool, segment),
      second.createWeight(context, Query::NEED_SCORES)->createScorer(pool, segment),
      term.createWeight(context, Query::NEED_SCORES)->createScorer(pool, segment)};
    auto* requiredScorer =
      required.createWeight(context, Query::NEED_SCORES)->createScorer(pool, segment);
    if (groupMembers[0] == nullptr || groupMembers[1] == nullptr
        || groupMembers[2] == nullptr
        || requiredScorer == nullptr) {
      ADD_FAILURE() << "missing legacy conjunction member scorer";
      return {};
    }
    auto* group = pool.make<BooleanQuery::MaxScoreDisjunctionScorer>(
      pool, groupMembers, segment.maxDoc());
    Query::Scorer* all[] = {group, requiredScorer};
    int64_t costs[] = {5, 4};
    Query::Scorer* scoring[] = {group, requiredScorer};
    auto* scorer = pool.make<BooleanQuery::ConjunctionScorer>(
      pool, all, costs, scoring, true);

    RequestRun run;
    for (int32_t doc = scorer->next(); doc != PostingsReader::END; doc = scorer->next()) {
      EXPECT_GE(doc, 5);
      EXPECT_LE(doc, 9);
      std::string id = "c" + std::to_string(doc - 5);
      run.ids.insert(id);
      run.scores.emplace(std::move(id), scorer->score());
      run.count++;
    }
    run.phraseVerifies = SkipStats::phraseVerifies;
    return run;
  }

  void assertScoredRouting(bool disabled) {
    DisjTwoPhaseGuard disjGuard(disabled);
    auto reader = helper.getIndexWriter()->getIndexReader();
    MemPool pool;
    Query::Context context(pool, *reader);
    std::string_view firstTerms[] = {"ca", "cb"};
    std::string_view secondTerms[] = {"cc", "cd"};
    int32_t positions[] = {0, 1};
    PhraseQuery first("body_w", firstTerms, positions);
    PhraseQuery second("body_w", secondTerms, positions);
    Query* optional[] = {&first, &second};
    BooleanQuery group({}, optional, {}, {});
    auto& segment = context.topReader.segments()[0];

    auto* standaloneWeight = group.createWeight(context, Query::NEED_SCORES);
    auto* standaloneSupplier = standaloneWeight->scorerSupplier(pool, segment);
    ASSERT_NE(standaloneSupplier, nullptr);
    auto* standalone = standaloneSupplier->get(
      pool, std::numeric_limits<int64_t>::max());
    EXPECT_NE(dynamic_cast<BooleanQuery::MaxScoreDisjunctionScorer*>(standalone),
              nullptr);

    auto* drivenWeight = group.createWeight(context, Query::NEED_SCORES);
    auto* drivenSupplier = drivenWeight->scorerSupplier(pool, segment);
    ASSERT_NE(drivenSupplier, nullptr);
    auto* driven = dynamic_cast<BooleanQuery::DisjunctionScorer*>(
      drivenSupplier->get(pool, 1));
    ASSERT_NE(driven, nullptr);
    EXPECT_EQ(!disabled, driven->hasTwoPhase());
  }

  RequestRun runProhibited(bool disabled) {
    DisjTwoPhaseGuard disjGuard(disabled);
    SkipStatsGuard statsGuard;
    auto req = localReq(helper.getSearchEngine());
    req->collection("main");
    auto& cur = req->topDocs("q");
    cur.getNumber().getScores().limit(32).batchSize(33).fields({"id"});
    cur.rawQuery() = qb::boolean(
      cur.mr(), {qb::match(cur.mr(), "body_w", "pc")}, {},
      {qb::phraseWords(cur.mr(), "body_w", {"pa", "pb"}),
       qb::match(cur.mr(), "body_w", "pd")});
    req->execute(false);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    return readRequestRun(*req);
  }

  void SetUp() override {
    SoluxTest::SetUp();
    helper.indexAll({
      flatdoc("id", "s0", "body_w", "sa sb"),
      flatdoc("id", "s1", "body_w", "st"),
      flatdoc("id", "s2", "body_w", "sa x sb"),
      flatdoc("id", "s3", "body_w", "sa x sb st"),
      flatdoc("id", "s4", "body_w", "sa sb st"),
      flatdoc("id", "c0", "body_w", "ca cb ce"),
      flatdoc("id", "c1", "body_w", "cc cd ce"),
      flatdoc("id", "c2", "body_w", "ca x cb ce"),
      flatdoc("id", "c3", "body_w", "ca x cb"),
      flatdoc("id", "c4", "body_w", "ca cb cc cd ct ce"),
      flatdoc("id", "p0", "body_w", "pc pa pb"),
      flatdoc("id", "p1", "body_w", "pc pa x pb"),
      flatdoc("id", "p2", "body_w", "pc pd"),
      flatdoc("id", "p3", "body_w", "pc pa x pb pd"),
      flatdoc("id", "p4", "body_w", "pc clean"),
      flatdoc("id", "p5", "body_w", "pa pb")
    }, UpdateMessage::COMMIT);
  }
};

TEST_F(DisjTwoPhaseTest, standaloneSelfDrivesAndScoresEveryConfirmedMember) {
  DirectRun deferred = runStandalone(false);
  DirectRun eager = runStandalone(true);

  EXPECT_EQ(eager.scores, deferred.scores);
  EXPECT_EQ((std::set<int32_t>{0, 1, 3, 4}), docsOf(deferred));
  EXPECT_EQ(4, deferred.phraseVerifies);
  EXPECT_EQ(eager.phraseVerifies, deferred.phraseVerifies);
}

TEST_F(DisjTwoPhaseTest, conjunctionDefersPhraseVerificationUntilAlignment) {
  DirectRun deferred = runConjunction(false);
  DirectRun eager = runConjunction(true);

  EXPECT_EQ(eager.scores, deferred.scores);
  EXPECT_EQ((std::set<int32_t>{5, 6, 9}), docsOf(deferred));
  EXPECT_EQ(5, deferred.phraseVerifies);
  EXPECT_EQ(6, eager.phraseVerifies);
}

TEST_F(DisjTwoPhaseTest, scoredConjunctionRoutesPlainAndMatchesLegacyBits) {
  assertScoredRouting(false);
  assertScoredRouting(true);
  RequestRun deferred = runScoredConjunction(false);
  RequestRun eager = runScoredConjunction(true);
  RequestRun legacy = runLegacyMaxScoreConjunction();

  EXPECT_EQ((std::set<std::string>{"c0", "c1", "c4"}), deferred.ids);
  EXPECT_EQ(legacy.count, deferred.count);
  EXPECT_EQ(legacy.ids, deferred.ids);
  EXPECT_EQ(eager.count, deferred.count);
  EXPECT_EQ(eager.ids, deferred.ids);
  EXPECT_EQ(5, deferred.phraseVerifies);
  EXPECT_EQ(6, eager.phraseVerifies);
  EXPECT_LT(deferred.phraseVerifies, eager.phraseVerifies);
  ASSERT_EQ(legacy.scores.size(), deferred.scores.size());
  ASSERT_EQ(eager.scores.size(), deferred.scores.size());
  for (const auto& [id, expected] : legacy.scores) {
    ASSERT_TRUE(deferred.scores.contains(id));
    ASSERT_TRUE(eager.scores.contains(id));
    EXPECT_EQ(std::bit_cast<uint32_t>(expected),
              std::bit_cast<uint32_t>(deferred.scores.at(id))) << id;
    EXPECT_EQ(std::bit_cast<uint32_t>(expected),
              std::bit_cast<uint32_t>(eager.scores.at(id))) << id;
  }
}

TEST_F(DisjTwoPhaseTest, prohibitedDisjunctionDefersThroughMandNot) {
  RequestRun deferred = runProhibited(false);
  RequestRun eager = runProhibited(true);

  EXPECT_EQ(eager.count, deferred.count);
  EXPECT_EQ(eager.ids, deferred.ids);
  EXPECT_EQ(eager.scores, deferred.scores);
  EXPECT_EQ((std::set<std::string>{"p1", "p4"}), deferred.ids);
  EXPECT_EQ(2, deferred.phraseVerifies);
  EXPECT_EQ(4, eager.phraseVerifies);
}
