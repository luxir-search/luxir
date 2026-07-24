// Behavior tests for the expr arm end-to-end: lowering through
// ProtobufQueryParser/QueryBuilder, match behavior over a real index, the
// splice of the parsed expansion into the request tree, and the rigorous
// error contract on the response.

#include <bit>
#include <cstdint>
#include <limits>
#include <memory_resource>
#include <span>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "test/SoluxTest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/QueryBuild.h"
#include "test/TestIndex.h"
#include "test/TestUtils.h"
#include "solux/query/BooleanQuery.h"
#include "solux/query/BoostQuery.h"
#include "solux/query/ConstantScoreQuery.h"
#include "solux/query/FuzzyQuery.h"
#include "solux/query/PhraseQuery.h"
#include "solux/query/PrefixQuery.h"
#include "solux/query/TermQuery.h"
#include "solux/search/Collector.h"

using namespace std;
using namespace solux;
using namespace solux::test;

class ExprQueryTest : public SoluxTest {
public:
  CollectionHelper helper{"main"};

  void SetUp() override {
    helper.index(flatdoc("id", "d1", "title_wl", "Blade Runner", "body_wl", "a replicant story",
                         "tag_s", "scifi", "year_i", "1982"),
                 UpdateMessage::NO_COMMIT);
    helper.index(flatdoc("id", "d2", "title_wl", "The Running Man", "body_wl", "arnold runs fast",
                         "tag_s", "action", "year_i", "1987"),
                 UpdateMessage::NO_COMMIT);
    helper.index(flatdoc("id", "d3", "title_wl", "Bladed Weapons", "body_wl", "swords and knives",
                         "tag_s", "scifi", "year_i", "1990"),
                 UpdateMessage::COMMIT);
  }


  std::vector<Doc> search(std::string_view q,
                          std::function<void(solux::api::Query&)> tweak = {}) {
    auto req = localReq(helper.getSearchEngine());
    auto& cur = req->collection("main").topDocs("q");
    cur.exprQuery(q).fields({"id"}).limit(-1);
    if (tweak) {
      tweak(cur.rawQuery());
    }
    req->execute();
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    return req->getDocs();
  }

  std::string searchErr(std::string_view q) {
    auto req = localReq(helper.getSearchEngine());
    req->collection("main").topDocs("q").exprQuery(q).fields({"id"}).limit(-1);
    req->execute();
    EXPECT_FALSE(req->ok()) << "expected an error for: " << q;
    return std::string(req->errorMsg());
  }

  static bool hasId(const std::vector<Doc>& docs, std::string_view id) {
    return containsDoc(docs, flatdoc("id", std::string(id)));
  }
};

class BoostQueryTest : public SoluxTest {
protected:
  static void assertPrunedTopKMatchesExhaustive(IndexReader& reader, Query& query,
                                                 std::string_view label,
                                                 int64_t k = 5,
                                                 bool expectPruning = true) {
    MemPool exhaustivePool;
    Query::Context exhaustiveContext(exhaustivePool, reader);
    auto* exhaustiveWeight = query.createWeight(exhaustiveContext, Query::NEED_SCORES);
    TopDocsCollector exhaustive(k);
    for (auto& segment : reader.segments()) {
      auto* scorer = exhaustiveWeight->createScorer(exhaustivePool, segment);
      if (scorer != nullptr) {
        collectTopK(segment.ord, scorer, nullptr, nullptr, exhaustive,
                    /*allowPruning=*/false);
      }
    }

    MemPool prunedPool;
    Query::Context prunedContext(prunedPool, reader);
    auto* prunedWeight = query.createWeight(prunedContext, Query::NEED_SCORES);
    TopDocsCollector pruned(k);
    for (auto& segment : reader.segments()) {
      auto* scorer = prunedWeight->createScorer(prunedPool, segment);
      if (scorer != nullptr) {
        collectTopK(segment.ord, scorer, nullptr, nullptr, pruned,
                    /*allowPruning=*/true);
      }
    }

    auto expected = exhaustive.sort();
    auto actual = pruned.sort();
    if (expectPruning) {
      EXPECT_LT(pruned.totalHits(), exhaustive.totalHits()) << label;
    }
    ASSERT_EQ(expected.size(), actual.size());
    for (size_t i = 0; i < expected.size(); i++) {
      EXPECT_EQ(expected[i].doc, actual[i].doc) << "rank " << i;
      EXPECT_EQ(std::bit_cast<uint32_t>(expected[i].score),
                std::bit_cast<uint32_t>(actual[i].score)) << "rank " << i;
    }
  }
};

TEST_F(BoostQueryTest, foldedBoundsMatchExhaustiveAcrossQueryKinds) {
  TestIndex index;
  TestField body(index, "body_w");
  body.startIndexing();
  const int32_t count = 12 * Postings::DOCS_BLOCK_SIZE + 17;
  for (int32_t doc = 0; doc < count; doc++) {
    std::string text;
    int32_t repeats = doc < 16 ? 8 - doc % 3 : 1;
    for (int32_t i = 0; i < repeats; i++) text += "alpha beta ";
    if (doc < 16) {
      for (int32_t i = 0; i < repeats; i++) text += "gamma ";
    }
    for (int32_t i = 0; i < (doc < 16 ? doc % 4 : 180 + doc % 29); i++) {
      text += "padding ";
    }
    body.add(doc, text);
  }
  index.flush();
  body.startReading();

  TermQuery term("body_w", "alpha");
  BoostQuery boostedTerm(&term, 2.0f);
  assertPrunedTopKMatchesExhaustive(*index.reader, boostedTerm, "term");

  std::string_view phraseTerms[] = {"alpha", "beta"};
  int32_t phrasePositions[] = {0, 1};
  PhraseQuery phrase("body_w", phraseTerms, phrasePositions);
  BoostQuery boostedPhrase(&phrase, 1.5f);
  assertPrunedTopKMatchesExhaustive(*index.reader, boostedPhrase, "phrase");

  TermQuery alpha("body_w", "alpha");
  TermQuery gamma("body_w", "gamma");
  std::vector<Query*> optional = {&alpha, &gamma};
  BooleanQuery boolean({}, optional, {}, {});
  BoostQuery boostedBoolean(&boolean, 3.0f);
  assertPrunedTopKMatchesExhaustive(*index.reader, boostedBoolean, "boolean");

  FuzzyQuery fuzzy("body_w", "alphi", 1, 0);
  BoostQuery boostedFuzzy(&fuzzy, 2.5f);
  assertPrunedTopKMatchesExhaustive(*index.reader, boostedFuzzy, "fuzzy");

  TermQuery nestedTerm("body_w", "alpha");
  BoostQuery inner(&nestedTerm, 2.0f);
  BoostQuery outer(&inner, 4.0f);
  assertPrunedTopKMatchesExhaustive(*index.reader, outer, "nested");

  TermQuery zeroTerm("body_w", "alpha");
  BoostQuery zero(&zeroTerm, 0.0f);
  assertPrunedTopKMatchesExhaustive(*index.reader, zero, "zero", 5, false);

  // constant_score as a scoring clause of a disjunction: its flat bound
  // (below and above the term's score range) must stay admissible
  TermQuery alphaOpt("body_w", "alpha");
  TermQuery gammaOpt("body_w", "gamma");
  ConstantScoreQuery lowConstant(&gammaOpt, 0.1f);
  std::vector<Query*> lowClauses = {&alphaOpt, &lowConstant};
  BooleanQuery lowOr({}, lowClauses, {}, {});
  assertPrunedTopKMatchesExhaustive(*index.reader, lowOr, "low-constant OR", 5, false);

  ConstantScoreQuery highConstant(&gammaOpt, 50.0f);
  std::vector<Query*> highClauses = {&alphaOpt, &highConstant};
  BooleanQuery highOr({}, highClauses, {}, {});
  assertPrunedTopKMatchesExhaustive(*index.reader, highOr, "high-constant OR", 5, false);

  PrefixQuery gammaPrefix("body_w", "gam");
  std::vector<Query*> autoUniformClauses = {&alphaOpt, &gammaPrefix};
  BooleanQuery autoUniformOr({}, autoUniformClauses, {}, {});
  assertPrunedTopKMatchesExhaustive(
      *index.reader, autoUniformOr, "auto-uniform OR", 5, false);

  BoostQuery boostedPrefix(&gammaPrefix, 3.0f);
  std::vector<Query*> boostedUniformClauses = {&alphaOpt, &boostedPrefix};
  BooleanQuery boostedUniformOr({}, boostedUniformClauses, {}, {});
  assertPrunedTopKMatchesExhaustive(
      *index.reader, boostedUniformOr, "boosted-uniform OR", 5, false);
}

TEST_F(BoostQueryTest, flatBoundScorersReportExactBoundsAndExhaust) {
  TestIndex index;
  TestField body(index, "body_w");
  body.startIndexing();
  body.add(0, "alpha");
  body.add(1, "alpha alternate");
  index.flush();
  body.startReading();

  MemPool pool;
  Query::Context context(pool, *index.reader);
  auto& segment = index.reader->segments()[0];

  TermQuery term("body_w", "alpha");
  ConstantScoreQuery constant(&term, 2.0f);
  auto* scorer = constant.createWeight(context, Query::NEED_SCORES)
      ->createScorer(pool, segment);
  ASSERT_NE(nullptr, scorer);
  EXPECT_FLOAT_EQ(2.0f, scorer->getMaxScore(PostingsReader::END));
  EXPECT_FLOAT_EQ(2.0f, scorer->getMaxScoreForSetup(PostingsReader::END));
  scorer->setMinCompetitiveScore(2.0f);  // a tie stays competitive
  EXPECT_EQ(0, scorer->next());
  scorer->setMinCompetitiveScore(2.5f);  // above the constant: exhausted
  EXPECT_EQ(PostingsReader::END, scorer->next());

  PrefixQuery prefix("body_w", "al");
  BoostQuery boosted(&prefix, 3.0f);
  auto* multiTerm = boosted.createWeight(context, Query::NEED_SCORES)
      ->createScorer(pool, segment);
  ASSERT_NE(nullptr, multiTerm);
  EXPECT_FLOAT_EQ(3.0f, multiTerm->getMaxScore(PostingsReader::END));
  EXPECT_EQ(0, multiTerm->next());
  multiTerm->setMinCompetitiveScore(3.0f);  // tie: keeps iterating
  EXPECT_EQ(1, multiTerm->next());
  multiTerm->setMinCompetitiveScore(3.5f);  // above the constant: exhausted
  EXPECT_EQ(PostingsReader::END, multiTerm->next());

  auto* unscoredConstant = constant.createWeight(context, 0)
      ->createScorer(pool, segment);
  ASSERT_NE(nullptr, unscoredConstant);
  ASSERT_EQ(0, unscoredConstant->next());
  EXPECT_FLOAT_EQ(0.0f, unscoredConstant->score());
  EXPECT_FLOAT_EQ(0.0f,
                  unscoredConstant->getMaxScoreForSetup(PostingsReader::END));

  auto* unscoredMultiTerm = boosted.createWeight(context, 0)
      ->createScorer(pool, segment);
  ASSERT_NE(nullptr, unscoredMultiTerm);
  ASSERT_EQ(0, unscoredMultiTerm->next());
  EXPECT_FLOAT_EQ(0.0f, unscoredMultiTerm->score());
  EXPECT_FLOAT_EQ(0.0f,
                  unscoredMultiTerm->getMaxScoreForSetup(PostingsReader::END));
}

TEST_F(BoostQueryTest, optionalBoundsIgnoreNegativeContributions) {
  TestIndex index;
  TestField body(index, "body_w");
  body.startIndexing();
  body.add(0, "alpha beta");
  body.add(1, "alpha gamma");
  body.add(2, "beta gamma");
  index.flush();
  body.startReading();

  MemPool pool;
  Query::Context context(pool, *index.reader);
  auto& segment = index.reader->segments()[0];

  TermQuery alpha("body_w", "alpha");
  TermQuery beta("body_w", "beta");
  TermQuery gamma("body_w", "gamma");
  ConstantScoreQuery positive(&alpha, 5.0f);
  ConstantScoreQuery negative(&beta, -7.0f);
  ConstantScoreQuery negative2(&gamma, -3.0f);

  std::vector<Query*> allNegativeClauses{&negative, &negative2};
  BooleanQuery allNegative({}, allNegativeClauses, {}, {});
  auto* allNegativeScorer = allNegative.createWeight(context, Query::NEED_SCORES)
      ->createScorer(pool, segment);
  ASSERT_NE(nullptr, allNegativeScorer);
  EXPECT_FLOAT_EQ(0.0f, allNegativeScorer->getMaxScore(PostingsReader::END));
  EXPECT_FLOAT_EQ(0.0f, allNegativeScorer->refineMaxScore(PostingsReader::END));

  std::vector<Query*> disjunctionClauses{&positive, &negative, &negative2};
  BooleanQuery disjunction({}, disjunctionClauses, {}, {});
  auto* disjunctionScorer = disjunction.createWeight(context, Query::NEED_SCORES)
      ->createScorer(pool, segment);
  ASSERT_NE(nullptr, disjunctionScorer);
  EXPECT_FLOAT_EQ(5.0f, disjunctionScorer->getMaxScore(PostingsReader::END));

  std::vector<Query*> required{&positive};
  std::vector<Query*> optional{&negative};
  BooleanQuery mandOpt(required, optional, {}, {});
  auto* mandOptScorer = mandOpt.createWeight(context, Query::NEED_SCORES)
      ->createScorer(pool, segment);
  ASSERT_NE(nullptr, mandOptScorer);
  EXPECT_FLOAT_EQ(5.0f, mandOptScorer->getMaxScore(PostingsReader::END));

  BooleanQuery minShouldMatch({}, disjunctionClauses, {}, {}, 2);
  auto* msmScorer = minShouldMatch.createWeight(context, Query::NEED_SCORES)
      ->createScorer(pool, segment);
  ASSERT_NE(nullptr, msmScorer);
  float msmMax = msmScorer->getMaxScore(PostingsReader::END);
  EXPECT_GE(msmMax, 5.0f);
  EXPECT_LT(msmMax, 5.01f);
}

TEST_F(BoostQueryTest, constantScoreAbsorbsOnlyOuterBoost) {
  TestIndex index;
  TestField body(index, "body_w");
  body.startIndexing();
  body.add(0, "alpha");
  index.flush();
  body.startReading();

  TermQuery term("body_w", "alpha");
  BoostQuery inner(&term, 7.0f);
  ConstantScoreQuery constant(&inner, 2.0f);
  BoostQuery outer(&constant, 3.0f);

  MemPool pool;
  Query::Context context(pool, *index.reader);
  auto* scorer = outer.createWeight(context, Query::NEED_SCORES)
      ->createScorer(pool, index.reader->segments()[0]);
  ASSERT_NE(nullptr, scorer);
  ASSERT_EQ(0, scorer->next());
  EXPECT_FLOAT_EQ(6.0f, scorer->score());
}

TEST_F(BoostQueryTest, wireValidationRejectsInvalidValuesAndOverflow) {
  CollectionHelper helper("main");
  helper.index(flatdoc("id", "one", "body_w", "alpha"), UpdateMessage::COMMIT);

  auto expectError = [&](const api::Query& query) {
    auto req = localReq(helper.getSearchEngine());
    auto& cur = req->collection("main").topDocs("q");
    cur.rawQuery() = query;
    cur.fields({"id"});
    req->execute();
    EXPECT_FALSE(req->ok());
  };

  std::pmr::monotonic_buffer_resource arena;
  auto term = qb::match(arena, "body_w", "alpha");
  expectError(qb::boost(arena, term, -1.0f));
  expectError(qb::boost(arena, term, std::numeric_limits<float>::infinity()));
  expectError(qb::boost(arena, term, std::numeric_limits<float>::quiet_NaN()));
  auto inner = qb::boost(arena, term, std::numeric_limits<float>::max());
  expectError(qb::boost(arena, inner, 2.0f));
}

TEST_F(ExprQueryTest, fieldedTermAndPhrase) {
  auto docs = search("title_wl:blade");
  ASSERT_EQ(1u, docs.size());
  EXPECT_TRUE(hasId(docs, "d1"));

  docs = search("title_wl:\"blade runner\"");
  ASSERT_EQ(1u, docs.size());
  EXPECT_TRUE(hasId(docs, "d1"));

  docs = search("tag_s:scifi");
  EXPECT_EQ(2u, docs.size());
}

TEST_F(ExprQueryTest, booleanComposition) {
  auto docs = search("tag_s:scifi AND NOT title_wl:blade");
  ASSERT_EQ(1u, docs.size());
  EXPECT_TRUE(hasId(docs, "d3"));

  docs = search("title_wl:blade OR body_wl:arnold");
  EXPECT_EQ(2u, docs.size());

  docs = search("+tag_s:scifi -title_wl:bladed");
  ASSERT_EQ(1u, docs.size());
  EXPECT_TRUE(hasId(docs, "d1"));
}

TEST_F(ExprQueryTest, rangesAndComparisons) {
  auto docs = search("year_i:[1982 TO 1987]");
  EXPECT_EQ(2u, docs.size());

  docs = search("year_i:{1982 TO 1990}");
  ASSERT_EQ(1u, docs.size());
  EXPECT_TRUE(hasId(docs, "d2"));

  docs = search("year_i:>=1987");
  EXPECT_EQ(2u, docs.size());

  docs = search("year_i:1982");  // exact numeric match
  ASSERT_EQ(1u, docs.size());
  EXPECT_TRUE(hasId(docs, "d1"));

  docs = search("year_i:*");  // has a value
  EXPECT_EQ(3u, docs.size());
}

TEST_F(ExprQueryTest, decorations) {
  auto docs = search("title_wl:runn*");
  EXPECT_EQ(2u, docs.size());  // runner, running

  docs = search("title_wl:blabe~1");
  ASSERT_EQ(1u, docs.size());
  EXPECT_TRUE(hasId(docs, "d1"));

  docs = search("*:*");
  EXPECT_EQ(3u, docs.size());
}

TEST_F(ExprQueryTest, multitermDecorationsAreNormalized) {
  // prefix/fuzzy text folds the way the field folds - Runn* finds "runner"
  auto docs = search("title_wl:Runn*");
  EXPECT_EQ(2u, docs.size());  // runner, running

  docs = search("title_wl:Blabe~1");
  ASSERT_EQ(1u, docs.size());
  EXPECT_TRUE(hasId(docs, "d1"));

  docs = search("tag_s:SCIFI*");  // STRING stays verbatim
  EXPECT_EQ(0u, docs.size());
}

TEST_F(ExprQueryTest, termRanges) {
  // tag_s: action(d2), scifi(d1, d3) - byte-order term ranges
  auto docs = search("tag_s:[action TO scifi]");
  EXPECT_EQ(3u, docs.size());
  docs = search("tag_s:[action TO scifi}");  // exclusive upper drops scifi
  ASSERT_EQ(1u, docs.size());
  EXPECT_TRUE(hasId(docs, "d2"));
  docs = search("tag_s:>=s");
  EXPECT_EQ(2u, docs.size());
  docs = search("title_wl:[Blade TO Bladed]");  // TEXT endpoints fold
  EXPECT_EQ(2u, docs.size());  // blade(d1), bladed(d3)
  docs = search("title_wl:{blade TO bladed]");  // exclusive lower
  ASSERT_EQ(1u, docs.size());
  EXPECT_TRUE(hasId(docs, "d3"));
}

TEST_F(ExprQueryTest, fieldGroupDistribution) {
  auto docs = search("title_wl:(blade OR running)");
  EXPECT_EQ(2u, docs.size());

  docs = search("year_i:(>=1982 AND <1990)");
  EXPECT_EQ(2u, docs.size());
}

TEST_F(ExprQueryTest, functionForm) {
  auto docs = search("match(blade runner, field=title_wl, operator=AND)");
  ASSERT_EQ(1u, docs.size());
  EXPECT_TRUE(hasId(docs, "d1"));

  docs = search("boolean(required=[tag_s:scifi], prohibited=[title_wl:blade])");
  ASSERT_EQ(1u, docs.size());
  EXPECT_TRUE(hasId(docs, "d3"));

  docs = search("constant_score(tag_s:scifi, score=2.5)");
  EXPECT_EQ(2u, docs.size());

  docs = search("simple_query(running blade, fields=[title_wl])");
  EXPECT_EQ(2u, docs.size());  // d1 (blade), d2 (running)
}

TEST_F(ExprQueryTest, varsBindAsValues) {
  auto docs = search("title_wl:$t", [&](solux::api::Query& q) {
    auto& e = std::get<solux::api::ExprQuery>(q.kind);
    using Pair = std::pair<std::string_view, ::hpp_proto::indirect_view<solux::api::Val>>;
    static solux::api::Val val;  // outlives the request in this test
    val.kind = std::string_view("blade");
    static Pair pair{"t", ::hpp_proto::indirect_view<solux::api::Val>{&val}};
    e.vars = solux::api::map_view<std::string_view, ::hpp_proto::indirect_view<solux::api::Val>>(
        std::span<const Pair>(&pair, 1));
  });
  ASSERT_EQ(1u, docs.size());
  EXPECT_TRUE(hasId(docs, "d1"));
}

TEST_F(ExprQueryTest, expansionSplicesIntoRequestTree) {
  // after execution the expr arm has been replaced by its structured
  // expansion (arena-same-lifetime), so serializing the request shows the
  // canonical equivalent - the echo-mode contract
  auto req = localReq(helper.getSearchEngine());
  auto& cur = req->collection("main").topDocs("q");
  cur.exprQuery("tag_s:scifi").fields({"id"}).limit(-1);
  req->execute();
  ASSERT_TRUE(req->ok()) << req->errorMsg();
  EXPECT_TRUE(std::holds_alternative<solux::api::Match>(cur.rawQuery().kind));
}

TEST_F(ExprQueryTest, rigorousErrors) {
  EXPECT_NE(searchErr("bareword").find("unfielded term"), std::string::npos);
  EXPECT_NE(searchErr("bogus_field:x").find("unknown field"), std::string::npos);
  EXPECT_NE(searchErr("title_wl:a AND").find("expected a clause"), std::string::npos);
  EXPECT_NE(searchErr("").find("non-empty"), std::string::npos);

  std::string deep;
  for (int i = 0; i < 200; i++) deep += "(";
  deep += "title_wl:a";
  for (int i = 0; i < 200; i++) deep += ")";
  EXPECT_NE(searchErr(deep).find("nesting exceeds"), std::string::npos);
}
