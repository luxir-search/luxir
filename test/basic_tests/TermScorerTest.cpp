#include <solux/query/AllQuery.h>
#include <cmath>
#include "gtest/gtest.h"
#include "test/SoluxTest.h"
#include "test/TestIndex.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "solux/query/TermQuery.h"
#include "solux/query/PhraseQuery.h"
#include "solux/query/BooleanQuery.h"
#include "solux/search/Collector.h"


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

struct DisjunctionTopKRun {
  int64_t visited = 0;
  int64_t nonEssentialLookups = 0;
  std::vector<TopDocsCollector::ScoreDoc> topDocs;
};

std::vector<TopDocsCollector::ScoreDoc> sortedCollectorDocs(TopDocsCollector& collector) {
  auto docs = collector.sort();
  std::vector<TopDocsCollector::ScoreDoc> out(docs.begin(), docs.end());
  std::sort(out.begin(), out.end(), TopDocsCollector::scoreAndDocComp);
  return out;
}

void addMaxScoreDisjunctionDocs(CollectionHelper& helper) {
  const int32_t segDocs = Postings::DOCS_BLOCK_SIZE + 40;
  const int32_t segCount = 3;
  helper.clear();

  for (int32_t seg = 0; seg < segCount; seg++) {
    std::vector<Doc> docs;
    docs.reserve(segDocs);
    for (int32_t local = 0; local < segDocs; local++) {
      int32_t doc = seg * segDocs + local;
      std::string body = "common";
      if (doc < 6) {
        // Strictly decreasing rare tf (doc 0 has the most) with constant length:
        // doc 0 alone maximizes the rare clause, so the top-k threshold stays
        // strictly below the sum of clause maxima.  That keeps the rare clause
        // essential while the near-zero-idf common clause is demoted, so the
        // non-essential lookup path is actually exercised (partial demotion).
        int32_t rareTf = 8 - doc;
        for (int32_t i = 0; i < rareTf; i++) body += " rare";
        for (int32_t i = rareTf; i < 8; i++) body += " pad";
        body += " medium";
      } else {
        if ((doc % 9) == 0) body += " medium";
        for (int32_t i = 0; i < 60; i++) body += " filler";
      }
      docs.push_back(flatdoc("id", "d" + std::to_string(doc), "body_w", body));
    }
    helper.indexAll(docs, UpdateMessage::COMMIT);
  }
}

DisjunctionTopKRun runMaxScoreDisjunctionTopK(IndexReader& reader, int32_t topK) {
  MemPool pool;
  Query::Context qContext(pool, reader);
  TermQuery common("body_w", "common");
  TermQuery medium("body_w", "medium");
  TermQuery rare("body_w", "rare");
  std::vector<Query*> optional = {&common, &medium, &rare};
  BooleanQuery query({}, optional, {}, {});
  auto* weight = query.createWeight(qContext, Query::NEED_SCORES);
  TopDocsCollector collector(topK);
  DisjunctionTopKRun result;

  auto segments = qContext.topReader.segments();
  for (int32_t segnum = 0; segnum < (int32_t) segments.size(); segnum++) {
    auto* scorer = weight->createScorer(pool, segments[segnum]);
    if (scorer == nullptr) continue;
    scorer->setMinCompetitiveScore(collector.minCompetitiveVal);
    collectTopK(segnum, scorer, nullptr, nullptr, collector);
    if (auto* maxScore = dynamic_cast<BooleanQuery::MaxScoreDisjunctionScorer*>(scorer)) {
      result.nonEssentialLookups += maxScore->nonEssentialLookupCount();
    }
  }

  result.visited = collector.totalHits();
  result.topDocs = sortedCollectorDocs(collector);
  return result;
}

DisjunctionTopKRun runExhaustiveDisjunctionTopK(IndexReader& reader, int32_t topK) {
  MemPool pool;
  Query::Context qContext(pool, reader);
  TermQuery common("body_w", "common");
  TermQuery medium("body_w", "medium");
  TermQuery rare("body_w", "rare");
  std::array<Query::Weight*, 3> weights = {
    common.createWeight(qContext, Query::NEED_SCORES),
    medium.createWeight(qContext, Query::NEED_SCORES),
    rare.createWeight(qContext, Query::NEED_SCORES)
  };
  TopDocsCollector collector(topK);

  auto segments = qContext.topReader.segments();
  for (int32_t segnum = 0; segnum < (int32_t) segments.size(); segnum++) {
    auto* arr = pool.make_arr<Query::Scorer*>(weights.size());
    int32_t count = 0;
    for (auto* weight : weights) {
      auto* scorer = weight->createScorer(pool, segments[segnum]);
      if (scorer != nullptr) arr[count++] = scorer;
    }
    if (count == 0) continue;
    Query::Scorer* scorer = count == 1
      ? arr[0]
      : pool.make<BooleanQuery::DisjunctionScorer>(
          pool, std::span<Query::Scorer*>(arr, (size_t) count));
    collectTopK(segnum, scorer, nullptr, nullptr, collector, false);
  }

  DisjunctionTopKRun result;
  result.visited = collector.totalHits();
  result.topDocs = sortedCollectorDocs(collector);
  return result;
}

void assertSameTopKDocs(const DisjunctionTopKRun& expected, const DisjunctionTopKRun& actual, int32_t topK) {
  ASSERT_EQ(actual.topDocs.size(), expected.topDocs.size()) << "k=" << topK;
  for (size_t i = 0; i < expected.topDocs.size(); i++) {
    EXPECT_EQ(actual.topDocs[i].doc, expected.topDocs[i].doc) << "k=" << topK << " i=" << i;
    EXPECT_FLOAT_EQ(actual.topDocs[i].score, expected.topDocs[i].score) << "k=" << topK << " i=" << i;
  }
}


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
    fieldStats.maxDoc = f.currentSegment()->postingsReader().maxDoc();

    Similarity::TermStats termStats;
    termStats.docFreq = denum.numDocs();
    termStats.totalTermFreq = denum.totalTermFreq();

    Similarity sim;
    auto simScorer = sim.getScorer(1.0, fieldStats, termStats);

    IntColReader& normsCol = *f.colReader;

    // lucene scores the docs as follows:
    // doc=5 score=0.36330473
    // doc=7 score=0.37098017
    TermQuery::Scorer termScorer(denum, &normsCol, &simScorer);
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

      auto* weight = tq.createWeight(qContext, Query::NEED_SCORES);
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

      auto* weight = tq.createWeight(qContext, Query::NEED_SCORES);
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

      auto* weight = tq.createWeight(qContext, Query::NEED_SCORES);
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
      auto* weight = allQuery.createWeight(qContext, Query::NEED_SCORES);
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
      auto* weight = q.createWeight(qContext, Query::NEED_SCORES);
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
      auto* weight = q.createWeight(qContext, Query::NEED_SCORES);
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
      auto* weight = q.createWeight(qContext, Query::NEED_SCORES);
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
      auto* weight = q.createWeight(qContext, Query::NEED_SCORES);
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
      auto* weight = q.createWeight(qContext, Query::NEED_SCORES);
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
      auto* weight = q.createWeight(qContext, Query::NEED_SCORES);
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
      auto* weight = q.createWeight(qContext, Query::NEED_SCORES);
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
      auto* weight = q.createWeight(qContext, Query::NEED_SCORES);
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
      auto* weight = phrase.createWeight(qContext, Query::NEED_SCORES);
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
      auto* weight = q.createWeight(qContext, Query::NEED_SCORES);
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
      auto* weight = q.createWeight(qContext, Query::NEED_SCORES);
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


// Regression for the norm-encoding fix (impact-scoring.md "Step 0"): the field-length
// column stores the SmallFloat-encoded norm byte, not the raw token count. Before the
// fix a doc over 40 tokens was mis-scored, and a doc over 255 tokens wrapped mod 256
// (e.g. 256 -> byte 0 -> "length 0", the shortest-doc bonus), so a very long doc could
// outscore a short one. With the same tf, the BM25 score must be monotone non-increasing
// in length, with no wrap across the 40- and 255-token boundaries.
TEST_F(TermScorerTest, normEncodingMonotone) {
  TestIndex testIndex;
  TestField f(testIndex, "foo_w");
  f.startIndexing();

  // Each doc has "needle" exactly once (tf=1) plus filler to hit a target token
  // count straddling the encoding boundaries. docids ascend with length, so the
  // scorer (docid order) yields scores that must descend.
  std::vector<int> lengths = {5, 40, 41, 100, 255, 256, 300, 600};
  std::vector<std::string> docs;
  for (int len : lengths) {
    std::string s = "needle";
    for (int i = 1; i < len; i++) s += " fill";
    docs.push_back(std::move(s));
  }
  for (size_t i = 0; i < docs.size(); i++) {
    f.add((int32_t)i, docs[i]);
  }
  testIndex.flush();
  f.startReading();

  TermQuery tq("foo_w", "needle");
  auto poolFree = testIndex.pool.rewindScopeGuard();
  Query::Context qContext(testIndex.pool, *testIndex.reader);
  auto* weight = tq.createWeight(qContext, Query::NEED_SCORES);
  auto* scorer = dynamic_cast<TermQuery::Scorer*>(
      weight->createScorer(testIndex.pool, qContext.topReader.segments()[0]));
  ASSERT_NE(scorer, nullptr);

  std::vector<float> scores;
  for (int32_t doc = scorer->next(); doc != PostingsReader::END; doc = scorer->next()) {
    ASSERT_EQ(scorer->termFreq(), 1);
    scores.push_back(scorer->score());
  }
  ASSERT_EQ(scores.size(), lengths.size());

  // Monotone non-increasing across every boundary, including 40/41 and 255/256.
  // (Adjacent lengths in the same quantization bucket score equal, hence <=.)
  for (size_t i = 1; i < scores.size(); i++) {
    EXPECT_LE(scores[i], scores[i - 1])
        << "length " << lengths[i] << " outscored length " << lengths[i - 1];
  }
  // The length effect is real (quantization didn't collapse it): the longest doc
  // scores strictly below the shortest. The pre-fix 256-token wrap inverted this.
  EXPECT_LT(scores.back(), scores.front());
}

TEST_F(TermScorerTest, termImpactMaxScoreBounds) {
  const int32_t N = 5 * Postings::DOCS_BLOCK_SIZE + 17;
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();

  for (int32_t doc = 0; doc < N; doc++) {
    int32_t tf = 1 + ((doc / Postings::DOCS_BLOCK_SIZE) * 3 + (doc % 5)) % 17;
    int32_t len = 4 + ((doc * 11) % 90);
    if (len < tf) {
      len = tf;
    }
    std::string text;
    for (int32_t i = 0; i < tf; i++) {
      text += "impact ";
    }
    for (int32_t i = tf; i < len; i++) {
      text += "filler ";
    }
    if (doc == 7) {
      text += "rare ";
    }
    f.add(doc, text);
  }
  testIndex.flush();
  f.startReading();

  auto poolFree = testIndex.pool.rewindScopeGuard();
  Query::Context qContext(testIndex.pool, *testIndex.reader);
  TermQuery impactForBounds("body_w", "impact");
  TermQuery impactForActuals("body_w", "impact");
  auto* boundWeight = impactForBounds.createWeight(qContext, Query::NEED_SCORES);
  auto* actualWeight = impactForActuals.createWeight(qContext, Query::NEED_SCORES);
  auto& segment = qContext.topReader.segments()[0];

  auto* actualScorer = dynamic_cast<TermQuery::Scorer*>(
      actualWeight->createScorer(testIndex.pool, segment));
  ASSERT_NE(actualScorer, nullptr);
  std::vector<std::pair<int32_t, float>> actualScores;
  for (int32_t doc = actualScorer->next(); doc != PostingsReader::END; doc = actualScorer->next()) {
    actualScores.push_back({doc, actualScorer->score()});
  }
  ASSERT_EQ((int32_t) actualScores.size(), N);

  auto actualMax = [&](int32_t current, int32_t upTo) {
    float maxScore = 0.0f;
    for (auto [doc, score] : actualScores) {
      if (doc >= current && doc <= upTo) {
        maxScore = std::max(maxScore, score);
      }
    }
    return maxScore;
  };

  auto* scorer = dynamic_cast<TermQuery::Scorer*>(
      boundWeight->createScorer(testIndex.pool, segment));
  ASSERT_NE(scorer, nullptr);

  auto assertBound = [&](int32_t upTo) {
    float bound = scorer->getMaxScore(upTo);
    ASSERT_TRUE(std::isfinite(bound)) << "upTo=" << upTo << " doc=" << scorer->docId();
    EXPECT_GE(bound + 1e-6f, actualMax(scorer->docId(), upTo))
        << "upTo=" << upTo << " doc=" << scorer->docId();
  };

  for (int32_t upTo : {0, 17, 127, 128, 255, 400, N - 1, PostingsReader::END}) {
    assertBound(upTo);
  }

  ASSERT_EQ(scorer->advance(200), 200);
  for (int32_t upTo : {200, 255, 511, N - 1, PostingsReader::END}) {
    assertBound(upTo);
  }

  ASSERT_EQ(scorer->advance(600), 600);
  for (int32_t upTo : {600, N - 1, PostingsReader::END}) {
    assertBound(upTo);
  }

  auto* shallowScorer = dynamic_cast<TermQuery::Scorer*>(
      boundWeight->createScorer(testIndex.pool, segment));
  ASSERT_NE(shallowScorer, nullptr);
  ASSERT_EQ(shallowScorer->docId(), -1);
  auto expectedBlockLastDoc = [&](int32_t target) {
    if (target >= N) {
      return PostingsReader::END;
    }
    int32_t block = target / Postings::DOCS_BLOCK_SIZE;
    return std::min(N - 1, (block + 1) * Postings::DOCS_BLOCK_SIZE - 1);
  };
  for (int32_t target : {0, 1, 127, 128, 129, 3 * Postings::DOCS_BLOCK_SIZE + 5, N - 1}) {
    EXPECT_EQ(shallowScorer->advanceShallow(target), expectedBlockLastDoc(target)) << target;
    EXPECT_EQ(shallowScorer->docId(), -1);
  }
  EXPECT_EQ(shallowScorer->advanceShallow(N + 10), PostingsReader::END);
  EXPECT_EQ(shallowScorer->docId(), -1);

  TermQuery rareQuery("body_w", "rare");
  auto* rareWeight = rareQuery.createWeight(qContext, Query::NEED_SCORES);
  auto* rareScorer = dynamic_cast<TermQuery::Scorer*>(
      rareWeight->createScorer(testIndex.pool, segment));
  ASSERT_NE(rareScorer, nullptr);
  EXPECT_TRUE(std::isinf(rareScorer->getMaxScore(PostingsReader::END)));
  EXPECT_EQ(rareScorer->advanceShallow(0), PostingsReader::END);
}

TEST_F(TermScorerTest, termImpactTopKSkippingMatchesExhaustive) {
  const int32_t N = 6 * Postings::DOCS_BLOCK_SIZE + 17;
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();

  for (int32_t doc = 0; doc < N; doc++) {
    int32_t tf;
    int32_t len;
    if (doc < Postings::DOCS_BLOCK_SIZE) {
      tf = 24 + (doc % 29);
      len = tf + (doc % 11);
    } else {
      tf = 1;
      len = 180 + (doc % 37);
    }
    std::string text;
    for (int32_t i = 0; i < tf; i++) {
      text += "impactskip ";
    }
    for (int32_t i = tf; i < len; i++) {
      text += "filler ";
    }
    f.add(doc, text);
  }
  testIndex.flush();
  f.startReading();

  auto poolFree = testIndex.pool.rewindScopeGuard();
  Query::Context qContext(testIndex.pool, *testIndex.reader);
  auto& segment = qContext.topReader.segments()[0];

  for (int32_t k : {1, 5, 50}) {
    TermQuery exhaustiveQuery("body_w", "impactskip");
    TermQuery prunedQuery("body_w", "impactskip");
    auto* exhaustiveWeight = exhaustiveQuery.createWeight(qContext, Query::NEED_SCORES);
    auto* prunedWeight = prunedQuery.createWeight(qContext, Query::NEED_SCORES);

    auto* exhaustiveScorer = dynamic_cast<TermQuery::Scorer*>(
        exhaustiveWeight->createScorer(testIndex.pool, segment));
    ASSERT_NE(exhaustiveScorer, nullptr);
    TopDocsCollector exhaustiveCollector(k);
    for (int32_t doc = exhaustiveScorer->next(); doc != PostingsReader::END; doc = exhaustiveScorer->next()) {
      exhaustiveCollector.collect(0, doc, exhaustiveScorer->score());
    }
    ASSERT_EQ(exhaustiveCollector.totalHits(), N);

    auto* prunedScorer = dynamic_cast<TermQuery::Scorer*>(
        prunedWeight->createScorer(testIndex.pool, segment));
    ASSERT_NE(prunedScorer, nullptr);
    TopDocsCollector prunedCollector(k);
    collectTopK(0, prunedScorer, nullptr, nullptr, prunedCollector);

    auto expected = exhaustiveCollector.sort();
    auto actual = prunedCollector.sort();
    ASSERT_EQ(actual.size(), expected.size()) << "k=" << k;
    for (size_t i = 0; i < expected.size(); i++) {
      EXPECT_EQ(actual[i].doc, expected[i].doc) << "k=" << k << " i=" << i;
      EXPECT_FLOAT_EQ(actual[i].score, expected[i].score) << "k=" << k << " i=" << i;
    }
    EXPECT_LT(prunedCollector.totalHits(), exhaustiveCollector.totalHits()) << "k=" << k;
  }
}

TEST_F(TermScorerTest, maxScoreDisjunctionTopKMatchesExhaustive) {
  CollectionHelper helper("main");
  addMaxScoreDisjunctionDocs(helper);
  auto reader = helper.getIndexWriter()->getIndexReader();
  const int32_t totalDocs = 3 * (Postings::DOCS_BLOCK_SIZE + 40);

  for (int32_t k : {3, totalDocs + 10}) {
    auto expected = runExhaustiveDisjunctionTopK(*reader, k);
    auto actual = runMaxScoreDisjunctionTopK(*reader, k);
    assertSameTopKDocs(expected, actual, k);
    if (k == 3) {
      EXPECT_LT(actual.visited, expected.visited);
      EXPECT_GT(actual.nonEssentialLookups, 0);
    } else {
      EXPECT_EQ(actual.visited, expected.visited);
      EXPECT_EQ(actual.nonEssentialLookups, 0);
    }
  }
  helper.clear();
}

// Regression for the 1f bug: impact block skipping under-counts the total hit count
// (matches / get_number), since skipped docs are never visited.  collectTopK must keep
// pruning OFF (allowPruning=false) when an exact count is needed, so every match is
// visited; only then does totalHits() equal the true docfreq.
TEST_F(TermScorerTest, getNumberDisablesImpactSkipping) {
  const int32_t N = 6 * Postings::DOCS_BLOCK_SIZE + 17;  // multi-block common term
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();
  for (int32_t doc = 0; doc < N; doc++) {
    int32_t tf = 1 + ((doc / Postings::DOCS_BLOCK_SIZE) * 3 + (doc % 5)) % 17;
    int32_t len = 4 + ((doc * 11) % 90);
    if (len < tf) len = tf;
    std::string text;
    for (int32_t i = 0; i < tf; i++) text += "needle ";
    for (int32_t i = tf; i < len; i++) text += "filler ";
    f.add(doc, text);
  }
  testIndex.flush();
  f.startReading();

  auto poolFree = testIndex.pool.rewindScopeGuard();
  Query::Context qContext(testIndex.pool, *testIndex.reader);
  auto& segment = qContext.topReader.segments()[0];
  const int32_t k = 5;  // small k: with pruning ON the threshold would rise and skip blocks.

  // allowPruning=false (the get_number path): pruning is disabled, so every match is
  // visited and totalHits() is the exact docfreq (== N).  "needle" is in every doc.
  // This is a valid guard: skipping DOES fire on this corpus when allowed, so a broken
  // gate (pruning despite allowPruning=false) would drop the count below N and fail here.
  TermQuery exactQuery("body_w", "needle");
  auto* exactWeight = exactQuery.createWeight(qContext, Query::NEED_SCORES);
  auto* exactScorer = dynamic_cast<TermQuery::Scorer*>(exactWeight->createScorer(testIndex.pool, segment));
  ASSERT_NE(exactScorer, nullptr);
  TopDocsCollector exactCollector(k);
  collectTopK(0, exactScorer, nullptr, nullptr, exactCollector, /*allowPruning=*/false);
  EXPECT_EQ(exactCollector.totalHits(), (int64_t) N);
}

// Regression: CachedTermInfo::useDocsEnum used to hand out the cached DocsEnum un-cloned
// when only one weight referenced the term (sharedCount==0).  Creating a SECOND weight
// for the same term (sharedCount->1) and a scorer AFTER the first scorer had already run
// then cloned the exhausted cached enum.  useDocsEnum now always clones, so interleaved
// createWeight / run / createWeight is safe.
TEST_F(TermScorerTest, interleavedScorersForSameTermAreIndependent) {
  const int32_t N = 3 * Postings::DOCS_BLOCK_SIZE + 7;  // multi-block, "needle" in every doc
  TestIndex testIndex;
  TestField f(testIndex, "body_w");
  f.startIndexing();
  for (int32_t doc = 0; doc < N; doc++) {
    f.add(doc, "needle filler filler");
  }
  testIndex.flush();
  f.startReading();

  auto poolFree = testIndex.pool.rewindScopeGuard();
  Query::Context qContext(testIndex.pool, *testIndex.reader);
  auto& segment = qContext.topReader.segments()[0];

  auto countAll = [&](const char* term) {
    TermQuery q("body_w", term);
    auto* w = q.createWeight(qContext, Query::NEED_SCORES);
    auto* s = dynamic_cast<TermQuery::Scorer*>(w->createScorer(testIndex.pool, segment));
    EXPECT_NE(s, nullptr);
    int64_t n = 0;
    for (int32_t doc = s->next(); doc != PostingsReader::END; doc = s->next()) {
      n++;
    }
    return n;
  };

  // First weight+scorer for "needle", fully consumed (its createWeight set sharedCount=0).
  EXPECT_EQ(countAll("needle"), (int64_t) N);
  // Second weight+scorer for the SAME term, created and run AFTER the first finished.
  // Pre-fix this cloned the exhausted cached enum and counted far fewer than N.
  EXPECT_EQ(countAll("needle"), (int64_t) N);
}
