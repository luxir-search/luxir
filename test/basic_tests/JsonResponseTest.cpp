#include <gtest/gtest.h>

#include <array>
#include <string>

#include "solux/server/JsonResponse.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/SoluxTest.h"
#include "test/TestUtils.h"

namespace solux::test {

class JsonResponseTest : public SoluxTest {};

TEST_F(JsonResponseTest, stringFacetRowsAndOptionalMetadata) {
  CollectionHelper helper;
  helper.indexAll(std::array{
    flatdoc("id", "1", "cat_s", "x"),
    flatdoc("id", "2", "cat_s", "x"),
    flatdoc("id", "3", "cat_s", "y"),
    flatdoc("id", "4", "other_s", "z"),
  }, UpdateMessage::COMMIT);

  auto req = localReq(helper.getSearchEngine());
  req->collection("main");
  auto& facet = req->facet("cats", "cat_s");
  facet.limit(-1);
  std::get<api::FieldFacet>(facet.rawOp().kind).missing = true;
  req->execute(false);
  ASSERT_OK(req);

  auto& result = const_cast<api::FacetResult&>(std::get<api::FacetResult>(
      req->responses[0]->proto.ops.at("cats")->kind));
  result.total_buckets = 2;
  EXPECT_EQ(
      R"({"ops":{"cats":{"buckets":[{"val":"x","count":2},{"val":"y","count":1}],"missing":1,"total_buckets":2}}})" "\n",
      renderSearchResponseLine(req->responses[0]->proto));
}

TEST_F(JsonResponseTest, integerFacetPreservesZeroBucketId) {
  CollectionHelper helper;
  helper.indexAll(std::array{
    flatdoc("id", "1", "price_i", (int64_t)0),
    flatdoc("id", "2", "price_i", (int64_t)0),
    flatdoc("id", "3", "price_i", (int64_t)7),
  }, UpdateMessage::COMMIT);

  auto req = localReq(helper.getSearchEngine());
  req->collection("main");
  req->facet("prices", "price_i").limit(-1);
  req->execute(false);
  ASSERT_OK(req);

  EXPECT_EQ(
      R"({"ops":{"prices":{"buckets":[{"val":0,"count":2},{"val":7,"count":1}]}}})" "\n",
      renderSearchResponseLine(req->responses[0]->proto));
}

TEST_F(JsonResponseTest, rangeFacetRowsUseIntegerBounds) {
  CollectionHelper helper;
  helper.indexAll(std::array{
    flatdoc("id", "1", "price_i", (int64_t)5),
    flatdoc("id", "2", "price_i", (int64_t)15),
    flatdoc("id", "3", "price_i", (int64_t)15),
    flatdoc("id", "4", "price_i", (int64_t)25),
  }, UpdateMessage::COMMIT);

  auto req = localReq(helper.getSearchEngine());
  req->collection("main");
  req->rangeFacet("prices", "price_i").range(0, 30, 10);
  req->execute(false);
  ASSERT_OK(req);

  EXPECT_EQ(
      R"({"ops":{"prices":{"buckets":[{"val":[0,10],"count":1},{"val":[10,20],"count":2},{"val":[20,30],"count":1}]}}})" "\n",
      renderSearchResponseLine(req->responses[0]->proto));
}

TEST_F(JsonResponseTest, facetStatsRenderPerBucketAndEmptyAsNull) {
  CollectionHelper helper;
  helper.indexAll(std::array{
    flatdoc("id", "1", "cat_s", "a", "price_i", (int64_t)10),
    flatdoc("id", "2", "cat_s", "a", "price_i", (int64_t)30),
    flatdoc("id", "3", "cat_s", "b"),
    flatdoc("id", "4", "cat_s", "b"),
    flatdoc("id", "5", "cat_s", "c", "price_i", (int64_t)5),
  }, UpdateMessage::COMMIT);

  auto req = localReq(helper.getSearchEngine());
  req->collection("main");
  auto& facet = req->facet("cats", "cat_s");
  facet.limit(-1);
  facet.min("minimum", "price_i");
  req->execute(false);
  ASSERT_OK(req);

  EXPECT_EQ(
      R"({"ops":{"cats":{"buckets":[{"val":"a","count":2,"minimum":10},{"val":"b","count":2,"minimum":null},{"val":"c","count":1,"minimum":5}]}}})" "\n",
      renderSearchResponseLine(req->responses[0]->proto));
}

TEST_F(JsonResponseTest, nestedFacetRowsRecurse) {
  CollectionHelper helper;
  helper.indexAll(std::array{
    flatdoc("id", "1", "cat_s", "x", "sub_s", "p"),
    flatdoc("id", "2", "cat_s", "x", "sub_s", "q"),
    flatdoc("id", "3", "cat_s", "y", "sub_s", "p"),
  }, UpdateMessage::COMMIT);

  auto req = localReq(helper.getSearchEngine());
  req->collection("main");
  auto& outer = req->facet("outer", "cat_s");
  outer.limit(-1);
  outer.facet("inner", "sub_s").limit(-1);
  req->execute(false);
  ASSERT_OK(req);

  EXPECT_EQ(
      R"({"ops":{"outer":{"buckets":[{"val":"x","count":2,"inner":{"buckets":[{"val":"p","count":1},{"val":"q","count":1}]}},{"val":"y","count":1,"inner":{"buckets":[{"val":"p","count":1}]}}]}}})" "\n",
      renderSearchResponseLine(req->responses[0]->proto));
}

TEST_F(JsonResponseTest, wholeDomainStatsStayUnderOpsWithoutDocs) {
  CollectionHelper helper;
  helper.indexAll(std::array{
    flatdoc("id", "1", "price_i", (int64_t)10),
    flatdoc("id", "2", "price_i", (int64_t)30),
  }, UpdateMessage::COMMIT);

  auto req = localReq(helper.getSearchEngine());
  req->collection("main");
  req->avg("average", "price_i");
  req->min("empty", "other_i");
  req->execute(false);
  ASSERT_OK(req);

  EXPECT_EQ(R"({"ops":{"average":20,"empty":null}})" "\n",
            renderSearchResponseLine(req->responses[0]->proto));
}

TEST_F(JsonResponseTest, rowsFormatRendersDocObjects) {
  CollectionHelper helper;
  helper.indexAll(std::array{
    flatdoc("id", "1", "cat_s", "x", "n_i", (int64_t)5),
    flatdoc("id", "2"),
  }, UpdateMessage::COMMIT);

  auto req = localReq(helper.getSearchEngine());
  req->collection("main");
  req->topDocs("q").allQuery().fields({"id", "cat_s", "n_i"})
      .documentFormat(api::DocFormat::ROWS).limit(-1);
  req->execute(false);
  ASSERT_OK(req);

  // Row maps signal missing structurally: doc 2 has no cat_s/n_i keys.
  EXPECT_EQ(
      R"({"docs":[{"id":"1","cat_s":"x","n_i":5},{"id":"2"}]})" "\n",
      renderSearchResponseLine(req->responses[0]->proto));
}

TEST_F(JsonResponseTest, firstDocListIsPromotedAndFacetRemains) {
  CollectionHelper helper;
  helper.indexAll(std::array{
    flatdoc("id", "1", "cat_s", "x"),
    flatdoc("id", "2", "cat_s", "x"),
    flatdoc("id", "3", "cat_s", "y"),
  }, UpdateMessage::COMMIT);

  auto req = localReq(helper.getSearchEngine());
  req->collection("main");
  req->topDocs("hits").allQuery().fields({"id"}).limit(2).getNumber();
  req->facet("cats", "cat_s").limit(-1);
  req->execute(false);
  ASSERT_OK(req);

  EXPECT_EQ(
      R"({"found":3,"docs":[{"id":"1"},{"id":"2"}],"ops":{"cats":{"buckets":[{"val":"x","count":2},{"val":"y","count":1}]}}})" "\n",
      renderSearchResponseLine(req->responses[0]->proto));
}

TEST_F(JsonResponseTest, promotedDocListNestedOpsHoistIntoOps) {
  CollectionHelper helper;
  helper.indexAll(std::array{
    flatdoc("id", "1", "cat_s", "x"),
    flatdoc("id", "2", "cat_s", "x"),
    flatdoc("id", "3", "cat_s", "y"),
  }, UpdateMessage::COMMIT);

  auto req = localReq(helper.getSearchEngine());
  req->collection("main");
  auto& td = req->topDocs("q");
  td.allQuery().fields({"id"}).limit(1).getNumber();
  td.facet("cats", "cat_s").limit(-1);
  req->execute(false);
  ASSERT_OK(req);

  EXPECT_EQ(
      R"({"found":3,"docs":[{"id":"1"}],"ops":{"cats":{"buckets":[{"val":"x","count":2},{"val":"y","count":1}]}}})" "\n",
      renderSearchResponseLine(req->responses[0]->proto));
}

TEST_F(JsonResponseTest, secondDocListRendersItsNestedOps) {
  CollectionHelper helper;
  helper.indexAll(std::array{
    flatdoc("id", "1", "cat_s", "x"),
    flatdoc("id", "2", "cat_s", "y"),
  }, UpdateMessage::COMMIT);

  auto req = localReq(helper.getSearchEngine());
  req->collection("main");
  req->topDocs("first").allQuery().fields({"id"}).limit(1).getNumber();
  auto& second = req->topDocs("second");
  second.allQuery().fields({"id"}).limit(1);
  second.facet("cats", "cat_s").limit(-1);
  req->execute(false);
  ASSERT_OK(req);

  EXPECT_EQ(
      R"({"found":2,"docs":[{"id":"1"}],"ops":{"second":{"docs":[{"id":"1"}],"ops":{"cats":{"buckets":[{"val":"x","count":1},{"val":"y","count":1}]}}}}})" "\n",
      renderSearchResponseLine(req->responses[0]->proto));
}

TEST_F(JsonResponseTest, secondDocListRendersUnderOps) {
  CollectionHelper helper;
  helper.indexAll(std::array{
    flatdoc("id", "1"),
    flatdoc("id", "2"),
    flatdoc("id", "3"),
  }, UpdateMessage::COMMIT);

  auto req = localReq(helper.getSearchEngine());
  req->collection("main");
  req->topDocs("first").allQuery().fields({"id"}).limit(1).getNumber();
  req->topDocs("second").allQuery().fields({"id"}).limit(2);
  req->execute(false);
  ASSERT_OK(req);

  EXPECT_EQ(
      R"({"found":3,"docs":[{"id":"1"}],"ops":{"second":{"docs":[{"id":"1"},{"id":"2"}]}}})" "\n",
      renderSearchResponseLine(req->responses[0]->proto));
}

} // namespace solux::test
