#include "luxir/index/PostingsWriter.h"
#include "luxir/reader/PostingsReader.h"
#include "gtest/gtest.h"
#include "test/LuxirTest.h"
#include "test/SegmentTest.h"
#include<boost/container/static_vector.hpp>

namespace luxir {

class PostingsReaderTest : public LuxirTest {
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

    // seekCeil of a target after every term has no ceil and reports exhausted.
    {
      std::string pastEnd;
      st.makeTerm(nTerms - 1, pastEnd);
      pastEnd.push_back('!');  // sorts after the last term
      ASSERT_FALSE(st.tenum->seekCeil(pastEnd));
    }
    // seekCeil of a target before every term positions on the first term.  Run
    // this last so the enum is left at ord 0 for the loop below.
    ASSERT_TRUE(st.tenum->seekCeil(""));
    ASSERT_EQ(st.tenum->ord(), 0);

    // random term lookups
    std::string fname;
    std::string missing;
    std::string term;
    // Track the current enum position so we can mix in seekForward (forward-only)
    // when the next target is >= the current term.  curOrd == -1 means fresh enum;
    // curOrd == nTerms means "position unknown after a miss" (disables seekForward
    // until the next successful (re)positioning).  This exercises seekForward the
    // same way applyDeletes does: a sorted run of forward seeks to present terms,
    // interleaved with full seeks.
    int curOrd = -1;
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
        curOrd = -1;
      }

      int tnum = rng.rint(0, nTerms);

      // Use seekForward when the target is at/ahead of the current position
      // (the forward-only contract).  curOrd == -1 means a fresh enum.
      bool canForward = (curOrd < 0) || (tnum >= curOrd);
      if (canForward && rng.rint(100) < 50) {
        if (rng.rbool()) {
          // forward seek to a PRESENT term at ord tnum
          st.makeTerm(tnum, term);
          bool found = st.tenum->seekForward(term);
          if (!found) {
            found = st.tenum->seekForward(term);  // place breakpoint here to debug
          }
          ASSERT_TRUE(found) << " seekForward present tnum=" << tnum
                             << " curOrd=" << curOrd << " term='" << term << "'";
          ASSERT_EQ(st.tenum->ord(), tnum) << " seekForward present tnum=" << tnum;
          curOrd = tnum;
        } else {
          // forward seek to an ABSENT term strictly between term[tnum] and
          // term[tnum+1] - the applyDeletes case (a sorted delete id missing
          // from this segment, sought via seekForward mid-iteration).
          st.makeTerm(tnum, term);
          term.push_back('!');  // sorts after term[tnum], before term[tnum+1]
          bool found = st.tenum->seekForward(term);
          if (found) {
            found = st.tenum->seekForward(term);  // place breakpoint here to debug
          }
          ASSERT_FALSE(found) << " seekForward absent between " << tnum << " and "
                              << (tnum+1) << " curOrd=" << curOrd << " term='" << term << "'";
          curOrd = tnum + 1;  // enum advances past the missed target
        }
        continue;
      }

      // seekCeil path: positions on the smallest term >= target.  Unlike
      // seekForward it may move backward, so it runs from any current position.
      if (rng.rint(100) < 40) {
        if (rng.rbool()) {
          // The ceil of a present term is that term itself.
          st.makeTerm(tnum, term);
          bool found = st.tenum->seekCeil(term);
          ASSERT_TRUE(found) << " seekCeil present tnum=" << tnum << " term='" << term << "'";
          ASSERT_EQ(st.tenum->ord(), tnum) << " seekCeil present tnum=" << tnum;
          curOrd = tnum;
        } else {
          // The ceil of a term strictly between term[tnum] and term[tnum+1] is
          // term[tnum+1] - or exhausted when tnum is the last term.
          st.makeTerm(tnum, term);
          term.push_back('!');  // sorts after term[tnum], before term[tnum+1]
          bool found = st.tenum->seekCeil(term);
          if (tnum + 1 < nTerms) {
            ASSERT_TRUE(found) << " seekCeil between tnum=" << tnum << " term='" << term << "'";
            ASSERT_EQ(st.tenum->ord(), tnum + 1) << " seekCeil ceil tnum=" << tnum;
            curOrd = tnum + 1;
          } else {
            ASSERT_FALSE(found) << " seekCeil past end tnum=" << tnum << " term='" << term << "'";
            curOrd = nTerms;  // exhausted -> position unknown, disable forward
          }
        }
        continue;
      }

      // Full seek / seekOrd path (may move backward).
      st.makeTerm(tnum, term);
      bool shouldFind = rng.rbool();
      if (!shouldFind) {
        term.push_back('!');
        if (rng.rbool()) {  // sometimes change first char to test before / after lookup failures
          term[0] = (char) rng.rbyte();
        }
      }
      bool found;
      if (shouldFind && rng.rint(100)<25) {  // look up by ord some of the time
        st.tenum->seekOrd(tnum);
        found = true;
        curOrd = tnum;
      } else {
        found = st.tenum->seek(term);
        if (found != shouldFind) {
          found = st.tenum->seek(term);  // place breakpoint here to debug
        }
        ASSERT_EQ(shouldFind, found) << " tnum=" << tnum << " term='" << term << "'";
        curOrd = found ? tnum : nTerms;  // unknown position after a miss -> disable forward
      }
      if (found) {
//        ASSERT_EQ(st.tenum->term(), term);
        auto ord = st.tenum->ord();
        ASSERT_EQ(ord, tnum) << " tnum=" << tnum << " term='" << term << "'";
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