#include <solux/query/AllQuery.h>
#include "gtest/gtest.h"
#include "test/SoluxTest.h"
#include "test/TestIndex.h"
#include "solux/query/TermQuery.h"
#include "solux/query/PhraseQuery.h"
#include "solux/query/BooleanQuery.h"


using namespace solux;
using namespace solux::test;

class TermScorerTest : public SoluxTest {
protected:

  std::vector<const char*> text = {
          "now is the time",
          "for all good men",
          "to come to the aid of their country",
          "to the moon!"
  };

  // Field stats for the above field values:
  // docCount == 4
  // maxDoc == 8 (0 through 7)
  // sumTotalTermFreq = 19
  // sumDocFreq = 18 (just one overlap... "to" appears twice in doc 5)
  // numTerms = 15  (repeated terms are "to":3, "the":"3", hence 15+2extra+2extra = 19 sumTotalTermFreq

  // Term stats for the term "to" in the above field.
  // docFreq == 2
  // totalTermFreq = 3
  int testScores(Query::Scorer* scorer, std::vector<int> expectedDocs, std::vector<float> expectedScores) {
    if (scorer == nullptr) {
      EXPECT_EQ(expectedDocs.size(), 0);
      return 0;
    }
    for (size_t i = 0; i < expectedDocs.size(); i++) {
      EXPECT_EQ(expectedDocs[i], scorer->next());
      EXPECT_FLOAT_EQ(expectedScores[i], scorer->score());
    }
    int32_t doc = scorer->next();
    EXPECT_EQ(doc, PostingsReader::END);
    return 0;
  }


};


TEST_F(TermScorerTest, singleSeg) {

  {
    TestIndex testIndex;
    TestField f(testIndex, "foo_w");
    f.startIndexing();
    f.add(1, text[0]);
    f.add(3, text[1]);
    f.add(5, text[2]);
    f.add(7, text[3]);
    testIndex.flush();
    f.startReading();

    // Field stats for foo_w:
    // docCount == 4
    // maxDoc == 8 (0 through 7)
    // sumTotalTermFreq = 19
    // sumDocFreq = 18 (just one overlap... "to" appears twice in doc 5)
    // numTerms = 15  (repeated terms are "to":3, "the":"3", hence 15+2extra+2extra = 19 sumTotalTermFreq

    // Term stats for foo_w:to
    // docFreq == 2
    // totalTermFreq = 3

    TermsEnum tenum = f.createTermsEnum();
    ASSERT_EQ(f.fieldInfo.docsWithField, 4);
    ASSERT_EQ(tenum.docsWithField(), 4);
    ASSERT_EQ(tenum.numTerms(), 15);
    ASSERT_EQ(tenum.sumTotalTermFreq(), 19);
    ASSERT_EQ(tenum.sumDocFreq(), 18);

    ASSERT_EQ(tenum.seek("to"), true);

    DocsEnum denum(testIndex.pool, f.currentSegment()->postingsReader(), tenum);
    ASSERT_EQ(denum.numDocs(), 2);
    ASSERT_EQ(denum.totalTermFreq(), 3);

    Similarity::FieldStats fieldStats;
    fieldStats.sumTotalTermFreq = tenum.sumTotalTermFreq();
    fieldStats.sumDocFreq = tenum.sumDocFreq();
    fieldStats.docsWithField = tenum.docsWithField();
    fieldStats.maxDoc = f.currentSegment()->postingsReader().numDocs();

    Similarity::TermStats termStats;
    termStats.docFreq = denum.numDocs();
    termStats.totalTermFreq = denum.totalTermFreq();

    Similarity sim;
    auto simScorer = sim.getScorer(1.0, fieldStats, termStats);

    IntColReader& normsCol = *f.colReader;

    // lucene scores the docs as follows:
    // doc=5 score=0.36330473
    // doc=7 score=0.37098017
    TermQuery::Scorer termScorer(denum, normsCol, simScorer);
    ASSERT_EQ(termScorer.next(), 5);
    ASSERT_EQ(termScorer.docId(), 5);
    ASSERT_EQ(termScorer.termFreq(), 2);
    ASSERT_EQ(termScorer.score(), 0.36330473f);
    ASSERT_EQ(termScorer.next(), 7);
    ASSERT_EQ(termScorer.termFreq(), 1);
    ASSERT_EQ(termScorer.score(), 0.37098017f);
    ASSERT_EQ(termScorer.next(), PostingsReader::END);


    // Now try from the beginning:
    {
      auto poolFree = testIndex.pool.rewindScopeGuard();
      TermQuery tq("foo_w", "to");
      Query::Context qContext(testIndex.pool, *testIndex.reader);

      auto* weight = tq.createWeight(qContext);
      TermQuery::Scorer* scorer = dynamic_cast<TermQuery::Scorer*>( weight->createScorer(testIndex.pool,
                                                                                         qContext.topReader.segments()[0]));
      testScores(scorer, {5, 7}, {0.36330473f, 0.37098017f});
    }
  }
}


TEST_F(TermScorerTest, multiSeg) {
  {
    TestIndex testIndex;
    TestField f(testIndex, "foo_w");
    f.startIndexing();
    f.add(1, text[0]);
    f.add(3, text[1]);
    testIndex.flush();
    f.startIndexing();
    f.add(2, text[2]);
    f.add(4, text[3]);
    testIndex.flush();
    f.startReading();

    {
      auto poolFree = testIndex.pool.rewindScopeGuard();
      TermQuery tq("foo_w", "to");
      Query::Context qContext(testIndex.pool, *testIndex.reader);

      auto* weight = tq.createWeight(qContext);
      // put the scorer creation in a separate scope to test that it's OK to rewind the pool after we are done with a single scorer.
      {
        auto g1 = testIndex.pool.rewindScopeGuard();
        TermQuery::Scorer* scorer = dynamic_cast<TermQuery::Scorer*>( weight->createScorer(
                testIndex.pool, qContext.topReader.segments()[0]));
        testScores(scorer, {}, {}); // first segment doesn't have "to"
      }
      {
        auto g2 = testIndex.pool.rewindScopeGuard();
        TermQuery::Scorer* scorer = dynamic_cast<TermQuery::Scorer*>( weight->createScorer(
                testIndex.pool, qContext.topReader.segments()[1]));
        testScores(scorer, {2, 4}, {0.36330473f, 0.37098017f});
      }
    }
  }

  // Now try the test again with "to" in both segments this time.
  {
    TestIndex testIndex;
    TestField f(testIndex, "foo_w");
    f.startIndexing();
    f.add(1, text[0]);
    f.add(3, text[2]);
    testIndex.flush();
    f.startIndexing();
    f.add(2, text[1]);
    f.add(4, text[3]);
    testIndex.flush();
    f.startReading();

    {
      auto poolFree = testIndex.pool.rewindScopeGuard();
      TermQuery tq("foo_w", "to");
      Query::Context qContext(testIndex.pool, *testIndex.reader);

      auto* weight = tq.createWeight(qContext);
      // put the scorer creation in a separate scope to test that it's OK to rewind the pool after we are done with a single scorer.
      {
        auto g1 = testIndex.pool.rewindScopeGuard();
        TermQuery::Scorer* scorer = dynamic_cast<TermQuery::Scorer*>( weight->createScorer(
                testIndex.pool, qContext.topReader.segments()[0]));
        testScores(scorer, {3}, {0.36330473f});
      }
      {
        auto g2 = testIndex.pool.rewindScopeGuard();
        TermQuery::Scorer* scorer = dynamic_cast<TermQuery::Scorer*>( weight->createScorer(
                testIndex.pool, qContext.topReader.segments()[1]));
        testScores(scorer, {4}, {0.37098017f});
      }
    }


    // try an all-scorer
    {
      auto poolFree = testIndex.pool.rewindScopeGuard();
      float score = 0.0f; // current expected score for an all-scorer is 0.0f
      AllQuery allQuery;
      Query::Context qContext(testIndex.pool, *testIndex.reader);
      auto* weight = allQuery.createWeight(qContext);
      // put the scorer creation in a separate scope to test that it's OK to rewind the pool after we are done with a single scorer.
      {
        auto g1 = testIndex.pool.rewindScopeGuard();
        AllQuery::Scorer* scorer = dynamic_cast<AllQuery::Scorer*>( weight->createScorer(
                testIndex.pool, qContext.topReader.segments()[0]));
        testScores(scorer, {0,1,2,3}, {score, score, score, score});
      }
      {
        auto g2 = testIndex.pool.rewindScopeGuard();
        AllQuery::Scorer* scorer = dynamic_cast<AllQuery::Scorer*>( weight->createScorer(
                testIndex.pool, qContext.topReader.segments()[1]));
        testScores(scorer, {0,1,2,3,4}, {score, score, score, score, score});
      }
    }
  }
}


TEST_F(TermScorerTest, boolScore) {
  {
    TestIndex testIndex;
    TestField f(testIndex, "foo_w");
    f.startIndexing();
    f.add(1, text[0]);
    f.add(3, text[1]);
    testIndex.flush();
    f.startIndexing();
    f.add(2, text[2]);
    f.add(4, text[3]);
    testIndex.flush();
    f.startReading();

    TermQuery to("foo_w", "to");   // appears in text[2,3] (docs 2,4)
    TermQuery the("foo_w", "the");  // appears in text[0,2,3] (docs 1,2,4)
    TermQuery moon("foo_w", "moon!");  // appears in text[3] (docs 4)

    // The "to" scorer should be null for seg 0 in this test
    {
      std::vector<Query*> queries = {&to, &the};
      BooleanQuery q({}, queries, {}, {});

      auto poolFree = testIndex.pool.rewindScopeGuard();
      Query::Context qContext(testIndex.pool, *testIndex.reader);
      auto* weight = q.createWeight(qContext);
      // put the scorer creation in a separate scope to test that it's OK to rewind the pool after we are done with a single scorer.
      {
        auto g = testIndex.pool.rewindScopeGuard();
        Query::Scorer* scorer = weight->createScorer(
                testIndex.pool, qContext.topReader.segments()[0]);
        testScores(scorer, {1}, {0.17332031f});
      }
      {
        auto g = testIndex.pool.rewindScopeGuard();
        Query::Scorer* scorer = weight->createScorer(
                testIndex.pool, qContext.topReader.segments()[1]);
        testScores(scorer, {2, 4}, {0.48997432f, 0.5618766f});
      }
    }


    // conjunction scorer
    {
      std::vector<Query*> queries = {&to, &the};
      BooleanQuery q(queries, {}, {}, {});

      auto poolFree = testIndex.pool.rewindScopeGuard();
      Query::Context qContext(testIndex.pool, *testIndex.reader);
      auto* weight = q.createWeight(qContext);
      // put the scorer creation in a separate scope to test that it's OK to rewind the pool after we are done with a single scorer.
      {
        auto g = testIndex.pool.rewindScopeGuard();
        Query::Scorer* scorer = weight->createScorer(
                testIndex.pool, qContext.topReader.segments()[0]);
        testScores(scorer, {}, {});
      }
      {
        auto g = testIndex.pool.rewindScopeGuard();
        Query::Scorer* scorer = weight->createScorer(
                testIndex.pool, qContext.topReader.segments()[1]);
        testScores(scorer, {2, 4}, {0.48997432f, 0.5618766f});
      }
    }

    // mix of conjunction, disjunction
    {
      std::vector<Query*> mand = {&to};
      std::vector<Query*> opt = {&the};
      BooleanQuery q(mand, opt, {}, {});

      auto poolFree = testIndex.pool.rewindScopeGuard();
      Query::Context qContext(testIndex.pool, *testIndex.reader);
      auto* weight = q.createWeight(qContext);
      // put the scorer creation in a separate scope to test that it's OK to rewind the pool after we are done with a single scorer.
      {
        auto g = testIndex.pool.rewindScopeGuard();
        Query::Scorer* scorer = weight->createScorer(
                testIndex.pool, qContext.topReader.segments()[0]);
        testScores(scorer, {}, {});
      }
      {
        auto g = testIndex.pool.rewindScopeGuard();
        Query::Scorer* scorer = weight->createScorer(
                testIndex.pool, qContext.topReader.segments()[1]);
        testScores(scorer, {2, 4}, {0.48997432f, 0.5618766f});
      }
    }

    // mix of conjunction, disjunction opposite order
    {
      std::vector<Query*> mand = {&the};
      std::vector<Query*> opt = {&to};
      BooleanQuery q(mand, opt, {}, {});

      auto poolFree = testIndex.pool.rewindScopeGuard();
      Query::Context qContext(testIndex.pool, *testIndex.reader);
      auto* weight = q.createWeight(qContext);
      // put the scorer creation in a separate scope to test that it's OK to rewind the pool after we are done with a single scorer.
      {
        auto g = testIndex.pool.rewindScopeGuard();
        Query::Scorer* scorer = weight->createScorer(
                testIndex.pool, qContext.topReader.segments()[0]);
        testScores(scorer, {1}, {0.17332031f});
      }
      {
        auto g = testIndex.pool.rewindScopeGuard();
        Query::Scorer* scorer = weight->createScorer(
                testIndex.pool, qContext.topReader.segments()[1]);
        testScores(scorer, {2, 4}, {0.48997432f, 0.5618766f});
      }
    }


    // add single prohibited clause
    {
      std::vector<Query*> mand = {&the};
      std::vector<Query*> opt = {&to};
      std::vector<Query*> neg = {&moon};

      BooleanQuery q(mand, opt, neg, {});

      auto poolFree = testIndex.pool.rewindScopeGuard();
      Query::Context qContext(testIndex.pool, *testIndex.reader);
      auto* weight = q.createWeight(qContext);
      // put the scorer creation in a separate scope to test that it's OK to rewind the pool after we are done with a single scorer.
      {
        auto g = testIndex.pool.rewindScopeGuard();
        Query::Scorer* scorer = weight->createScorer(
                testIndex.pool, qContext.topReader.segments()[0]);
        testScores(scorer, {1}, {0.17332031f});
      }
      {
        auto g = testIndex.pool.rewindScopeGuard();
        Query::Scorer* scorer = weight->createScorer(
                testIndex.pool, qContext.topReader.segments()[1]);
        testScores(scorer, {2}, {0.48997432f});
      }
    }

    // add more prohibited clauses
    {
      TermQuery does_not_exist("foo_w", "does_not_exist");
      TermQuery time("foo_w", "time");  // matches doc 1
      TermQuery men("foo_w", "men");  // matches doc 3

      std::vector<Query*> mand = {&the};
      std::vector<Query*> opt = {&to};
      std::vector<Query*> neg = {&does_not_exist, &moon, &time, &men};

      BooleanQuery q(mand, opt, neg, {});

      auto poolFree = testIndex.pool.rewindScopeGuard();
      Query::Context qContext(testIndex.pool, *testIndex.reader);
      auto* weight = q.createWeight(qContext);
      // put the scorer creation in a separate scope to test that it's OK to rewind the pool after we are done with a single scorer.
      {
        auto g = testIndex.pool.rewindScopeGuard();
        Query::Scorer* scorer = weight->createScorer(
                testIndex.pool, qContext.topReader.segments()[0]);
        testScores(scorer, {}, {});
      }
      {
        auto g = testIndex.pool.rewindScopeGuard();
        Query::Scorer* scorer = weight->createScorer(
                testIndex.pool, qContext.topReader.segments()[1]);
        testScores(scorer, {2}, {0.48997432f});
      }
    }

    // mandatory clause with filter
    {
      std::vector<Query*> mand = {&the};
      std::vector<Query*> filter = {&to};

      BooleanQuery q(mand, {}, {},
                     filter);  // this should match the same as a "to" and "the" conjunction, but score differently.


      auto poolFree = testIndex.pool.rewindScopeGuard();
      Query::Context qContext(testIndex.pool, *testIndex.reader);
      auto* weight = q.createWeight(qContext);
      // put the scorer creation in a separate scope to test that it's OK to rewind the pool after we are done with a single scorer.
      {
        auto g = testIndex.pool.rewindScopeGuard();
        Query::Scorer* scorer = weight->createScorer(
                testIndex.pool, qContext.topReader.segments()[0]);
        testScores(scorer, {}, {});
      }
      {
        auto g = testIndex.pool.rewindScopeGuard();
        Query::Scorer* scorer = weight->createScorer(
                testIndex.pool, qContext.topReader.segments()[1]);
        testScores(scorer, {2, 4}, {0.1266696f, 0.19089644f});
      }
    }

    // optional clause with filter
    {
      std::vector<Query*> opt = {&the};
      std::vector<Query*> filter = {&to};

      BooleanQuery q({}, opt, {},
                     filter);  // this should match the same as a "to" and "the" conjunction, but score differently.


      auto poolFree = testIndex.pool.rewindScopeGuard();
      Query::Context qContext(testIndex.pool, *testIndex.reader);
      auto* weight = q.createWeight(qContext);
      // put the scorer creation in a separate scope to test that it's OK to rewind the pool after we are done with a single scorer.
      {
        auto g = testIndex.pool.rewindScopeGuard();
        Query::Scorer* scorer = weight->createScorer(
                testIndex.pool, qContext.topReader.segments()[0]);
        testScores(scorer, {}, {});
      }
      {
        auto g = testIndex.pool.rewindScopeGuard();
        Query::Scorer* scorer = weight->createScorer(
                testIndex.pool, qContext.topReader.segments()[1]);
        testScores(scorer, {2, 4}, {0.1266696f, 0.19089644f});
      }
    }


    // phrase scoring (add idfs of terms, and termfreq is number of occurances of phrase)
    // optional clause with filter
    {
      std::vector<std::string_view> terms = {"to", "the"};
      std::vector<std::int32_t> positions = {0, 1};

      PhraseQuery phrase("foo_w", terms, positions);

      auto poolFree = testIndex.pool.rewindScopeGuard();
      Query::Context qContext(testIndex.pool, *testIndex.reader);
      auto* weight = phrase.createWeight(qContext);
      // put the scorer creation in a separate scope to test that it's OK to rewind the pool after we are done with a single scorer.
      {
        auto g = testIndex.pool.rewindScopeGuard();
        Query::Scorer* scorer = weight->createScorer(
                testIndex.pool, qContext.topReader.segments()[0]);
        testScores(scorer, {}, {});
      }
      {
        auto g = testIndex.pool.rewindScopeGuard();
        Query::Scorer* scorer = weight->createScorer(
                testIndex.pool, qContext.topReader.segments()[1]);
        testScores(scorer, {2, 4}, {0.37283403f, 0.56187654f});
      }
    }

    // reversed phrase shouldn't match anything
    {
      std::vector<std::string_view> terms = {"the", "to"};
      std::vector<std::int32_t> positions = {0,1};

      PhraseQuery q("foo_w", terms, positions);

      auto poolFree = testIndex.pool.rewindScopeGuard();
      Query::Context qContext(testIndex.pool, *testIndex.reader);
      auto* weight = q.createWeight(qContext);
      // put the scorer creation in a separate scope to test that it's OK to rewind the pool after we are done with a single scorer.
      {
        auto g = testIndex.pool.rewindScopeGuard();
        Query::Scorer* scorer = weight->createScorer(
                testIndex.pool, qContext.topReader.segments()[0]);
        testScores(scorer, {}, {});
      }
      {
        auto g = testIndex.pool.rewindScopeGuard();
        Query::Scorer* scorer = weight->createScorer(
                testIndex.pool, qContext.topReader.segments()[1]);
        testScores(scorer, {}, {});
      }
    }

    // test that reused terms are handled correctly (i.e. cached docsenum are cloned when needed)
    {
      std::vector<std::string_view> terms = {"to", "the"};
      std::vector<std::int32_t> positions = {0, 1};
      PhraseQuery phrase("foo_w", terms, positions);

      std::vector<Query*> queries = {&to, &the, &phrase};
      BooleanQuery q({}, queries, {}, {});

      auto poolFree = testIndex.pool.rewindScopeGuard();
      Query::Context qContext(testIndex.pool, *testIndex.reader);
      auto* weight = q.createWeight(qContext);
      // put the scorer creation in a separate scope to test that it's OK to rewind the pool after we are done with a single scorer.
      {
        auto g = testIndex.pool.rewindScopeGuard();
        Query::Scorer* scorer = weight->createScorer(
                testIndex.pool, qContext.topReader.segments()[0]);
        testScores(scorer, {1}, {0.17332031f});
      }
      {
        auto g = testIndex.pool.rewindScopeGuard();
        Query::Scorer* scorer = weight->createScorer(
                testIndex.pool, qContext.topReader.segments()[1]);
        testScores(scorer, {2, 4}, {0.86280835f, 1.1237532f});
      }
    }

  }
}
