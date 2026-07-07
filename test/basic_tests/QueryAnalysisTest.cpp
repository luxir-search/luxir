#include <gtest/gtest.h>

#include <cstddef>

#include "test/SoluxTest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/QueryBuild.h"
#include "solux/util/StrRef.h"

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

  static std::vector<std::byte> bytes(std::string_view text) {
    return std::vector<std::byte>((const std::byte*)text.data(),
                                  (const std::byte*)text.data() + text.size());
  }

  QueryAnalysisTest() {
    helper.clear();
    helper.index(flatdoc("id", "d1", "body_wl", "Welcome Thomas Anderson here",
                         "body_w", "Welcome Thomas Anderson here"), UpdateMessage::NO_COMMIT);
    helper.index(flatdoc("id", "d2", "body_wl", "Anderson met Thomas",
                         "body_w", "Anderson met Thomas"), UpdateMessage::COMMIT);
  }

  // Run a phrase-over-text query and return the total match count.
  int64_t phraseTextCount(std::string_view field, std::string_view text) {
    auto req = localReq(helper.getSearchEngine());
    req->collection("main").topDocs("q").phraseText(field, text).withStats();
    req->execute();
    return req->getMatchCount();
  }

  // Run a match query (default OR) and return the total match count.
  int64_t matchCount(std::string_view field, std::string_view value) {
    auto req = localReq(helper.getSearchEngine());
    req->collection("main").topDocs("q").matchQuery(field, value).withStats();
    req->execute();
    return req->getMatchCount();
  }

  // Run a match query with min_match set, return the total match count.
  int64_t matchMinMatchCount(std::string_view field, std::string_view value, int minMatch) {
    auto req = localReq(helper.getSearchEngine());
    auto& cur = req->collection("main").topDocs("q").matchQuery(field, value);
    std::get<solux::api::Match>(cur.rawQuery().kind).min_match = minMatch;
    cur.withStats();
    req->execute();
    return req->getMatchCount();
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
  auto req = localReq(helper.getSearchEngine());
  req->collection("main").topDocs("q").phraseQuery("body_wl", {"THOMAS", "ANDERSON"}).withStats();
  req->execute();
  EXPECT_EQ(1, req->getMatchCount());
}

TEST_F(QueryAnalysisTest, wordListEntryFlattensToMultiplePositions) {
  // A single words[] entry that the analyzer splits into several tokens expands
  // to several phrase positions (flatten, like text). "Thomas Anderson" as one
  // entry -> [thomas, anderson].
  auto req = localReq(helper.getSearchEngine());
  req->collection("main").topDocs("q").phraseQuery("body_wl", {"Thomas Anderson"}).withStats();
  req->execute();
  EXPECT_EQ(1, req->getMatchCount());
}

TEST_F(QueryAnalysisTest, wordListWithPositionsAdjustsForExpansion) {
  // words + positions: one position per word. "Thomas Anderson" is one word
  // entry that analyzes to two tokens, so it consumes an extra position and the
  // following word ("here", given position 1) is shifted to 2. The phrase then
  // matches d1's "... Thomas Anderson here" (relative deltas 0,1,2). Without
  // the shift, "here" would collide with "anderson" and never match.
  auto req = localReq(helper.getSearchEngine());
  auto& cur = req->collection("main").topDocs("q");
  auto& ph = cur.rawQuery().kind.emplace<solux::api::PhraseQuery>();
  ph.field = "body_wl";
  auto& mr = cur.mr();
  std::string_view* w = build::allocArray(ph.words, 2, mr);
  w[0] = build::arenaStr(mr, "Thomas Anderson");
  w[1] = build::arenaStr(mr, "here");
  std::int32_t* pos = build::allocArray(ph.positions, 2, mr);
  pos[0] = 0;
  pos[1] = 1;
  cur.withStats();
  req->execute();
  EXPECT_EQ(1, req->getMatchCount());
}

TEST_F(QueryAnalysisTest, preAnalyzedTermsUsedVerbatim) {
  // terms[] are already analyzed: they must match the index bytes exactly.
  {
    auto req = localReq(helper.getSearchEngine());
    req->collection("main").topDocs("q").phraseTerms("body_wl", {"thomas", "anderson"}).withStats();
    req->execute();
    EXPECT_EQ(1, req->getMatchCount());  // already folded -> matches
  }
  {
    auto req = localReq(helper.getSearchEngine());
    req->collection("main").topDocs("q").phraseTerms("body_wl", {"Thomas", "Anderson"}).withStats();
    req->execute();
    EXPECT_EQ(0, req->getMatchCount());  // NOT analyzed; uppercase misses folded index
  }
}

TEST_F(QueryAnalysisTest, preAnalyzedTermsBinUsedVerbatim) {
  // terms_bin is the binary equivalent of terms: already analyzed, verbatim.
  auto req = localReq(helper.getSearchEngine());
  auto& cur = req->collection("main").topDocs("q");
  auto& ph = cur.rawQuery().kind.emplace<solux::api::PhraseQuery>();
  ph.field = "body_wl";
  auto& mr = cur.mr();
  auto* tb = build::allocArray(ph.terms_bin, 2, mr);
  tb[0] = build::arenaBytes(mr, bytes("thomas"));
  tb[1] = build::arenaBytes(mr, bytes("anderson"));
  cur.withStats();
  req->execute();
  EXPECT_EQ(1, req->getMatchCount());
}

TEST_F(QueryAnalysisTest, multiplePhraseInputsRejected) {
  // Only one of text / words / terms / terms_bin may be set.
  auto req = localReq(helper.getSearchEngine());
  auto& cur = req->collection("main").topDocs("q");
  auto& ph = cur.rawQuery().kind.emplace<solux::api::PhraseQuery>();
  ph.field = "body_wl";
  ph.text = "Thomas Anderson";
  auto& mr = cur.mr();
  std::string_view* w = build::allocArray(ph.words, 1, mr);
  w[0] = build::arenaStr(mr, "here");
  cur.withStats();
  req->execute();
  ASSERT_FALSE(req->responses.empty());
  EXPECT_TRUE(hasError(req->responses[0]->proto));
}

TEST_F(QueryAnalysisTest, positionsWithoutTermsRejected) {
  // positions with no phrase input is a malformed request.
  auto req = localReq(helper.getSearchEngine());
  auto& cur = req->collection("main").topDocs("q");
  auto& ph = cur.rawQuery().kind.emplace<solux::api::PhraseQuery>();
  ph.field = "body_wl";
  auto& mr = cur.mr();
  std::int32_t* pos = build::allocArray(ph.positions, 2, mr);
  pos[0] = 0;
  pos[1] = 1;
  cur.withStats();
  req->execute();
  ASSERT_FALSE(req->responses.empty());
  EXPECT_TRUE(hasError(req->responses[0]->proto));
}

TEST_F(QueryAnalysisTest, caseSensitiveFieldRespectsCase) {
  // body_w is whitespace-only, case-sensitive: query analysis leaves bytes alone.
  EXPECT_EQ(1, phraseTextCount("body_w", "Thomas Anderson"));   // exact case matches
  EXPECT_EQ(0, phraseTextCount("body_w", "thomas anderson"));   // wrong case misses
}

// --- max term length (indexed terms truncate to PackedTerm::MAX_LEN) ----------

TEST_F(QueryAnalysisTest, oversizedTextTokenTruncatesConsistently) {
  std::string longTok(PackedTerm::MAX_LEN + 17, 'x');
  helper.index(flatdoc("id", "d3", "body_w", "before " + longTok + " after"), UpdateMessage::COMMIT);
  // Index and query time truncate identically, so the full oversized token
  // matches, and the indexed term is exactly the MAX_LEN-byte prefix.
  EXPECT_EQ(1, matchCount("body_w", longTok));
  EXPECT_EQ(1, matchCount("body_w", longTok.substr(0, PackedTerm::MAX_LEN)));
  EXPECT_EQ(0, matchCount("body_w", longTok.substr(PackedTerm::MAX_LEN)));  // the cut tail is not a term
  // Truncation keeps one token per token: phrase positions stay adjacent.
  EXPECT_EQ(1, phraseTextCount("body_w", "before " + longTok + " after"));
}

TEST_F(QueryAnalysisTest, oversizedStringValueTruncatesConsistently) {
  std::string longVal(PackedTerm::MAX_LEN + 33, 'y');
  helper.index(flatdoc("id", "d3", "tag_s", longVal), UpdateMessage::COMMIT);
  EXPECT_EQ(1, matchCount("tag_s", longVal));
  // Values sharing their first MAX_LEN bytes are the same term (accepted
  // truncation semantics); a shorter value is a different term.
  EXPECT_EQ(1, matchCount("tag_s", longVal + "zzz"));
  EXPECT_EQ(0, matchCount("tag_s", longVal.substr(0, PackedTerm::MAX_LEN - 1)));
}

TEST_F(QueryAnalysisTest, oversizedIdTruncatesConsistently) {
  std::string longId(PackedTerm::MAX_LEN + 9, 'i');
  helper.index(flatdoc("id", longId, "body_w", "first"), UpdateMessage::NO_COMMIT, true /*overwrite*/);
  helper.index(flatdoc("id", longId, "body_w", "second"), UpdateMessage::COMMIT, true /*overwrite*/);
  // The same oversized id overwrites, it does not duplicate.
  EXPECT_EQ(0, matchCount("body_w", "first"));
  EXPECT_EQ(1, matchCount("body_w", "second"));
  // Delete-by-id truncates the same way and finds the doc.
  helper.deleteById(longId, UpdateMessage::COMMIT);
  EXPECT_EQ(0, matchCount("body_w", "second"));
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
  auto req = localReq(helper.getSearchEngine());
  req->collection("main").topDocs("q").matchQuery("body_wl", "Thomas here",
                                     solux::api::Match_::Operator::AND).withStats();
  req->execute();
  EXPECT_EQ(1, req->getMatchCount());
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

TEST_F(QueryAnalysisTest, matchMinShouldMatch) {
  // d1: "welcome thomas anderson here"; d2: "anderson met thomas".
  // "welcome here met" -> d1 has {welcome,here}=2, d2 has {met}=1.
  EXPECT_EQ(2, matchCount("body_wl", "welcome here met"));             // OR baseline
  EXPECT_EQ(1, matchMinMatchCount("body_wl", "welcome here met", 2));  // >=2 -> only d1
  EXPECT_EQ(0, matchMinMatchCount("body_wl", "welcome here met", 3));  // all three -> none
  EXPECT_EQ(0, matchMinMatchCount("body_wl", "welcome here met", 10)); // clamped to 3 -> none
}

TEST_F(QueryAnalysisTest, matchMinShouldMatchMissingTermLowersCeiling) {
  // "zzz" exists nowhere, so only 2 of the 3 terms can ever match.
  EXPECT_EQ(2, matchMinMatchCount("body_wl", "thomas anderson zzz", 2));  // both real terms present
  EXPECT_EQ(0, matchMinMatchCount("body_wl", "thomas anderson zzz", 3));  // can't reach 3 -> none
}

TEST_F(QueryAnalysisTest, booleanMinMatchOptionalClauses) {
  // The same min-should-match scorer, reached through parseBoolean.
  auto req = localReq(helper.getSearchEngine());
  auto& cur = req->collection("main").topDocs("q");
  auto& mr = cur.mr();
  cur.rawQuery() = qb::boolean(mr, /*required=*/{},
      /*optional=*/{qb::match(mr, "body_wl", "welcome"),
                    qb::match(mr, "body_wl", "here"),
                    qb::match(mr, "body_wl", "met")},
      /*prohibited=*/{}, /*filter=*/{}, /*minMatch=*/2);
  cur.withStats();
  req->execute();
  EXPECT_EQ(1, req->getMatchCount());  // only d1 has >= 2 of welcome/here/met
}

TEST_F(QueryAnalysisTest, booleanMinMatchComposesWithRequired) {
  // min_match >= 1 makes the optional group a constraint alongside required
  // clauses; unset means the optionals only rank.
  auto run = [&](int minMatch) {
    auto req = localReq(helper.getSearchEngine());
    auto& cur = req->collection("main").topDocs("q");
    auto& mr = cur.mr();
    cur.rawQuery() = qb::boolean(mr, /*required=*/{qb::match(mr, "body_wl", "anderson")},
        /*optional=*/{qb::match(mr, "body_wl", "thomas"), qb::match(mr, "body_wl", "welcome")},
        /*prohibited=*/{}, /*filter=*/{}, minMatch);
    cur.withStats();
    req->execute();
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    return req->getMatchCount();
  };
  EXPECT_EQ(2, run(0));  // both docs have anderson; optionals rank only
  EXPECT_EQ(2, run(1));  // both also have thomas
  EXPECT_EQ(1, run(2));  // only d1 has thomas AND welcome
}

TEST_F(QueryAnalysisTest, matchMinShouldMatchManyTerms) {
  // Terms with distinct doc frequencies (a:4, b:3, c:2, d:1) exercise the
  // cost-ordered lead/tail split across several thresholds.
  helper.clear();
  helper.index(flatdoc("id", "d1", "body_wl", "a b c d"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id", "d2", "body_wl", "a b c"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id", "d3", "body_wl", "a b"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id", "d4", "body_wl", "a"), UpdateMessage::COMMIT);

  EXPECT_EQ(4, matchMinMatchCount("body_wl", "a b c d", 1));  // OR -> all have 'a'
  EXPECT_EQ(3, matchMinMatchCount("body_wl", "a b c d", 2));  // d1,d2,d3
  EXPECT_EQ(2, matchMinMatchCount("body_wl", "a b c d", 3));  // d1,d2
  EXPECT_EQ(1, matchMinMatchCount("body_wl", "a b c d", 4));  // d1 (conjunction)
}

TEST_F(QueryAnalysisTest, matchMinShouldMatchScoreIncludesAllMatches) {
  // A doc that matches all three terms must score identically under OR and
  // under min_match=2: the scorer stops iterating at minMatch but score() has
  // to complete the tail so the BM25 sum covers every matching term.
  helper.clear();
  helper.index(flatdoc("id", "x", "body_wl", "alpha beta gamma"), UpdateMessage::COMMIT);

  auto score = [&](int minMatch) {
    auto req = localReq(helper.getSearchEngine());
    auto& cur = req->collection("main").topDocs("q").matchQuery("body_wl", "alpha beta gamma");
    if (minMatch > 0) std::get<solux::api::Match>(cur.rawQuery().kind).min_match = minMatch;
    cur.withStats();
    req->execute();
    EXPECT_EQ(1, req->getMatchCount());
    const auto* dl = req->docList("q");
    const auto& scores = std::get<solux::api::ColFloat>(dl->columns.at("_score_").kind).v;
    float s = scores[0];
    return s;
  };

  EXPECT_FLOAT_EQ(score(0), score(2));  // OR vs min-should-match, same matched set
}
