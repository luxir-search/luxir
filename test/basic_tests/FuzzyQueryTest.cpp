// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <string>
#include <vector>

#include "test/LuxirTest.h"
#include "test/TestIndex.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "luxir/query/BooleanQuery.h"
#include "luxir/query/FuzzyQuery.h"
#include "luxir/query/QueryBuilder.h"
#include "luxir/reader/Postings.h"
#include "luxir/schema/Schema.h"
#include "luxir/search/Collector.h"

using namespace luxir;
using namespace luxir::test;

// Direct per-segment fuzzy scorer checks.
class FuzzyQueryTest : public LuxirTest {
protected:
  std::vector<int32_t> fuzzyDocs(TestIndex& ti, std::string_view field, std::string_view term,
                                 int maxEdits, int prefixLength, int segOrd, int32_t flags = 0) {
    auto g = ti.pool.rewindScopeGuard();
    FuzzyQuery fq(field, term, maxEdits, prefixLength);
    Query::Context ctx(ti.pool, *ti.reader);
    auto* weight = fq.createWeight(ctx, flags);
    Query::Scorer* scorer = weight->createScorer(ti.pool, ctx.topReader.segments()[segOrd]);
    std::vector<int32_t> docs;
    if (scorer != nullptr) {
      for (int32_t d = scorer->next(); d != PostingsReader::END; d = scorer->next()) {
        docs.push_back(d);
      }
    }
    return docs;
  }
};

struct FuzzyTopKRun {
  int64_t visited = 0;
  int64_t maxScoreVisited = 0;
  int64_t nonEssentialLookups = 0;
  int64_t clauseBoundsChecked = 0;
  int32_t maxSplit = 0;
  std::vector<TopDocsCollector::ScoreDoc> topDocs;
};

static std::vector<TopDocsCollector::ScoreDoc> sortedFuzzyDocs(TopDocsCollector& collector) {
  auto docs = collector.sort();
  return {docs.begin(), docs.end()};
}

static int64_t expectFuzzyClauseBoundsSound(
    BooleanQuery::MaxScoreDisjunctionScorer& maxScore, int32_t segnum) {
  int64_t checked = 0;
  auto clauses = maxScore.clauseScorersForTests();
  for (size_t i = 0; i < clauses.size(); i++) {
    Query::Scorer* clause = clauses[i];
    float bound = clause->getMaxScore(PostingsReader::END);
    float observed = 0.0f;
    for (int32_t doc = clause->next(); doc != PostingsReader::END; doc = clause->next()) {
      observed = std::max(observed, clause->score());
    }
    EXPECT_GE(bound * 1.0000001f, observed)
        << "seg=" << segnum << " clause=" << i
        << " bound=" << bound << " observed=" << observed;
    checked++;
  }
  return checked;
}

static FuzzyTopKRun runFuzzyTopK(IndexReader& reader, int32_t topK, bool allowPruning,
                                 bool checkClauseBounds = false, float boost = 1.0f) {
  MemPool pool;
  Query::Context ctx(pool, reader);
  FuzzyQuery fq("body_w", "aaaaa", 1, 0, 0, boost);
  auto* weight = fq.createWeight(ctx, Query::NEED_SCORES);
  TopDocsCollector collector(topK);
  FuzzyTopKRun result;

  auto segments = ctx.topReader.segments();
  for (int32_t segnum = 0; segnum < (int32_t)segments.size(); segnum++) {
    if (checkClauseBounds) {
      auto* boundScorer = weight->createScorer(pool, segments[segnum]);
      if (boundScorer != nullptr) {
        auto* maxScore = dynamic_cast<BooleanQuery::MaxScoreDisjunctionScorer*>(boundScorer);
        EXPECT_NE(maxScore, nullptr);
        if (maxScore != nullptr) {
          result.clauseBoundsChecked += expectFuzzyClauseBoundsSound(*maxScore, segnum);
        }
      }
    }

    auto* scorer = weight->createScorer(pool, segments[segnum]);
    if (scorer == nullptr) continue;
    auto* maxScore = dynamic_cast<BooleanQuery::MaxScoreDisjunctionScorer*>(scorer);
    EXPECT_NE(maxScore, nullptr);
    collectTopK(segnum, scorer, nullptr, nullptr, collector, allowPruning);
    if (maxScore != nullptr) {
      result.maxScoreVisited += maxScore->visited();
      result.nonEssentialLookups += maxScore->nonEssentialLookupCount();
      result.maxSplit = std::max(result.maxSplit, maxScore->currentSplitIndex());
    }
  }

  result.visited = collector.totalHits();
  result.topDocs = sortedFuzzyDocs(collector);
  return result;
}

static void expectSameTopDocs(const FuzzyTopKRun& expected, const FuzzyTopKRun& actual) {
  ASSERT_EQ(expected.topDocs.size(), actual.topDocs.size());
  for (size_t i = 0; i < expected.topDocs.size(); i++) {
    EXPECT_EQ(expected.topDocs[i].doc, actual.topDocs[i].doc);
    EXPECT_EQ(std::bit_cast<uint32_t>(expected.topDocs[i].score),
              std::bit_cast<uint32_t>(actual.topDocs[i].score));
  }
}

static void appendRepeatedFuzzyTerm(std::string& body, std::string_view term, int32_t count) {
  for (int32_t i = 0; i < count; i++) {
    if (!body.empty()) body.push_back(' ');
    body.append(term);
  }
}

TEST_F(FuzzyQueryTest, editDistance) {
  TestIndex ti;
  TestField f(ti, "foo_w");
  f.startIndexing();
  f.add(0, "apple");     // exact
  f.add(1, "apply");     // 1 substitution
  f.add(2, "ample");     // 1 substitution
  f.add(3, "appl");      // 1 deletion
  f.add(4, "apples");    // 1 insertion
  f.add(5, "maple");     // 2 edits
  f.add(6, "appel");     // transposition -> 2 edits under plain Levenshtein
  f.add(7, "banana");    // unrelated
  ti.flush();
  f.startReading();

  using V = std::vector<int32_t>;
  // Exact term only.
  EXPECT_EQ(fuzzyDocs(ti, "foo_w", "apple", 0, 0, 0), (V{0}));
  // Single-edit neighbors.
  EXPECT_EQ(fuzzyDocs(ti, "foo_w", "apple", 1, 0, 0), (V{0, 1, 2, 3, 4}));
  // Two edits add "maple" and the transposition "appel".
  EXPECT_EQ(fuzzyDocs(ti, "foo_w", "apple", 2, 0, 0), (V{0, 1, 2, 3, 4, 5, 6}));
  EXPECT_TRUE(fuzzyDocs(ti, "foo_w", "zzzzz", 2, 0, 0).empty());
  EXPECT_TRUE(fuzzyDocs(ti, "missing_w", "apple", 2, 0, 0).empty());
}

TEST_F(FuzzyQueryTest, nonFuzzyPrefix) {
  TestIndex ti;
  TestField f(ti, "foo_w");
  f.startIndexing();
  f.add(0, "apple");
  f.add(1, "apply");   // shares prefix "ap"
  f.add(2, "ample");   // diverges at index 1 (a[m]ple)
  f.add(3, "maple");   // different first byte
  ti.flush();
  f.startReading();

  using V = std::vector<int32_t>;
  // prefix_length 2 keeps only terms beginning with "ap".
  EXPECT_EQ(fuzzyDocs(ti, "foo_w", "apple", 2, 2, 0), (V{0, 1}));
  // prefix_length 1 allows "ample" but not "maple".
  EXPECT_EQ(fuzzyDocs(ti, "foo_w", "apple", 2, 1, 0), (V{0, 1, 2}));
  // A prefix longer than the term becomes exact.
  EXPECT_EQ(fuzzyDocs(ti, "foo_w", "apple", 2, 99, 0), (V{0}));
}

TEST_F(FuzzyQueryTest, dedupAcrossTerms) {
  TestIndex ti;
  TestField f(ti, "foo_w");
  f.startIndexing();
  f.add(0, "apple apply");   // matches via two distinct fuzzy terms
  f.add(1, "banana");
  ti.flush();
  f.startReading();

  using V = std::vector<int32_t>;
  // Multi-term matches dedup to one doc.
  EXPECT_EQ(fuzzyDocs(ti, "foo_w", "apple", 1, 0, 0), (V{0}));
}

TEST_F(FuzzyQueryTest, advanceAndScore) {
  TestIndex ti;
  TestField f(ti, "foo_w");
  f.startIndexing();
  f.add(0, "apple");   // exact -> dist 0
  f.add(2, "apply");   // 1 sub  -> dist 1
  f.add(4, "ample");   // 1 sub  -> dist 1
  f.add(5, "banana");
  ti.flush();
  f.startReading();

  // advance() jumps to the first match >= target.
  {
    auto g = ti.pool.rewindScopeGuard();
    FuzzyQuery fq("foo_w", "apple", 1, 0);  // matches docs 0, 2, 4
    Query::Context ctx(ti.pool, *ti.reader);
    auto* weight = fq.createWeight(ctx, 0);
    auto* scorer = weight->createScorer(ti.pool, ctx.topReader.segments()[0]);
    ASSERT_NE(scorer, nullptr);
    EXPECT_EQ(scorer->advance(2), 2);
    EXPECT_EQ(scorer->advance(3), 4);
    EXPECT_EQ(scorer->advance(5), PostingsReader::END);
  }

  // Unscored and scored weights match the identical doc set: the expansion
  // cap is a property of the query, not of NEED_SCORES. (score() is only
  // part of the contract when NEED_SCORES was requested.)
  {
    EXPECT_EQ(fuzzyDocs(ti, "foo_w", "apple", 1, 0, 0, 0),
              fuzzyDocs(ti, "foo_w", "apple", 1, 0, 0, Query::NEED_SCORES));
  }

  // Scoring path applies the edit-distance boost to each BM25 term score.
  {
    auto g = ti.pool.rewindScopeGuard();
    FuzzyQuery fq("foo_w", "apple", 1, 0);
    Query::Context ctx(ti.pool, *ti.reader);
    auto* weight = fq.createWeight(ctx, Query::NEED_SCORES);
    EXPECT_FALSE(weight->isConstantScoring());
    auto* scorer = weight->createScorer(ti.pool, ctx.topReader.segments()[0]);
    ASSERT_NE(scorer, nullptr);
    EXPECT_EQ(scorer->next(), 0);            // exact "apple"
    float exact = scorer->score();
    EXPECT_GT(exact, 0.0f);
    EXPECT_EQ(scorer->next(), 2);            // "apply", one edit
    float oneEdit = scorer->score();
    EXPECT_FLOAT_EQ(oneEdit, 0.8f * exact);
    EXPECT_EQ(scorer->next(), 4);            // "ample", also one edit -> same score
    EXPECT_FLOAT_EQ(scorer->score(), oneEdit);
  }
}

// Multiple fuzzy variants in one doc sum their term scores.
TEST_F(FuzzyQueryTest, scoredDisjunctionSums) {
  TestIndex ti;
  TestField f(ti, "foo_w");
  f.startIndexing();
  f.add(0, "apple zzzzz");    // one exact term (+ pad so both docs are length 2)
  f.add(1, "apply ample");    // two 1-edit terms
  ti.flush();
  f.startReading();

  auto g = ti.pool.rewindScopeGuard();
  FuzzyQuery fq("foo_w", "apple", 1, 0);
  Query::Context ctx(ti.pool, *ti.reader);
  auto* weight = fq.createWeight(ctx, Query::NEED_SCORES);
  auto* scorer = weight->createScorer(ti.pool, ctx.topReader.segments()[0]);
  ASSERT_NE(scorer, nullptr);
  EXPECT_EQ(scorer->next(), 0);
  float single = scorer->score();          // exact only
  EXPECT_EQ(scorer->next(), 1);
  float summed = scorer->score();          // two 1-edit terms added
  EXPECT_GT(summed, single);
}

TEST_F(FuzzyQueryTest, multiVariantDocScoreIsSumOfVariantScores) {
  TestIndex ti;
  TestField f(ti, "foo_w");
  f.startIndexing();
  f.add(0, "apply pad");
  f.add(1, "ample pad");
  f.add(2, "apply ample");
  ti.flush();
  f.startReading();

  auto g = ti.pool.rewindScopeGuard();
  FuzzyQuery fq("foo_w", "apple", 1, 0);
  Query::Context ctx(ti.pool, *ti.reader);
  auto* scorer = fq.createWeight(ctx, Query::NEED_SCORES)
      ->createScorer(ti.pool, ctx.topReader.segments()[0]);
  ASSERT_NE(scorer, nullptr);
  EXPECT_EQ(scorer->next(), 0);
  float apply = scorer->score();
  EXPECT_EQ(scorer->next(), 1);
  float ample = scorer->score();
  EXPECT_EQ(scorer->next(), 2);
  EXPECT_FLOAT_EQ(apply + ample, scorer->score());
}

TEST_F(FuzzyQueryTest, multiSegment) {
  TestIndex ti;
  TestField f(ti, "foo_w");
  f.startIndexing();
  f.add(0, "apple");
  f.add(1, "apply");
  ti.flush();
  f.startIndexing();
  f.add(0, "ample");
  f.add(1, "banana");
  ti.flush();
  f.startReading();

  using V = std::vector<int32_t>;
  // Each segment is scored independently in its own doc-id space.
  EXPECT_EQ(fuzzyDocs(ti, "foo_w", "apple", 1, 0, 0), (V{0, 1}));  // apple, apply
  EXPECT_EQ(fuzzyDocs(ti, "foo_w", "apple", 1, 0, 1), (V{0}));     // ample
}

// QueryBuilder validates field type and resolves AUTO fuzziness.
TEST_F(FuzzyQueryTest, builderValidationAndAuto) {
  auto schema = Schema::createDefaultSchema();
  MemPool pool;
  QueryBuilder builder(pool, *schema, CoerceContext{});

  EXPECT_NE(builder.createFuzzyQuery("foo_w", "apple"), nullptr);  // TEXT
  EXPECT_NE(builder.createFuzzyQuery("foo_s", "apple"), nullptr);  // STRING
  EXPECT_NE(builder.createFuzzyQuery("id", "d1"), nullptr);        // ID
  EXPECT_THROW(builder.createFuzzyQuery("foo_i", "1"), std::runtime_error);     // INT: no terms
  EXPECT_THROW(builder.createFuzzyQuery("foo_w", "apple", 3), std::runtime_error);  // max_edits > 2
  // Invalid explicit knobs are rejected.
  EXPECT_THROW(builder.createFuzzyQuery("foo_w", "apple", -1), std::runtime_error);     // negative max_edits
  EXPECT_THROW(builder.createFuzzyQuery("foo_w", "apple", 1, -1), std::runtime_error);  // negative prefix_length
  EXPECT_THROW(builder.createFuzzyQuery("foo_w", "apple", 1, 0, -1), std::runtime_error);  // negative max_expansions

  // AUTO thresholds.
  EXPECT_EQ(QueryBuilder::autoMaxEdits(2), 0);
  EXPECT_EQ(QueryBuilder::autoMaxEdits(5), 1);
  EXPECT_EQ(QueryBuilder::autoMaxEdits(6), 2);
}

// Unset prefix_length defaults to 1; explicit 0 disables it.
TEST_F(FuzzyQueryTest, builderDefaultPrefixLength) {
  auto schema = Schema::createDefaultSchema();
  MemPool pool;
  QueryBuilder builder(pool, *schema, CoerceContext{});

  auto* dflt = (FuzzyQuery*)builder.createFuzzyQuery("foo_w", "apple", 2);  // prefix unset
  EXPECT_EQ(dflt->getPrefixLength(), 1);
  auto* full = (FuzzyQuery*)builder.createFuzzyQuery("foo_w", "apple", 2, 0);  // explicit 0
  EXPECT_EQ(full->getPrefixLength(), 0);
}

// The default prefix excludes leading edits.
TEST_F(FuzzyQueryTest, defaultPrefixExcludesLeadingEdit) {
  TestIndex ti;
  TestField f(ti, "foo_w");
  f.startIndexing();
  f.add(0, "apple");
  f.add(1, "maple");  // 2 edits from "apple", but differs in the first character
  ti.flush();
  f.startReading();

  auto schema = Schema::createDefaultSchema();
  using V = std::vector<int32_t>;

  // Default prefix (1): "maple" is excluded (first char differs).
  {
    auto g = ti.pool.rewindScopeGuard();
    QueryBuilder builder(ti.pool, *schema, CoerceContext{});
    auto* q = builder.createFuzzyQuery("foo_w", "apple", 2);  // default prefix 1
    Query::Context ctx(ti.pool, *ti.reader);
    auto* scorer = q->createWeight(ctx, 0)->createScorer(ti.pool, ctx.topReader.segments()[0]);
    std::vector<int32_t> docs;
    for (int32_t d = scorer->next(); d != PostingsReader::END; d = scorer->next()) docs.push_back(d);
    EXPECT_EQ(docs, (V{0}));
  }
  // Explicit prefix 0: "maple" is now reachable.
  {
    auto g = ti.pool.rewindScopeGuard();
    QueryBuilder builder(ti.pool, *schema, CoerceContext{});
    auto* q = builder.createFuzzyQuery("foo_w", "apple", 2, 0);  // full fuzzy
    Query::Context ctx(ti.pool, *ti.reader);
    auto* scorer = q->createWeight(ctx, 0)->createScorer(ti.pool, ctx.topReader.segments()[0]);
    std::vector<int32_t> docs;
    for (int32_t d = scorer->next(); d != PostingsReader::END; d = scorer->next()) docs.push_back(d);
    EXPECT_EQ(docs, (V{0, 1}));
  }
}

// End-to-end protobuf coverage.
class FuzzyQueryE2ETest : public LuxirTest {
public:
  CollectionHelper helper;

  FuzzyQueryE2ETest() {
    helper.index(flatdoc("id", "d1", "body_w", "apple", "color_s", "red"),
                 UpdateMessage::NO_COMMIT);
    helper.index(flatdoc("id", "d2", "body_w", "apply", "color_s", "reddish"),
                 UpdateMessage::NO_COMMIT);
    helper.index(flatdoc("id", "d3", "body_w", "ample", "color_s", "read"),
                 UpdateMessage::NO_COMMIT);
    helper.index(flatdoc("id", "d4", "body_w", "banana", "color_s", "blue"),
                 UpdateMessage::COMMIT);
  }

  std::vector<std::string> fuzzyIds(std::string_view field, std::string_view term,
                                    int maxEdits = -1, int prefixLength = -1) {
    auto req = localReq(helper.getSearchEngine());
    req->collection("main").topDocs("q").fuzzyQuery(field, term, maxEdits, prefixLength)
        .fields({"id"}).limit(100);
    req->execute();
    std::vector<std::string> ids;
    for (auto& doc : req->getDocs()) {
      if (auto* v = find(doc, "id")) ids.push_back(std::get<std::string>(*v));
    }
    std::sort(ids.begin(), ids.end());
    return ids;
  }
};

TEST_F(FuzzyQueryE2ETest, textField) {
  using S = std::vector<std::string>;
  EXPECT_EQ(fuzzyIds("body_w", "apple", 1), (S{"d1", "d2", "d3"}));  // apple, apply, ample
  EXPECT_EQ(fuzzyIds("body_w", "apple", 0), (S{"d1"}));              // exact only
  EXPECT_EQ(fuzzyIds("body_w", "banan", 1), (S{"d4"}));             // banana within 1
  EXPECT_TRUE(fuzzyIds("body_w", "zzzzz", 2).empty());
}

TEST_F(FuzzyQueryE2ETest, autoFuzziness) {
  using S = std::vector<std::string>;
  // "apple" is 5 bytes -> AUTO picks 1 edit.
  EXPECT_EQ(fuzzyIds("body_w", "apple"), (S{"d1", "d2", "d3"}));
}

TEST_F(FuzzyQueryE2ETest, textTermIsNormalized) {
  using S = std::vector<std::string>;
  // multiterm input folds the way the field folds; AUTO edits come from the
  // normalized bytes
  helper.index(flatdoc("id", "d5", "title_un", "Blade Runner"), UpdateMessage::COMMIT);
  EXPECT_EQ(fuzzyIds("title_un", "Blabe", 1), (S{"d5"}));
  EXPECT_EQ(fuzzyIds("title_un", "RUNNER", 0), (S{"d5"}));  // fold, then exact
  // STRING fields are unanalyzed: the term stays verbatim
  EXPECT_TRUE(fuzzyIds("color_s", "RED", 1).empty());
}

TEST_F(FuzzyQueryE2ETest, stringFieldWithPrefix) {
  using S = std::vector<std::string>;
  // STRING field, exact term storage: "red"/"reddish"/"read".
  // 1 edit from "red": "read" (insert a) -> d3; "reddish" is far.
  EXPECT_EQ(fuzzyIds("color_s", "red", 1), (S{"d1", "d3"}));
  // prefix_length 3 pins "red", so "read" (diverges at index 2) drops out.
  EXPECT_EQ(fuzzyIds("color_s", "red", 1, 3), (S{"d1"}));
}

TEST_F(FuzzyQueryE2ETest, negativeMaxExpansionsRejected) {
  auto req = localReq(helper.getSearchEngine());
  auto& cur = req->collection("main").topDocs("q");
  cur.fuzzyQuery("body_w", "apple", 1, 0).fields({"id"}).limit(10);
  auto& fuzzy = std::get<api::FuzzyQuery>(cur.rawQuery().kind);
  fuzzy.max_expansions = -1;
  req->execute();
  EXPECT_FALSE(req->ok());
  EXPECT_NE(req->errorMsg().find("max_expansions"), std::string::npos) << req->errorMsg();
}

static std::string twoEditVariant(int32_t idx) {
  std::string out(8, 'a');
  for (int32_t p1 = 0; p1 < 8; p1++) {
    for (int32_t p2 = p1 + 1; p2 < 8; p2++) {
      if (idx < 25 * 25) {
        out[(size_t)p1] = (char)('b' + idx / 25);
        out[(size_t)p2] = (char)('b' + idx % 25);
        return out;
      }
      idx -= 25 * 25;
    }
  }
  return out;
}

TEST_F(FuzzyQueryTest, exactOutranksRareOneAndTwoEditVariants) {
  CollectionHelper helper{"main"};
  for (int32_t i = 0; i < 16; i++) {
    helper.index(flatdoc("id", "exact" + std::to_string(i),
                         "body_w", "apple pad"), UpdateMessage::NO_COMMIT);
  }
  helper.index(flatdoc("id", "one_typo", "body_w", "apply pad"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id", "two_typo", "body_w", "appel pad"), UpdateMessage::COMMIT);

  auto req = localReq(helper.getSearchEngine());
  req->collection("main").topDocs("q")
      .fuzzyQuery("body_w", "apple", 2, 0)
      .fields({"id"}).limit(-1).getScores();
  req->execute();
  ASSERT_TRUE(req->ok()) << req->errorMsg();
  const auto* docs = req->docList();
  ASSERT_NE(docs, nullptr);
  const auto& ids = std::get<api::ColStr>(docs->columns.at("id").kind).v;
  const auto& scores = std::get<api::ColFloat>(docs->columns.at("_score_").kind).v;

  auto findIndex = [&](std::string_view id) {
    for (size_t i = 0; i < ids.size(); i++) {
      if (ids[i] == id) return i;
    }
    return ids.size();
  };
  size_t firstExact = findIndex("exact0");
  size_t oneTypo = findIndex("one_typo");
  size_t twoTypo = findIndex("two_typo");
  ASSERT_LT(firstExact, ids.size());
  ASSERT_LT(oneTypo, ids.size());
  ASSERT_LT(twoTypo, ids.size());
  EXPECT_LT(firstExact, oneTypo);
  EXPECT_LT(firstExact, twoTypo);
  EXPECT_GT(scores[firstExact], scores[oneTypo]);
  EXPECT_GT(scores[firstExact], scores[twoTypo]);
}

TEST_F(FuzzyQueryTest, unsetMaxExpansionsDefaultsToFifty) {
  CollectionHelper helper{"main"};
  for (int32_t i = 0; i < 60; i++) {
    std::string term = "aaaaa";
    int32_t pos = 1 + i / 25;
    term[(size_t)pos] = (char)('b' + (i % 25));
    helper.index(flatdoc("id", "d" + std::to_string(i), "body_w", term),
                 i == 59 ? UpdateMessage::COMMIT : UpdateMessage::NO_COMMIT);
  }

  // Unset max_expansions caps at the Lucene-compatible default (50), silently
  // (matching Lucene/ES; the default is documented, not a surprise clamp).
  auto req = localReq(helper.getSearchEngine());
  req->collection("main").topDocs("q")
      .fuzzyQuery("body_w", "aaaaa")
      .fields({"id"}).limit(100);
  req->execute();
  ASSERT_TRUE(req->ok()) << req->errorMsg();
  EXPECT_EQ((size_t)FuzzyQuery::DEFAULT_MAX_EXPANSIONS, req->getDocs().size());
  EXPECT_TRUE(req->respWarnings().empty());

  // An explicit max_expansions above the matched count returns everything.
  auto req2 = localReq(helper.getSearchEngine());
  req2->collection("main").topDocs("q")
      .fuzzyQuery("body_w", "aaaaa", 1, 0, 100)
      .fields({"id"}).limit(100);
  req2->execute();
  ASSERT_TRUE(req2->ok()) << req2->errorMsg();
  EXPECT_EQ(60u, req2->getDocs().size());
}

TEST_F(FuzzyQueryTest, scoringClauseBudgetTruncatesSilently) {
  CollectionHelper helper{"main"};
  constexpr int32_t kMatches = FuzzyQuery::FUZZY_CLAUSE_BUDGET + 1;
  for (int32_t i = 0; i < kMatches; i++) {
    std::string term = "aaaaa";
    int32_t pos = 1 + i / 25;
    term[(size_t)pos] = (char)('b' + (i % 25));
    helper.index(flatdoc("id", "d" + std::to_string(i), "body_w", term),
                 i == kMatches - 1 ? UpdateMessage::COMMIT : UpdateMessage::NO_COMMIT);
  }

  // Explicit max_expansions above the budget: the budget clamps without a
  // request warning. The truncation is execution policy, not response data.
  auto req = localReq(helper.getSearchEngine());
  req->collection("main").topDocs("q")
      .fuzzyQuery("body_w", "aaaaa", 1, 0, 100)
      .fields({"id"}).limit(100).getScores();
  req->execute();
  ASSERT_TRUE(req->ok()) << req->errorMsg();
  EXPECT_EQ((size_t)FuzzyQuery::FUZZY_CLAUSE_BUDGET, req->getDocs().size());
  EXPECT_TRUE(req->respWarnings().empty());
}

TEST_F(FuzzyQueryTest, scoredFuzzyTopKMatchesBruteForceWithPruningEngaged) {
  CollectionHelper helper{"main"};

  std::vector<Doc> docs;
  docs.reserve((size_t)(8 + 3 + 5 * 384));
  std::string body;
  // Medium-strength exact matches FIRST: these raise the top-k threshold
  // early. The true top-3 docs are appended LAST (below), so an
  // under-estimated clause bound would wrongly skip the late window that
  // contains them - this ordering is what lets the brute-force comparison
  // detect unsound pruning bounds, not just measure that pruning happened.
  for (int32_t i = 0; i < 8; i++) {
    body.clear();
    appendRepeatedFuzzyTerm(body, "aaaaa", 40 - i);
    docs.push_back(flatdoc("id", "h" + std::to_string(i), "body_w", body));
  }

  std::vector<std::string> variants = {"aaaab", "aaaac", "aaaad", "aaaae", "aaaaf"};
  for (size_t v = 0; v < variants.size(); v++) {
    for (int32_t i = 0; i < 384; i++) {
      body.clear();
      appendRepeatedFuzzyTerm(body, variants[v], 1);
      appendRepeatedFuzzyTerm(body, "filler", 320);
      docs.push_back(flatdoc("id", "l" + std::to_string(v) + "_" + std::to_string(i),
                             "body_w", body));
    }
  }
  // True top-3 at the LAST docids (see the ordering comment above).
  for (int32_t i = 0; i < 3; i++) {
    body.clear();
    appendRepeatedFuzzyTerm(body, "aaaaa", 80 - i);
    docs.push_back(flatdoc("id", "z" + std::to_string(i), "body_w", body));
  }
  helper.indexAll(docs, UpdateMessage::COMMIT);

  auto reader = helper.getIndexWriter()->getIndexReader();
  FuzzyTopKRun bruteForce = runFuzzyTopK(*reader, 3, false, true);
  FuzzyTopKRun pruned = runFuzzyTopK(*reader, 3, true);
  expectSameTopDocs(bruteForce, pruned);
  EXPECT_GT(bruteForce.clauseBoundsChecked, 0);
  EXPECT_LT(pruned.visited, bruteForce.visited);
  EXPECT_LT(pruned.maxScoreVisited, bruteForce.maxScoreVisited);
  EXPECT_GT(pruned.maxSplit, 0);
  EXPECT_GT(pruned.nonEssentialLookups, 0);
}

TEST_F(FuzzyQueryTest, boostedFuzzyTopKMatchesBruteForceWithPruningEngaged) {
  CollectionHelper helper{"main"};

  std::vector<Doc> docs;
  docs.reserve((size_t)(5 + 3 * 256 + 3));
  std::string body;
  for (int32_t i = 0; i < 5; i++) {
    body.clear();
    appendRepeatedFuzzyTerm(body, "aaaaa", 28 - i);
    docs.push_back(flatdoc("id", "bh" + std::to_string(i), "body_w", body));
  }
  std::vector<std::string> variants = {"aaaab", "aaaac", "aaaad"};
  for (size_t v = 0; v < variants.size(); v++) {
    for (int32_t i = 0; i < 256; i++) {
      body.clear();
      appendRepeatedFuzzyTerm(body, variants[v], 1);
      appendRepeatedFuzzyTerm(body, "filler", 260);
      docs.push_back(flatdoc("id", "bl" + std::to_string(v) + "_" + std::to_string(i),
                             "body_w", body));
    }
  }
  for (int32_t i = 0; i < 3; i++) {
    body.clear();
    appendRepeatedFuzzyTerm(body, "aaaaa", 60 - i);
    docs.push_back(flatdoc("id", "bz" + std::to_string(i), "body_w", body));
  }
  helper.indexAll(docs, UpdateMessage::COMMIT);

  auto reader = helper.getIndexWriter()->getIndexReader();
  FuzzyTopKRun bruteForce = runFuzzyTopK(*reader, 3, false, true, 2.75f);
  FuzzyTopKRun pruned = runFuzzyTopK(*reader, 3, true, false, 2.75f);
  expectSameTopDocs(bruteForce, pruned);
  EXPECT_GT(bruteForce.clauseBoundsChecked, 0);
  EXPECT_LT(pruned.visited, bruteForce.visited);
}

TEST_F(FuzzyQueryTest, operatorExpansionClampIsSilent) {
  CollectionHelper helper{"main"};
  constexpr int32_t kDocs = 10005;
  std::vector<Doc> docs;
  docs.reserve(kDocs);
  for (int32_t i = 0; i < kDocs; i++) {
    docs.push_back(
        flatdoc("id", "d" + std::to_string(i), "body_w", twoEditVariant(i)));
  }
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);

  // Explicit max_expansions above the operator limit: the operator clamp
  // engages without making response contents depend on a dictionary walk.
  auto req = localReq(helper.getSearchEngine());
  req->collection("main").topDocs("q")
      .fuzzyQuery("body_w", "aaaaaaaa", 2, 0, 20000)
      .fields({"id"}).limit(1);
  req->execute();
  ASSERT_TRUE(req->ok()) << req->errorMsg();
  EXPECT_TRUE(req->respWarnings().empty());
}
