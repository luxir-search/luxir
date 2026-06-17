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

  // Run a match query (default OR) and return the total match count.
  int64_t matchCount(std::string_view field, std::string_view value) {
    auto* req = LocalReq::create(helper.getSearchEngine());
    req->collection("main").matchQuery(field, value).withStats().execute();
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

TEST_F(QueryAnalysisTest, preAnalyzedTermsBinUsedVerbatim) {
  // terms_bin is the binary equivalent of terms: already analyzed, verbatim.
  auto* req = LocalReq::create(helper.getSearchEngine());
  auto& ph = *req->topDocs("q").mutable_query()->mutable_phrase();
  ph.set_field("body_wl");
  *ph.mutable_terms_bin()->Add() = "thomas";
  *ph.mutable_terms_bin()->Add() = "anderson";
  req->collection("main").withStats().execute();
  EXPECT_EQ(1, req->getMatchCount());
  req->done();
}

TEST_F(QueryAnalysisTest, multiplePhraseInputsRejected) {
  // Only one of text / words / terms / terms_bin may be set.
  auto* req = LocalReq::create(helper.getSearchEngine());
  auto& ph = *req->topDocs("q").mutable_query()->mutable_phrase();
  ph.set_field("body_wl");
  ph.set_text("Thomas Anderson");
  *ph.mutable_words()->Add() = "here";
  req->collection("main").withStats().execute();
  ASSERT_FALSE(req->responses.empty());
  EXPECT_TRUE(req->responses[0]->proto.has_error());
  req->done();
}

TEST_F(QueryAnalysisTest, positionsWithoutTermsRejected) {
  // positions with no phrase input is a malformed request.
  auto* req = LocalReq::create(helper.getSearchEngine());
  auto& ph = *req->topDocs("q").mutable_query()->mutable_phrase();
  ph.set_field("body_wl");
  ph.add_positions(0);
  ph.add_positions(1);
  req->collection("main").withStats().execute();
  ASSERT_FALSE(req->responses.empty());
  EXPECT_TRUE(req->responses[0]->proto.has_error());
  req->done();
}

TEST_F(QueryAnalysisTest, caseSensitiveFieldRespectsCase) {
  // body_w is whitespace-only, case-sensitive: query analysis leaves bytes alone.
  EXPECT_EQ(1, phraseTextCount("body_w", "Thomas Anderson"));   // exact case matches
  EXPECT_EQ(0, phraseTextCount("body_w", "thomas anderson"));   // wrong case misses
}

// --- match (parseMatch) query-time analysis -----------------------------------

TEST_F(QueryAnalysisTest, matchSingleTermCaseFolded) {
  // Uppercase match query against a case-folded field matches (the parity fix);
  // one analyzed term collapses to a TermQuery. d1 and d2 both contain anderson.
  EXPECT_EQ(2, matchCount("body_wl", "ANDERSON"));
}

TEST_F(QueryAnalysisTest, matchMultiTermDefaultsToOr) {
  // "Thomas here" -> [thomas, here]; default operator OR. d1 has both, d2 has
  // thomas only -> both match.
  EXPECT_EQ(2, matchCount("body_wl", "Thomas here"));
}

TEST_F(QueryAnalysisTest, matchAndRequiresAllTerms) {
  // Same terms with operator AND: only d1 contains both thomas and here.
  auto* req = LocalReq::create(helper.getSearchEngine());
  req->collection("main").matchQuery("body_wl", "Thomas here", proto::Match::AND).withStats().execute();
  EXPECT_EQ(1, req->getMatchCount());
  req->done();
}

TEST_F(QueryAnalysisTest, matchEmptyAnalyzesToNothing) {
  // Non-empty input that analyzes to zero tokens -> MatchNoDocsQuery.
  EXPECT_EQ(0, matchCount("body_wl", " ... "));
}

TEST_F(QueryAnalysisTest, matchOnNonTextFieldIsVerbatim) {
  // ID / STRING fields are matched verbatim (no analysis), so case matters.
  EXPECT_EQ(1, matchCount("id", "d1"));
  EXPECT_EQ(0, matchCount("id", "D1"));
}

TEST_F(QueryAnalysisTest, matchMinMatchRejected) {
  // min_match has wire presence but no scorer yet -> error, not silent OR.
  auto* req = LocalReq::create(helper.getSearchEngine());
  auto& m = *req->topDocs("q").mutable_query()->mutable_match();
  m.set_field("body_wl");
  m.mutable_val()->set_s("Thomas Anderson");
  m.set_min_match(2);
  req->collection("main").withStats().execute();
  ASSERT_FALSE(req->responses.empty());
  EXPECT_TRUE(req->responses[0]->proto.has_error());
  req->done();
}
