// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// Query::mayMatch over hand-built summaries: every proof each query kind can
// give from presence and bounds, and the conservative answers for unknown
// evidence.

#include <gtest/gtest.h>
#include "luxir/index/SegmentSummary.h"
#include "luxir/index/SummaryCollector.h"
#include "luxir/query/AllQuery.h"
#include "luxir/query/BooleanQuery.h"
#include "luxir/query/BoostQuery.h"
#include "luxir/query/ConstantScoreQuery.h"
#include "luxir/query/DisMaxQuery.h"
#include "luxir/query/ExistsQuery.h"
#include "luxir/query/GeoBoxQuery.h"
#include "luxir/query/GeoDistanceQuery.h"
#include "luxir/query/MatchNoDocsQuery.h"
#include "luxir/query/NumericPredicateQuery.h"
#include "luxir/query/PhraseQuery.h"
#include "luxir/query/PrefixQuery.h"
#include "luxir/query/SummaryView.h"
#include "luxir/query/TermInSetQuery.h"
#include "luxir/query/TermQuery.h"
#include "luxir/query/TermRangeQuery.h"
#include "luxir/reader/SegFieldInfo.h"
#include "luxir/util/geo.h"

using namespace luxir;

namespace {

using MatchState = Query::MatchState;

SegFieldInfo fieldInfo(FieldType::Type type, int32_t flags, int32_t docs, int64_t values = 0, int64_t nTerms = 0) {
  SegFieldInfo info{};
  info.type = type;
  info.flags = flags;
  info.docsWithField = docs;
  info.numValues = values;
  info.nTerms = nTerms;
  return info;
}

// One segment: price_i in [10, 20], tag_s terms in [green, red], body_t
// terms in [hello, world], where near (10, 170), and an empty field gone_i.
std::shared_ptr<const SegmentSummary> buildSummary(bool complete = true) {
  SegmentSummaryBuilder builder;
  FieldSummaryCollector price;
  price.addInt64(10);
  price.addInt64(20);
  builder.add("price_i", fieldInfo(FieldType::INT, FieldType::COLUMN_STORED, 2, 2), price);
  FieldSummaryCollector tag;
  tag.setTermBounds("green", "red", 2);
  builder.add("tag_s", fieldInfo(FieldType::STRING, FieldType::INDEX_DOCS, 2, 0, 2), tag);
  FieldSummaryCollector body;
  body.setTermBounds("hello", "world", 2);
  builder.add("body_t", fieldInfo(FieldType::TEXT, FieldType::INDEX_DOCS_FREQS_POSITIONS, 2, 0, 2), body);
  FieldSummaryCollector where;
  where.addGeoPacked(geo::encodePoint(10.0, 170.0));
  builder.add("where", fieldInfo(FieldType::GEO_POINT, FieldType::COLUMN_STORED, 1, 1), where);
  builder.add("gone_i", fieldInfo(FieldType::INT, FieldType::COLUMN_STORED, 0), FieldSummaryCollector{});
  return SegmentSummary::fromBytes(builder.encode(1, 2, complete));
}

MatchState evaluate(const Query& query, const SegmentSummary& summary, std::string* reason = nullptr) {
  return query.mayMatch(SummaryView(&summary, reason));
}

} // namespace

TEST(QueryPruningUnit, termsAndPrefixesUseBoundsAndPresence) {
  auto summary = buildSummary();
  std::string reason;
  EXPECT_EQ(MatchState::UNKNOWN, evaluate(TermQuery("tag_s", "red"), *summary));
  EXPECT_EQ(MatchState::EMPTY, evaluate(TermQuery("tag_s", "zzz"), *summary, &reason));
  EXPECT_EQ("term outside term bounds", reason);
  EXPECT_EQ(MatchState::EMPTY, evaluate(TermQuery("tag_s", "apple"), *summary));
  // Inside the bounds nothing is known: there are no membership structures.
  EXPECT_EQ(MatchState::UNKNOWN, evaluate(TermQuery("tag_s", "pink"), *summary));
  EXPECT_EQ(MatchState::EMPTY, evaluate(TermQuery("missing_s", "x"), *summary));
  EXPECT_EQ(MatchState::EMPTY, evaluate(TermQuery("gone_i", "x"), *summary));
  std::vector<std::string_view> terms = {"blue", "zebra"};
  EXPECT_EQ(MatchState::EMPTY, evaluate(TermInSetQuery("tag_s", terms), *summary));
  terms.push_back("pink");
  EXPECT_EQ(MatchState::UNKNOWN, evaluate(TermInSetQuery("tag_s", terms), *summary));
  EXPECT_EQ(MatchState::UNKNOWN, evaluate(PrefixQuery("tag_s", "gr"), *summary));
  EXPECT_EQ(MatchState::EMPTY, evaluate(PrefixQuery("tag_s", "s"), *summary));
  EXPECT_EQ(MatchState::EMPTY, evaluate(TermRangeQuery("tag_s", std::string_view("s"), true, std::nullopt, true), *summary));
  EXPECT_EQ(MatchState::UNKNOWN,
            evaluate(TermRangeQuery("tag_s", std::string_view("h"), true, std::string_view("q"), true), *summary));
  EXPECT_EQ(MatchState::EMPTY,
            evaluate(TermRangeQuery("tag_s", std::nullopt, true, std::string_view("green"), false), *summary));
  // A phrase needs every term.
  std::string_view words[] = {"hello", "zzz"};
  int32_t positions[] = {0, 1};
  EXPECT_EQ(MatchState::EMPTY, evaluate(PhraseQuery("body_t", words, positions), *summary));
  words[1] = "world";
  EXPECT_EQ(MatchState::UNKNOWN, evaluate(PhraseQuery("body_t", words, positions), *summary));
}

TEST(QueryPruningUnit, numericExistsAndGeo) {
  auto summary = buildSummary();
  EXPECT_EQ(MatchState::UNKNOWN, evaluate(NumericPredicateQuery("price_i", 15, 100), *summary));
  EXPECT_EQ(MatchState::UNKNOWN, evaluate(NumericPredicateQuery("price_i", 20, 20), *summary));
  EXPECT_EQ(MatchState::UNKNOWN, evaluate(NumericPredicateQuery("price_i", INT64_MIN, 10), *summary));
  EXPECT_EQ(MatchState::EMPTY, evaluate(NumericPredicateQuery("price_i", 21, INT64_MAX), *summary));
  EXPECT_EQ(MatchState::EMPTY, evaluate(NumericPredicateQuery("price_i", INT64_MIN, 9), *summary));
  EXPECT_EQ(MatchState::EMPTY, evaluate(NumericPredicateQuery("price_i", 5, 4), *summary));
  EXPECT_EQ(MatchState::EMPTY, evaluate(NumericPredicateQuery("gone_i", 0, 0), *summary));
  EXPECT_EQ(MatchState::EMPTY, evaluate(NumericPredicateQuery("absent_i", 0, 0), *summary));
  EXPECT_EQ(MatchState::UNKNOWN, evaluate(ExistsQuery("price_i"), *summary));
  EXPECT_EQ(MatchState::EMPTY, evaluate(ExistsQuery("gone_i"), *summary));
  EXPECT_EQ(MatchState::EMPTY, evaluate(ExistsQuery("absent_i"), *summary));
  EXPECT_EQ(MatchState::UNKNOWN, evaluate(GeoBoxQuery("where", 0.0, 20.0, 160.0, 175.0), *summary));
  EXPECT_EQ(MatchState::EMPTY, evaluate(GeoBoxQuery("where", 30.0, 40.0, 160.0, 175.0), *summary));
  EXPECT_EQ(MatchState::UNKNOWN, evaluate(GeoBoxQuery("where", 0.0, 20.0, 160.0, -170.0), *summary));
  EXPECT_EQ(MatchState::EMPTY, evaluate(GeoBoxQuery("where", 0.0, 20.0, 175.0, -170.0), *summary));
  EXPECT_EQ(MatchState::UNKNOWN, evaluate(GeoDistanceQuery("where", 10.0, 170.0, 1000.0), *summary));
  EXPECT_EQ(MatchState::EMPTY, evaluate(GeoDistanceQuery("where", -40.0, 20.0, 1000.0), *summary));
}

TEST(QueryPruningUnit, booleanCombination) {
  auto summary = buildSummary();
  TermQuery present("tag_s", "red");
  TermQuery absent("tag_s", "blue");
  NumericPredicateQuery outside("price_i", 50, 60);
  std::vector<Query*> none;
  std::vector<Query*> must = {&present};
  std::vector<Query*> filter = {&outside};
  EXPECT_EQ(MatchState::EMPTY, evaluate(BooleanQuery(must, none, none, filter), *summary));
  std::vector<Query*> should = {&absent, &outside};
  EXPECT_EQ(MatchState::EMPTY, evaluate(BooleanQuery(none, should, none, none), *summary));
  should.push_back(&present);
  EXPECT_EQ(MatchState::UNKNOWN, evaluate(BooleanQuery(none, should, none, none), *summary));
  EXPECT_EQ(MatchState::EMPTY, evaluate(BooleanQuery(none, should, none, none, 2), *summary));
  // Under a required clause, optional clauses only rank unless min_match asks for them.
  std::span<Query*> twoImpossible(should.data(), 2);
  EXPECT_EQ(MatchState::UNKNOWN, evaluate(BooleanQuery(must, twoImpossible, none, none), *summary));
  EXPECT_EQ(MatchState::EMPTY, evaluate(BooleanQuery(must, twoImpossible, none, none, 1), *summary));
  // Prohibited clauses prove nothing.
  std::vector<Query*> prohibited = {&present};
  EXPECT_EQ(MatchState::UNKNOWN, evaluate(BooleanQuery(none, none, prohibited, none), *summary));
  EXPECT_EQ(MatchState::UNKNOWN, evaluate(BooleanQuery(must, none, prohibited, none), *summary));
  EXPECT_EQ(MatchState::NONEMPTY, evaluate(AllQuery(), *summary));
  EXPECT_EQ(MatchState::EMPTY, evaluate(MatchNoDocsQuery(), *summary));
  EXPECT_EQ(MatchState::EMPTY, evaluate(BoostQuery(&absent, 2.0f), *summary));
  EXPECT_EQ(MatchState::EMPTY, evaluate(ConstantScoreQuery(&absent), *summary));
  EXPECT_EQ(MatchState::UNKNOWN, evaluate(ConstantScoreQuery(&present), *summary));
  MemPool pool;
  std::vector<Query*> disjuncts = {&absent, &outside};
  EXPECT_EQ(MatchState::EMPTY, evaluate(*DisMaxQuery::make(pool, disjuncts, 0.0f), *summary));
}

TEST(QueryPruningUnit, incompleteSummaryProvesNoAbsence) {
  auto summary = buildSummary(false);
  EXPECT_EQ(MatchState::UNKNOWN, evaluate(TermQuery("missing_s", "x"), *summary));
  EXPECT_EQ(MatchState::UNKNOWN, evaluate(ExistsQuery("absent_i"), *summary));
  // Records that exist still carry usable bounds.
  EXPECT_EQ(MatchState::EMPTY, evaluate(NumericPredicateQuery("price_i", 21, 100), *summary));
  EXPECT_FALSE(SummaryView::provesTermAbsent(summary.get(), "missing_s", "x"));
  EXPECT_TRUE(SummaryView::provesTermAbsent(summary.get(), "tag_s", "zzz"));
  SummaryView none(nullptr);
  EXPECT_EQ(MatchState::UNKNOWN, TermQuery("tag_s", "x").mayMatch(none));
  EXPECT_EQ(MatchState::NONEMPTY, AllQuery().mayMatch(none));
  EXPECT_FALSE(SummaryView::provesTermAbsent(nullptr, "tag_s", "zzz"));
}
