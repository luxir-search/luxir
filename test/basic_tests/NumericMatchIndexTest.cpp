// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// index: match on numeric fields: one docs-only term per value beside the
// column. Exact values and value sets read the postings, narrow ranges read
// them without points, and merges rebuild them from the merged column.

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include "luxir/reader/SkipStats.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/SchemaBuilder.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

namespace {

using FieldClass = api::FieldDef::FieldClass;
using IndexMode = api::FieldDef::IndexMode;

struct Arms {
  int64_t term = 0;
  int64_t column = 0;  // points, zone map, scan or sparse verification
};

} // namespace

class NumericMatchIndexTest : public LuxirTest {
public:
  CollectionHelper helper;
  Arms arms;

  void setSchema(IndexMode code, IndexMode when = IndexMode::MATCH) {
    SchemaBuilder b;
    auto& f = b.field("code");
    f.type = FieldClass::INT;
    f.index = code;
    auto& w = b.field("when");
    w.type = FieldClass::DATE;
    w.index = when;
    auto& p = b.field("price");
    p.type = FieldClass::DOUBLE;
    p.index = IndexMode::MATCH;
    p.multi = true;
    b.set(helper.collection());
  }

  void indexDocs(int first, int count, UpdateMessage::CommitType commit = UpdateMessage::COMMIT,
                 uint32_t maxSegments = 0) {
    std::vector<Doc> docs;
    for (int i = first; i < first + count; i++) {
      // Codes cycle through 200, 201, 404, 500; prices repeat within a doc.
      int64_t code = std::vector<int64_t>{200, 201, 404, 500}[(size_t)i % 4];
      docs.push_back(flatdoc("id", "d" + std::to_string(i), "code", code,
                             "when", i % 2 == 0 ? "2024-01-01T10:00:00Z" : "2024-01-02T10:00:00Z",
                             "price", std::vector<double>{1.5, 1.5, 2.5 + i % 3}));
    }
    ASSERT_TRUE(helper.indexAll(docs, commit, false, maxSegments).success);
  }

  std::vector<std::string> search(std::string_view expr) {
    auto req = localReq(helper.getSearchEngine());
    req->collection("main").topDocs("q").exprQuery(expr).fields({"id"}).limit(-1);
    SkipStats::reset();
    SkipStats::enabled = true;
    req->execute();
    SkipStats::enabled = false;
    arms.term = SkipStats::numericPredicateTermArms;
    arms.column = SkipStats::numericPredicateSparseVerifyArms + SkipStats::numericPredicatePointsArms
        + SkipStats::numericPredicateZoneArms + SkipStats::numericPredicateScanArms;
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    std::vector<std::string> ids;
    for (const Doc& doc : req->getDocs()) ids.push_back(std::get<std::string>(*find(doc, "id")));
    std::sort(ids.begin(), ids.end());
    return ids;
  }

  static std::vector<std::string> ids(std::initializer_list<int> docs) {
    std::vector<std::string> out;
    for (int doc : docs) out.push_back("d" + std::to_string(doc));
    std::sort(out.begin(), out.end());
    return out;
  }
};

TEST_F(NumericMatchIndexTest, schemaAcceptsMatchOnNumerics) {
  auto build = [](FieldClass type, IndexMode mode, bool column = true) {
    SchemaBuilder b;
    auto& f = b.field("f");
    f.type = type;
    f.index = mode;
    f.column = column;
    return b.build();
  };
  for (auto type : {FieldClass::INT, FieldClass::FLOAT, FieldClass::DOUBLE, FieldClass::DATE}) {
    auto* match = build(type, IndexMode::MATCH)->getFieldTypePtr("f");
    EXPECT_TRUE(match->indexed());
    EXPECT_FALSE(match->rangeIndexed());
    auto* both = build(type, IndexMode::MATCH_RANGE)->getFieldTypePtr("f");
    EXPECT_TRUE(both->indexed());
    EXPECT_TRUE(both->rangeIndexed());
    EXPECT_THROW(build(type, IndexMode::MATCH, false), SchemaError);
  }
  EXPECT_THROW(build(FieldClass::STRING, IndexMode::MATCH_RANGE), SchemaError);
  EXPECT_THROW(build(FieldClass::GEO_POINT, IndexMode::MATCH_RANGE), SchemaError);
}

TEST_F(NumericMatchIndexTest, exactSetAndRangeReadTheTerms) {
  setSchema(IndexMode::MATCH);
  indexDocs(0, 8);

  EXPECT_EQ(ids({2, 6}), search("code:404"));
  EXPECT_GT(arms.term, 0);
  EXPECT_EQ(0, arms.column);

  EXPECT_EQ(ids({0, 2, 4, 6}), search("code:=(200, 404)"));
  EXPECT_GT(arms.term, 0);
  EXPECT_EQ(ids({}), search("code:=(202, 403)"));

  // Few distinct values and no points: the range unions its terms.
  EXPECT_EQ(ids({0, 1, 4, 5}), search("code:[200 TO 299]"));
  EXPECT_GT(arms.term, 0);
  EXPECT_EQ(0, arms.column);

  // A DATE match is the day's range; a multi-valued DOUBLE repeats a value.
  EXPECT_EQ(ids({1, 3, 5, 7}), search("when:2024-01-02"));
  EXPECT_GT(arms.term, 0);
  EXPECT_EQ(ids({0, 3, 6}), search("price:2.5"));
  EXPECT_EQ(ids({0, 1, 2, 3, 4, 5, 6, 7}), search("price:1.5"));

  // Conjunctions of term arms and the column still agree.
  EXPECT_EQ(ids({4}), search("code:200 AND price:3.5"));
}

TEST_F(NumericMatchIndexTest, rangesKeepTheColumnArmsWhenTermsDoNotServe) {
  setSchema(IndexMode::MATCH_RANGE, IndexMode::MATCH);
  indexDocs(0, 8);
  // Points present: a range keeps the points arm, an exact value reads terms.
  EXPECT_EQ(ids({0, 1, 4, 5}), search("code:[200 TO 299]"));
  EXPECT_EQ(0, arms.term);
  EXPECT_GT(arms.column, 0);
  EXPECT_EQ(ids({3, 7}), search("code:500"));
  EXPECT_GT(arms.term, 0);
  EXPECT_EQ(0, arms.column);

  // More distinct values than a narrow range allows: the column arms run.
  std::vector<Doc> docs;
  for (int i = 0; i < 200; i++) {
    docs.push_back(flatdoc("id", "m" + std::to_string(i), "price", std::vector<double>{1000.0 + i}));
  }
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT, false, 1).success);
  EXPECT_EQ(100u, search("price:[1000 TO 1099]").size());
  EXPECT_EQ(0, arms.term);
  EXPECT_GT(arms.column, 0);
}

TEST_F(NumericMatchIndexTest, segmentsWithoutTermsAndMergeRebuild) {
  // The first segment predates the index; the second carries terms.
  setSchema(IndexMode::NONE, IndexMode::NONE);
  indexDocs(0, 4);
  setSchema(IndexMode::MATCH);
  indexDocs(4, 4);
  EXPECT_EQ(ids({2, 6}), search("code:404"));
  EXPECT_GT(arms.term, 0);
  EXPECT_GT(arms.column, 0);
  EXPECT_EQ(ids({0, 2, 4, 6}), search("when:2024-01-01"));

  // Delete one doc, then merge everything: the merged segment's terms cover
  // the older source too and drop the deleted doc.
  ASSERT_TRUE(helper.deleteByIds({"d2"}, UpdateMessage::COMMIT).success);
  indexDocs(8, 4, UpdateMessage::COMMIT, 1);
  EXPECT_EQ(ids({6, 10}), search("code:404"));
  EXPECT_GT(arms.term, 0);
  EXPECT_EQ(0, arms.column);
  EXPECT_EQ(ids({0, 1, 4, 5, 8, 9}), search("code:=(200, 201)"));
  EXPECT_EQ(ids({0, 3, 6, 9}), search("price:2.5"));
  EXPECT_EQ(ids({1, 3, 5, 7, 9, 11}), search("when:2024-01-02"));
}
