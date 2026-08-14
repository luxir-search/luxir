#include <gtest/gtest.h>
#include <cmath>
#include "test/LuxirTest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/QueryBuild.h"
#include "luxir/util/NumericUtils.h"

using namespace luxir;
using namespace luxir::test;

class FloatFieldTest : public LuxirTest {
};

// The sortable encodings must order exactly like the floating point values
// when compared as signed integers, and must round-trip bit-exactly.
TEST_F(FloatFieldTest, sortableEncoding) {
  // Strictly increasing in the encoded space (-0.0 encodes below +0.0).
  double ordered[] = {
    -std::numeric_limits<double>::infinity(),
    std::numeric_limits<double>::lowest(),
    -3.5, -0.5,
    -std::numeric_limits<double>::denorm_min(),
    -0.0, 0.0,
    std::numeric_limits<double>::denorm_min(),
    0.5, 3.5,
    std::numeric_limits<double>::max(),
    std::numeric_limits<double>::infinity(),
  };
  for (size_t i = 1; i < std::size(ordered); i++) {
    ASSERT_LT(doubleToSortableInt64(ordered[i - 1]), doubleToSortableInt64(ordered[i]))
      << ordered[i - 1] << " vs " << ordered[i];
  }
  for (double v : ordered) {
    ASSERT_EQ(std::bit_cast<int64_t>(v), std::bit_cast<int64_t>(sortableInt64ToDouble(doubleToSortableInt64(v))));
  }
  // NaN of either sign canonicalizes above +Inf.
  auto inf = doubleToSortableInt64(std::numeric_limits<double>::infinity());
  ASSERT_GT(doubleToSortableInt64(std::numeric_limits<double>::quiet_NaN()), inf);
  ASSERT_GT(doubleToSortableInt64(-std::numeric_limits<double>::quiet_NaN()), inf);
  ASSERT_TRUE(std::isnan(sortableInt64ToDouble(doubleToSortableInt64(std::numeric_limits<double>::quiet_NaN()))));

  float orderedF[] = {
    -std::numeric_limits<float>::infinity(),
    std::numeric_limits<float>::lowest(),
    -3.5f, -0.5f, -0.0f, 0.0f, 0.5f, 3.5f,
    std::numeric_limits<float>::max(),
    std::numeric_limits<float>::infinity(),
  };
  for (size_t i = 1; i < std::size(orderedF); i++) {
    // compare sign-extended to int64, the way FLOAT columns store values
    ASSERT_LT((int64_t)floatToSortableInt32(orderedF[i - 1]), (int64_t)floatToSortableInt32(orderedF[i]));
  }
  for (float v : orderedF) {
    ASSERT_EQ(std::bit_cast<int32_t>(v), std::bit_cast<int32_t>(sortableInt32ToFloat(floatToSortableInt32(v))));
  }
  auto infF = (int64_t)floatToSortableInt32(std::numeric_limits<float>::infinity());
  ASSERT_GT((int64_t)floatToSortableInt32(std::numeric_limits<float>::quiet_NaN()), infF);

  // Random bit patterns: any two non-NaN doubles must encode in value order.
  double prev = 0;
  bool havePrev = false;
  for (int i = 0; i < 10000; i++) {
    auto v = std::bit_cast<double>((int64_t)rng());
    if (std::isnan(v)) continue;
    if (havePrev && prev < v) {
      ASSERT_LT(doubleToSortableInt64(prev), doubleToSortableInt64(v));
    } else if (havePrev && v < prev) {
      ASSERT_LT(doubleToSortableInt64(v), doubleToSortableInt64(prev));
    }
    prev = v;
    havePrev = true;
  }
}

TEST_F(FloatFieldTest, roundTrip) {
  CollectionHelper helper;

  // multi-valued float input goes in as vector<double> (vector<float> is
  // reserved for dense VECTOR fields); values come back as vector<float>.
  helper.index(flatdoc("id_s", "d1", "price_f", -1.5f, "weight_d", -2.5,
                       "vals_ds", vec(0.5, -2.75), "vals_fs", vec(7.5, -0.25)),
               UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "d2", "price_f", 0.25f, "weight_d", 1e100,
                       "vals_ds", vec(42.0)),
               UpdateMessage::COMMIT);
  helper.index(flatdoc("id_s", "d3", "weight_d", std::numeric_limits<double>::infinity()),
               UpdateMessage::COMMIT);

  auto req = localReq(luxirNode->getSearchEngine());
  req->collection("main").topDocs("q")
      .allQuery()
      .fields({"id_s", "price_f", "weight_d", "vals_fs", "vals_ds"})
      .limit(10);
  req->execute();

  auto docs = req->getDocs();
  ASSERT_EQ(3u, docs.size());
  EXPECT_CONTAINS_DOC(docs, flatdoc("id_s", "d1", "price_f", -1.5f, "weight_d", -2.5,
                                    "vals_fs", vec(7.5f, -0.25f), "vals_ds", vec(0.5, -2.75)));
  EXPECT_CONTAINS_DOC(docs, flatdoc("id_s", "d2", "price_f", 0.25f, "weight_d", 1e100,
                                    "vals_ds", vec(42.0)));
  EXPECT_CONTAINS_DOC(docs, flatdoc("id_s", "d3", "weight_d", std::numeric_limits<double>::infinity()));
}

// Sorting compares the encoded column values directly; negative values are
// where a raw IEEE-bits encoding would order wrong.
TEST_F(FloatFieldTest, sortFloat) {
  CollectionHelper helper;

  helper.index(flatdoc("id_s", "a", "price_f", 2.0f), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "b", "price_f", -3.5f), UpdateMessage::COMMIT);
  helper.index(flatdoc("id_s", "c", "price_f", -0.5f), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "d", "price_f", 0.25f), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "e"), UpdateMessage::COMMIT);  // missing value, sorts last

  auto sortBy = [&](qb::SortDir dir) {
    auto req = localReq(luxirNode->getSearchEngine());
    req->collection("main");
    auto& cur = req->topDocs("q").allQuery().fields({"id_s"}).limit(10);
    qb::sort(cur, "price_f", dir);
    req->execute();
    std::vector<std::string> ids;
    for (auto& doc : req->getDocs()) {
      ids.push_back(std::get<std::string>(*find(doc, "id_s")));
    }
    return ids;
  };

  auto asc = sortBy(qb::ASC);
  ASSERT_EQ((std::vector<std::string>{"b", "c", "d", "a", "e"}), asc);

  // missing values sort at a fixed edge regardless of direction (same
  // convention as string and expression sort keys): last under ASC and DESC
  auto desc = sortBy(qb::DESC);
  ASSERT_EQ((std::vector<std::string>{"a", "d", "c", "b", "e"}), desc);
}

TEST_F(FloatFieldTest, sortDouble) {
  CollectionHelper helper;

  helper.index(flatdoc("id_s", "a", "weight_d", -1e300), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "b", "weight_d", 1e-300), UpdateMessage::COMMIT);
  helper.index(flatdoc("id_s", "c", "weight_d", -2.5), UpdateMessage::COMMIT);

  auto req = localReq(luxirNode->getSearchEngine());
  req->collection("main");
  auto& cur = req->topDocs("q").allQuery().fields({"id_s"}).limit(10);
  qb::sort(cur, "weight_d", qb::ASC);
  req->execute();

  std::vector<std::string> ids;
  for (auto& doc : req->getDocs()) {
    ids.push_back(std::get<std::string>(*find(doc, "id_s")));
  }
  ASSERT_EQ((std::vector<std::string>{"a", "c", "b"}), ids);
}

// Facet-inline avg (the per-bucket InlineCalc path) must iterate every value
// of a multi-valued field and divide by the number of values seen, matching
// the non-inline path - not by the bucket's doc count.
TEST_F(FloatFieldTest, avgFacetInline) {
  CollectionHelper helper;

  helper.index(flatdoc("id_s", "a", "color_s", "red", "vals_ds", vec(1.0, 3.0), "nums_is", vec_i(10, 20)),
               UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "b", "color_s", "red", "vals_ds", vec(5.0), "nums_is", 30),
               UpdateMessage::COMMIT);
  helper.index(flatdoc("id_s", "c", "color_s", "blue", "vals_ds", vec(6.0)), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "d", "color_s", "blue"), UpdateMessage::COMMIT);  // no values

  auto req = localReq(luxirNode->getSearchEngine());
  req->collection("main").topDocs("q").allQuery().limit(10);
  auto& facet = req->facet("f", "color_s");
  facet.limit(-1);
  facet.avg("avgd", "vals_ds");
  facet.avg("avgi", "nums_is");
  qb::sort(facet, "avgd", qb::ASC);
  req->execute();
  ASSERT_OK(req);

  const auto* fr = req->responses[0]->proto.ops.at("f")->facetResult();
  ASSERT_NE(fr, nullptr);
  auto& bids = std::get<luxir::api::ColStr>(fr->bucket_ids->kind);
  ASSERT_EQ(2u, bids.v.size());
  ASSERT_EQ("red", bids.v[0]);
  ASSERT_EQ("blue", bids.v[1]);

  // red: (1+3+5)/3 values = 3.0 (dividing by its 2 docs would give 4.5)
  // blue: 6/1 value = 6.0 (dividing by its 2 docs would give 3.0)
  ASSERT_DOUBLE_EQ(3.0, std::get<luxir::api::ArrDouble>(fr->ops.at("avgd")->kind).v[0]);
  ASSERT_DOUBLE_EQ(6.0, std::get<luxir::api::ArrDouble>(fr->ops.at("avgd")->kind).v[1]);

  // multi-valued int through the same inline path
  // red: (10+20+30)/3 = 20.0 ; blue has no values -> NaN, matching the
  // non-inline path (rendered as null by the JSON layer)
  ASSERT_DOUBLE_EQ(20.0, std::get<luxir::api::ArrDouble>(fr->ops.at("avgi")->kind).v[0]);
  ASSERT_TRUE(std::isnan(std::get<luxir::api::ArrDouble>(fr->ops.at("avgi")->kind).v[1]));
}

// avg must decode the sortable bits before summing - the sum of raw encoded
// values is meaningless even though their order is correct.
TEST_F(FloatFieldTest, avg) {
  CollectionHelper helper;

  helper.index(flatdoc("id_s", "a", "price_f", 1.0f, "weight_d", -1.5), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "b", "price_f", 2.0f, "weight_d", 2.5), UpdateMessage::COMMIT);
  helper.index(flatdoc("id_s", "c", "price_f", 6.0f, "weight_d", 5.0), UpdateMessage::COMMIT);

  auto req = localReq(luxirNode->getSearchEngine());
  req->collection("main").topDocs("q").allQuery().limit(10);
  req->avg("avgf", "price_f");
  req->avg("avgd", "weight_d");
  req->execute();
  ASSERT_OK(req);

  ASSERT_DOUBLE_EQ(3.0, req->scalar<double>("avgf"));
  ASSERT_DOUBLE_EQ(2.0, req->scalar<double>("avgd"));
}
