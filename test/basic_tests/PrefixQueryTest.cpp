#include <gtest/gtest.h>

#include "test/SoluxTest.h"
#include "test/TestIndex.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/QueryBuild.h"
#include "solux/query/PrefixQuery.h"
#include "solux/query/QueryBuilder.h"
#include "solux/schema/Schema.h"

using namespace solux;
using namespace solux::test;

// Drive PrefixQuery::Weight::createScorer directly to assert per-segment doc ids.
class PrefixQueryTest : public SoluxTest {
protected:
  // Collect the docs a prefix query matches in one segment, in iteration order.
  std::vector<int32_t> prefixDocs(TestIndex& ti, std::string_view field,
                                  std::string_view prefix, int segOrd, int32_t flags = 0) {
    auto g = ti.pool.rewindScopeGuard();
    PrefixQuery pq(field, prefix);
    Query::Context ctx(ti.pool, *ti.reader);
    auto* weight = pq.createWeight(ctx, flags);
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

TEST_F(PrefixQueryTest, singleSegment) {
  TestIndex ti;
  TestField f(ti, "foo_w");
  f.startIndexing();
  f.add(0, "apple apricot");
  f.add(1, "banana");
  f.add(2, "apricot avocado");
  f.add(3, "cherry");
  f.add(4, "apple");
  ti.flush();
  f.startReading();

  using V = std::vector<int32_t>;
  // Several terms share the prefix; a doc matching two of them appears once.
  EXPECT_EQ(prefixDocs(ti, "foo_w", "ap", 0), (V{0, 2, 4}));
  EXPECT_EQ(prefixDocs(ti, "foo_w", "apr", 0), (V{0, 2}));
  EXPECT_EQ(prefixDocs(ti, "foo_w", "a", 0), (V{0, 2, 4}));
  EXPECT_EQ(prefixDocs(ti, "foo_w", "av", 0), (V{2}));
  EXPECT_EQ(prefixDocs(ti, "foo_w", "b", 0), (V{1}));
  // An exact full term is just a one-term prefix.
  EXPECT_EQ(prefixDocs(ti, "foo_w", "apple", 0), (V{0, 4}));
  // Empty prefix matches every doc that has the field.
  EXPECT_EQ(prefixDocs(ti, "foo_w", "", 0), (V{0, 1, 2, 3, 4}));
  EXPECT_TRUE(prefixDocs(ti, "foo_w", "z", 0).empty());
  EXPECT_TRUE(prefixDocs(ti, "foo_w", "cherryX", 0).empty());
  EXPECT_TRUE(prefixDocs(ti, "foo_w", "bb", 0).empty());
  // No matches: field absent in the index.
  EXPECT_TRUE(prefixDocs(ti, "missing_w", "a", 0).empty());
}

TEST_F(PrefixQueryTest, advanceAndScore) {
  TestIndex ti;
  TestField f(ti, "foo_w");
  f.startIndexing();
  f.add(0, "apple");
  f.add(2, "apricot");
  f.add(4, "avocado");
  f.add(5, "banana");
  ti.flush();
  f.startReading();

  // advance() jumps to the first match >= target.
  {
    auto g = ti.pool.rewindScopeGuard();
    PrefixQuery pq("foo_w", "a");  // matches docs 0, 2, 4
    Query::Context ctx(ti.pool, *ti.reader);
    auto* weight = pq.createWeight(ctx, 0);
    auto* scorer = weight->createScorer(ti.pool, ctx.topReader.segments()[0]);
    ASSERT_NE(scorer, nullptr);
    EXPECT_EQ(scorer->advance(2), 2);    // lands on an exact match
    EXPECT_EQ(scorer->advance(3), 4);    // skips to the next match
    EXPECT_EQ(scorer->advance(5), PostingsReader::END);  // past the last match
  }

  // With NEED_SCORES, every hit gets the constant boost (1.0 by default).
  {
    auto g = ti.pool.rewindScopeGuard();
    PrefixQuery pq("foo_w", "a");
    Query::Context ctx(ti.pool, *ti.reader);
    auto* weight = pq.createWeight(ctx, Query::NEED_SCORES);
    EXPECT_TRUE(weight->isConstantScoring());
    auto* scorer = weight->createScorer(ti.pool, ctx.topReader.segments()[0]);
    ASSERT_NE(scorer, nullptr);
    EXPECT_EQ(scorer->next(), 0);
    EXPECT_FLOAT_EQ(scorer->score(), 1.0f);
  }
}

TEST_F(PrefixQueryTest, multiSegment) {
  TestIndex ti;
  TestField f(ti, "foo_w");
  f.startIndexing();
  f.add(0, "apple");
  f.add(1, "apricot");
  ti.flush();
  f.startIndexing();
  f.add(0, "avocado");
  f.add(1, "banana");
  ti.flush();
  f.startReading();

  using V = std::vector<int32_t>;
  // Each segment is scored independently with its own doc-id space / maxDoc.
  EXPECT_EQ(prefixDocs(ti, "foo_w", "a", 0), (V{0, 1}));  // apple, apricot
  EXPECT_EQ(prefixDocs(ti, "foo_w", "a", 1), (V{0}));     // avocado
  EXPECT_TRUE(prefixDocs(ti, "foo_w", "b", 0).empty());   // no b* in seg 0
  EXPECT_EQ(prefixDocs(ti, "foo_w", "b", 1), (V{1}));     // banana
}

// QueryBuilder validates that prefix queries only run on term-backed fields.
TEST_F(PrefixQueryTest, fieldTypeValidation) {
  auto schema = Schema::createDefaultSchema();
  MemPool pool;
  QueryBuilder builder(pool, *schema, CoerceContext{});

  EXPECT_NE(builder.createPrefixQuery("foo_s", "ab"), nullptr);   // STRING
  EXPECT_NE(builder.createPrefixQuery("foo_w", "ab"), nullptr);   // TEXT
  EXPECT_NE(builder.createPrefixQuery("id", "d"), nullptr);       // ID
  EXPECT_THROW(builder.createPrefixQuery("foo_i", "1"), std::runtime_error);  // INT: no terms
}

// End-to-end coverage for protobuf parsing, query building, and execution.
class PrefixQueryE2ETest : public SoluxTest {
public:
  CollectionHelper helper;

  PrefixQueryE2ETest() {
    helper.index(flatdoc("id", "d1", "body_w", "apple apricot", "color_s", "red"),
                 UpdateMessage::NO_COMMIT);
    helper.index(flatdoc("id", "d2", "body_w", "banana", "color_s", "reddish"),
                 UpdateMessage::NO_COMMIT);
    helper.index(flatdoc("id", "d3", "body_w", "apricot avocado", "color_s", "blue"),
                 UpdateMessage::NO_COMMIT);
    helper.index(flatdoc("id", "d4", "body_w", "cherry apple", "color_s", "red"),
                 UpdateMessage::COMMIT);
  }

  int64_t prefixCount(std::string_view field, std::string_view prefix) {
    auto req = localReq(helper.getSearchEngine());
    req->collection("main").topDocs("q").prefixQuery(field, prefix).withStats();
    req->execute();
    return req->getMatchCount();
  }

  std::vector<std::string> prefixIds(std::string_view field, std::string_view prefix) {
    auto req = localReq(helper.getSearchEngine());
    req->collection("main").topDocs("q").prefixQuery(field, prefix).fields({"id"}).limit(100);
    req->execute();
    std::vector<std::string> ids;
    for (auto& doc : req->getDocs()) {
      if (auto* v = find(doc, "id")) ids.push_back(std::get<std::string>(*v));
    }
    std::sort(ids.begin(), ids.end());
    return ids;
  }
};

TEST_F(PrefixQueryE2ETest, textField) {
  EXPECT_EQ(prefixCount("body_w", "ap"), 3);   // apple, apricot -> d1, d3, d4
  EXPECT_EQ(prefixCount("body_w", "a"), 3);    // + avocado, still d1, d3, d4
  EXPECT_EQ(prefixCount("body_w", "ban"), 1);  // banana -> d2
  EXPECT_EQ(prefixCount("body_w", ""), 4);     // empty prefix matches every doc
  EXPECT_EQ(prefixCount("body_w", "z"), 0);    // no matches
}

TEST_F(PrefixQueryE2ETest, asBooleanFilter) {
  // Exercises prefix as a filter clause under conjunction planning.
  auto req = localReq(helper.getSearchEngine());
  req->collection("main");
  auto& cur = req->topDocs("q");
  cur.rawQuery() = qb::boolean(cur.mr(),
      /*required=*/{qb::match(cur.mr(), "body_w", "apple")},
      /*optional=*/{}, /*prohibited=*/{},
      /*filter=*/{qb::prefix(cur.mr(), "color_s", "re")});
  cur.fields({"id"}).limit(100);
  req->execute();

  std::vector<std::string> ids;
  for (auto& doc : req->getDocs()) {
    if (auto* v = find(doc, "id")) ids.push_back(std::get<std::string>(*v));
  }
  std::sort(ids.begin(), ids.end());
  EXPECT_EQ(ids, (std::vector<std::string>{"d1", "d4"}));
}

TEST_F(PrefixQueryE2ETest, textPrefixIsNormalized) {
  // multiterm input folds the way the field folds (never tokenized), so a
  // capitalized prefix finds lowercased indexed terms
  helper.index(flatdoc("id", "d5", "title_wl", "Blade Runner"), UpdateMessage::COMMIT);
  EXPECT_EQ(prefixCount("title_wl", "Runn"), 1);
  EXPECT_EQ(prefixCount("title_wl", "runn"), 1);
  EXPECT_EQ(prefixCount("title_wl", "BLADE"), 1);
  // STRING fields are unanalyzed: the prefix stays verbatim
  EXPECT_EQ(prefixCount("color_s", "RED"), 0);
}

TEST_F(PrefixQueryE2ETest, stringField) {
  EXPECT_EQ(prefixCount("color_s", "re"), 3);    // red (d1, d4), reddish (d2)
  EXPECT_EQ(prefixCount("color_s", "red"), 3);   // "red" still matches "reddish"
  EXPECT_EQ(prefixCount("color_s", "redd"), 1);  // only reddish -> d2
  EXPECT_EQ(prefixIds("color_s", "blue"), (std::vector<std::string>{"d3"}));
  EXPECT_EQ(prefixIds("color_s", "re"), (std::vector<std::string>{"d1", "d2", "d4"}));
}
