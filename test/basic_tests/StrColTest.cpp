
#include "gtest/gtest.h"
#include "test/SoluxTest.h"
#include "test/TestIndex.h"
#include "test/TestUtils.h"
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


TEST_F(StrColTest, multiValued) {

  {
    // single doc, single valued (but multi-valued field)
    TestIndex testIndex;
    TestField f(testIndex, "foo_ss");
    f.startIndexing();
    f.add(0, "mystring");
    testIndex.flush();
    f.startReading();
    ASSERT_EQ(0, f.nextDoc());
    ASSERT_EQ(1, f.ord());
    ASSERT_EQ(-1, f.nextDoc());
  }

  {
    // single doc, multi-valued
    TestIndex testIndex;
    TestField f(testIndex, "foo_ss");
    f.startIndexing();
    f.addStrings(0, {"b", "a"});
    testIndex.flush();
    f.startReading();
    ASSERT_EQ(0, f.nextDoc());
    std::vector<int64_t> ords;
    f.ords(ords);
    ASSERT_EQ(ords, vec(1l, 2l));
    ASSERT_EQ(-1, f.nextDoc());
  }

  {
    // multiple docs, multi-valued
    TestIndex testIndex;
    TestField f(testIndex, "foo_ss");
    f.startIndexing();
    f.addStrings(5, {"b", "a"});
    f.addStrings(10, {"c", "c"});  // handle duplicates (or throw an error)
    f.addStrings(15, {"c", "b", "a"});
    testIndex.flush();
    f.startReading();
    ASSERT_EQ(5, f.nextDoc());
    std::vector<int64_t> ords;
    f.ords(ords);
    ASSERT_EQ(ords, vec(1l, 2l));
    ASSERT_EQ(10, f.nextDoc());
    f.ords(ords);
    ASSERT_EQ(ords, vec(3l));
    ASSERT_EQ(15, f.nextDoc());
    f.ords(ords);
    ASSERT_EQ(ords, vec(1l, 2l, 3l));
    ASSERT_EQ(-1, f.nextDoc());
  }

  // multi-valued dense merge, with one segment being single-valued
  {
    TestIndex testIndex;
    TestField f(testIndex, "foo_ss");
    f.startIndexing();
    f.addStrings(0, {"a"});
    f.addStrings(1, {"c"});
    testIndex.flush();
    f.startIndexing();
    f.addStrings(0, {"c", "b", "a"});
    f.addStrings(1, {"b"});
    testIndex.flush();

    testIndex.iw->mergeSegments();
    f.startReading();
    std::vector<int64_t> ords;
    ASSERT_EQ(0, f.nextDoc());
    f.ords(ords);
    ASSERT_EQ(ords, vec(1l));
    ASSERT_EQ(1, f.nextDoc());
    f.ords(ords);
    ASSERT_EQ(ords, vec(3l));
    ASSERT_EQ(2, f.nextDoc());
    f.ords(ords);
    ASSERT_EQ(ords, vec(1l, 2l, 3l));
    ASSERT_EQ(3, f.nextDoc());
    f.ords(ords);
    ASSERT_EQ(ords, vec(2l));
    ASSERT_EQ(-1, f.nextDoc());
  }
}

