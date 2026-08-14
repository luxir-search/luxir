#include "luxir/index/Inverter.h"
#include "luxir/index/PostingsWriter.h"
#include "luxir/reader/PostingsReader.h"
#include "gtest/gtest.h"
#include "test/LuxirTest.h"
#include "test/TestIndex.h"
#include "test/TestUtils.h"
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
  ASSERT_EQ(vals, vec_i(3, 9, -2));
  ASSERT_EQ(2, f.nextDoc());
  f.vals(vals);
  ASSERT_EQ(vals, vec_i(7));
  ASSERT_EQ(3, f.nextDoc());
  f.vals(vals);
  ASSERT_EQ(vals, vec_i(5, 1));
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
  ASSERT_EQ(vals, vec_i(3, 9, -2));
  ASSERT_EQ(2, f.nextDoc());
  f.vals(vals);
  ASSERT_EQ(vals, vec_i(7));
  ASSERT_EQ(3, f.nextDoc());
  f.vals(vals);
  ASSERT_EQ(vals, vec_i(5, 1));
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