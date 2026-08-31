#include <gtest/gtest.h>

#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/QueryBuild.h"
#include "test/LuxirTest.h"
#include "luxir/query/AutomatonQuery.h"
#include "luxir/query/QueryBuilder.h"
#include "luxir/schema/Schema.h"

using namespace luxir;
using namespace luxir::test;

class WildcardQueryE2ETest : public LuxirTest {
public:
  CollectionHelper helper;

  WildcardQueryE2ETest() {
    helper.index(flatdoc("id", "d1", "body_w", "Foobar apple", "title_un", "Foobar", "color_s", "foo*bar"), UpdateMessage::NO_COMMIT);
    helper.index(flatdoc("id", "d2", "body_w", "Food apple", "title_un", "Food", "color_s", "foobar"), UpdateMessage::NO_COMMIT);
    helper.index(flatdoc("id", "d3", "body_w", "fool", "title_un", "fool", "color_s", "FOOBAR"), UpdateMessage::NO_COMMIT);
    helper.index(flatdoc("id", "d4", "body_w", "banana", "color_s", "other"), UpdateMessage::COMMIT);
  }

  int64_t wildcardCount(std::string_view field, std::string_view pattern) {
    auto req = localReq(helper.getSearchEngine());
    auto& cur = req->collection("main").topDocs("q");
    cur.rawQuery() = qb::wildcard(cur.mr(), field, pattern);
    cur.withStats();
    req->execute();
    return req->getMatchCount();
  }

  int64_t prefixCount(std::string_view field, std::string_view prefix) {
    auto req = localReq(helper.getSearchEngine());
    req->collection("main").topDocs("q").prefixQuery(field, prefix).withStats();
    req->execute();
    return req->getMatchCount();
  }
};

TEST_F(WildcardQueryE2ETest, protoExprAndFilter) {
  EXPECT_EQ(wildcardCount("body_w", "foo*"), prefixCount("body_w", "foo"));
  EXPECT_EQ(wildcardCount("color_s", "foo\\*bar"), 1);
  EXPECT_EQ(wildcardCount("body_w", "*"), 4);
  EXPECT_EQ(wildcardCount("color_s", "foo*bar"), 2);  // NORMAL includes exact "foobar"
  EXPECT_EQ(wildcardCount("body_w", std::string(301, 'a') + "*"), 0);
  EXPECT_EQ(wildcardCount("title_un", "FOO*"), 3);      // whole-pattern TEXT folding
  EXPECT_EQ(wildcardCount("color_s", "FOO*"), 1);      // STRING stays verbatim

  // Empty wildcard matches only an empty term; this schema does not index one.
  EXPECT_EQ(wildcardCount("body_w", ""), 0);

  auto req = localReq(helper.getSearchEngine());
  req->collection("main").topDocs("q").exprQuery("wildcard('foo*bar', field=color_s)").withStats();
  req->execute();
  EXPECT_EQ(req->getMatchCount(), 2);

  auto filtered = localReq(helper.getSearchEngine());
  auto& cur = filtered->collection("main").topDocs("q");
  cur.rawQuery() = qb::boolean(cur.mr(), {qb::match(cur.mr(), "body_w", "apple")}, {}, {},
                               {qb::wildcard(cur.mr(), "color_s", "foo*bar")});
  cur.withStats();
  filtered->execute();
  EXPECT_EQ(filtered->getMatchCount(), 2);
}

TEST_F(WildcardQueryE2ETest, invalidPatternNamesInput) {
  auto req = localReq(helper.getSearchEngine());
  auto& cur = req->collection("main").topDocs("q");
  cur.rawQuery() = qb::wildcard(cur.mr(), "color_s", "foo\\");
  req->execute();
  EXPECT_FALSE(req->ok());
  EXPECT_NE(req->errorMsg().find("color_s"), std::string::npos);
  EXPECT_NE(req->errorMsg().find("foo\\"), std::string::npos);
}

TEST_F(WildcardQueryE2ETest, suffixScanPlanPlumbing) {
  auto schema = Schema::createDefaultSchema();
  MemPool pool;
  QueryBuilder builder(pool, *schema, CoerceContext{});

  for (std::string_view pattern : {"*sband", "*ology", "comp*ing"}) {
    auto* query = dynamic_cast<AutomatonQuery*>(
        builder.createWildcardQuery("foo_s", pattern));
    ASSERT_NE(query, nullptr) << pattern;
    EXPECT_FALSE(query->getScanPlan().commonSuffix.empty()) << pattern;
  }
  auto* regex = dynamic_cast<AutomatonQuery*>(
      builder.createRegexQuery("foo_s", ".*ization"));
  ASSERT_NE(regex, nullptr);
  EXPECT_EQ(regex->getScanPlan().commonSuffix, "ization");
}
