#include "solux/index/Inverter.h"
#include "solux/index/PostingsWriter.h"
#include "solux/search/PostingsReader.h"
#include "gtest/gtest.h"
#include "test/SoluxTest.h"
#include "test/TestIndex.h"
#include <vector>

using namespace solux;
using namespace solux::test;

class TextMergeTest : public SoluxTest {
protected:

};

TEST_F(TextMergeTest, basicMerge) {
  TestIndex testIndex;
  TestField f(testIndex, "foo_w");
  f.startIndexing();
  f.add(0, "seg1 both seg1 both seg1a");
  testIndex.flush();
  f.startIndexing();
  f.add(0, "both seg2 both seg2 seg2a");
  testIndex.flush();

  // TODO: force reopen of IndexReader since that is what mergeSegments uses?
  testIndex.iw->mergeSegments();

  f.startReading();

  // test docs-with-value
  ASSERT_EQ(2, testIndex.reader->numDocs());
  ASSERT_EQ(1, testIndex.reader->segments().size());
  ASSERT_EQ(0, f.nextDoc());
  ASSERT_EQ(1, f.nextDoc());
  ASSERT_EQ(-1, f.nextDoc());

  TermsEnum tenum = f.createTermsEnum();
  ASSERT_EQ(tenum.nextTerm(), true);
  ASSERT_EQ(std::string_view(tenum.term()), "both");
  ASSERT_EQ(f.readDocsAndPositions(tenum), (std::vector<int32_t>{0, 2, 2, 4,   1, 2, 1, 3}));
  ASSERT_EQ(tenum.nextTerm(), true);
  ASSERT_EQ(std::string_view(tenum.term()), "seg1");
  ASSERT_EQ(f.readDocsAndPositions(tenum), (std::vector<int32_t>{0, 2, 1, 3}));
  ASSERT_EQ(tenum.nextTerm(), true);
  ASSERT_EQ(std::string_view(tenum.term()), "seg1a");
  ASSERT_EQ(f.readDocsAndPositions(tenum), (std::vector<int32_t>{0, 1, 5}));
  ASSERT_EQ(tenum.nextTerm(), true);
  ASSERT_EQ(std::string_view(tenum.term()), "seg2");
  ASSERT_EQ(f.readDocsAndPositions(tenum), (std::vector<int32_t>{1, 2, 2, 4}));
  ASSERT_EQ(tenum.nextTerm(), true);
  ASSERT_EQ(std::string_view(tenum.term()), "seg2a");
  ASSERT_EQ(f.readDocsAndPositions(tenum), (std::vector<int32_t>{1, 1, 5}));
  ASSERT_EQ(tenum.nextTerm(), false);
}

