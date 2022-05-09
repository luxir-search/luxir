#include "solux/index/PostingsWriter.h"
#include "solux/search/PostingsReader.h"
#include "gtest/gtest.h"
#include "test/SoluxTest.h"
#include "test/SegmentTest.h"
#include<boost/container/static_vector.hpp>

namespace solux {

class PostingsReaderTest : public SoluxTest {
protected:
  SegmentTest st;
public:

  void testTermSeek(int nTerms) {
    int nFields = rng.rint(1,4);
    st.positionsPerDocMax = 3;
    st.docsPerTermMax = 3;
    // st.termsPerFieldMax = Postings::TERMS_BLOCK_SIZE * 5 / 2;

    st.initWriter();
    st.addFields(false, nFields, nTerms, -1, -1);
    st.initReader();
    st.addFields(true, nFields, nTerms, -1, -1);

    bool found = st.tenum->seek("\0\0\0\0before beginning");
    ASSERT_EQ(false, found);

    // random term lookups
    std::string fname;
    std::string missing;
    std::string term;
    for (int i=0; i < nTerms*nFields*2; i++) {
      if (rng.rint(0,10) == 0) { // 10% of the time, switch fields.
        st.getFieldName(fname, rng.rint(0, nFields));
        if (rng.rbool()) { // sometimes try to find a field that doesn't exist first
          missing = fname;
          missing.push_back('!');
          if (rng.rbool()) {  // sometimes change first char to test before / after lookup failures
            missing[0] = (char) rng.rbyte();
          }
          bool found = st.fieldReader->seek(missing);
          ASSERT_FALSE(found);
        }

        bool found = st.fieldReader->seek(fname);
        ASSERT_TRUE(found);
        st.makeTermsEnum();  // refresh the terms enum st.tenum
      }

      int tnum = rng.rint(0, nTerms);
      st.makeTerm(tnum, term);

      bool shouldFind = rng.rbool();
      if (!shouldFind) {
        term.push_back('!');
        if (rng.rbool()) {  // sometimes change first char to test before / after lookup failures
          term[0] = (char) rng.rbyte();
        }
      }
      auto found = st.tenum->seek(term);
      ASSERT_EQ(shouldFind, found);
      if (found) {
        ASSERT_EQ(st.tenum->term(), term);
        // TODO: verify the postings for the term are correct
      }
    }
  }

};

TEST_F(PostingsReaderTest, termSeek) {
  // testTermSeek(0);  // TODO: need to handle case of no terms in field in postings writer
  testTermSeek(1);
  testTermSeek(Postings::TERMS_BLOCK_SIZE-1);
  testTermSeek(Postings::TERMS_BLOCK_SIZE);
  testTermSeek(Postings::TERMS_BLOCK_SIZE+1);
  testTermSeek(Postings::TERMS_BLOCK_SIZE*2-1);
  testTermSeek(Postings::TERMS_BLOCK_SIZE*2);
  testTermSeek(Postings::TERMS_BLOCK_SIZE*2+1);
  for (int i=0; i<10; i++) {
    testTermSeek(rng.rint(1,Postings::TERMS_BLOCK_SIZE*10));
  }
}



} // end namespace