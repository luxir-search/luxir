// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <random>
#include <string>
#include <vector>

#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/QueryBuild.h"
#include "test/TestIndex.h"
#include "test/TestUtils.h"
#include "luxir/query/PhrasePrefixQuery.h"
#include "luxir/query/PhraseQuery.h"

using namespace luxir;
using namespace luxir::test;

class MatchPhrasePrefixTest : public LuxirTest {
public:
  CollectionHelper helper;

  MatchPhrasePrefixTest() {
    helper.index(flatdoc("id", "d1", "body_w", "quick brown fox"), UpdateMessage::NO_COMMIT);
    helper.index(flatdoc("id", "d2", "body_w", "quick brother jumps"), UpdateMessage::NO_COMMIT);
    helper.index(flatdoc("id", "d3", "body_w", "quick red fox"), UpdateMessage::NO_COMMIT);
    helper.index(flatdoc("id", "d4", "body_w", "slow brown fox"), UpdateMessage::NO_COMMIT);
    helper.index(flatdoc("id", "d5", "body_w", "quick and brown"), UpdateMessage::NO_COMMIT);
    helper.index(flatdoc("id", "d6", "body_w", "brazen quick"), UpdateMessage::NO_COMMIT);
    helper.index(flatdoc("id", "d7", "body_w", "quick braid"), UpdateMessage::NO_COMMIT);
    helper.index(flatdoc("id", "d8", "body_w", "quick brave"), UpdateMessage::COMMIT);
  }

  std::vector<std::string> ids(const api::Query& query) {
    auto req = localReq(helper.getSearchEngine());
    auto& cur = req->collection("main").topDocs("q");
    cur.rawQuery() = query;
    cur.fields({"id"}).limit(100);
    req->execute();
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    std::vector<std::string> out;
    for (const Doc& doc : req->getDocs()) out.push_back(std::get<std::string>(*find(doc, "id")));
    std::sort(out.begin(), out.end());
    return out;
  }

  std::vector<std::string> mpp(std::string_view text, int32_t slop = 0,
                               int32_t maxExpansions = 0) {
    std::pmr::monotonic_buffer_resource mr;
    return ids(qb::matchPhrasePrefix(mr, "body_w", text, slop, maxExpansions));
  }

  std::string error(std::string_view text, int32_t slop, int32_t maxExpansions) {
    auto req = localReq(helper.getSearchEngine());
    auto& cur = req->collection("main").topDocs("q");
    cur.rawQuery() = qb::matchPhrasePrefix(cur.mr(), "body_w", text, slop, maxExpansions);
    req->execute();
    EXPECT_FALSE(req->ok());
    return req->errorMsg();
  }
};

using V = std::vector<std::string>;

TEST_F(MatchPhrasePrefixTest, basicMatch) {
  EXPECT_EQ(mpp("quick bro"), (V{"d1", "d2"}));
  EXPECT_EQ(mpp("quick brown"), (V{"d1"}));  // a whole word is its own prefix
}

TEST_F(MatchPhrasePrefixTest, slop) {
  EXPECT_EQ(mpp("quick bro", 0), (V{"d1", "d2"}));
  EXPECT_EQ(mpp("quick bro", 1), (V{"d1", "d2", "d5"}));
}

TEST_F(MatchPhrasePrefixTest, maxExpansionsInTermOrder) {
  // "br" expands in term order: braid, brave, brazen, brother, brown.
  EXPECT_EQ(mpp("quick br", 0, 2), (V{"d7", "d8"}));
  EXPECT_EQ(mpp("quick br", 0, 3), (V{"d7", "d8"}));  // brazen only precedes quick
  EXPECT_EQ(mpp("quick br", 0, 5), (V{"d1", "d2", "d7", "d8"}));
  EXPECT_EQ(mpp("quick br"), (V{"d1", "d2", "d7", "d8"}));  // default 50
}

TEST_F(MatchPhrasePrefixTest, noMatch) {
  EXPECT_EQ(mpp("quick zz"), V{});
  EXPECT_EQ(mpp("zz"), V{});
  EXPECT_EQ(mpp("missing bro"), V{});
  EXPECT_EQ(mpp(""), V{});
}

TEST_F(MatchPhrasePrefixTest, singleToken) {
  EXPECT_EQ(mpp("bro"), (V{"d1", "d2", "d4", "d5"}));
  EXPECT_EQ(mpp("br", 0, 1), (V{"d7"}));
}

TEST_F(MatchPhrasePrefixTest, exprFunctionAndConjunction) {
  auto req = localReq(helper.getSearchEngine());
  req->collection("main").topDocs("q")
      .exprQuery("match_phrase_prefix('quick bro', field=body_w, slop=1)").withStats();
  req->execute();
  EXPECT_EQ(req->getMatchCount(), 3);

  std::pmr::monotonic_buffer_resource mr;
  auto prefix = qb::matchPhrasePrefix(mr, "body_w", "quick bro");
  auto fox = qb::match(mr, "body_w", "fox");
  EXPECT_EQ(ids(qb::boolean(mr, {prefix, fox})), (V{"d1"}));
}

TEST_F(MatchPhrasePrefixTest, rejectsNegativeArguments) {
  EXPECT_NE(error("quick bro", 0, -1).find("max_expansions"), std::string::npos);
  EXPECT_NE(error("quick bro", -1, 0).find("slop"), std::string::npos);
}

// Engine-level checks over explicit segments and scores.
class MatchPhrasePrefixEngineTest : public LuxirTest {
protected:
  struct Hit {
    int32_t doc;
    uint32_t scoreBits;
    bool operator==(const Hit&) const = default;
  };

  static std::vector<Hit> run(TestIndex& ti, Query& query, int segOrd = 0) {
    Query::Context ctx(ti.pool, *ti.reader);
    auto* weight = query.createWeight(ctx, Query::NEED_SCORES);
    auto* scorer = weight->createScorer(ti.pool, ctx.topReader.segments()[segOrd]);
    std::vector<Hit> hits;
    if (scorer == nullptr) return hits;
    for (int32_t doc = scorer->next(); doc != PostingsReader::END; doc = scorer->next()) {
      hits.push_back({doc, std::bit_cast<uint32_t>(scorer->score())});
    }
    return hits;
  }

  static std::vector<int32_t> docs(const std::vector<Hit>& hits) {
    std::vector<int32_t> out;
    for (const Hit& hit : hits) out.push_back(hit.doc);
    return out;
  }
};

TEST_F(MatchPhrasePrefixEngineTest, expansionsSpanSegments) {
  TestIndex ti;
  TestField f(ti, "body_w");
  f.startIndexing();
  f.add(0, "quick brown");
  f.add(1, "quick brown");
  ti.flush();
  f.startIndexing();
  f.add(0, "quick braid");
  f.add(1, "quick brown");
  ti.flush();
  f.startReading();
  ASSERT_EQ(ti.reader->segments().size(), 2u);

  std::string_view terms[] = {"quick"};
  int32_t positions[] = {0, 1};
  // Only the merged first term in order, "braid", is kept even though the
  // first segment never saw it.
  PhrasePrefixQuery first("body_w", terms, positions, "br", 0, 1);
  EXPECT_EQ(docs(run(ti, first, 0)), std::vector<int32_t>{});
  EXPECT_EQ(docs(run(ti, first, 1)), std::vector<int32_t>{0});

  PhrasePrefixQuery both("body_w", terms, positions, "br", 0, 2);
  EXPECT_EQ(docs(run(ti, both, 0)), (std::vector<int32_t>{0, 1}));
  EXPECT_EQ(docs(run(ti, both, 1)), (std::vector<int32_t>{0, 1}));
}

TEST_F(MatchPhrasePrefixEngineTest, scoresMatchExplicitPhrases) {
  TestIndex ti;
  TestField f(ti, "body_w");
  f.startIndexing();
  f.add(0, "quick brown fox");
  f.add(1, "quick brother brown");
  f.add(2, "quick fox brown quick brown");
  f.add(3, "brown quick");
  f.add(4, "quick blue");
  ti.flush();
  f.startReading();

  std::string_view terms[] = {"quick"};
  int32_t positions[] = {0, 1};
  for (int32_t slop : {0, 2}) {
    // One expansion: the slot's stats are the term's, so scores are exact.
    PhrasePrefixQuery prefix("body_w", terms, positions, "brow", slop, 50);
    std::string_view phraseTerms[] = {"quick", "brown"};
    PhraseQuery phrase("body_w", phraseTerms, positions, slop);
    auto expected = run(ti, phrase);
    EXPECT_FALSE(expected.empty());
    EXPECT_EQ(run(ti, prefix), expected) << "slop=" << slop;

    // Several expansions: the matches are the union of the explicit phrases.
    PhrasePrefixQuery wide("body_w", terms, positions, "b", slop, 50);
    std::vector<int32_t> expectedDocs;
    for (std::string_view last : {"blue", "brother", "brown"}) {
      std::string_view explicitTerms[] = {"quick", last};
      PhraseQuery one("body_w", explicitTerms, positions, slop);
      for (int32_t doc : docs(run(ti, one))) expectedDocs.push_back(doc);
    }
    std::sort(expectedDocs.begin(), expectedDocs.end());
    expectedDocs.erase(std::unique(expectedDocs.begin(), expectedDocs.end()), expectedDocs.end());
    EXPECT_EQ(docs(run(ti, wide)), expectedDocs) << "slop=" << slop;
  }
}

// Random documents over a small vocabulary whose terms prefix one another, so
// leading terms repeat and overlap the expanded slot. The match set must equal
// the union of the explicit phrases, exact and sloppy.
TEST_F(MatchPhrasePrefixEngineTest, randomAgainstExplicitPhrases) {
  const char* vocab[] = {"a", "ab", "abc", "b"};
  std::mt19937 rng(7);
  TestIndex ti;
  TestField f(ti, "body_w");
  f.startIndexing();
  for (int32_t doc = 0; doc < 300; doc++) {
    std::string body;
    int32_t len = 1 + (int32_t) (rng() % 6);
    for (int32_t i = 0; i < len; i++) {
      if (!body.empty()) body.push_back(' ');
      body.append(vocab[rng() % 4]);
    }
    f.add(doc, body);
  }
  ti.flush();
  f.startReading();

  struct Case {
    std::vector<std::string_view> leading;
    std::string_view prefix;
    std::vector<std::string_view> expansions;
  };
  std::vector<Case> cases = {
      {{"b"}, "a", {"a", "ab", "abc"}},
      {{"a"}, "a", {"a", "ab", "abc"}},
      {{"a", "b"}, "ab", {"ab", "abc"}},
      {{"ab", "a"}, "a", {"a", "ab", "abc"}},
  };
  for (const Case& c : cases) {
    std::vector<std::string_view> terms = c.leading;
    std::vector<int32_t> positions;
    for (size_t i = 0; i <= terms.size(); i++) positions.push_back((int32_t) i);
    for (int32_t slop : {0, 1, 3}) {
      PhrasePrefixQuery prefix("body_w", terms, positions, c.prefix, slop, 50);
      std::vector<int32_t> expected;
      for (std::string_view expansion : c.expansions) {
        std::vector<std::string_view> explicitTerms = c.leading;
        explicitTerms.push_back(expansion);
        PhraseQuery one("body_w", explicitTerms, positions, slop);
        for (int32_t doc : docs(run(ti, one))) expected.push_back(doc);
      }
      std::sort(expected.begin(), expected.end());
      expected.erase(std::unique(expected.begin(), expected.end()), expected.end());
      EXPECT_FALSE(expected.empty());
      EXPECT_EQ(docs(run(ti, prefix)), expected)
          << "leading=" << c.leading.size() << " prefix=" << c.prefix << " slop=" << slop;
    }
  }
}
