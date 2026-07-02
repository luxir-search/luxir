
#include <gtest/gtest.h>
#include <cmath>
#include <iostream>
#include <map>
#include "test/SoluxTest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/QueryBuild.h"
#include "solux/server/GRPCServer.h"

using namespace solux;
using namespace solux::test;

namespace {
// FieldFacet.missing has no fluent OpCursor setter; reach through the raw op.
OpCursor& facetMissing(OpCursor& cur) {
  std::get<solux::api::FieldFacet>(cur.rawOp().kind).missing = true;
  return cur;
}
}  // namespace

class SearchEngineTest : public SoluxTest {
public:
};

TEST_F(SearchEngineTest, avgOpsEmptyIndexEmitNan) {
  CollectionHelper helper;
  helper.clear();

  auto req = localReq(soluxNode->getSearchEngine());
  req->collection("main");
  req->requestId("test_avg_ops_empty_index_emit_nan");

  auto& topDocs = req->topDocs("q").allQuery().getNumber();
  topDocs.avg("nested_avg", "foo_i");

  req->avg("root_avg", "foo_i");

  req->execute();

  ASSERT_EQ(1u, req->responses.size()) << req->toString();
  const auto& response = req->responses[0]->proto;
  ASSERT_FALSE(hasError(response)) << req->toString();
  ASSERT_TRUE(response.ops.contains("root_avg")) << req->toString();
  EXPECT_TRUE(std::isnan(req->scalar<double>("root_avg")));

  const auto* docs = req->docList("q");
  ASSERT_NE(docs, nullptr);
  ASSERT_EQ(0, docs->matches.value_or(0));
  ASSERT_TRUE(docs->ops.contains("nested_avg")) << req->toString();
  EXPECT_TRUE(std::isnan(std::get<double>(docs->ops.at("nested_avg")->kind)));
}

TEST_F(SearchEngineTest, basic) {
  bool para = true;

  CollectionHelper helper;
  helper.clear();
  helper.index(flatdoc("foo_w","how now brown cow", "foo_i", 17, "color_s","red", "colors_ss", "red", "prices_is", vec_i(20, 35, 45)),UpdateMessage::COMMIT);
  helper.index(flatdoc("foo_w","charlie brown", "foo_i", 23, "color_s","blue", "prices_is", 35),UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("foo_w","brown", "foo_i", 5, "color_s","brown", "colors_ss",vecs("red","black")),UpdateMessage::COMMIT);
  // should be 2 segments now.

  {
    auto req = localReq(soluxNode->getSearchEngine());
    req->collection("main");
    req->requestId("myrequestid");

    req->topDocs("q").matchQuery("foo_w", "brown").withStats()
        .fields({"foo_i", "color_s", "colors_ss", "prices_is"});
    size_t ncols = 4 + 1; // 4 requested fields + _score_

    req->facet("f", "foo_i");
    facetMissing(req->facet("f2", "prices_is")); // include missing values in the facet
    facetMissing(req->facet("f3", "noexist_i")); // include missing values in the facet
    auto& f4 = req->facet("f4", "color_s").limit(-1);
    f4.avg("avgsub", "foo_i");
    qb::sort(f4, "avgsub", qb::ASC); // sort by avg ascending
    req->facet("f5", "colors_ss");
    req->facet("f6", "prices_is").mincount(2);
    req->facet("f7", "colors_ss").mincount(2);
    req->rangeFacet("f8", "foo_i").range(-5, 34, 20);
    req->facet("f9", "foo_w");
    req->avg("avg", "foo_i");

    req->execute(para);
    // LOG_DEBUG("ENGINE REQ: {}", req->toString());

    const auto& resp = req->responses[0]->proto;
    ASSERT_EQ(req->proto.request_id, resp.request_id);
    const auto& docs = *req->docList("q");
    ASSERT_EQ(3, docs.matches.value_or(0));
    ASSERT_EQ(ncols, docs.columns.size());

    // Accessors: column arms (columns is a plain map) and facet arms (ops is indirect).
    auto colI = [&](const char* n) -> const solux::api::ColInt& {
      return std::get<solux::api::ColInt>(docs.columns.at(n).kind);
    };
    auto colS = [&](const char* n) -> const solux::api::ColStr& {
      return std::get<solux::api::ColStr>(docs.columns.at(n).kind);
    };
    auto multiS = [&](const char* n) -> const solux::api::ArrArrStr& {
      return std::get<solux::api::ArrArrStr>(docs.columns.at(n).kind);
    };
    auto multiI = [&](const char* n) -> const solux::api::ArrArrInt& {
      return std::get<solux::api::ArrArrInt>(docs.columns.at(n).kind);
    };
    auto facetOf = [&](const char* n) -> const solux::api::FacetResult& {
      return std::get<solux::api::FacetResult>(resp.ops.at(n)->kind);
    };
    auto fBidsI = [&](const char* n) -> const solux::api::ColInt& {
      return std::get<solux::api::ColInt>(facetOf(n).bucket_ids->kind);
    };
    auto fBidsS = [&](const char* n) -> const solux::api::ColStr& {
      return std::get<solux::api::ColStr>(facetOf(n).bucket_ids->kind);
    };
    auto fBidsMultiI = [&](const char* n) -> const solux::api::ArrArrInt& {
      return std::get<solux::api::ArrArrInt>(facetOf(n).bucket_ids->kind);
    };

    // docs will be ordered by shortest field first since term freq is same for all.
    ASSERT_EQ(5, colI("foo_i").v[0]);
    ASSERT_EQ("brown", colS("color_s").v[0]);
    ASSERT_EQ(23, colI("foo_i").v[1]);
    ASSERT_EQ("blue", colS("color_s").v[1]);
    ASSERT_EQ(17, colI("foo_i").v[2]);
    ASSERT_EQ("red", colS("color_s").v[2]);

    // check the multi-valued strings
    ASSERT_EQ(2, multiS("colors_ss").v[0].v.size());
    ASSERT_EQ("black", multiS("colors_ss").v[0].v[0]);
    ASSERT_EQ(0, multiS("colors_ss").v[1].v.size()); // missing for this doc
    ASSERT_EQ(1, multiS("colors_ss").v[2].v.size()); // single-valued for this doc
    ASSERT_EQ("red", multiS("colors_ss").v[2].v[0]);

    // check the multi-valued integers
    ASSERT_EQ(3, multiI("prices_is").v[2].v.size());
    ASSERT_EQ(20, multiI("prices_is").v[2].v[0]);
    ASSERT_EQ(35, multiI("prices_is").v[2].v[1]);
    ASSERT_EQ(45, multiI("prices_is").v[2].v[2]);
    ASSERT_EQ(1, multiI("prices_is").v[1].v.size()); // single-valued for this doc
    ASSERT_EQ(35, multiI("prices_is").v[1].v[0]);
    ASSERT_EQ(0, multiI("prices_is").v[0].v.size()); // missing for this doc

    // check the facet
    ASSERT_EQ(3, fBidsI("f").v.size());
    ASSERT_EQ(5, fBidsI("f").v[0]);
    ASSERT_EQ(17, fBidsI("f").v[1]);
    ASSERT_EQ(23, fBidsI("f").v[2]);
    ASSERT_EQ(3, facetOf("f").counts.size());
    ASSERT_EQ(1, facetOf("f").counts.at(0));
    ASSERT_EQ(1, facetOf("f").counts.at(1));
    ASSERT_EQ(1, facetOf("f").counts.at(2));
    //check for the abscence of missing
    ASSERT_FALSE(facetOf("f").missing.has_value());

    // check the second facet
    ASSERT_EQ(3, fBidsI("f2").v.size());
    ASSERT_EQ(35, fBidsI("f2").v[0]);
    ASSERT_EQ(20, fBidsI("f2").v[1]);
    ASSERT_EQ(45, fBidsI("f2").v[2]);
    ASSERT_EQ(3, facetOf("f2").counts.size());
    ASSERT_EQ(2, facetOf("f2").counts.at(0));
    ASSERT_EQ(1, facetOf("f2").counts.at(1));
    ASSERT_EQ(1, facetOf("f2").counts.at(2));
    ASSERT_EQ(1, facetOf("f2").missing.value());

    //check the third facet
    ASSERT_EQ(0, fBidsI("f3").v.size());
    ASSERT_EQ(3, facetOf("f3").missing.value());

    // check the fourth facet
    ASSERT_EQ(3, fBidsS("f4").v.size());
    ASSERT_EQ("brown", fBidsS("f4").v[0]);
    ASSERT_EQ("red", fBidsS("f4").v[1]);
    ASSERT_EQ("blue", fBidsS("f4").v[2]);
    ASSERT_EQ(3, facetOf("f4").counts.size());
    ASSERT_EQ(1, facetOf("f4").counts.at(0));
    ASSERT_EQ(1, facetOf("f4").counts.at(1));
    ASSERT_EQ(1, facetOf("f4").counts.at(2));
    // check the sub-op avg
    ASSERT_EQ(3, std::get<solux::api::ArrDouble>(facetOf("f4").ops.at("avgsub")->kind).v.size());
    ASSERT_EQ(5, std::get<solux::api::ArrDouble>(facetOf("f4").ops.at("avgsub")->kind).v[0]);
    ASSERT_EQ(17, std::get<solux::api::ArrDouble>(facetOf("f4").ops.at("avgsub")->kind).v[1]);
    ASSERT_EQ(23, std::get<solux::api::ArrDouble>(facetOf("f4").ops.at("avgsub")->kind).v[2]);

    // check the fifth facet
    ASSERT_EQ(2, fBidsS("f5").v.size());
    ASSERT_EQ("red", fBidsS("f5").v[0]);
    ASSERT_EQ("black", fBidsS("f5").v[1]);
    ASSERT_EQ(2, facetOf("f5").counts.size());
    ASSERT_EQ(2, facetOf("f5").counts.at(0));
    ASSERT_EQ(1, facetOf("f5").counts.at(1));

    //check the sixth facet
    ASSERT_EQ(1, fBidsI("f6").v.size());
    ASSERT_EQ(35, fBidsI("f6").v[0]);
    ASSERT_EQ(1, facetOf("f6").counts.size());
    ASSERT_EQ(2, facetOf("f6").counts.at(0));

    //check the seventh facet
    ASSERT_EQ(1, fBidsS("f7").v.size());
    ASSERT_EQ("red", fBidsS("f7").v[0]);
    ASSERT_EQ(1, facetOf("f7").counts.size());
    ASSERT_EQ(2, facetOf("f7").counts.at(0));

    //check the eighth facet
    ASSERT_EQ(2, fBidsMultiI("f8").v.size());
    ASSERT_EQ(-5, fBidsMultiI("f8").v[0].v[0]);
    ASSERT_EQ(15, fBidsMultiI("f8").v[0].v[1]);
    ASSERT_EQ(15, fBidsMultiI("f8").v[1].v[0]);
    ASSERT_EQ(34, fBidsMultiI("f8").v[1].v[1]);
    ASSERT_EQ(2, facetOf("f8").counts.size());
    ASSERT_EQ(1, facetOf("f8").counts.at(0));
    ASSERT_EQ(2, facetOf("f8").counts.at(1));

    //check the ninth facet
    ASSERT_EQ(5, fBidsS("f9").v.size());
    ASSERT_EQ("brown", fBidsS("f9").v[0]);
    ASSERT_EQ("charlie", fBidsS("f9").v[1]);
    ASSERT_EQ("cow", fBidsS("f9").v[2]);
    ASSERT_EQ("how", fBidsS("f9").v[3]);
    ASSERT_EQ("now", fBidsS("f9").v[4]);
    ASSERT_EQ(5, facetOf("f9").counts.size());
    ASSERT_EQ(3, facetOf("f9").counts.at(0));
    ASSERT_EQ(1, facetOf("f9").counts.at(1));
    ASSERT_EQ(1, facetOf("f9").counts.at(2));
    ASSERT_EQ(1, facetOf("f9").counts.at(3));
    ASSERT_EQ(1, facetOf("f9").counts.at(4));

    // check the avg
    ASSERT_EQ(std::get<double>(resp.ops.at("avg")->kind), 15);
  }


#ifdef REMOVED
  // FIXME
  {
    auto req = localReq(soluxNode->getSearchEngine());
    req->collection("main");
    req->requestId("myrequestid");

    req->topDocs("q").matchQuery("foo_w", "brown").withStats()
        .fields({"foo_i", "color_s", "colors_ss"});

    req->facet("f", "foo_i").limit(2);
    req->facet("f2", "foo_i").limit(1);

    req->execute(para);
    // LOG_DEBUG("ENGINE REQ: {}", req->toString());

    const auto& resp = req->responses[0]->proto;
    auto fBidsI = [&](const char* n) -> const solux::api::ColInt& {
      return std::get<solux::api::ColInt>(std::get<solux::api::FacetResult>(resp.ops.at(n)->kind).bucket_ids->kind);
    };
    auto fCounts = [&](const char* n) -> const auto& {
      return std::get<solux::api::FacetResult>(resp.ops.at(n)->kind).counts;
    };

    // check the facet
    ASSERT_EQ(2, fBidsI("f").v.size());
    ASSERT_EQ(5, fBidsI("f").v[0]);
    ASSERT_EQ(17, fBidsI("f").v[1]);
    ASSERT_EQ(2, fCounts("f").size());
    ASSERT_EQ(1, fCounts("f").at(0));
    ASSERT_EQ(1, fCounts("f").at(1));

    //check the second facet
    ASSERT_EQ(1, fBidsI("f2").v.size());
    ASSERT_EQ(5, fBidsI("f2").v[0]);
    ASSERT_EQ(1, fCounts("f2").size());
    ASSERT_EQ(1, fCounts("f2").at(0));
  }
#endif


  // now lets do the same request, but try to get multiple responses.
  {
    auto req = localReq(soluxNode->getSearchEngine());
    req->collection("main");
    req->requestId("myrequestid");
    req->topDocs("q").matchQuery("foo_w", "brown").withStats()
        .fields({"foo_i", "color_s"}).batchSize(2);

    req->execute(para);
// LOG_DEBUG("ENGINE REQ: {}", req->toString());

    ASSERT_EQ(req->proto.request_id, req->responses[0]->proto.request_id);
    const auto& docs = *req->docList("q");
    auto colI = [&](const char* n) -> const solux::api::ColInt& {
      return std::get<solux::api::ColInt>(docs.columns.at(n).kind);
    };
    auto colS = [&](const char* n) -> const solux::api::ColStr& {
      return std::get<solux::api::ColStr>(docs.columns.at(n).kind);
    };
    ASSERT_EQ(3, docs.matches.value_or(0));
    ASSERT_EQ(3, docs.columns.size());

    // docs will be ordered by shortest field first since term freq is same for all.
    ASSERT_EQ(2, colI("foo_i").v.size());  // only the first 2 docs this time.  should I explicitly return the number of docs in this batch?
    ASSERT_EQ(2, colS("color_s").v.size());  // only the first 2 docs this time.  should I explicitly return the number of docs in this batch?
    ASSERT_EQ(5, colI("foo_i").v[0]);
    ASSERT_EQ("brown", colS("color_s").v[0]);
    ASSERT_EQ(23, colI("foo_i").v[1]);
    ASSERT_EQ("blue", colS("color_s").v[1]);
    // check that "more" flags are set both at DocList level and at Response level
    ASSERT_TRUE(docs.more);
    ASSERT_TRUE(req->responses[0]->proto.more);


    ASSERT_EQ(req->proto.request_id, req->responses[1]->proto.request_id);
    const auto& docs2 = std::get<solux::api::DocList>(req->responses[1]->proto.ops.at("q")->kind);
    auto colI2 = [&](const char* n) -> const solux::api::ColInt& {
      return std::get<solux::api::ColInt>(docs2.columns.at(n).kind);
    };
    auto colS2 = [&](const char* n) -> const solux::api::ColStr& {
      return std::get<solux::api::ColStr>(docs2.columns.at(n).kind);
    };
    ASSERT_EQ(3, docs2.matches.value_or(0));
    ASSERT_EQ(2, docs2.offset);
    ASSERT_EQ(3, docs2.columns.size());
    ASSERT_EQ(1, colI2("foo_i").v.size());  // only the first 2 docs this time.  should I explicitly return the number of docs in this batch?
    ASSERT_EQ(1, colS2("color_s").v.size());

    // docs will be ordered by shortest field first since term freq is same for all.
    ASSERT_EQ(17, colI2("foo_i").v[0]);
    ASSERT_EQ("red", colS2("color_s").v[0]);
    // check that more flags are false at DocList level and at Response level
    ASSERT_FALSE(docs2.more);
    ASSERT_FALSE(req->responses[1]->proto.more);
  }

  {
    // new let's try for 3 responses

    auto req = localReq(soluxNode->getSearchEngine());
    req->collection("main");
    req->requestId("myrequestid");
    req->topDocs("q").matchQuery("foo_w", "brown").withStats()
        .fields({"foo_i", "color_s"}).batchSize(1).limit(7);

    req->execute(para);
    ASSERT_EQ(3, req->responses.size());
    // check offsets are correct
    ASSERT_EQ(0, std::get<solux::api::DocList>(req->responses[0]->proto.ops.at("q")->kind).offset);
    ASSERT_EQ(1, std::get<solux::api::DocList>(req->responses[1]->proto.ops.at("q")->kind).offset);
    ASSERT_EQ(2, std::get<solux::api::DocList>(req->responses[2]->proto.ops.at("q")->kind).offset);
  }
}

TEST_F(SearchEngineTest, forcePrepareWrapperMatchesChild) {
  CollectionHelper helper;
  helper.clear();
  helper.index(flatdoc("foo_w", "how now brown cow", "foo_i", 17, "color_s", "red"), UpdateMessage::COMMIT);
  helper.index(flatdoc("foo_w", "charlie brown", "foo_i", 23, "color_s", "blue"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("foo_w", "brown", "foo_i", 5, "color_s", "brown"), UpdateMessage::COMMIT);

  auto addBrownTopDocs = [](OpCursor& cur) {
    cur.withStats().fields({"foo_i", "color_s"}).matchQuery("foo_w", "brown");
  };

  auto req = localReq(soluxNode->getSearchEngine());
  req->collection("main");
  addBrownTopDocs(req->topDocs("normal"));

  auto& forced = req->topDocs("forced").withStats().fields({"foo_i", "color_s"});
  forced.rawQuery() = qb::forcePrepare(forced.mr(), qb::match(forced.mr(), "foo_w", "brown"));
  forced.facet("colors", "color_s").limit(-1);

  req->execute();
  ASSERT_EQ(1, req->responses.size()) << req->toString();
  ASSERT_FALSE(hasError(req->responses[0]->proto)) << req->toString();

  const auto& normalDocs = *req->docList("normal");
  const auto& forcedDocs = *req->docList("forced");
  ASSERT_EQ(normalDocs.matches.value_or(0), forcedDocs.matches.value_or(0));

  const auto& normalFoo = std::get<solux::api::ColInt>(normalDocs.columns.at("foo_i").kind).v;
  const auto& forcedFoo = std::get<solux::api::ColInt>(forcedDocs.columns.at("foo_i").kind).v;
  ASSERT_EQ(normalFoo.size(), forcedFoo.size());
  for (size_t i = 0; i < normalFoo.size(); i++) {
    EXPECT_EQ(normalFoo[i], forcedFoo[i]);
  }

  const auto& normalColor = std::get<solux::api::ColStr>(normalDocs.columns.at("color_s").kind).v;
  const auto& forcedColor = std::get<solux::api::ColStr>(forcedDocs.columns.at("color_s").kind).v;
  ASSERT_EQ(normalColor.size(), forcedColor.size());
  for (size_t i = 0; i < normalColor.size(); i++) {
    EXPECT_EQ(normalColor[i], forcedColor[i]);
  }

  const auto& normalScore = std::get<solux::api::ColFloat>(normalDocs.columns.at("_score_").kind).v;
  const auto& forcedScore = std::get<solux::api::ColFloat>(forcedDocs.columns.at("_score_").kind).v;
  ASSERT_EQ(normalScore.size(), forcedScore.size());
  for (size_t i = 0; i < normalScore.size(); i++) {
    EXPECT_FLOAT_EQ(normalScore[i], forcedScore[i]);
  }

  const auto& facet = std::get<solux::api::FacetResult>(forcedDocs.ops.at("colors")->kind);
  const auto& facetBids = std::get<solux::api::ColStr>(facet.bucket_ids->kind);
  ASSERT_EQ(3, facetBids.v.size());
  std::map<std::string, int64_t> facetCounts;
  for (size_t i = 0; i < facetBids.v.size(); i++) {
    facetCounts[std::string(facetBids.v[i])] = facet.counts.at(i);
  }
  EXPECT_EQ(1, facetCounts["blue"]);
  EXPECT_EQ(1, facetCounts["brown"]);
  EXPECT_EQ(1, facetCounts["red"]);
}

TEST_F(SearchEngineTest, constantScoreWrapperSetsScore) {
  CollectionHelper helper;
  helper.clear();
  helper.index(flatdoc("foo_w", "how now brown cow", "foo_i", 17, "color_s", "red"), UpdateMessage::COMMIT);
  helper.index(flatdoc("foo_w", "charlie brown", "foo_i", 23, "color_s", "blue"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("foo_w", "brown", "foo_i", 5, "color_s", "brown"), UpdateMessage::COMMIT);

  auto req = localReq(soluxNode->getSearchEngine());
  req->collection("main");

  auto& cur = req->topDocs("constant").withStats().fields({"foo_i", "color_s"});
  cur.rawQuery() = qb::constantScore(cur.mr(),
      qb::forcePrepare(cur.mr(), qb::match(cur.mr(), "foo_w", "brown")), 7.5f);
  cur.facet("colors", "color_s").limit(-1);

  req->execute();
  ASSERT_EQ(1, req->responses.size()) << req->toString();
  ASSERT_FALSE(hasError(req->responses[0]->proto)) << req->toString();

  const auto& docs = *req->docList("constant");
  ASSERT_EQ(3, docs.matches.value_or(0));

  const auto& foo = std::get<solux::api::ColInt>(docs.columns.at("foo_i").kind).v;
  ASSERT_EQ(3, foo.size());
  std::map<int64_t, bool> seenFoo;
  for (size_t i = 0; i < foo.size(); i++) {
    seenFoo[foo[i]] = true;
  }
  EXPECT_TRUE(seenFoo[5]);
  EXPECT_TRUE(seenFoo[17]);
  EXPECT_TRUE(seenFoo[23]);

  const auto& scores = std::get<solux::api::ColFloat>(docs.columns.at("_score_").kind).v;
  ASSERT_EQ(3, scores.size());
  for (size_t i = 0; i < scores.size(); i++) {
    EXPECT_FLOAT_EQ(7.5f, scores[i]);
  }

  const auto& facetResult = std::get<solux::api::FacetResult>(docs.ops.at("colors")->kind);
  const auto& facetBids = std::get<solux::api::ColStr>(facetResult.bucket_ids->kind);
  ASSERT_EQ(3, facetBids.v.size());
  std::map<std::string, int64_t> facetCounts;
  for (size_t i = 0; i < facetBids.v.size(); i++) {
    facetCounts[std::string(facetBids.v[i])] = facetResult.counts.at(i);
  }
  EXPECT_EQ(1, facetCounts["blue"]);
  EXPECT_EQ(1, facetCounts["brown"]);
  EXPECT_EQ(1, facetCounts["red"]);
}

// Op and filter names are path-safe ([A-Za-z0-9_-]+): they appear in path-based
// debug/warning addressing, URL overlays, and Domain include/exclude references.
TEST_F(SearchEngineTest, opAndFilterNameCharset) {
  CollectionHelper helper;
  helper.clear();
  helper.index(flatdoc("foo_w", "hello"), UpdateMessage::COMMIT);

  {  // unusual but legal name
    auto req = localReq(soluxNode->getSearchEngine());
    req->collection("main");
    req->topDocs("My-Op_2").matchQuery("foo_w", "hello");
    req->execute();
    ASSERT_EQ(1, req->responses.size()) << req->toString();
    ASSERT_FALSE(hasError(req->responses[0]->proto)) << req->toString();
  }
  {  // path-unsafe op name is rejected with the teaching message
    auto req = localReq(soluxNode->getSearchEngine());
    req->collection("main");
    req->topDocs("bad.name!").matchQuery("foo_w", "hello");
    ExpectLog quiet("Search request failed:");
    req->execute();
    ASSERT_FALSE(req->responses.empty());
    EXPECT_NE(req->errorMsg().find("restricted to"), std::string::npos) << req->errorMsg();
  }
  {  // filter names use the same rule
    auto req = localReq(soluxNode->getSearchEngine());
    req->collection("main");
    auto& cur = req->topDocs("q").matchQuery("foo_w", "hello");
    auto& td = std::get<solux::api::TopDocs>(cur.rawOp().kind);
    auto* f = solux::api::build::allocArray(td.filter, 1, cur.mr());
    f[0].name = "bad name";
    auto* q = (solux::api::Query*)cur.mr().allocate(sizeof(solux::api::Query),
                                                    alignof(solux::api::Query));
    new (q) solux::api::Query(qb::match(cur.mr(), "foo_w", "hello"));
    f[0].query = q;
    ExpectLog quiet("Search request failed:");
    req->execute();
    ASSERT_FALSE(req->responses.empty());
    EXPECT_NE(req->errorMsg().find("restricted to"), std::string::npos) << req->errorMsg();
  }
}

// Missing or unknown collections error cleanly (previously a null deref in getResources).
TEST_F(SearchEngineTest, missingCollectionErrors) {
  {  // no collection at all
    auto req = localReq(soluxNode->getSearchEngine());
    req->topDocs("q").allQuery();
    ExpectLog quiet("Search request failed:");
    req->execute();
    ASSERT_FALSE(req->responses.empty());
    EXPECT_NE(req->errorMsg().find("no collection"), std::string::npos) << req->errorMsg();
  }
  // NOTE: the unknown-collection-NAME error is untestable today: SoluxNode::getCollection
  // is a single-collection stub that ignores the name. The engine's null-check guards the
  // path for when real lookup lands.
}
