#include "solux/index/Inverter.h"
#include "solux/index/PostingsWriter.h"
#include "solux/reader/PostingsReader.h"
#include "gtest/gtest.h"
#include "test/SoluxTest.h"
#include "test/TestIndex.h"
#include "test/TestUtils.h"
#include <vector>

using namespace solux;
using namespace solux::test;

class IntColMultiTest : public SoluxTest {
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