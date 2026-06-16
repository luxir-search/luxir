#include <gtest/gtest.h>

#include "test/SoluxTest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"

using namespace solux;
using namespace solux::test;

// Exercises query-time analysis for phrase queries (QueryBuilder), reached
// through the protobuf parser. The index side and the query side must run the
// same analyzer for an analyzed field, so an uppercase query has to match a
// case-folded index. body_wl is unicode_word + nfkc_cf (case folding); body_w
// is whitespace, case- and accent-sensitive (no analysis changes the bytes).
class QueryAnalysisTest : public SoluxTest {
public:
  CollectionHelper helper;

  QueryAnalysisTest() {
    helper.clear();
    helper.index(flatdoc("id", "d1", "body_wl", "Welcome Thomas Anderson here",
                         "body_w", "Welcome Thomas Anderson here"), UpdateMessage::NO_COMMIT);
    helper.index(flatdoc("id", "d2", "body_wl", "Anderson met Thomas",
                         "body_w", "Anderson met Thomas"), UpdateMessage::COMMIT);
  }

  // Run a phrase-over-text query and return the total match count.
  int64_t phraseTextCount(std::string_view field, std::string_view text) {
    auto* req = LocalReq::create(helper.getSearchEngine());
    req->collection("main").phraseText(field, text).withStats().execute();
    int64_t n = req->getMatchCount();
    req->done();
    return n;
  }
};

TEST_F(QueryAnalysisTest, textPhraseAnalyzedAndCaseFolded) {
  // Uppercase query text against a case-folded field still matches.
  EXPECT_EQ(1, phraseTextCount("body_wl", "Thomas Anderson"));
  EXPECT_EQ(1, phraseTextCount("body_wl", "THOMAS ANDERSON"));
}

TEST_F(QueryAnalysisTest, textPhraseOrderMatters) {
  // Reversed order is not an adjacent phrase in either doc.
  EXPECT_EQ(0, phraseTextCount("body_wl", "Anderson Thomas"));
}

TEST_F(QueryAnalysisTest, singleTokenTextCollapsesToTermQuery) {
  // One analyzed token => TermQuery (PhraseQuery needs >= 2). Matches both docs.
  EXPECT_EQ(2, phraseTextCount("body_wl", "ANDERSON"));
}

TEST_F(QueryAnalysisTest, emptyAnalysisMatchesNothing) {
  // Non-empty input that analyzes to zero tokens => MatchNoDocsQuery, no crash.
  EXPECT_EQ(0, phraseTextCount("body_wl", "   "));
  EXPECT_EQ(0, phraseTextCount("body_wl", " ... "));
}

TEST_F(QueryAnalysisTest, wordListAnalyzed) {
  auto* req = LocalReq::create(helper.getSearchEngine());
  req->collection("main").phraseQuery("body_wl", {"THOMAS", "ANDERSON"}).withStats().execute();
  EXPECT_EQ(1, req->getMatchCount());
  req->done();
}

TEST_F(QueryAnalysisTest, wordListEntryFlattensToMultiplePositions) {
  // A single words[] entry that the analyzer splits into several tokens expands
  // to several phrase positions (flatten, like text). "Thomas Anderson" as one
  // entry -> [thomas, anderson].
  auto* req = LocalReq::create(helper.getSearchEngine());
  req->collection("main").phraseQuery("body_wl", {"Thomas Anderson"}).withStats().execute();
  EXPECT_EQ(1, req->getMatchCount());
  req->done();
}

TEST_F(QueryAnalysisTest, wordListWithPositionsAdjustsForExpansion) {
  // words + positions: one position per word. "Thomas Anderson" is one word
  // entry that analyzes to two tokens, so it consumes an extra position and the
  // following word ("here", given position 1) is shifted to 2. The phrase then
  // matches d1's "... Thomas Anderson here" (relative deltas 0,1,2). Without
  // the shift, "here" would collide with "anderson" and never match.
  auto* req = LocalReq::create(helper.getSearchEngine());
  auto& ph = *req->topDocs("q").mutable_query()->mutable_phrase();
  ph.set_field("body_wl");
  ph.add_words("Thomas Anderson");
  ph.add_words("here");
  ph.add_positions(0);
  ph.add_positions(1);
  req->collection("main").withStats().execute();
  EXPECT_EQ(1, req->getMatchCount());
  req->done();
}

TEST_F(QueryAnalysisTest, preAnalyzedTermsUsedVerbatim) {
  // terms[] are already analyzed: they must match the index bytes exactly.
  {
    auto* req = LocalReq::create(helper.getSearchEngine());
    req->collection("main").phraseTerms("body_wl", {"thomas", "anderson"}).withStats().execute();
    EXPECT_EQ(1, req->getMatchCount());  // already folded -> matches
    req->done();
  }
  {
    auto* req = LocalReq::create(helper.getSearchEngine());
    req->collection("main").phraseTerms("body_wl", {"Thomas", "Anderson"}).withStats().execute();
    EXPECT_EQ(0, req->getMatchCount());  // NOT analyzed; uppercase misses folded index
    req->done();
  }
}

TEST_F(QueryAnalysisTest, caseSensitiveFieldRespectsCase) {
  // body_w is whitespace-only, case-sensitive: query analysis leaves bytes alone.
  EXPECT_EQ(1, phraseTextCount("body_w", "Thomas Anderson"));   // exact case matches
  EXPECT_EQ(0, phraseTextCount("body_w", "thomas anderson"));   // wrong case misses
}
