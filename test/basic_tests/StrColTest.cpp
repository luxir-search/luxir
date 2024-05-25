#include "solux/index/Inverter.h"
#include "solux/index/PostingsWriter.h"
#include "solux/reader/PostingsReader.h"
#include "gtest/gtest.h"
#include "test/SoluxTest.h"
#include "test/TestIndex.h"
#include <vector>

using namespace solux;
using namespace solux::test;

class StrColTest : public SoluxTest {
protected:

};


TEST_F(StrColTest, basic) {
  {
    // single doc
    TestIndex testIndex;
    TestField f(testIndex, "foo_s");
    f.startIndexing();
    f.add(0, "mystring");
    testIndex.flush();
    f.startReading();
    ASSERT_EQ(0, f.nextDoc());
    ASSERT_EQ(1, f.ord());
    ASSERT_EQ(-1, f.nextDoc());
  }

  {
    // multiple docs
    TestIndex testIndex;
    TestField f(testIndex, "foo_s");
    f.startIndexing();
    f.add(0, "ccc");  // ord 3
    f.add(1, "bbb");  // ord 2
    f.add(2, "aaa");  // ord 1
    f.add(3, "bbb");  // ord 2
    testIndex.flush();
    f.startReading();
    ASSERT_EQ(0, f.nextDoc());
    ASSERT_EQ(3, f.ord());
    ASSERT_EQ(1, f.nextDoc());
    ASSERT_EQ(2, f.ord());
    ASSERT_EQ(2, f.nextDoc());
    ASSERT_EQ(1, f.ord());
    ASSERT_EQ(3, f.nextDoc());
    ASSERT_EQ(2, f.ord());
    ASSERT_EQ(-1, f.nextDoc());
  }

  {
    // sparse docs
    TestIndex testIndex;
    TestField f(testIndex, "foo_s");
    f.startIndexing();
    f.add(5, "ccc");  // ord 3
    f.add(500, "bbb");  // ord 2
    f.add(500000, "aaa");  // ord 1
    f.add(5000000, "bbb");  // ord 2
    testIndex.flush();
    f.startReading();
    ASSERT_EQ(5, f.nextDoc());
    ASSERT_EQ(3, f.ord());
    ASSERT_EQ(500, f.nextDoc());
    ASSERT_EQ(2, f.ord());
    ASSERT_EQ(500000, f.nextDoc());
    ASSERT_EQ(1, f.ord());
    ASSERT_EQ(5000000, f.nextDoc());
    ASSERT_EQ(2, f.ord());
    ASSERT_EQ(-1, f.nextDoc());
  }
}


TEST_F(StrColTest, basicMerge) {

  // dense merge
  {
    TestIndex testIndex;
    TestField f(testIndex, "foo_s");
    f.startIndexing();
    f.add(0, "ccc");  // global ord 3
    f.add(1, "bbb");  // global ord 2
    testIndex.flush();
    f.startIndexing();
    f.add(0, "aaa");  // global ord 1
    f.add(1, "bbb");  // global ord 2
    testIndex.flush();

    // TODO: force reopen of IndexReader since that is what mergeSegments uses?
    testIndex.iw->mergeSegments();

    f.startReading();

    ASSERT_EQ(0, f.nextDoc());
    ASSERT_EQ(3, f.ord());
    ASSERT_EQ(1, f.nextDoc());
    ASSERT_EQ(2, f.ord());
    ASSERT_EQ(2, f.nextDoc());
    ASSERT_EQ(1, f.ord());
    ASSERT_EQ(3, f.nextDoc());
    ASSERT_EQ(2, f.ord());
    ASSERT_EQ(-1, f.nextDoc());
  }


  // sparse merge
  {
    TestIndex testIndex;
    TestField f(testIndex, "foo_s");
    f.startIndexing();
    f.add(10, "ccc");      // global ord 3
    f.add(100000, "bbb");  // global ord 2
    testIndex.flush();
    f.startIndexing();
    f.add(7, "aaa");      // global ord 1
    f.add(70000, "bbb");  // global ord 2
    testIndex.flush();

    // TODO: force reopen of IndexReader since that is what mergeSegments uses?
    testIndex.iw->mergeSegments();

    f.startReading();

    ASSERT_EQ(10, f.nextDoc());
    ASSERT_EQ(3, f.ord());
    ASSERT_EQ(100000, f.nextDoc());
    ASSERT_EQ(2, f.ord());
    ASSERT_EQ(100000+1+7, f.nextDoc());
    ASSERT_EQ(1, f.ord());
    ASSERT_EQ(100000+1+70000, f.nextDoc());
    ASSERT_EQ(2, f.ord());
    ASSERT_EQ(-1, f.nextDoc());
  }

}
