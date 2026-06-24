#include <gtest/gtest.h>

#include <algorithm>

#include "test/SoluxTest.h"
#include "test/TestIndex.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "solux/query/FuzzyQuery.h"
#include "solux/query/QueryBuilder.h"
#include "solux/schema/Schema.h"

using namespace solux;
using namespace solux::test;

// Direct per-segment fuzzy scorer checks.
class FuzzyQueryTest : public SoluxTest {
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

  // Filter path is constant-scoring.
  {
    auto g = ti.pool.rewindScopeGuard();
    FuzzyQuery fq("foo_w", "apple", 1, 0);
    Query::Context ctx(ti.pool, *ti.reader);
    auto* weight = fq.createWeight(ctx, 0);
    EXPECT_TRUE(weight->isConstantScoring());
    auto* scorer = weight->createScorer(ti.pool, ctx.topReader.segments()[0]);
    ASSERT_NE(scorer, nullptr);
    EXPECT_EQ(scorer->next(), 0);
    EXPECT_FLOAT_EQ(scorer->score(), 1.0f);
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
  QueryBuilder builder(pool, *schema);

  EXPECT_NE(builder.createFuzzyQuery("foo_w", "apple"), nullptr);  // TEXT
  EXPECT_NE(builder.createFuzzyQuery("foo_s", "apple"), nullptr);  // STRING
  EXPECT_NE(builder.createFuzzyQuery("id", "d1"), nullptr);        // ID
  EXPECT_THROW(builder.createFuzzyQuery("foo_i", "1"), std::runtime_error);     // INT: no terms
  EXPECT_THROW(builder.createFuzzyQuery("foo_w", "apple", 3), std::runtime_error);  // max_edits > 2
  // Invalid explicit knobs are rejected.
  EXPECT_THROW(builder.createFuzzyQuery("foo_w", "apple", -1), std::runtime_error);     // negative max_edits
  EXPECT_THROW(builder.createFuzzyQuery("foo_w", "apple", 1, -1), std::runtime_error);  // negative prefix_length

  // AUTO thresholds.
  EXPECT_EQ(QueryBuilder::autoMaxEdits(2), 0);
  EXPECT_EQ(QueryBuilder::autoMaxEdits(5), 1);
  EXPECT_EQ(QueryBuilder::autoMaxEdits(6), 2);
}

// Unset prefix_length defaults to 1; explicit 0 disables it.
TEST_F(FuzzyQueryTest, builderDefaultPrefixLength) {
  auto schema = Schema::createDefaultSchema();
  MemPool pool;
  QueryBuilder builder(pool, *schema);

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
    QueryBuilder builder(ti.pool, *schema);
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
    QueryBuilder builder(ti.pool, *schema);
    auto* q = builder.createFuzzyQuery("foo_w", "apple", 2, 0);  // full fuzzy
    Query::Context ctx(ti.pool, *ti.reader);
    auto* scorer = q->createWeight(ctx, 0)->createScorer(ti.pool, ctx.topReader.segments()[0]);
    std::vector<int32_t> docs;
    for (int32_t d = scorer->next(); d != PostingsReader::END; d = scorer->next()) docs.push_back(d);
    EXPECT_EQ(docs, (V{0, 1}));
  }
}

// End-to-end protobuf coverage.
class FuzzyQueryE2ETest : public SoluxTest {
public:
  CollectionHelper helper;

  FuzzyQueryE2ETest() {
    helper.clear();
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
    auto* req = LocalReq::create(helper.getSearchEngine());
    req->collection("main").fuzzyQuery(field, term, maxEdits, prefixLength)
        .fields({"id"}).limit(100).execute();
    std::vector<std::string> ids;
    for (auto& doc : req->getDocs()) {
      if (auto* v = find(doc, "id")) ids.push_back(std::get<std::string>(*v));
    }
    req->done();
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

TEST_F(FuzzyQueryE2ETest, stringFieldWithPrefix) {
  using S = std::vector<std::string>;
  // STRING field, exact term storage: "red"/"reddish"/"read".
  // 1 edit from "red": "read" (insert a) -> d3; "reddish" is far.
  EXPECT_EQ(fuzzyIds("color_s", "red", 1), (S{"d1", "d3"}));
  // prefix_length 3 pins "red", so "read" (diverges at index 2) drops out.
  EXPECT_EQ(fuzzyIds("color_s", "red", 1, 3), (S{"d1"}));
}
