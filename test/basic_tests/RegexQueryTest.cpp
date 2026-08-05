#include <gtest/gtest.h>

#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/QueryBuild.h"
#include "test/SoluxTest.h"

using namespace solux;
using namespace solux::test;

class RegexQueryE2ETest : public SoluxTest {
public:
  CollectionHelper helper;
  RegexQueryE2ETest() {
    helper.index(flatdoc("id", "d1", "title_wl", "Foobar", "color_s", "foobar"), UpdateMessage::NO_COMMIT);
    helper.index(flatdoc("id", "d2", "title_wl", "Food", "color_s", "FOOBAR"), UpdateMessage::COMMIT);
  }
  int64_t count(std::string_view field, std::string_view pattern) {
    auto req = localReq(helper.getSearchEngine());
    auto& cur = req->collection("main").topDocs("q");
    cur.rawQuery() = qb::regex(cur.mr(), field, pattern);
    cur.withStats();
    req->execute();
    return req->getMatchCount();
  }
};

TEST_F(RegexQueryE2ETest, protoExprAndTextLiteralFolding) {
  EXPECT_EQ(count("title_wl", "foo.*"), 2);
  EXPECT_EQ(count("title_wl", "FOO.*"), 2);  // TEXT literal atoms fold
  EXPECT_EQ(count("title_wl", "[A-Z].*"), 0);  // classes remain codepoint-exact
  EXPECT_EQ(count("color_s", "FOO.*"), 1);
  auto req = localReq(helper.getSearchEngine());
  req->collection("main").topDocs("q").exprQuery("regex('foo.*', field=title_wl)").withStats();
  req->execute();
  EXPECT_EQ(req->getMatchCount(), 2);
}
