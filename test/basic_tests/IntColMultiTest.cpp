// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/index/Inverter.h"
#include "luxir/index/PostingsWriter.h"
#include "luxir/reader/PostingsReader.h"
#include "gtest/gtest.h"
#include "test/CollectionHelper.h"
#include "test/LuxirTest.h"
#include "test/SchemaBuilder.h"
#include "test/TestIndex.h"
#include "test/TestUtils.h"
#include <array>
#include <vector>

using namespace luxir;
using namespace luxir::test;

class IntColMultiTest : public LuxirTest {
};

TEST_F(IntColMultiTest, basic) {
  TestIndex testIndex;
  TestField f(testIndex, "foo_is");
  f.startIndexing();
  f.add(0, arr_i(3, 9, -2));
  f.add(2, 7);
  f.add(3, arr_i(5, 1));
  testIndex.flush();
  f.startReading();
  ASSERT_EQ(0, f.nextDoc());
  std::vector<int64_t> vals;
  f.vals(vals);
  ASSERT_EQ(vals, vec_i(-2, 3, 9));  // ascending per doc
  ASSERT_EQ(2, f.nextDoc());
  f.vals(vals);
  ASSERT_EQ(vals, vec_i(7));
  ASSERT_EQ(3, f.nextDoc());
  f.vals(vals);
  ASSERT_EQ(vals, vec_i(1, 5));
  ASSERT_EQ(-1, f.nextDoc());
}

// same docs as above, but in multiple segments that get merged together.
TEST_F(IntColMultiTest, segMerge) {
  TestIndex testIndex;
  TestField f(testIndex, "foo_is");
  f.startIndexing();
  f.add(0, arr_i(3, 9, -2));
  testIndex.flush();
  f.startIndexing();
  f.add(1, 7);
  f.add(2, arr_i(5, 1));
  testIndex.flush();
  testIndex.iw->mergeSegments();


  f.startReading();
  ASSERT_EQ(0, f.nextDoc());
  std::vector<int64_t> vals;
  f.vals(vals);
  ASSERT_EQ(vals, vec_i(-2, 3, 9));  // ascending per doc
  ASSERT_EQ(2, f.nextDoc());
  f.vals(vals);
  ASSERT_EQ(vals, vec_i(7));
  ASSERT_EQ(3, f.nextDoc());
  f.vals(vals);
  ASSERT_EQ(vals, vec_i(1, 5));
  ASSERT_EQ(-1, f.nextDoc());
}

TEST_F(IntColMultiTest, deleteAndMerge) {
  TestIndex testIndex;
  TestField f(testIndex, "foo_is");
  
  // Add first document in first segment
  f.startIndexing();
  f.add(5, arr_i(10, 20));
  f.add(7, arr_i(30));
  f.add(11, arr_i(40,50,60));
  testIndex.deleteDoc(7);
  testIndex.flush();
  
  // Add second document in second segment
  f.startIndexing();
  f.add(0, arr_i(100, 200));
  testIndex.flush();
  
  // Merge segments - this should trigger the merge bug
  testIndex.iw->mergeSegments();
  
  // Should only see the first document after merge
  f.startReading();
  ASSERT_EQ(5, f.nextDoc());
  std::vector<int64_t> vals;
  f.vals(vals);
  ASSERT_EQ(vals, vec_i(10, 20));
  ASSERT_EQ(10, f.nextDoc());
  f.vals(vals);
  ASSERT_EQ(vals, vec_i(40, 50, 60));
  ASSERT_EQ(11, f.nextDoc());
  f.vals(vals);
  ASSERT_EQ(vals, vec_i(100,200));
  ASSERT_EQ(-1, f.nextDoc());
}

TEST_F(IntColMultiTest, mergeSingleAfterMultiWithDeletes) {
  using FieldDef = api::FieldDef;
  CollectionHelper helper;
  auto writer = helper.getIndexWriter();
  writer->mergePolicy->setMergeFactor(100);
  for (bool multi : {true, false}) {
    SchemaBuilder schema;
    auto& number = schema.field("number");
    number.type = FieldDef::FieldClass::INT;
    number.multi = multi ? FieldDef::MultiMode::ARRAY : FieldDef::MultiMode::SINGLE;
    number.index = multi ? FieldDef::IndexMode::RANGE : FieldDef::IndexMode::NONE;
    schema.set(helper.collection());
    Doc drop = multi ? flatdoc("id", "drop_multi", "number", vec_i(90, 91, 92))
                     : flatdoc("id", "drop_single", "number", 99);
    Doc keep = multi ? flatdoc("id", "multi", "number", vec_i(20, 30))
                     : flatdoc("id", "single", "number", 10);
    ASSERT_TRUE(helper.indexAll({drop, keep}, UpdateMessage::COMMIT).success);
  }
  ASSERT_TRUE(helper.deleteById("drop_single", UpdateMessage::COMMIT).success);
  ASSERT_TRUE(helper.deleteById("drop_multi", UpdateMessage::COMMIT).success);
  auto before = writer->snapshots.readers.getReader();
  ASSERT_EQ(2u, before->segments().size());
  auto fields = before->field("number");
  EXPECT_TRUE(fields.at(0)->endValueRanks());
  EXPECT_FALSE(fields.at(1)->endValueRanks());

  writer->mergeSegments();
  auto reader = writer->snapshots.readers.getReader();
  ASSERT_EQ(1u, reader->segments().size());
  auto& segment = reader->segments()[0];
  ASSERT_EQ(2, segment.maxDoc());
  const auto& info = *reader->field("number").at(0);
  EXPECT_TRUE(info.points());
  std::array<std::vector<int64_t>, 2> actual;
  IntColReader::getValues(MemPool::threadLocal(), segment.postingsReader(), info, std::array{0, 1},
      [&](size_t i, int32_t, int64_t value, int64_t, int64_t) { actual[i].push_back(value); });
  EXPECT_EQ(vec_i(10), actual[1]);
  EXPECT_EQ(vec_i(20, 30), actual[0]);
}
