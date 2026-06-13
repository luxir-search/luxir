#include <gtest/gtest.h>
#include "test/SoluxTest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"

using namespace solux;
using namespace solux::test;

// missing_val on single-valued response columns is chosen per column per
// batch so that it never equals a real value: the v[i] != missing_val check
// is exact.  These tests force each step of the selection ladder and the
// collision cases that a fixed sentinel would get wrong.
class MissingValuesTest : public SoluxTest {
protected:
  // Run an all-docs query returning `fields` and hand back the LocalReq for
  // both Doc-level and proto-level assertions.  Caller must call done().
  LocalReq* query(std::initializer_list<std::string> fields) {
    auto* lreq = LocalReq::create(soluxNode->getSearchEngine());
    lreq->collection("main").allQuery().fields(fields).limit(10).execute();
    return lreq;
  }

  const proto::Column& column(LocalReq* lreq, std::string_view field) {
    return lreq->responses[0]->proto.ops().at("q").docs().columns().at(field);
  }
};

TEST_F(MissingValuesTest, intLadder) {
  CollectionHelper helper;
  helper.clear();

  // all values positive -> 0 is free
  helper.index(flatdoc("id_s", "a", "num_i", 5), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "b"), UpdateMessage::COMMIT);
  {
    auto* lreq = query({"id_s", "num_i"});
    ASSERT_EQ(0, column(lreq, "num_i").col_i().missing_val());
    auto docs = lreq->getDocs();
    ASSERT_TRUE(containsDoc(docs, flatdoc("id_s", "a", "num_i", (int64_t)5)));
    ASSERT_TRUE(containsDoc(docs, flatdoc("id_s", "b")));
    lreq->done();
  }

  // 0 present -> int64 min
  helper.index(flatdoc("id_s", "c", "num_i", 0), UpdateMessage::COMMIT);
  {
    auto* lreq = query({"id_s", "num_i"});
    ASSERT_EQ(std::numeric_limits<int64_t>::min(), column(lreq, "num_i").col_i().missing_val());
    ASSERT_TRUE(containsDoc(lreq->getDocs(), flatdoc("id_s", "c", "num_i", (int64_t)0)));
    lreq->done();
  }

  // int64 min present too -> int64 max.  This is the collision a fixed
  // sentinel gets wrong: the real min value must round-trip, the missing
  // doc must be dropped.
  helper.index(flatdoc("id_s", "d", "num_i", std::numeric_limits<int64_t>::min()), UpdateMessage::COMMIT);
  {
    auto* lreq = query({"id_s", "num_i"});
    ASSERT_EQ(std::numeric_limits<int64_t>::max(), column(lreq, "num_i").col_i().missing_val());
    auto docs = lreq->getDocs();
    ASSERT_TRUE(containsDoc(docs, flatdoc("id_s", "d", "num_i", std::numeric_limits<int64_t>::min())));
    ASSERT_TRUE(containsDoc(docs, flatdoc("id_s", "b")));
    lreq->done();
  }

  // both extremes and 0 present -> gap walk finds an unused value
  helper.index(flatdoc("id_s", "e", "num_i", std::numeric_limits<int64_t>::max()), UpdateMessage::COMMIT);
  {
    auto* lreq = query({"id_s", "num_i"});
    auto& col = column(lreq, "num_i").col_i();
    auto filler = col.missing_val();
    for (auto v : col.v()) {
      if (v == filler) {
        // only the missing doc's slot may hold the filler
        ASSERT_EQ(1, std::count(col.v().begin(), col.v().end(), filler));
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
  helper.clear();

  // lowest() is a real value (the old fixed sentinel); -0.0 occupies the
  // zero slot so 0.0 can't be the filler either.
  helper.index(flatdoc("id_s", "a", "w_d", std::numeric_limits<double>::lowest()), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "b", "w_d", -0.0), UpdateMessage::COMMIT);
  helper.index(flatdoc("id_s", "c"), UpdateMessage::COMMIT);

  auto* lreq = query({"id_s", "w_d"});
  auto& col = column(lreq, "w_d").col_d();
  double filler = col.missing_val();
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
  helper.clear();

  helper.index(flatdoc("id_s", "a", "p_f", 1.5f), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "b"), UpdateMessage::COMMIT);

  auto* lreq = query({"id_s", "p_f"});
  ASSERT_EQ(0.0f, column(lreq, "p_f").col_f().missing_val());
  auto docs = lreq->getDocs();
  ASSERT_TRUE(containsDoc(docs, flatdoc("id_s", "a", "p_f", 1.5f)));
  ASSERT_TRUE(containsDoc(docs, flatdoc("id_s", "b")));
  lreq->done();
}

TEST_F(MissingValuesTest, stringEmptyCollision) {
  CollectionHelper helper;
  helper.clear();

  // A real empty-string value: the "" filler would conflate it with the
  // missing doc, so the filler becomes a string above every batch value.
  helper.index(flatdoc("id_s", "a", "tag_sc", ""), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "b", "tag_sc", "zebra"), UpdateMessage::COMMIT);
  helper.index(flatdoc("id_s", "c"), UpdateMessage::COMMIT);

  auto* lreq = query({"id_s", "tag_sc"});
  auto& col = column(lreq, "tag_sc").col_s();
  ASSERT_GT(col.missing_val(), std::string("zebra"));

  auto docs = lreq->getDocs();
  ASSERT_TRUE(containsDoc(docs, flatdoc("id_s", "a", "tag_sc", std::string(""))));
  ASSERT_TRUE(containsDoc(docs, flatdoc("id_s", "b", "tag_sc", std::string("zebra"))));
  ASSERT_TRUE(containsDoc(docs, flatdoc("id_s", "c")));
  lreq->done();
}

TEST_F(MissingValuesTest, stringDenseDefault) {
  CollectionHelper helper;
  helper.clear();

  // No empty strings in the batch: filler stays the default "".
  helper.index(flatdoc("id_s", "a", "tag_sc", "x"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id_s", "b"), UpdateMessage::COMMIT);

  auto* lreq = query({"id_s", "tag_sc"});
  ASSERT_EQ("", column(lreq, "tag_sc").col_s().missing_val());
  auto docs = lreq->getDocs();
  ASSERT_TRUE(containsDoc(docs, flatdoc("id_s", "a", "tag_sc", std::string("x"))));
  ASSERT_TRUE(containsDoc(docs, flatdoc("id_s", "b")));
  lreq->done();
}
