#include "solux/search/PostingsReader.h"
#include "gtest/gtest.h"
#include "test/SoluxTest.h"
#include "test/TestIndex.h"

using namespace solux;
using namespace solux::test;

class TermScorerTest : public SoluxTest {
protected:
};


TEST_F(TermScorerTest, textLen) {
  {
    TestIndex testIndex;
    TestField f(testIndex, "foo_w");
    f.startIndexing();
    f.add(1, "now is the time");
    f.add(3, "for all good men");
    f.add(5, "to come to the aid of their country");
    f.add(7, "to the moon!");
    testIndex.flush();
    f.startReading();

    // Field stats for foo_w:
    // docCount == 4
    // maxDoc == 8 (0 through 7)
    // sumTotalTermFreq = 19
    // sumDocFreq = 18 (just one overlap... "to" appears twice in doc 5)
    // numTerms = 15  (repeated terms are "to":3, "the":"3", hense 15+2extra+2extra = 19 sumTotalTermFreq

    // Term stats for foo_w:to
    // docFreq == 2
    // totalTermFreq = 3

    TermsEnum tenum = f.createTermsEnum();
    ASSERT_EQ(f.fieldInfo.docsWithField, 4);
    ASSERT_EQ(tenum.docsWithField(), 4);
    ASSERT_EQ(tenum.numTerms(), 15);
    ASSERT_EQ(tenum.sumTotalTermFreq(), 19);
    ASSERT_EQ(tenum.sumDocFreq(), 18);


  }


}
