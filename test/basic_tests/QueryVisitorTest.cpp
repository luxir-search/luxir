// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include <map>
#include "test/TestIndex.h"
#include "luxir/query/BooleanQuery.h"
#include "luxir/query/CombinedFieldQuery.h"
#include "luxir/query/FuzzyQuery.h"
#include "luxir/query/PhraseQuery.h"
#include "luxir/query/PrefixQuery.h"
#include "luxir/query/QueryBuilder.h"
#include "luxir/query/TermInSetQuery.h"
#include "luxir/query/AllQuery.h"

using namespace luxir;
using namespace luxir::test;

namespace {
struct RecordingVisitor : QueryVisitor {
  std::map<std::string, float> terms;
  std::vector<Occur> occurrences;
  std::vector<QueryKind> leaves;
  bool includeProhibited = false;
  void term(std::string_view field, std::string_view text, float boost) override {
    terms.emplace(std::string(field) + ":" + std::string(text), boost);
  }
  QueryVisitor* sub(Occur occur, const Query& parent) override {
    occurrences.push_back(occur);
    return includeProhibited ? this : QueryVisitor::sub(occur, parent);
  }
  void leaf(const Query& query) override { leaves.push_back(query.getKind()); }
};
class QueryVisitorTest : public LuxirTest {};
}

TEST_F(QueryVisitorTest, rawTreeTermsOccurrencesAndBoosts) {
  TestIndex ti;
  TestField field(ti, "foo_w");
  field.startIndexing();
  field.add(0, "apple");
  field.add(1, "apply");
  field.add(2, "banana");
  ti.flush();
  field.startReading();
  Query::Context context(ti.pool, *ti.reader);
  TermQuery term("foo_w", "required", 2), filter("foo_w", "filter"), prohibited("foo_w", "no");
  BoostQuery boosted(&term, 3);
  std::string_view phraseTerms[] = {"one", "two"};
  int32_t positions[] = {0, 1};
  PhraseQuery phrase("foo_w", phraseTerms, positions);
  FuzzyQuery fuzzy("foo_w", "apple", 1, 0, 0, 2);
  PrefixQuery prefix("foo_w", "a");
  std::string_view fields[] = {"title", "body"};
  float weights[] = {3, 2};
  CombinedFieldQuery::Term entries[] = {{0, "x", 0.5f}, {1, "y", 1}};
  CombinedFieldQuery combined(fields, weights, entries);
  Query* must[] = {&boosted};
  Query* should[] = {&phrase, &fuzzy, &prefix, &combined};
  Query* filters[] = {&filter};
  Query* mustNot[] = {&prohibited};
  BooleanQuery root(must, should, mustNot, filters);
  RecordingVisitor visitor;
  root.visit(visitor, context, 2);
  EXPECT_EQ((std::map<std::string, float>{{"foo_w:required", 12}, {"foo_w:one", 2},
      {"foo_w:two", 2}, {"foo_w:apple", 4}, {"foo_w:apply", 3.2f},
      {"foo_w:filter", 1}, {"title:x", 3}, {"body:y", 4}}), visitor.terms);
  EXPECT_EQ((std::vector<QueryVisitor::Occur>{QueryVisitor::Occur::MUST,
      QueryVisitor::Occur::SHOULD, QueryVisitor::Occur::SHOULD,
      QueryVisitor::Occur::FILTER, QueryVisitor::Occur::MUST_NOT}), visitor.occurrences);
  EXPECT_EQ((std::vector<QueryKind>{prefix.getKind()}), visitor.leaves);
  RecordingVisitor all;
  all.includeProhibited = true;
  root.visit(all, context, 2);
  EXPECT_EQ(1, all.terms.at("foo_w:no"));
}

TEST_F(QueryVisitorTest, fuzzyInspectionSharesExpansionWithEveryWeight) {
  TestIndex ti;
  TestField field(ti, "foo_w");
  field.startIndexing();
  field.add(0, "apple");
  field.add(1, "apply");
  ti.flush();
  field.startReading();
  for (bool visitFirst : {true, false}) {
    MultiTermBudget budget({.maxTermsExamined = 100});
    Query::Context context(ti.pool, *ti.reader, Query::Context::Limits(&budget));
    FuzzyQuery fuzzy("foo_w", "apple", 1, 0);
    RecordingVisitor visitor;
    if (visitFirst) fuzzy.visit(visitor, context);
    else fuzzy.createWeight(context, Query::NEED_SCORES);
    auto charge = budget.termsExamined();
    EXPECT_GT(charge, 0);
    fuzzy.visit(visitor, context);
    fuzzy.createWeight(context, Query::NEED_SCORES);
    fuzzy.createWeight(context, 0);
    EXPECT_EQ(charge, budget.termsExamined());
    EXPECT_EQ(2u, visitor.terms.size());
  }
}

TEST_F(QueryVisitorTest, caseVariantsFollowExecutionRepresentation) {
  TestIndex ti;
  TestField field(ti, "foo_w");
  field.startIndexing();
  field.add(0, "Foo");
  field.add(1, "foo");
  ti.flush();
  field.startReading();
  auto schema = Schema::createDefaultSchema();
  QueryBuilder builder(ti.pool, *schema, CoerceContext{});
  auto* query = builder.createMatchQuery("foo_w", "Foo", QueryBuilder::Operator::OR, 0, 1, true);
  Query::Context context(ti.pool, *ti.reader);
  RecordingVisitor before;
  query->visit(before, context);
  EXPECT_EQ(1u, before.leaves.size());
  EXPECT_TRUE(before.terms.empty());
  query->createWeight(context, 0);
  RecordingVisitor unscored;
  query->visit(unscored, context);
  EXPECT_EQ(before.leaves, unscored.leaves);
  query->createWeight(context, Query::NEED_SCORES);
  RecordingVisitor after;
  query->visit(after, context, 2);
  EXPECT_TRUE(after.leaves.empty());
  EXPECT_EQ((std::map<std::string, float>{{"foo_w:Foo", 2}, {"foo_w:foo", 1.8f}}), after.terms);
}

TEST_F(QueryVisitorTest, explicitSetAndEmptyQueries) {
  TestIndex ti;
  TestField field(ti, "foo_w");
  field.startIndexing();
  field.add(0, "x");
  ti.flush();
  field.startReading();
  Query::Context context(ti.pool, *ti.reader);
  std::string_view terms[] = {"a", "b"};
  TermInSetQuery set("foo_w", terms);
  RecordingVisitor visitor;
  set.visit(visitor, context);
  AllQuery all;
  MatchNoDocsQuery none;
  all.visit(visitor, context);
  none.visit(visitor, context);
  EXPECT_EQ(2u, visitor.terms.size());
  EXPECT_TRUE(visitor.leaves.empty());
}
