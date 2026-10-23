// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// Query::mayMatch over hand-built summaries: every proof each query kind can
// give, tier gating, and the conservative answers for unknown evidence.

#include <gtest/gtest.h>
#include "luxir/index/SegmentSummary.h"
#include "luxir/index/SummaryCollector.h"
#include "luxir/query/AllQuery.h"
#include "luxir/query/BooleanQuery.h"
#include "luxir/query/BoostQuery.h"
#include "luxir/query/ConstantScoreQuery.h"
#include "luxir/query/ExistsQuery.h"
#include "luxir/query/GeoBoxQuery.h"
#include "luxir/query/GeoDistanceQuery.h"
#include "luxir/query/MatchNoDocsQuery.h"
#include "luxir/query/NumericPredicateQuery.h"
#include "luxir/query/PrefixQuery.h"
#include "luxir/query/SummaryView.h"
#include "luxir/query/TermInSetQuery.h"
#include "luxir/query/TermQuery.h"
#include "luxir/query/TermRangeQuery.h"
#include "luxir/util/geo.h"

using namespace luxir;

namespace {

using MatchState = Query::MatchState;

// One segment: price_i in [10, 20], tag_s in {red, green} (exact set),
// body_t with a fuse filter over {hello, world}, where near (10, 170), and
// an empty field gone_i.
std::shared_ptr<const SegmentSummary> buildSummary(bool complete = true) {
  SegmentSummaryBuilder builder;
  auto& price = builder.field("price_i", FieldType::INT, FieldType::COLUMN_STORED, SummaryPolicy{});
  price.addInt64(10);
  price.addInt64(20);
  price.setCounts(2, 2, true, false);

  SummaryPolicy setPolicy;
  setPolicy.terms = SummaryPolicy::Terms::SET;
  auto& tag = builder.field("tag_s", FieldType::STRING, FieldType::INDEX_DOCS | FieldType::COLUMN_STORED, setPolicy);
  TermKeyCollector tagKeys(setPolicy);
  tagKeys.add("green");
  tagKeys.add("red");
  tag.setTermBounds("green", "red", 2);
  tag.setTermKeys(std::move(tagKeys));
  tag.setCounts(2, 2, true, false);
  tag.setHeaderTotals(2, 2);

  SummaryPolicy filterPolicy;
  filterPolicy.terms = SummaryPolicy::Terms::FILTER;
  auto& body = builder.field("body_t", FieldType::TEXT, FieldType::INDEX_DOCS_FREQS_POSITIONS, filterPolicy);
  TermKeyCollector bodyKeys(filterPolicy);
  bodyKeys.add("hello");
  bodyKeys.add("world");
  body.setTermBounds("hello", "world", 2);
  body.setTermKeys(std::move(bodyKeys));
  body.setCounts(2, 0, false, false);
  body.setHeaderTotals(2, 4);

  auto& where = builder.field("where", FieldType::GEO_POINT, FieldType::COLUMN_STORED, SummaryPolicy{});
  where.addGeoPacked(geo::encodePoint(10.0, 170.0));
  where.setCounts(1, 1, true, false);

  auto& gone = builder.field("gone_i", FieldType::INT, FieldType::COLUMN_STORED, SummaryPolicy{});
  gone.setCounts(0, 0, true, false);

  return SegmentSummary::fromBytes(std::make_shared<std::vector<std::byte>>(builder.encode(1, 2, complete)));
}

MatchState evaluate(const Query& query, const SegmentSummary& summary, PruneTier tier = PruneTier::FILTER,
                    std::string* reason = nullptr) {
  SummaryView view(&summary, 2, tier, reason);
  return query.mayMatch(view);
}

} // namespace

TEST(QueryPruningUnit, termsBoundsSetsAndFilters) {
  auto summary = buildSummary();
  std::string reason;
  EXPECT_EQ(MatchState::UNKNOWN, evaluate(TermQuery("tag_s", "red"), *summary));
  EXPECT_EQ(MatchState::EMPTY, evaluate(TermQuery("tag_s", "blue"), *summary, PruneTier::FILTER, &reason));
  EXPECT_FALSE(reason.empty());
  EXPECT_EQ(MatchState::EMPTY, evaluate(TermQuery("tag_s", "zzz"), *summary, PruneTier::BOUNDS));
  // Inside the term bounds, the exact set is the proof; the bounds tier alone cannot tell.
  EXPECT_EQ(MatchState::UNKNOWN, evaluate(TermQuery("tag_s", "pink"), *summary, PruneTier::BOUNDS));
  EXPECT_EQ(MatchState::EMPTY, evaluate(TermQuery("tag_s", "pink"), *summary, PruneTier::EXACT));
  EXPECT_EQ(MatchState::EMPTY, evaluate(TermQuery("missing_s", "x"), *summary, PruneTier::PRESENCE));
  EXPECT_EQ(MatchState::EMPTY, evaluate(TermQuery("gone_i", "x"), *summary, PruneTier::PRESENCE));
  EXPECT_EQ(MatchState::UNKNOWN, evaluate(TermQuery("body_t", "hello"), *summary));
  EXPECT_EQ(MatchState::EMPTY, evaluate(TermQuery("body_t", "jello"), *summary));
  EXPECT_EQ(MatchState::UNKNOWN, evaluate(TermQuery("body_t", "jello"), *summary, PruneTier::EXACT));
  std::vector<std::string_view> terms = {"blue", "pink"};
  EXPECT_EQ(MatchState::EMPTY, evaluate(TermInSetQuery("tag_s", terms), *summary));
  terms.push_back("red");
  EXPECT_EQ(MatchState::UNKNOWN, evaluate(TermInSetQuery("tag_s", terms), *summary));
  EXPECT_EQ(MatchState::UNKNOWN, evaluate(PrefixQuery("tag_s", "gr"), *summary));
  EXPECT_EQ(MatchState::EMPTY, evaluate(PrefixQuery("tag_s", "gz"), *summary));
  EXPECT_EQ(MatchState::EMPTY, evaluate(PrefixQuery("tag_s", "s"), *summary, PruneTier::BOUNDS));
  EXPECT_EQ(MatchState::EMPTY, evaluate(TermRangeQuery("tag_s", std::string_view("s"), true, std::nullopt, true), *summary));
  EXPECT_EQ(MatchState::UNKNOWN, evaluate(TermRangeQuery("tag_s", std::string_view("h"), true, std::string_view("q"), true), *summary));
  EXPECT_EQ(MatchState::EMPTY, evaluate(TermRangeQuery("tag_s", std::nullopt, true, std::string_view("green"), false), *summary));
}

TEST(QueryPruningUnit, numericExistsAndGeo) {
  auto summary = buildSummary();
  EXPECT_EQ(MatchState::UNKNOWN, evaluate(NumericPredicateQuery("price_i", 15, 100), *summary));
  EXPECT_EQ(MatchState::EMPTY, evaluate(NumericPredicateQuery("price_i", 21, 100), *summary));
  EXPECT_EQ(MatchState::EMPTY, evaluate(NumericPredicateQuery("price_i", -5, 9), *summary));
  EXPECT_EQ(MatchState::EMPTY, evaluate(NumericPredicateQuery("price_i", 5, 4), *summary));
  EXPECT_EQ(MatchState::UNKNOWN, evaluate(NumericPredicateQuery("price_i", 21, 100), *summary, PruneTier::PRESENCE));
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
  // Under a required clause, optional clauses only rank unless minShouldMatch asks for them.
  EXPECT_EQ(MatchState::UNKNOWN, evaluate(BooleanQuery(must, std::span<Query*>(should.data(), 2), none, none), *summary));
  EXPECT_EQ(MatchState::EMPTY, evaluate(BooleanQuery(must, std::span<Query*>(should.data(), 2), none, none, 1), *summary));
  // Prohibited clauses prove nothing.
  std::vector<Query*> prohibited = {&present};
  EXPECT_EQ(MatchState::UNKNOWN, evaluate(BooleanQuery(none, none, prohibited, none), *summary));
  EXPECT_EQ(MatchState::NONEMPTY, evaluate(AllQuery(), *summary));
  EXPECT_EQ(MatchState::EMPTY, evaluate(MatchNoDocsQuery(), *summary));
  EXPECT_EQ(MatchState::EMPTY, evaluate(BoostQuery(&absent, 2.0f), *summary));
  EXPECT_EQ(MatchState::EMPTY, evaluate(ConstantScoreQuery(&absent), *summary));
  EXPECT_EQ(MatchState::UNKNOWN, evaluate(ConstantScoreQuery(&present), *summary));
}

TEST(QueryPruningUnit, incompleteSummaryProvesNoAbsence) {
  auto summary = buildSummary(false);
  EXPECT_EQ(MatchState::UNKNOWN, evaluate(TermQuery("missing_s", "x"), *summary));
  EXPECT_EQ(MatchState::UNKNOWN, evaluate(ExistsQuery("absent_i"), *summary));
  // Records that exist still carry usable bounds.
  EXPECT_EQ(MatchState::EMPTY, evaluate(NumericPredicateQuery("price_i", 21, 100), *summary));
  SummaryView none(nullptr, -1);
  EXPECT_EQ(MatchState::UNKNOWN, TermQuery("tag_s", "x").mayMatch(none));
  EXPECT_EQ(MatchState::NONEMPTY, AllQuery().mayMatch(none));
}
