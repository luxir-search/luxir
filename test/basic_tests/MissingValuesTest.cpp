// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>
#include "test/LuxirTest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"

using namespace luxir;
using namespace luxir::test;

// missing_val on single-valued response columns is chosen per column per
// batch so that it never equals a real value: the v[i] != missing_val check
// is exact.  These tests force each step of the selection ladder and the
// collision cases that a fixed sentinel would get wrong.
class MissingValuesTest : public LuxirTest {
protected:
  // Run an all-docs query returning `fields` and hand back the LocalReq for
  // both Doc-level and column-level assertions.  Caller must call done().
  LocalReq* query(std::initializer_list<std::string> fields) {
    auto* lreq = LocalReq::create(luxirNode->getSearchEngine());
    lreq->collection("main").topDocs("q").allQuery().fields(fields).limit(10);
    lreq->execute();
    return lreq;
  }

  const luxir::api::Column& column(LocalReq* lreq, std::string_view field) {
    const auto* docs = lreq->docList("q");
    return docs->columns.at(field);
  }
};

TEST_F(MissingValuesTest, intLadder) {
  CollectionHelper helper;

  // all values positive -> 0 is free
  helper.index(flatdoc("id_s", "a", "num_i", 5), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "b"), UpdateMessage::COMMIT);
  {
    auto* lreq = query({"id_s", "num_i"});
    ASSERT_EQ(0, std::get<luxir::api::ColInt>(column(lreq, "num_i").kind).missing_val);
    auto docs = lreq->getDocs();
    ASSERT_TRUE(containsDoc(docs, flatdoc("id_s", "a", "num_i", (int64_t)5)));
    ASSERT_TRUE(containsDoc(docs, flatdoc("id_s", "b")));
    lreq->done();
  }

  // 0 present -> int64 min
  helper.index(flatdoc("id_s", "c", "num_i", 0), UpdateMessage::COMMIT);
  {
    auto* lreq = query({"id_s", "num_i"});
    ASSERT_EQ(std::numeric_limits<int64_t>::min(), std::get<luxir::api::ColInt>(column(lreq, "num_i").kind).missing_val);
    ASSERT_TRUE(containsDoc(lreq->getDocs(), flatdoc("id_s", "c", "num_i", (int64_t)0)));
    lreq->done();
  }

  // int64 min present too -> int64 max.  This is the collision a fixed
  // sentinel gets wrong: the real min value must round-trip, the missing
  // doc must be dropped.
  helper.index(flatdoc("id_s", "d", "num_i", std::numeric_limits<int64_t>::min()), UpdateMessage::COMMIT);
  {
    auto* lreq = query({"id_s", "num_i"});
    ASSERT_EQ(std::numeric_limits<int64_t>::max(), std::get<luxir::api::ColInt>(column(lreq, "num_i").kind).missing_val);
    auto docs = lreq->getDocs();
    ASSERT_TRUE(containsDoc(docs, flatdoc("id_s", "d", "num_i", std::numeric_limits<int64_t>::min())));
    ASSERT_TRUE(containsDoc(docs, flatdoc("id_s", "b")));
    lreq->done();
  }

  // both extremes and 0 present -> gap walk finds an unused value
  helper.index(flatdoc("id_s", "e", "num_i", std::numeric_limits<int64_t>::max()), UpdateMessage::COMMIT);
  {
    auto* lreq = query({"id_s", "num_i"});
    auto& col = std::get<luxir::api::ColInt>(column(lreq, "num_i").kind);
    auto filler = col.missing_val;
    for (auto v : col.v) {
      if (v == filler) {
        // only the missing doc's slot may hold the filler
        ASSERT_EQ(1, std::count(col.v.begin(), col.v.end(), filler));
      }
    }
    auto docs = lreq->getDocs();
    ASSERT_TRUE(containsDoc(docs, flatdoc("id_s", "d", "num_i", std::numeric_limits<int64_t>::min())));
    ASSERT_TRUE(containsDoc(docs, flatdoc("id_s", "e", "num_i", std::numeric_limits<int64_t>::max())));
    ASSERT_TRUE(containsDoc(docs, flatdoc("id_s", "b")));
    lreq->done();
  }
}

TEST_F(MissingValuesTest, doubleCollisions) {
  CollectionHelper helper;

  // lowest() is a real value (the old fixed sentinel); -0.0 occupies the
  // zero slot so 0.0 can't be the filler either.
  helper.index(flatdoc("id_s", "a", "w_d", std::numeric_limits<double>::lowest()), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "b", "w_d", -0.0), UpdateMessage::COMMIT);
  helper.index(flatdoc("id_s", "c"), UpdateMessage::COMMIT);

  auto* lreq = query({"id_s", "w_d"});
  auto& col = std::get<luxir::api::ColDouble>(column(lreq, "w_d").kind);
  double filler = col.missing_val;
  ASSERT_NE(std::numeric_limits<double>::lowest(), filler);
  ASSERT_NE(0.0, filler);  // also excludes -0.0 (they compare equal)
  ASSERT_FALSE(std::isnan(filler));

  auto docs = lreq->getDocs();
  ASSERT_TRUE(containsDoc(docs, flatdoc("id_s", "a", "w_d", std::numeric_limits<double>::lowest())));
  ASSERT_TRUE(containsDoc(docs, flatdoc("id_s", "c")));
  lreq->done();
}

TEST_F(MissingValuesTest, floatZeroPreferred) {
  CollectionHelper helper;

  helper.index(flatdoc("id_s", "a", "p_f", 1.5f), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "b"), UpdateMessage::COMMIT);

  auto* lreq = query({"id_s", "p_f"});
  ASSERT_EQ(0.0f, std::get<luxir::api::ColFloat>(column(lreq, "p_f").kind).missing_val);
  auto docs = lreq->getDocs();
  ASSERT_TRUE(containsDoc(docs, flatdoc("id_s", "a", "p_f", 1.5f)));
  ASSERT_TRUE(containsDoc(docs, flatdoc("id_s", "b")));
  lreq->done();
}

TEST_F(MissingValuesTest, stringEmptyCollision) {
  CollectionHelper helper;

  // A real empty-string value: the "" filler would conflate it with the
  // missing doc, so the filler becomes a string above every batch value.
  helper.index(flatdoc("id_s", "a", "tag_sc", ""), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "b", "tag_sc", "zebra"), UpdateMessage::COMMIT);
  helper.index(flatdoc("id_s", "c"), UpdateMessage::COMMIT);

  auto* lreq = query({"id_s", "tag_sc"});
  auto& col = std::get<luxir::api::ColStr>(column(lreq, "tag_sc").kind);
  ASSERT_GT(col.missing_val, std::string("zebra"));

  auto docs = lreq->getDocs();
  ASSERT_TRUE(containsDoc(docs, flatdoc("id_s", "a", "tag_sc", std::string(""))));
  ASSERT_TRUE(containsDoc(docs, flatdoc("id_s", "b", "tag_sc", std::string("zebra"))));
  ASSERT_TRUE(containsDoc(docs, flatdoc("id_s", "c")));
  lreq->done();
}

TEST_F(MissingValuesTest, stringDenseDefault) {
  CollectionHelper helper;

  // No empty strings in the batch: filler stays the default "".
  helper.index(flatdoc("id_s", "a", "tag_sc", "x"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "b"), UpdateMessage::COMMIT);

  auto* lreq = query({"id_s", "tag_sc"});
  ASSERT_EQ("", std::get<luxir::api::ColStr>(column(lreq, "tag_sc").kind).missing_val);
  auto docs = lreq->getDocs();
  ASSERT_TRUE(containsDoc(docs, flatdoc("id_s", "a", "tag_sc", std::string("x"))));
  ASSERT_TRUE(containsDoc(docs, flatdoc("id_s", "b")));
  lreq->done();
}
