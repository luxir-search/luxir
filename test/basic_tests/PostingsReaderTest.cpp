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
    st.positionsPerDocMax = 3;
    st.docsPerTermMax = 3;
    // st.termsPerFieldMax = Postings::TERMS_BLOCK_SIZE * 5 / 2;

    st.initWriter();
    st.addFields(false, 1, nTerms, -1, -1);
    st.initReader();
    st.addFields(true, 1, nTerms, -1, -1);

    bool found = st.tenum->seek("\0\0\0\0before beginning");
    ASSERT_EQ(false, found);

    // random term lookups
    std::string term;
    for (int i=0; i<nTerms*2; i++) {
      int tnum = rng.rint(0, nTerms);
      st.makeTerm(tnum, term);

      bool shouldFind = rng.rbool();
      if (!shouldFind) {
        term.push_back('!');
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