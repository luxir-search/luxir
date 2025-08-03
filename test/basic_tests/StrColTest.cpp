
#include "gtest/gtest.h"
#include "test/SoluxTest.h"
#include "test/TestIndex.h"
#include "test/TestUtils.h"
#include "solux/index/handler/StrColHandler.h"
#include "solux/reader/StrColReader.h"
#include "solux/reader/FieldReader.h"
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
    // single doc, multi-valued, dense
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
    // single doc, multi-valued, sparse (designed to trigger a bug
    // where inverter would think column was dense because numValues==numDocs
    TestIndex testIndex;
    TestField f(testIndex, "foo_ss");
    f.startIndexing();
    f.addStrings(1, {"b", "a"});
    testIndex.flush();
    f.startReading();
    ASSERT_EQ(1, f.nextDoc());
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

TEST_F(StrColTest, deleteAndMerge) {
  TestIndex testIndex;
  TestField f(testIndex, "foo_s");
  
  // Add documents in first segment
  f.startIndexing();
  f.add(5, "hello");    // will be ord 1
  f.add(7, "world");    // will be ord 2  
  f.add(11, "test");    // will be ord 3
  testIndex.deleteDoc(7);  // delete the middle document
  testIndex.flush();
  
  // Add document in second segment
  f.startIndexing();
  f.add(0, "apple");    // will be doc10 after merge and term will be ord 1tes
  testIndex.flush();
  
  // Merge segments - this should trigger the merge bug
  testIndex.iw->mergeSegments();
  

  f.startReading();
  ASSERT_EQ(5, f.nextDoc());
  ASSERT_EQ(2, f.ord());  // "hello" 
  ASSERT_EQ(10, f.nextDoc());
  ASSERT_EQ(3, f.ord());  // "test"
  ASSERT_EQ(11, f.nextDoc());
  ASSERT_EQ(1, f.ord());  // "test"
  ASSERT_EQ(-1, f.nextDoc());
}

TEST_F(StrColTest, deleteAndMergeMultiValued) {
  TestIndex testIndex;
  TestField f(testIndex, "foo_ss");
  
  // Add documents in first segment
  f.startIndexing();
  f.addStrings(5, {"hello", "world"});    // hello=ord 2, world=ord 6
  f.addStrings(7, {"test", "data", "hello", "string", "banana"});  // both unique terms and terms in other docs.
  f.addStrings(11, {"final", "string"});  // final=ord 3, string=ord 4
  testIndex.deleteDoc(7);  // delete the middle document
  testIndex.flush();
  
  // Add document in second segment
  f.startIndexing();
  f.addStrings(0, {"apple", "banana"});   // apple=ord 1, banana=ord 2 (before global merge)
  testIndex.flush();
  
  testIndex.iw->mergeSegments();
  
  // After merge, global ordinals should be:
  // "apple"=1, "banana"=2, "final"=3, "hello"=4, "string"=5, "world"=6
  // (deleted terms "data" and "test" should not appear)
  
  f.startReading();
  std::vector<int64_t> ords;
  
  ASSERT_EQ(5, f.nextDoc());
  f.ords(ords);
  ASSERT_EQ(ords, vec(4l, 6l));  // "hello"=4, "world"=6
  
  ASSERT_EQ(10, f.nextDoc());  // renumbered from 11
  f.ords(ords);
  ASSERT_EQ(ords, vec(3l, 5l));  // "final"=3, "string"=5
  
  ASSERT_EQ(11, f.nextDoc());  // renumbered from 0 in second segment
  f.ords(ords);
  ASSERT_EQ(ords, vec(1l, 2l));  // "apple"=1, "banana"=2
  
  ASSERT_EQ(-1, f.nextDoc());
}


TEST_F(StrColTest, strColReaderBasic) {
  // Test StrColReader for column-only string storage
  TestIndex testIndex;
  TestField f(testIndex, "description_sc");
  
  // Add documents with some missing values
  f.startIndexing();
  f.add(0, "First document description");
  // Doc 1 has no value
  f.add(2, "Second doc with text");
  f.add(3, "");  // Empty string
  // Doc 4 has no value
  f.add(5, "Final document with a longer description text");
  
  testIndex.flush();
  
  // Now read using StrColReader
  testIndex.initReader();
  auto& segment = testIndex.reader->segments()[0];
  auto& postingsReader = segment.postingsReader();
  FieldReader fieldReader(MemPool::threadLocal(), postingsReader);
  bool found = fieldReader.seek("description_sc");
  ASSERT_TRUE(found);
  
  SegFieldInfo segFieldInfo;
  fieldReader.readFieldInfo(segFieldInfo);
  
  StrColReader strReader(postingsReader, segFieldInfo);
  ASSERT_EQ(4, strReader.docsWithValue());  // 4 docs have values (0, 2, 3, 5)
  
  // Iterate through docs with values
  StrColReader::Iterator iter(strReader);
  
  ASSERT_EQ(0, iter.advance(0));
  auto val = iter.value();
  
  ASSERT_EQ("First document description", val);
  
  ASSERT_EQ(2, iter.next());  // Skip doc 1 which has no value
  ASSERT_EQ("Second doc with text", iter.value());
  
  ASSERT_EQ(3, iter.next());
  ASSERT_EQ("", iter.value());  // Empty string
  
  ASSERT_EQ(5, iter.next());  // Skip doc 4
  ASSERT_EQ("Final document with a longer description text", iter.value());
  
  ASSERT_EQ(StrColReader::Iterator::ENDDOC, iter.next());
  
  // Test advance
  StrColReader::Iterator iter2(strReader);
  ASSERT_EQ(2, iter2.advance(2));
  ASSERT_EQ("Second doc with text", iter2.value());
  
  ASSERT_EQ(5, iter2.advance(4));  // Advance to 4, but doc 4 has no value, so we get doc 5
  ASSERT_EQ("Final document with a longer description text", iter2.value());
  
  // Test static getValues method
  std::vector<int32_t> docIds = {0, 1, 2, 3, 4, 5};
  std::vector<std::pair<int32_t, std::string>> results;
  
  StrColReader::getValues(MemPool::threadLocal(), postingsReader, segFieldInfo, docIds,
    [&results](size_t idx, int32_t docid, std::string_view value) {
      results.emplace_back(docid, std::string(value));
    });
  
  ASSERT_EQ(4, results.size());
  ASSERT_EQ(0, results[0].first);
  ASSERT_EQ("First document description", results[0].second);
  ASSERT_EQ(2, results[1].first);
  ASSERT_EQ("Second doc with text", results[1].second);
  ASSERT_EQ(3, results[2].first);
  ASSERT_EQ("", results[2].second);
  ASSERT_EQ(5, results[3].first);
  ASSERT_EQ("Final document with a longer description text", results[3].second);
}


TEST_F(StrColTest, fixedSizeOptimization) {
  // Test that fixed-size strings skip the mono column
  {
    // All strings same size (10 chars)
    TestIndex testIndex;
    TestField f(testIndex, "fixed_sc");

    f.startIndexing();
    f.add(0, "0123456789");
    f.add(1, "abcdefghij");
    f.add(3, "!@#$%^&*()");

    testIndex.flush();

    // Read and verify values
    testIndex.initReader();
    auto& segment = testIndex.reader->segments()[0];
    auto& postingsReader = segment.postingsReader();
    FieldReader fieldReader(MemPool::threadLocal(), postingsReader);
    bool found = fieldReader.seek("fixed_sc");
    ASSERT_TRUE(found);

    SegFieldInfo segFieldInfo;
    fieldReader.readFieldInfo(segFieldInfo);

    // Test reading with StrColReader
    StrColReader strReader(postingsReader, segFieldInfo);

    ASSERT_EQ(nullptr, strReader.getEndRankReader());  // No end rank reader for fixed-size strings

    StrColReader::Iterator iter(strReader);

    ASSERT_EQ(0, iter.advance(0));
    ASSERT_EQ("0123456789", iter.value());

    ASSERT_EQ(1, iter.next());
    ASSERT_EQ("abcdefghij", iter.value());

    ASSERT_EQ(3, iter.next());
    ASSERT_EQ("!@#$%^&*()", iter.value());

    ASSERT_EQ(StrColReader::Iterator::ENDDOC, iter.next());
  }
}

