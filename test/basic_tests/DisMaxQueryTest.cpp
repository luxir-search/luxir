// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <map>

#include "luxir/query/BooleanWeight.h"
#include "luxir/query/DisMaxQuery.h"
#include "luxir/query/ForcePrepareQuery.h"
#include "luxir/query/RescoreQuery.h"
#include "luxir/search/Collector.h"
#include "luxir/value/ValueExprParser.h"
#include "test/CollectionHelper.h"
#include "test/LuxirTest.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

namespace {

using Scores = std::map<segdoc, float>;

Scores scores(IndexReader& reader, Query& query) {
  MemPool pool;
  Query::Context context(pool, reader);
  auto* weight = query.createWeight(context, Query::NEED_SCORES);
  Query::Weight::PrepareContext prepareContext{reader, {}, false, nullptr};
  auto prepared = weight->needsPrepare() ? weight->prepare(prepareContext) : nullptr;
  Scores result;
  for (auto& segment : reader.segments()) {
    auto* scorer = prepared ? prepared->createScorer(pool, segment)
                            : weight->createScorer(pool, segment);
    if (scorer == nullptr) continue;
    for (int32_t doc = scorer->next(); doc != PostingsReader::END; doc = scorer->next()) {
      result.emplace(segdoc(segment.ord, doc), scorer->score());
    }
  }
  return result;
}

Scores combine(const Scores& a, const Scores& b, float tie) {
  Scores result = a;
  for (const auto& [doc, score] : b) {
    auto first = a.find(doc);
    if (first == a.end()) result[doc] = score;
    else {
      float best = std::max(first->second, score);
      result[doc] = tie == 0 ? best : best + tie * (first->second + score - best);
    }
  }
  return result;
}

void expectScores(const Scores& expected, const Scores& actual) {
  ASSERT_EQ(expected.size(), actual.size());
  for (const auto& [doc, score] : expected) {
    ASSERT_TRUE(actual.contains(doc)) << doc.docId();
    EXPECT_NEAR(score, actual.at(doc), 1e-5f * std::max(1.0f, std::abs(score))) << doc.docId();
  }
}

void expectTopOne(IndexReader& reader, Query& query, const Scores& expected,
                  bool pruning, int32_t windowSize = 0) {
  MemPool pool;
  Query::Context context(pool, reader);
  int flags = Query::NEED_SCORES | (pruning ? Query::ALLOW_PRUNING : 0);
  auto* weight = query.createWeight(context, flags);
  TopDocsCollector collector(1);
  for (auto& segment : reader.segments()) {
    Query::Scorer* scorer;
    if (windowSize != 0) {
      bool disMax = query.getKind() == QueryKind::DIS_MAX;
      std::span<Query* const> children = disMax
          ? std::span<Query* const>(static_cast<DisMaxQuery&>(query).getDisjuncts())
          : static_cast<BooleanQuery&>(query).compiledPlanForTest(context).optional;
      auto scorers = pool.make_span<Query::Scorer*>(children.size());
      for (size_t i = 0; i < children.size(); i++) {
        scorers[i] = children[i]->createWeight(context, flags)->createScorer(pool, segment);
        ASSERT_NE(nullptr, scorers[i]);
      }
      if (disMax) {
        scorer = pool.make<BooleanQuery::MaxScoreDisjunctionScorerT<true>>(
            pool, scorers, segment.maxDoc(), windowSize,
            static_cast<DisMaxQuery&>(query).getTie());
      } else {
        scorer = pool.make<BooleanQuery::MaxScoreDisjunctionScorer>(
            pool, scorers, segment.maxDoc(), windowSize);
      }
    } else {
      scorer = weight->createScorer(pool, segment);
    }
    if (scorer != nullptr) collectTopK(segment.ord, scorer, nullptr, nullptr, collector, pruning);
  }
  if (!pruning) {
    EXPECT_EQ((int64_t) expected.size(), collector.totalHits());
  }
  auto top = collector.sort();
  ASSERT_EQ(1u, top.size());
  auto best = std::max_element(expected.begin(), expected.end(),
      [](const auto& a, const auto& b) { return a.second < b.second; });
  ASSERT_NE(expected.end(), best);
  EXPECT_EQ(best->first, top[0].doc);
  EXPECT_NEAR(best->second, top[0].score, 1e-5f * std::max(1.0f, std::abs(best->second)));
}

FilterKey key(const Query& query) {
  FilterKeyBuilder builder;
  auto scope = query.appendFilterKey(builder, {});
  return std::move(builder).finishStructural(scope, {});
}

} // namespace

class DisMaxQueryTest : public LuxirTest {};

TEST_F(DisMaxQueryTest, maxScoreGlobalAndWindowed) {
  CollectionHelper helper;
  std::vector<Doc> docs;
  constexpr int32_t count = DocsEnumMeta::L1_DOCS + 257;
  for (int32_t doc = 0; doc < count; doc++) {
    std::string a = "padding", b = "padding";
    if (doc % 256 < 160) a += doc < 4 ? " dune dune dune" : " dune";
    if (doc % 256 >= 96) b += doc == count - 2 ? " dune dune dune dune" : " dune";
    docs.push_back(flatdoc("id", std::to_string(doc), "title_w", a, "body_w", b));
  }
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  ASSERT_EQ(1u, reader->segments().size());
  TermQuery a("title_w", "dune"), b("body_w", "dune");
  auto aScores = scores(*reader, a), bScores = scores(*reader, b);
  MemPool pool;
  Query* clauses[] = {&a, &b};
  for (float tie : {0.0f, 0.3f, 1.0f}) {
    SCOPED_TRACE(tie);
    auto* query = DisMaxQuery::make(pool, clauses, tie);
    auto expected = combine(aScores, bScores, tie);
    expectScores(expected, scores(*reader, *query));
    for (bool pruning : {false, true}) {
      expectTopOne(*reader, *query, expected, pruning);
      for (int32_t window : {DocsEnumMeta::L1_DOCS, count + 1}) {
        expectTopOne(*reader, *query, expected, pruning, window);
      }
    }
    if (tie == 1.0f) {
      BooleanQuery sum({}, clauses, {}, {});
      EXPECT_EQ(QueryKind::BOOLEAN, query->getKind());
      expectScores(scores(*reader, sum), scores(*reader, *query));
    }
  }
}

TEST_F(DisMaxQueryTest, externallyDrivenPhraseAndOptionalClause) {
  CollectionHelper helper;
  std::vector<Doc> docs;
  for (int32_t doc = 0; doc < 257; doc++) {
    docs.push_back(flatdoc("id", std::to_string(doc), "title_w", doc % 3 ? "dune" : "water",
        "body_w", doc % 2 ? "red dune" : "red gap dune", "keep_s", doc % 31 ? "no" : "yes"));
  }
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  TermQuery a("title_w", "dune"), filter("keep_s", "yes");
  std::string_view terms[] = {"red", "dune"};
  int32_t positions[] = {0, 1};
  PhraseQuery phrase("body_w", terms, positions);
  auto aScores = scores(*reader, a), phraseScores = scores(*reader, phrase);
  auto filterScores = scores(*reader, filter);
  MemPool pool;
  Query* clauses[] = {&a, &phrase};
  AllQuery all;
  ConstantScoreQuery zero(&all, 0.0f);
  for (float tie : {0.0f, 0.3f, 1.0f}) {
    SCOPED_TRACE(tie);
    auto* query = DisMaxQuery::make(pool, clauses, tie);
    auto expected = combine(aScores, phraseScores, tie);
    Query* mandatory[] = {query};
    Query* filters[] = {&filter};
    BooleanQuery conjunction(mandatory, {}, {}, filters);
    Scores filtered;
    for (const auto& [doc, score] : expected) {
      if (filterScores.contains(doc)) filtered[doc] = score;
    }
    expectScores(filtered, scores(*reader, conjunction));
    expectTopOne(*reader, conjunction, filtered, true);

    Query* required[] = {&zero};
    Query* optional[] = {query};
    BooleanQuery mandOpt(required, optional, {}, {});
    auto optionalExpected = scores(*reader, zero);
    for (const auto& [doc, score] : expected) optionalExpected[doc] += score;
    expectScores(optionalExpected, scores(*reader, mandOpt));
    expectTopOne(*reader, mandOpt, optionalExpected, true);

    if (tie == 1.0f) continue;
    Query::Context context(pool, *reader);
    auto* weight = query->createWeight(context, Query::NEED_SCORES | Query::ALLOW_PRUNING);
    auto* supplier = weight->scorerSupplier(pool, reader->segments()[0]);
    auto* plan = resolveScorerPlanForTests(pool, *supplier, 1);
    EXPECT_EQ(Query::ReportedTwoPhase::YES, plan->shape().reportedTwoPhase);
    EXPECT_EQ(Query::ClauseShape::NONE, plan->shape().windowFillClause);
    auto* driven = plan->build(pool);
    ASSERT_NE(nullptr, dynamic_cast<BooleanQuery::DisjunctionScorerT<true>*>(driven));
    EXPECT_TRUE(driven->flatDisjunctionScorers().empty());
  }
}

TEST_F(DisMaxQueryTest, signedPartialBoundKeepsCompetitiveDoc) {
  CollectionHelper helper;
  ASSERT_TRUE(helper.indexAll({
      flatdoc("id", "seed", "active_s", "yes", "a_i", 69, "b_i", 0),
      flatdoc("id", "winner", "active_s", "yes", "a_i", 100, "b_i", 10),
      flatdoc("id", "bounds", "active_s", "no", "a_i", 200, "b_i", -20)
    }, UpdateMessage::COMMIT).success);
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  auto schema = helper.collection().getSchema();
  google::protobuf::Arena arena;
  ValueExprOptions options{schema.get(), {}};
  ValueExprParser parser(options, arena);
  TermQuery active("active_s", "yes");
  RescoreQuery a(&active, parser.parse("div(a_i,10)"));
  RescoreQuery b(&active, parser.parse("neg(b_i)"));
  ConstantScoreQuery one(&active, 1.0f);
  Query* clauses[] = {&a, &b, &one};
  MemPool pool;
  auto* query = DisMaxQuery::make(pool, clauses, 0.3f);
  Scores expected;
  auto sa = scores(*reader, a), sb = scores(*reader, b), sc = scores(*reader, one);
  for (const auto& [doc, score] : sa) {
    float best = std::max({score, sb.at(doc), sc.at(doc)});
    expected[doc] = best + 0.3f * (score + sb.at(doc) + sc.at(doc) - best);
  }
  expectScores(expected, scores(*reader, *query));
  for (bool pruning : {false, true}) {
    expectTopOne(*reader, *query, expected, pruning);
    expectTopOne(*reader, *query, expected, pruning, 1);
  }
  Query::Context context(pool, *reader);
  auto* weight = query->createWeight(context, Query::NEED_SCORES | Query::ALLOW_PRUNING);
  auto* scorer = dynamic_cast<BooleanQuery::MaxScoreDisjunctionScorerT<true>*>(
      weight->createScorer(pool, reader->segments()[0]));
  ASSERT_NE(nullptr, scorer);
  scorer->setMinCompetitiveScore(7.2f);
  EXPECT_EQ(1, scorer->advance(1));
  EXPECT_EQ(1, scorer->currentSplitIndex());
  EXPECT_FLOAT_EQ(7.3f, scorer->score());
  EXPECT_GT(scorer->nonEssentialLookupCount(), 0);
}

TEST_F(DisMaxQueryTest, duplicatesIdentityMembershipAndBulk) {
  CollectionHelper helper;
  ASSERT_TRUE(helper.indexAll({flatdoc("id", "both", "title_w", "dune", "body_w", "dune"),
                              flatdoc("id", "one", "title_w", "dune")},
                             UpdateMessage::COMMIT).success);
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  MemPool pool;
  TermQuery a("title_w", "dune"), boosted("title_w", "dune", 2.0f), b("body_w", "dune");
  Query* clauses[] = {&a, &b};
  Query* duplicates[] = {&a, &a, &b};
  Query* weighted[] = {&boosted, &b};
  auto* query = DisMaxQuery::make(pool, clauses, 0.0f);
  auto* repeated = DisMaxQuery::make(pool, duplicates, 0.0f);
  auto* otherTie = DisMaxQuery::make(pool, clauses, 0.3f);
  auto* otherWeight = DisMaxQuery::make(pool, weighted, 0.0f);
  EXPECT_FALSE(query->equals(*otherTie));
  EXPECT_FALSE(query->equals(*otherWeight));
  EXPECT_NE(query->hash(), otherTie->hash());
  EXPECT_NE(query->hash(), otherWeight->hash());
  BoostQuery wrapper(&a, 2.0f);
  Query* wrapped[] = {&wrapper, &b};
  auto* sameWeight = DisMaxQuery::make(pool, wrapped, 0.0f);
  EXPECT_TRUE(otherWeight->equals(*sameWeight));
  EXPECT_EQ(otherWeight->hash(), sameWeight->hash());
  expectScores(scores(*reader, *query), scores(*reader, *repeated));
  auto expected = combine(scores(*reader, a), scores(*reader, b), 0.3f);
  for (const auto& [doc, score] : scores(*reader, a)) expected[doc] += 0.3f * score;
  expectScores(expected, scores(*reader, *DisMaxQuery::make(pool, duplicates, 0.3f)));
  Query* singleton[] = {&a};
  EXPECT_EQ(&a, DisMaxQuery::make(pool, singleton, 0.3f));

  BooleanQuery membership({}, clauses, {}, {});
  EXPECT_EQ(key(membership), key(*query));
  EXPECT_EQ(key(*query), key(*otherTie));
  Query::Context context(pool, *reader);
  auto* weight = otherTie->createWeight(context, Query::NEED_SCORES | Query::ALLOW_PRUNING);
  EXPECT_FALSE(weight->isConstantScoring());
  EXPECT_FALSE(weight->canComposeExactCountTopK());
  EXPECT_EQ(-1, weight->sparseFilteredTopKCost(pool, reader->segments()[0]));
  auto* supplier = weight->scorerSupplier(pool, reader->segments()[0]);
  using Supplier = Query::ScorerSupplier;
  EXPECT_EQ(Supplier::BulkAnswer::NO, supplier->planBulk(Supplier::BulkUse::SCORED_WINDOWS, {}).available);
  for (auto* unscored : {query, (Query*)&membership}) {
    auto* filterWeight = unscored->createWeight(context, 0);
    auto count = filterWeight->countShortcut(reader->segments()[0], nullptr);
    EXPECT_EQ(2, count.maxMatches);
    auto* scorer = filterWeight->createScorer(pool, reader->segments()[0]);
    int matches = 0;
    while (scorer->next() != PostingsReader::END) matches++;
    EXPECT_EQ(2, matches);
  }

  TermQuery missing("body_w", "absent");
  Query* oneLive[] = {&a, &missing};
  auto* oneLiveQuery = DisMaxQuery::make(pool, oneLive, 0.3f);
  expectScores(scores(*reader, a), scores(*reader, *oneLiveQuery));
  auto* oneLiveSupplier = oneLiveQuery->createWeight(context, Query::NEED_SCORES)
      ->scorerSupplier(pool, reader->segments()[0]);
  EXPECT_EQ(Supplier::BulkAnswer::NO,
            oneLiveSupplier->planBulk(Supplier::BulkUse::SCORED_WINDOWS, {}).available);
  auto oneLiveContext = oneLiveSupplier->makePlanContext(Query::Demand::fromLeadCost(1));
  EXPECT_EQ(Query::DirectScorerKind::OTHER,
            oneLiveSupplier->describeScorer(oneLiveContext).directKind);

  ForcePrepareQuery forced(&a);
  Query* preparedClauses[] = {&forced, &b};
  auto* preparedQuery = DisMaxQuery::make(pool, preparedClauses, 0.3f);
  expectScores(scores(*reader, *otherTie), scores(*reader, *preparedQuery));
}

TEST_F(DisMaxQueryTest, zeroTieDoesNotMultiplyOverflowedSum) {
  CollectionHelper helper;
  ASSERT_TRUE(helper.index(flatdoc("id", "one"), UpdateMessage::COMMIT).success);
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  AllQuery all;
  ConstantScoreQuery huge(&all, std::numeric_limits<float>::max());
  Query* clauses[] = {&huge, &huge};
  MemPool pool;
  auto* query = DisMaxQuery::make(pool, clauses, 0.0f);
  auto hits = scores(*reader, *query);
  ASSERT_EQ(1u, hits.size());
  EXPECT_FLOAT_EQ(std::numeric_limits<float>::max(), hits.begin()->second);
  Query::Context context(pool, *reader);
  auto* supplier = query->createWeight(context, Query::NEED_SCORES)
      ->scorerSupplier(pool, reader->segments()[0]);
  auto* driven = buildScorerForTests(pool, *supplier, 1);
  ASSERT_EQ(0, driven->next());
  EXPECT_FLOAT_EQ(std::numeric_limits<float>::max(), driven->score());
}
