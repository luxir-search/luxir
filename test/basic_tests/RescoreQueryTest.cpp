// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <bit>
#include <cmath>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "luxir/query/BooleanQuery.h"
#include "luxir/query/AllQuery.h"
#include "luxir/query/ConstantScoreQuery.h"
#include "luxir/query/ForcePrepareQuery.h"
#include "luxir/query/ParseContext.h"
#include "luxir/query/PhraseQuery.h"
#include "luxir/query/ProtobufQueryParser.h"
#include "luxir/query/RescoreQuery.h"
#include "luxir/query/TermQuery.h"
#include "luxir/search/Collector.h"
#include "luxir/value/ValueExprParser.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/QueryBuild.h"
#include "test/LuxirTest.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

namespace {

std::vector<std::string> resultIds(const LocalReq& req) {
  const api::DocList* docs = req.docList();
  if (docs == nullptr) return {};
  const auto& ids = std::get<api::ColStr>(docs->columns.at("id_s").kind).v;
  return {ids.begin(), ids.end()};
}

std::vector<float> resultScores(const LocalReq& req) {
  const api::DocList* docs = req.docList();
  if (docs == nullptr) return {};
  const auto& scores =
      std::get<api::ColFloat>(docs->columns.at("_score_").kind).v;
  return {scores.begin(), scores.end()};
}

ValueProgram* parseValue(google::protobuf::Arena& arena, Schema& schema,
                         std::string_view expression) {
  ValueExprOptions options{&schema, {}};
  return ValueExprParser(options, arena).parse(expression);
}

struct TopKResult {
  std::vector<TopDocsCollector::ScoreDoc> docs;
  int64_t visited;
};

TopKResult collect(IndexReader& reader, Query& query, bool pruning,
                   int64_t count = 7, float multiplier = 1.0f) {
  MemPool pool;
  Query::Context context(pool, reader);
  int32_t flags = Query::NEED_SCORES
      | (pruning ? Query::ALLOW_PRUNING : 0);
  Query::Weight* weight = query.createWeight(context, flags, multiplier);
  TopDocsCollector collector(count);
  for (auto& segment : reader.segments()) {
    Query::Scorer* scorer = weight->createScorer(pool, segment);
    if (scorer != nullptr) {
      collectTopK(segment.ord, scorer, nullptr, nullptr, collector, pruning);
    }
  }
  int64_t visited = collector.totalHits();
  std::span<TopDocsCollector::ScoreDoc> sorted = collector.sort();
  return {{sorted.begin(), sorted.end()}, visited};
}

void expectSameTopK(IndexReader& reader, Query& query, std::string_view label,
                    int64_t count = 7, float multiplier = 1.0f) {
  TopKResult exhaustive = collect(reader, query, false, count, multiplier);
  TopKResult pruned = collect(reader, query, true, count, multiplier);
  ASSERT_EQ(exhaustive.docs.size(), pruned.docs.size()) << label;
  for (size_t i = 0; i < exhaustive.docs.size(); i++) {
    EXPECT_EQ(exhaustive.docs[i].doc, pruned.docs[i].doc) << label << " rank " << i;
    EXPECT_EQ(std::bit_cast<uint32_t>(exhaustive.docs[i].score),
              std::bit_cast<uint32_t>(pruned.docs[i].score))
        << label << " rank " << i;
  }
}

} // namespace

class RescoreQueryTest : public LuxirTest {};

TEST_F(RescoreQueryTest, nativeDecayPruningMatchesExhaustiveResults) {
  CollectionHelper helper;
  std::vector<Doc> docs;
  for (int i = 0; i < 400; i++) {
    docs.push_back(flatdoc("id_s", std::to_string(i), "body_w", "alpha",
                          "v_i", (i * 37) % 401 - 200));
  }
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  auto schema = helper.collection().getSchema();
  google::protobuf::Arena arena;
  TermQuery child("body_w", "alpha");
  for (auto name : {"gauss_decay", "exp_decay", "linear_decay"}) {
    std::string expression = fmt::format("score * {}(v_i, 0, 50, 10, 0.3)", name);
    auto* program = parseValue(arena, *schema, expression);
    RescoreQuery query(&child, program);
    expectSameTopK(*reader, query, name);
  }
}

TEST_F(RescoreQueryTest, columnWindowBoundsMatchExhaustiveResults) {
  CollectionHelper helper;
  std::mt19937 gen((uint32_t)rng_seed);
  auto jitter = [&](int span) { return (int)(gen() % (uint32_t)span); };
  std::vector<Doc> docs;
  for (int i = 0; i < 6000; i++) {
    // Values cluster by leaf-sized doc range with many ties, so per-leaf
    // bounds differ and ties recur across and within leaves.
    int cluster = (i / 512) % 7;
    Doc doc = flatdoc("id_s", std::to_string(i),
                      "body_w", i % 3 == 0 ? "alpha beta" : "alpha",
                      "v_i", cluster * 40 - 120 + jitter(4),
                      "d_d", (double)(cluster - 3) * 2.5 + jitter(3) * 0.25,
                      "when_dt", int64_t{1700000000000} + cluster * 3600000,
                      "m_is", vec_i(cluster - 3, jitter(5)));
    if (jitter(3) != 0) {
      doc.push_back(flatdoc("sp_i", cluster * 11 - 30 + jitter(3))[0]);
    }
    docs.push_back(std::move(doc));
    if (i == 3100) {
      ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
      docs.clear();
    }
  }
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  auto schema = helper.collection().getSchema();
  google::protobuf::Arena arena;
  AllQuery all;
  TermQuery term("body_w", "alpha");
  // Every eligible shape, plus those the bounds must leave unproven.
  for (std::string_view expression : {
           "v_i", "2.5 * v_i", "-v_i", "0 - v_i * 1.5", "abs(v_i)",
           "d_d", "d_d * -2.0", "v_i + d_d",
           "def(sp_i, 0)", "def(sp_i, 1000) * 2", "-def(sp_i, -3)",
           "1 / (abs(v_i) + 1)", "1 / (abs(def(sp_i, 7)) + 1)",
           "sqrt(abs(v_i))", "log(abs(v_i) + 1)", "log1p(abs(d_d))",
           "pow(v_i, 2)", "pow(abs(v_i) + 1, 0.5)", "pow(abs(v_i) + 1, -1)",
           "v_i > 20 ? v_i : 0 - v_i", "def(sp_i, 0) > 5 ? 3 : 1",
           "max(v_i, def(sp_i, 0))", "min(v_i, d_d * 4)",
           "score * def(sp_i, 1)", "score + v_i", "min(score, v_i)",
           "max(score, d_d)", "(score + v_i) / 2", "score - v_i",
           "gauss_decay(v_i, 0, 50, 10, 0.3)",
           "score * exp_decay(def(sp_i, 0), 0, 20)",
           "linear_decay(when_dt, 1700000000000, 3600000)",
           "max(m_is) + v_i", "min(m_is) * 2", "count(m_is) + d_d"}) {
    auto* program = parseValue(arena, *schema, expression);
    for (Query* child : {(Query*)&all, (Query*)&term}) {
      RescoreQuery query(child, program);
      for (int64_t count : {3, 7, 25}) {
        expectSameTopK(*reader, query, expression, count);
      }
      expectSameTopK(*reader, query, expression, 7, 0.25f);
    }
  }
}

TEST_F(RescoreQueryTest, columnWindowsSkipWithoutChildBlocks) {
  CollectionHelper helper;
  std::vector<Doc> docs;
  for (int i = 0; i < 5000; i++) {
    docs.push_back(flatdoc("id_s", std::to_string(i), "v_i", 4 - i / 1000));
  }
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  auto schema = helper.collection().getSchema();
  google::protobuf::Arena arena;
  AllQuery all;
  RescoreQuery query(&all, parseValue(arena, *schema, "1 + v_i * 2"));
  TopKResult pruned = collect(*reader, query, true, 5);
  TopKResult exhaustive = collect(*reader, query, false, 5);
  expectSameTopK(*reader, query, "1 + v_i * 2", 5);
  EXPECT_EQ(5000, exhaustive.visited);
  // Once the heap holds the top value, the lower leaves are skipped.
  EXPECT_LT(pruned.visited, exhaustive.visited / 2);
}

TEST_F(RescoreQueryTest, fieldMetadataBelongsToEachWeight) {
  CollectionHelper first("first");
  CollectionHelper second("second");
  ASSERT_TRUE(first.index(flatdoc("aaa_i", 1, "popularity_i", 3),
                          UpdateMessage::COMMIT).success);
  ASSERT_TRUE(second.index(flatdoc("popularity_i", 29), UpdateMessage::COMMIT).success);
  auto firstReader = first.getIndexWriter()->snapshots.readers.getReader();
  auto secondReader = second.getIndexWriter()->snapshots.readers.getReader();
  google::protobuf::Arena arena;
  auto schema = first.collection().getSchema();
  auto* program = parseValue(arena, *schema, "popularity_i");
  AllQuery all;
  ForcePrepareQuery forced(&all);
  for (bool prepare : {false, true}) {
    RescoreQuery query(prepare ? (Query*)&forced : &all, program);
    MemPool firstPool;
    MemPool secondPool;
    Query::Context firstContext(firstPool, *firstReader);
    Query::Context secondContext(secondPool, *secondReader);
    auto* firstWeight = query.createWeight(firstContext, Query::NEED_SCORES);
    auto* secondWeight = query.createWeight(secondContext, Query::NEED_SCORES);
    std::unique_ptr<Query::Weight::PreparedWeight> firstPrepared;
    std::unique_ptr<Query::Weight::PreparedWeight> secondPrepared;
    if (prepare) {
      Query::Weight::PrepareContext firstPrepare{*firstReader, {}, false, nullptr};
      Query::Weight::PrepareContext secondPrepare{*secondReader, {}, false, nullptr};
      firstPrepared = firstWeight->prepare(firstPrepare);
      secondPrepared = secondWeight->prepare(secondPrepare);
    }
    auto& firstSource = firstPrepared ? (Query::SegmentSource&)*firstPrepared : *firstWeight;
    auto& secondSource = secondPrepared ? (Query::SegmentSource&)*secondPrepared : *secondWeight;
    auto* firstScorer = firstSource.createScorer(firstPool, firstReader->segments()[0]);
    auto* secondScorer = secondSource.createScorer(secondPool, secondReader->segments()[0]);
    ASSERT_NE(nullptr, firstScorer);
    ASSERT_NE(nullptr, secondScorer);
    EXPECT_EQ(0, firstScorer->next());
    EXPECT_EQ(0, secondScorer->next());
    EXPECT_EQ(3, firstScorer->score());
    EXPECT_EQ(29, secondScorer->score());
    EXPECT_EQ(3, firstScorer->score());
    EXPECT_EQ("popularity_i", program->root().text);
  }
}

TEST_F(RescoreQueryTest, grammarExecutionPreservesMembershipAndAllowsNegativeScores) {
  CollectionHelper helper;
  helper.index(flatdoc("id_s", "one", "body_w", "alpha", "popularity_i", 1),
               UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "five", "body_w", "alpha", "popularity_i", 5),
               UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "other", "body_w", "beta", "popularity_i", 0),
               UpdateMessage::COMMIT);

  auto req = localReq(helper.getSearchEngine());
  req->collection("main").topDocs("q")
      .exprQuery("rescore(body_w:alpha, neg(popularity_i))^2")
      .fields({"id_s"}).getScores().countLimit().limit(-1);
  req->execute(false);
  ASSERT_TRUE(req->ok()) << req->errorMsg();
  EXPECT_EQ((std::vector<std::string>{"one", "five"}), resultIds(*req));
  EXPECT_EQ(2, req->getMatchCount());
  EXPECT_EQ((std::vector<float>{-2.0f, -10.0f}), resultScores(*req));

  auto baseline = localReq(helper.getSearchEngine());
  baseline->collection("main").topDocs("q").exprQuery("body_w:alpha")
      .fields({"id_s"}).getScores().limit(-1);
  baseline->execute(false);
  ASSERT_TRUE(baseline->ok()) << baseline->errorMsg();

  auto nested = localReq(helper.getSearchEngine());
  nested->collection("main").topDocs("q")
      .exprQuery("rescore(rescore(body_w:alpha,neg(score)),neg(score))")
      .fields({"id_s"}).getScores().limit(-1);
  nested->execute(false);
  ASSERT_TRUE(nested->ok()) << nested->errorMsg();
  EXPECT_EQ(resultIds(*baseline), resultIds(*nested));
  EXPECT_EQ(resultScores(*baseline), resultScores(*nested));
}

// A rescore matches whatever its child matched, so over *:* it inherits
// "matches all docs" - but it reorders what it matched, so TopDocs must not
// take the match-all shortcut of ranking the domain's first K in doc order.
TEST_F(RescoreQueryTest, rescoredMatchAllStillRanks) {
  CollectionHelper helper;
  for (int i = 1; i <= 4; i++) {
    helper.index(flatdoc("id_s", std::to_string(i), "popularity_i", i),
                 UpdateMessage::NO_COMMIT);
  }
  helper.commit();

  auto req = localReq(helper.getSearchEngine());
  req->collection("main").topDocs("q")
      .exprQuery("rescore(*:*, popularity_i)")
      .fields({"id_s"}).getScores().countLimit().limit(2);
  req->execute(false);
  ASSERT_TRUE(req->ok()) << req->errorMsg();
  // Doc order would give 1,2; the rescore ranks by popularity.
  EXPECT_EQ((std::vector<std::string>{"4", "3"}), resultIds(*req));
  EXPECT_EQ((std::vector<float>{4.0f, 3.0f}), resultScores(*req));
  EXPECT_EQ(4, req->getMatchCount());
}

TEST_F(RescoreQueryTest, missingErrorsNameBothTotalizationChoices) {
  CollectionHelper helper;
  helper.index(flatdoc("id_s", "present", "body_w", "alpha",
                       "popularity_i", 4, "huge_d", 1e100),
               UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "missing", "body_w", "alpha"),
               UpdateMessage::COMMIT);

  auto run = [&](std::string_view expression) {
    auto req = localReq(helper.getSearchEngine());
    req->collection("main").topDocs("q").exprQuery(expression)
        .fields({"id_s"}).getScores().limit(-1);
    req->execute(false);
    return std::pair{req->ok(), req->errorMsg()};
  };

  auto [ok, error] = run("rescore(body_w:alpha, popularity_i)");
  EXPECT_FALSE(ok);
  EXPECT_NE(std::string::npos, error.find("def()")) << error;
  EXPECT_NE(std::string::npos, error.find("exists()")) << error;

  auto [defaulted, defaultError] =
      run("rescore(body_w:alpha, def(popularity_i,0))");
  EXPECT_TRUE(defaulted) << defaultError;
  auto [restricted, restrictedError] =
      run("rescore(exists(popularity_i), popularity_i)");
  EXPECT_TRUE(restricted) << restrictedError;

  auto [absent, absentError] = run("rescore(body_w:alpha, never_i)");
  EXPECT_FALSE(absent);
  EXPECT_NE(std::string::npos, absentError.find("always missing"))
      << absentError;

  auto [finite, finiteError] = run("rescore(body_w:alpha, huge_d)");
  EXPECT_FALSE(finite);
  EXPECT_NE(std::string::npos, finiteError.find("finite float"))
      << finiteError;
}

TEST_F(RescoreQueryTest, normalizationIsIdentityOnly) {
  auto schema = Schema::createDefaultSchema();
  google::protobuf::Arena arena;
  ArenaResource wire(&arena);
  MemPool pool;
  ParseContext context{pool, *schema, arena, CoerceContext{}, ""};
  ProtobufQueryParser parser(context);

  auto lower = [&](std::string_view expression) {
    api::Query child = qb::match(wire, "body_w", "alpha");
    api::Query query = qb::rescore(wire, child, expression);
    return parser.parse(query);
  };

  EXPECT_NE(nullptr, dynamic_cast<TermQuery*>(lower("score")));
  EXPECT_NE(nullptr, dynamic_cast<ConstantScoreQuery*>(lower("2.0")));
  EXPECT_NE(nullptr, dynamic_cast<ConstantScoreQuery*>(lower("add(1,2)")));
  EXPECT_NE(nullptr, dynamic_cast<ConstantScoreQuery*>(lower("16777216")));
  EXPECT_NE(nullptr, dynamic_cast<RescoreQuery*>(lower("16777217")));
  EXPECT_NE(nullptr, dynamic_cast<RescoreQuery*>(lower("0.1")));
  EXPECT_NE(nullptr, dynamic_cast<RescoreQuery*>(lower("mul(score,2)")));
}

TEST_F(RescoreQueryTest, childScoreDependencyAndScorelessBinding) {
  CollectionHelper helper;
  helper.index(flatdoc("id_s", "one", "body_w", "alpha", "popularity_i", 3),
               UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "other", "body_w", "beta"),
               UpdateMessage::COMMIT);
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  auto schema = helper.collection().getSchema();
  google::protobuf::Arena arena;
  ArenaResource wire(&arena);
  MemPool pool;
  ParseContext parseContext{pool, *schema, arena, CoerceContext{}, ""};
  ProtobufQueryParser parser(parseContext);

  auto lower = [&](std::string_view expression) {
    api::Query child = qb::match(wire, "body_w", "alpha");
    api::Query query = qb::rescore(wire, child, expression);
    return dynamic_cast<RescoreQuery*>(parser.parse(query));
  };

  RescoreQuery* column = lower("popularity_i");
  RescoreQuery* score = lower("add(score,1)");
  ASSERT_NE(nullptr, column);
  ASSERT_NE(nullptr, score);

  Query::Context scoredColumnContext(pool, *reader);
  auto* scoredColumn = dynamic_cast<RescoreQuery::Weight*>(
      column->createWeight(scoredColumnContext, Query::NEED_SCORES));
  ASSERT_NE(nullptr, scoredColumn);
  EXPECT_FALSE(scoredColumn->childNeedsScoresForTest());

  Query::Context scoredScoreContext(pool, *reader);
  auto* scoredScore = dynamic_cast<RescoreQuery::Weight*>(
      score->createWeight(scoredScoreContext, Query::NEED_SCORES));
  ASSERT_NE(nullptr, scoredScore);
  EXPECT_TRUE(scoredScore->childNeedsScoresForTest());

  Query::Context scorelessContext(pool, *reader);
  auto* scoreless = dynamic_cast<RescoreQuery::Weight*>(
      score->createWeight(scorelessContext, 0));
  ASSERT_NE(nullptr, scoreless);
  EXPECT_FALSE(scoreless->childNeedsScoresForTest());

  ValueProgram* absent = parseValue(arena, *schema, "never_i");
  TermQuery term("body_w", "alpha");
  RescoreQuery unbound(&term, absent);
  FilterKeyContext filterKeyContext;
  FilterKeyBuilder childKeyBuilder;
  FilterKeyBuilder rescoreKeyBuilder;
  FilterKeyScope childScope =
      term.appendFilterKey(childKeyBuilder, filterKeyContext);
  FilterKeyScope rescoreScope =
      unbound.appendFilterKey(rescoreKeyBuilder, filterKeyContext);
  EXPECT_EQ(childScope, rescoreScope);
  EXPECT_EQ(std::move(childKeyBuilder).finish(childScope, filterKeyContext),
            std::move(rescoreKeyBuilder).finish(rescoreScope, filterKeyContext));

  Query::Context filterContext(pool, *reader);
  Query::Weight* filterWeight = unbound.createWeight(filterContext, 0);
  Query::Context childFilterContext(pool, *reader);
  Query::Weight* childFilterWeight = term.createWeight(childFilterContext, 0);
  EXPECT_EQ(countForTests(childFilterWeight, reader->segments()[0]),
            countForTests(filterWeight, reader->segments()[0]));
  Query::Scorer* filterScorer =
      filterWeight->createScorer(pool, reader->segments()[0]);
  ASSERT_NE(nullptr, filterScorer);
  EXPECT_EQ(0, filterScorer->next());

  RescoreQuery debugQuery(
      &term, parseValue(arena, *schema, "popularity_i"));
  Query::Context debugContext(pool, *reader);
  Query::Scorer* debugScorer =
      debugQuery.createWeight(debugContext, Query::NEED_SCORES)
          ->createScorer(pool, reader->segments()[0]);
  ASSERT_NE(nullptr, debugScorer);
  EXPECT_EQ("popularity_i", debugScorer->pruningBlockerForDebug());

  ForcePrepareQuery preparedChild(&term);
  RescoreQuery preparedQuery(
      &preparedChild, parseValue(arena, *schema, "add(score,1)"));
  Query::Context preparedContext(pool, *reader);
  Query::Weight* preparedWeight =
      preparedQuery.createWeight(preparedContext, Query::NEED_SCORES);
  ASSERT_TRUE(preparedWeight->needsPrepare());
  Query::Weight::PrepareContext prepareContext{
      *reader, std::span<DocSet* const>{}, false, nullptr};
  auto prepared = preparedWeight->prepare(prepareContext);
  ASSERT_NE(nullptr, prepared);
  Query::Scorer* preparedScorer =
      prepared->createScorer(pool, reader->segments()[0]);
  ASSERT_NE(nullptr, preparedScorer);
  EXPECT_EQ(0, preparedScorer->next());
  EXPECT_TRUE(std::isfinite(preparedScorer->score()));
}

TEST_F(RescoreQueryTest, forwardsTwoPhaseVerification) {
  CollectionHelper helper;
  helper.index(flatdoc("id_s", "phrase", "body_w", "alpha beta"),
               UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "gap", "body_w", "alpha gap beta"),
               UpdateMessage::COMMIT);
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  auto schema = helper.collection().getSchema();
  auto& segment = reader->segments()[0];

  std::string_view terms[] = {"alpha", "beta"};
  int32_t positions[] = {0, 1};
  PhraseQuery phrase("body_w", terms, positions);
  google::protobuf::Arena arena;
  RescoreQuery rescore(&phrase, parseValue(arena, *schema, "neg(1)"));
  MemPool pool;
  Query::Context context(pool, *reader);
  auto* supplier = rescore.createWeight(context, Query::NEED_SCORES)
      ->scorerSupplier(pool, segment);
  ASSERT_NE(nullptr, supplier);
  auto* plan = resolveScorerPlanForTests(
      pool, *supplier, std::numeric_limits<int64_t>::max());
  Query::Scorer* scorer = plan->build(pool);
  ASSERT_NE(nullptr, scorer);
  ASSERT_EQ(Query::ReportedTwoPhase::YES,
            plan->shape().reportedTwoPhase);
  EXPECT_FALSE(scorer->approximationEnums().empty());
  EXPECT_TRUE(scorer->flatDisjunctionScorers().empty());
  EXPECT_TRUE(scorer->flatConjunctionScorers().empty());

  std::vector<int32_t> matches;
  for (int32_t doc = scorer->approximationNext();
       doc != PostingsReader::END; doc = scorer->approximationNext()) {
    if (scorer->matches()) {
      matches.push_back(doc);
      EXPECT_FLOAT_EQ(-1.0f, scorer->score());
    }
  }
  EXPECT_EQ((std::vector<int32_t>{0}), matches);
}

TEST_F(RescoreQueryTest, optionalNegativeExpressionsPruneSoundly) {
  CollectionHelper helper;
  constexpr int32_t COUNT = 8 * Postings::DOCS_BLOCK_SIZE + 13;
  for (int32_t doc = 0; doc < COUNT; doc++) {
    std::string body = "alpha ";
    if ((doc & 1) == 0) body += "beta ";
    int32_t repeats = doc < 12 ? 8 - doc % 3 : 1;
    for (int32_t i = 0; i < repeats; i++) body += "alpha beta ";
    for (int32_t i = 0; i < (doc < 12 ? 0 : 160 + doc % 23); i++) {
      body += "padding ";
    }
    if ((doc % 3) != 0) {
      helper.index(flatdoc("id_s", std::to_string(doc), "body_w", body,
                           "signal_i", doc % 17 - 8),
                   doc + 1 == COUNT ? UpdateMessage::COMMIT
                                    : UpdateMessage::NO_COMMIT);
    } else {
      helper.index(flatdoc("id_s", std::to_string(doc), "body_w", body),
                   doc + 1 == COUNT ? UpdateMessage::COMMIT
                                    : UpdateMessage::NO_COMMIT);
    }
  }
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  auto schema = helper.collection().getSchema();
  google::protobuf::Arena arena;
  TermQuery alpha("body_w", "alpha");
  TermQuery beta("body_w", "beta");
  Query* required[] = {&alpha};

  auto check = [&](std::string_view expression, std::string_view label) {
    RescoreQuery optional(
        &beta, parseValue(arena, *schema, expression));
    Query* optionalClauses[] = {&optional};
    BooleanQuery query(required, optionalClauses, {}, {});
    expectSameTopK(*reader, query, label);
  };

  check("neg(score)", "negative");
  check("neg(def(signal_i,0))", "missing-defaulted");
  check("neg(abs(sub(score,1)))", "non-monotone");

  RescoreQuery inner(&beta, parseValue(arena, *schema, "neg(score)"));
  RescoreQuery outer(
      &inner, parseValue(arena, *schema, "add(score,def(signal_i,-10))"));
  Query* nestedOptional[] = {&outer};
  BooleanQuery nested(required, nestedOptional, {}, {});
  expectSameTopK(*reader, nested, "nested");

  RescoreQuery scalable(
      &alpha, parseValue(arena, *schema, "mul(score,2)"));
  TopKResult exhaustive = collect(*reader, scalable, false, 5);
  TopKResult pruned = collect(*reader, scalable, true, 5);
  EXPECT_LT(pruned.visited, exhaustive.visited);
}
