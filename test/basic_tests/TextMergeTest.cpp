// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "luxir/index/Inverter.h"
#include "luxir/index/PostingsWriter.h"
#include "luxir/reader/PostingsReader.h"
#include "gtest/gtest.h"
#include "test/LuxirTest.h"
#include "test/TestIndex.h"
#include <vector>

using namespace luxir;
using namespace luxir::test;

class TextMergeTest : public LuxirTest {
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
  ASSERT_EQ(2, testIndex.reader->maxDoc());
  ASSERT_EQ(1, testIndex.reader->segments().size());
  ASSERT_EQ(0, f.nextDoc());
  ASSERT_EQ(1, f.nextDoc());
  ASSERT_EQ(-1, f.nextDoc());

  TermsEnum tenum = f.createTermsEnum();
  ASSERT_EQ(tenum.nextTerm(), true);
  ASSERT_EQ(std::string_view(tenum.term()), "both");
  ASSERT_EQ(f.readDocsAndPositions(tenum), (std::vector<int32_t>{0, 2, 1, 3,   1, 2, 0, 2}));
  ASSERT_EQ(tenum.nextTerm(), true);
  ASSERT_EQ(std::string_view(tenum.term()), "seg1");
  ASSERT_EQ(f.readDocsAndPositions(tenum), (std::vector<int32_t>{0, 2, 0, 2}));
  ASSERT_EQ(tenum.nextTerm(), true);
  ASSERT_EQ(std::string_view(tenum.term()), "seg1a");
  ASSERT_EQ(f.readDocsAndPositions(tenum), (std::vector<int32_t>{0, 1, 4}));
  ASSERT_EQ(tenum.nextTerm(), true);
  ASSERT_EQ(std::string_view(tenum.term()), "seg2");
  ASSERT_EQ(f.readDocsAndPositions(tenum), (std::vector<int32_t>{1, 2, 1, 3}));
  ASSERT_EQ(tenum.nextTerm(), true);
  ASSERT_EQ(std::string_view(tenum.term()), "seg2a");
  ASSERT_EQ(f.readDocsAndPositions(tenum), (std::vector<int32_t>{1, 1, 4}));
  ASSERT_EQ(tenum.nextTerm(), false);

  // Now lets test merging a new field
  TestField f2(testIndex, "foo2_w");
  f2.startIndexing();
  f2.add(0, "yeah!");
  testIndex.flush();
  testIndex.iw->mergeSegments();

  f2.startReading();
  ASSERT_EQ(3, testIndex.reader->maxDoc());
  ASSERT_EQ(1, testIndex.reader->segments().size());
  ASSERT_EQ(2, f2.nextDoc());
  ASSERT_EQ(-1, f2.nextDoc());

  TermsEnum tenum2 = f2.createTermsEnum();
  ASSERT_EQ(tenum2.nextTerm(), true);
  ASSERT_EQ(std::string_view(tenum2.term()), "yeah!");
  ASSERT_EQ(f2.readDocsAndPositions(tenum2), (std::vector<int32_t>{2, 1, 0}));
  ASSERT_EQ(tenum2.nextTerm(), false);

  // Make sure the original is still there...
  f.startReading();
  TermsEnum tenum_f1 = f.createTermsEnum();
  ASSERT_EQ(tenum_f1.docsWithField(), 2);
  ASSERT_EQ(tenum_f1.nextTerm(), true);
  ASSERT_EQ(std::string_view(tenum_f1.term()), "both");
  ASSERT_EQ(f.readDocsAndPositions(tenum_f1), (std::vector<int32_t>{0, 2, 1, 3,   1, 2, 0, 2}));
}

