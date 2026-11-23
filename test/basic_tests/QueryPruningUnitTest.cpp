// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// Query::mayMatch over field record headers: the dictionary bound checks in
// every bounds mode, the proofs each query kind gives from presence and
// bounds over real segments, merged headers for the collection view, and
// the conservative answers for unknown evidence.

#include <gtest/gtest.h>
#include "luxir/query/AllQuery.h"
#include "luxir/query/BooleanQuery.h"
#include "luxir/query/BoostQuery.h"
#include "luxir/query/CatalogPredicates.h"
#include "luxir/query/CatalogSummaryView.h"
#include "luxir/query/ConstantScoreQuery.h"
#include "luxir/query/DisMaxQuery.h"
#include "luxir/query/ExistsQuery.h"
#include "luxir/query/MatchNoDocsQuery.h"
#include "luxir/query/NumericPredicateQuery.h"
#include "luxir/query/PhraseQuery.h"
#include "luxir/query/PrefixQuery.h"
#include "luxir/query/QueryPruning.h"
#include "luxir/query/TermInSetQuery.h"
#include "luxir/query/TermQuery.h"
#include "luxir/query/TermRangeQuery.h"
#include "test/CollectionHelper.h"
#include "test/LuxirTest.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

namespace {

using MatchState = Query::MatchState;
using Bounds = field_parts::DictionaryBounds;

Bounds bounds(MemPool& pool, std::string_view lower, Bounds::Lower lowerMode, std::string_view upper,
              Bounds::Upper upperMode) {
  return {PackedTerm(pool, lower), upperMode == Bounds::Upper::UNBOUNDED ? PackedTerm(nullptr) : PackedTerm(pool, upper),
          lowerMode, upperMode};
}

// A query kind without proofs of its own.
class NoProofQuery final : public Query {
public:
  NoProofQuery() : Query(QueryKind::TEST) {}
  Query::Weight* createWeight(Context&, int32_t, float) override { return nullptr; }
  FilterKeyScope appendFilterKey(FilterKeyBuilder& out, const FilterKeyContext&) const override {
    out.appendKind(kind);
    return FilterKeyScope::SEGMENT_STABLE;
  }
};

class QueryPruningUnit : public LuxirTest {
protected:
  std::unique_ptr<CollectionHelper> helper;

  void SetUp() override {
    LuxirTest::SetUp();
    helper = std::make_unique<CollectionHelper>("prune_unit");
    helper->clear();
    helper->getIndexWriter()->mergePolicy->setMergeFactor(100);
  }

  void TearDown() override { helper.reset(); }

  std::shared_ptr<IndexReader> reader() { return helper->getIndexWriter()->snapshots.readers.getReader(); }

  // One segment: price_i in [10, 20], tag_s terms in [green, red], body_t
  // terms in [hello, world]; a second with price_i 50 and tag_s "zebra".
  void indexTwoSegments() {
    ASSERT_TRUE(helper->indexAll({flatdoc("id", "1", "price_i", (int64_t)10, "tag_s", "green", "body_t", "hello"),
                                  flatdoc("id", "2", "price_i", (int64_t)20, "tag_s", "red", "body_t", "world")},
                                 UpdateMessage::COMMIT).success);
    ASSERT_TRUE(helper->indexAll({flatdoc("id", "3", "price_i", (int64_t)50, "tag_s", "zebra")},
                                 UpdateMessage::COMMIT).success);
  }

  MatchState evaluate(const Query& query, int32_t ord = 0, std::string* reason = nullptr) {
    auto current = reader();
    return query.mayMatch(CatalogSummaryView(*current, ord, reason));
  }
};

} // namespace

TEST(QueryPruningBounds, termChecksFollowEveryMode) {
  MemPool pool;
  using L = Bounds::Lower;
  using U = Bounds::Upper;
  auto exact = bounds(pool, "b", L::EXACT, "d", U::INCLUSIVE);
  EXPECT_TRUE(proof::boundsExcludeTerm(exact, "a"));
  EXPECT_FALSE(proof::boundsExcludeTerm(exact, "b"));
  EXPECT_FALSE(proof::boundsExcludeTerm(exact, "d"));
  EXPECT_TRUE(proof::boundsExcludeTerm(exact, "da"));
  // A rounded lower is a strict prefix of the smallest term; an exclusive
  // upper is above every term.
  auto cut = bounds(pool, "b", L::ROUNDED, "d", U::EXCLUSIVE);
  EXPECT_TRUE(proof::boundsExcludeTerm(cut, "b"));
  EXPECT_FALSE(proof::boundsExcludeTerm(cut, "ba"));
  EXPECT_FALSE(proof::boundsExcludeTerm(cut, "cz"));
  EXPECT_TRUE(proof::boundsExcludeTerm(cut, "d"));
  auto open = bounds(pool, "b", L::EXACT, "", U::UNBOUNDED);
  EXPECT_FALSE(proof::boundsExcludeTerm(open, "zzz"));
  EXPECT_TRUE(proof::boundsExcludeTerm(open, "a"));
}

TEST(QueryPruningBounds, prefixAndRangeChecks) {
  MemPool pool;
  using L = Bounds::Lower;
  using U = Bounds::Upper;
  auto exact = bounds(pool, "bob", L::EXACT, "dan", U::INCLUSIVE);
  EXPECT_FALSE(proof::boundsExcludePrefix(exact, "b"));
  EXPECT_FALSE(proof::boundsExcludePrefix(exact, "bo"));
  EXPECT_TRUE(proof::boundsExcludePrefix(exact, "ba"));
  EXPECT_TRUE(proof::boundsExcludePrefix(exact, "dz"));
  EXPECT_FALSE(proof::boundsExcludePrefix(exact, "da"));
  EXPECT_FALSE(proof::boundsExcludePrefix(exact, ""));
  auto cut = bounds(pool, "bob", L::ROUNDED, "dan", U::EXCLUSIVE);
  EXPECT_TRUE(proof::boundsExcludePrefix(cut, "dan"));
  EXPECT_FALSE(proof::boundsExcludePrefix(cut, "da"));
  EXPECT_FALSE(proof::boundsExcludePrefix(cut, "bob"));
  EXPECT_FALSE(proof::boundsExcludePrefix(bounds(pool, "b", L::EXACT, "", U::UNBOUNDED), "zz"));

  auto sv = [](std::string_view s) { return std::optional<std::string_view>(s); };
  std::optional<std::string_view> none;
  EXPECT_TRUE(proof::boundsExcludeTermRange(exact, none, true, sv("bo"), true));
  EXPECT_FALSE(proof::boundsExcludeTermRange(exact, none, true, sv("bob"), true));
  EXPECT_TRUE(proof::boundsExcludeTermRange(exact, none, true, sv("bob"), false));
  EXPECT_TRUE(proof::boundsExcludeTermRange(cut, none, true, sv("bob"), true));
  EXPECT_FALSE(proof::boundsExcludeTermRange(exact, sv("dan"), true, none, true));
  EXPECT_TRUE(proof::boundsExcludeTermRange(exact, sv("dan"), false, none, true));
  EXPECT_TRUE(proof::boundsExcludeTermRange(cut, sv("dan"), true, none, true));
  EXPECT_FALSE(proof::boundsExcludeTermRange(cut, sv("da"), true, none, true));
  EXPECT_FALSE(proof::boundsExcludeTermRange(exact, sv("a"), true, sv("z"), true));
}

TEST_F(QueryPruningUnit, termsAndPrefixesUseBoundsAndPresence) {
  indexTwoSegments();
  std::string reason;
  EXPECT_EQ(MatchState::UNKNOWN, evaluate(TermQuery("tag_s", "red")));
  EXPECT_EQ(MatchState::EMPTY, evaluate(TermQuery("tag_s", "zzz"), 0, &reason));
  EXPECT_EQ("term outside term bounds", reason);
  EXPECT_EQ(MatchState::EMPTY, evaluate(TermQuery("tag_s", "apple")));
  // Inside the bounds nothing is known: there are no membership structures.
  EXPECT_EQ(MatchState::UNKNOWN, evaluate(TermQuery("tag_s", "pink")));
  reason.clear();
  EXPECT_EQ(MatchState::EMPTY, evaluate(TermQuery("missing_s", "x"), 0, &reason));
  EXPECT_EQ("field absent", reason);
  // body_t has no record in the second segment.
  EXPECT_EQ(MatchState::EMPTY, evaluate(TermQuery("body_t", "hello"), 1));
  std::vector<std::string_view> terms = {"blue", "zebra"};
  EXPECT_EQ(MatchState::EMPTY, evaluate(TermInSetQuery("tag_s", terms)));
  EXPECT_EQ(MatchState::UNKNOWN, evaluate(TermInSetQuery("tag_s", terms), 1));
  EXPECT_EQ(MatchState::UNKNOWN, evaluate(PrefixQuery("tag_s", "gr")));
  EXPECT_EQ(MatchState::EMPTY, evaluate(PrefixQuery("tag_s", "s")));
  EXPECT_EQ(MatchState::EMPTY, evaluate(TermRangeQuery("tag_s", std::string_view("s"), true, std::nullopt, true)));
  EXPECT_EQ(MatchState::UNKNOWN,
            evaluate(TermRangeQuery("tag_s", std::string_view("h"), true, std::string_view("q"), true)));
  EXPECT_EQ(MatchState::EMPTY,
            evaluate(TermRangeQuery("tag_s", std::nullopt, true, std::string_view("green"), false)));
  // A phrase needs every term.
  std::string_view words[] = {"hello", "zzz"};
  int32_t positions[] = {0, 1};
  EXPECT_EQ(MatchState::EMPTY, evaluate(PhraseQuery("body_t", words, positions)));
  words[1] = "world";
  EXPECT_EQ(MatchState::UNKNOWN, evaluate(PhraseQuery("body_t", words, positions)));
}

TEST_F(QueryPruningUnit, numericAndExists) {
  indexTwoSegments();
  EXPECT_EQ(MatchState::UNKNOWN, evaluate(NumericPredicateQuery("price_i", 15, 100)));
  EXPECT_EQ(MatchState::UNKNOWN, evaluate(NumericPredicateQuery("price_i", 20, 20)));
  EXPECT_EQ(MatchState::UNKNOWN, evaluate(NumericPredicateQuery("price_i", INT64_MIN, 10)));
  EXPECT_EQ(MatchState::EMPTY, evaluate(NumericPredicateQuery("price_i", 21, INT64_MAX)));
  EXPECT_EQ(MatchState::EMPTY, evaluate(NumericPredicateQuery("price_i", INT64_MIN, 9)));
  EXPECT_EQ(MatchState::EMPTY, evaluate(NumericPredicateQuery("price_i", 5, 4)));
  EXPECT_EQ(MatchState::EMPTY, evaluate(NumericPredicateQuery("absent_i", 0, 0)));
  EXPECT_EQ(MatchState::UNKNOWN, evaluate(ExistsQuery("price_i")));
  EXPECT_EQ(MatchState::EMPTY, evaluate(ExistsQuery("absent_i")));
  EXPECT_EQ(MatchState::EMPTY, evaluate(ExistsQuery("body_t"), 1));
}

TEST_F(QueryPruningUnit, booleanCombination) {
  indexTwoSegments();
  TermQuery present("tag_s", "red");
  TermQuery absent("tag_s", "blue");
  NumericPredicateQuery outside("price_i", 50, 60);
  std::vector<Query*> none;
  std::vector<Query*> must = {&present};
  std::vector<Query*> filter = {&outside};
  EXPECT_EQ(MatchState::EMPTY, evaluate(BooleanQuery(must, none, none, filter)));
  std::vector<Query*> should = {&absent, &outside};
  EXPECT_EQ(MatchState::EMPTY, evaluate(BooleanQuery(none, should, none, none)));
  should.push_back(&present);
  EXPECT_EQ(MatchState::UNKNOWN, evaluate(BooleanQuery(none, should, none, none)));
  EXPECT_EQ(MatchState::EMPTY, evaluate(BooleanQuery(none, should, none, none, 2)));
  // Under a required clause, optional clauses only rank unless min_match asks for them.
  std::span<Query*> twoImpossible(should.data(), 2);
  EXPECT_EQ(MatchState::UNKNOWN, evaluate(BooleanQuery(must, twoImpossible, none, none)));
  EXPECT_EQ(MatchState::EMPTY, evaluate(BooleanQuery(must, twoImpossible, none, none, 1)));
  // Prohibited clauses prove nothing.
  std::vector<Query*> prohibited = {&present};
  EXPECT_EQ(MatchState::UNKNOWN, evaluate(BooleanQuery(none, none, prohibited, none)));
  EXPECT_EQ(MatchState::UNKNOWN, evaluate(BooleanQuery(must, none, prohibited, none)));
  EXPECT_EQ(MatchState::NONEMPTY, evaluate(AllQuery()));
  EXPECT_EQ(MatchState::EMPTY, evaluate(MatchNoDocsQuery()));
  EXPECT_EQ(MatchState::EMPTY, evaluate(BoostQuery(&absent, 2.0f)));
  EXPECT_EQ(MatchState::EMPTY, evaluate(ConstantScoreQuery(&absent)));
  EXPECT_EQ(MatchState::UNKNOWN, evaluate(ConstantScoreQuery(&present)));
  MemPool pool;
  std::vector<Query*> disjuncts = {&absent, &outside};
  EXPECT_EQ(MatchState::EMPTY, evaluate(*DisMaxQuery::make(pool, disjuncts, 0.0f)));
}

// The collection view merges every segment's header: bounds union, counts
// sum, and a field is absent only when no segment has it.
TEST_F(QueryPruningUnit, collectionViewMergesHeaders) {
  indexTwoSegments();
  auto current = reader();
  CatalogSummaryView collection(*current, CatalogSummaryView::COLLECTION);
  const auto* price = collection.header("price_i");
  ASSERT_NE(nullptr, price);
  EXPECT_EQ(3, price->docsWithField);
  const auto* numeric = std::get_if<field_parts::ScalarBounds>(&price->bounds);
  ASSERT_NE(nullptr, numeric);
  EXPECT_EQ(10, numeric->min);
  EXPECT_EQ(50, numeric->max);
  const auto* tag = std::get_if<field_parts::DictionaryBounds>(&collection.header("tag_s")->bounds);
  ASSERT_NE(nullptr, tag);
  EXPECT_EQ("green", std::string_view(tag->lower));
  EXPECT_EQ("zebra", std::string_view(tag->upper));
  EXPECT_EQ(nullptr, collection.header("never_s"));
  EXPECT_NE(nullptr, collection.header("body_t"));
  EXPECT_EQ(MatchState::UNKNOWN, TermQuery("tag_s", "zebra").mayMatch(collection));
  EXPECT_EQ(MatchState::EMPTY, TermQuery("tag_s", "zz").mayMatch(collection));
  // The union of [10, 20] and [50, 50] does not see the gap between them.
  EXPECT_EQ(MatchState::UNKNOWN, NumericPredicateQuery("price_i", 21, 49).mayMatch(collection));
  EXPECT_EQ(MatchState::EMPTY, NumericPredicateQuery("price_i", 21, 49).mayMatch(CatalogSummaryView(*current, 0)));
}

// A query without proofs marks the view opaque; the collection proof then
// stands aside, as it could not have looked up that query's fields.
TEST_F(QueryPruningUnit, opaqueQueriesDisableTheCollectionProof) {
  indexTwoSegments();
  auto current = reader();
  QueryPruning pruning(*current);
  TermQuery absent("tag_s", "aaa");
  EXPECT_TRUE(pruning.collectionEmpty(absent));
  EXPECT_EQ("term outside term bounds", pruning.collectionReason(absent));
  NoProofQuery opaque;
  std::vector<Query*> must = {&absent, &opaque};
  BooleanQuery both(must, {}, {}, {});
  EXPECT_FALSE(pruning.collectionEmpty(both));
  // Per segment the proof still holds: weights exist by then.
  EXPECT_FALSE(pruning.membership(both).test(0));
  EXPECT_FALSE(pruning.membership(both).test(1));
}
