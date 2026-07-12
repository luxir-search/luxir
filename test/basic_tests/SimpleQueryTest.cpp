// Behavior tests for the simple_query arm end-to-end: request envelope
// validation, lowering through ProtobufQueryParser/QueryBuilder, match
// behavior over a real index, and the warnings channel on the response.

#include <gtest/gtest.h>

#include "test/SoluxTest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/TestUtils.h"

using namespace std;
using namespace solux;
using namespace solux::test;

using Operator = solux::api::Match_::Operator;

class SimpleQueryTest : public SoluxTest {
public:
  CollectionHelper helper{"main"};

  void SetUp() override {
    helper.index(flatdoc("id", "d1", "title_wl", "Blade Runner",
                         "body_wl", "a replicant story", "tag_s", "scifi"),
                 UpdateMessage::NO_COMMIT);
    helper.index(flatdoc("id", "d2", "title_wl", "The Running Man",
                         "body_wl", "arnold runs fast", "tag_s", "action"),
                 UpdateMessage::NO_COMMIT);
    helper.index(flatdoc("id", "d3", "title_wl", "Bladed Weapons",
                         "body_wl", "swords and knives", "tag_s", "scifi"),
                 UpdateMessage::COMMIT);
  }


  // run q over the given fields; returns doc ids (empty on error)
  std::vector<Doc> search(std::string_view q, std::initializer_list<std::string> fields,
                          std::function<void(solux::api::SimpleQuery&)> tweak = {}) {
    auto req = localReq(helper.getSearchEngine());
    auto& cur = req->collection("main").topDocs("q");
    cur.simpleQuery(q, fields).fields({"id"}).limit(-1);
    if (tweak) {
      tweak(std::get<solux::api::SimpleQuery>(cur.rawQuery().kind));
    }
    req->execute();
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    return req->getDocs();
  }

  static bool hasId(const std::vector<Doc>& docs, std::string_view id) {
    return containsDoc(docs, flatdoc("id", std::string(id)));
  }
};

TEST_F(SimpleQueryTest, bareWordsOverFields) {
  // OR semantics: any clause may match; scores sum across fields
  auto docs = search("blade story", {"title_wl", "body_wl"});
  ASSERT_EQ(1u, docs.size());
  EXPECT_TRUE(hasId(docs, "d1"));

  docs = search("blade swords", {"title_wl", "body_wl"});
  EXPECT_EQ(2u, docs.size());  // d1 (blade), d3 (swords)
}

TEST_F(SimpleQueryTest, mustAndMustNot) {
  auto docs = search("+running +man", {"title_wl"});
  ASSERT_EQ(1u, docs.size());
  EXPECT_TRUE(hasId(docs, "d2"));

  // '-' is a unary prohibition that RESTRICTS even under the default OR
  // (classic QueryParser): scifi-tagged docs minus those with "blade" in the
  // title leaves d3 (d1 is excluded)
  docs = search("tag_s:scifi -blade", {"title_wl"});
  ASSERT_EQ(1u, docs.size());
  EXPECT_TRUE(hasId(docs, "d3"));

  // a purely negative query is "everything except": all docs minus title:blade
  docs = search("-blade", {"title_wl"});
  EXPECT_EQ(2u, docs.size());  // d2, d3
  EXPECT_FALSE(hasId(docs, "d1"));
}

TEST_F(SimpleQueryTest, fieldedTermStaysOnField) {
  auto docs = search("tag_s:scifi", {"title_wl"});
  EXPECT_EQ(2u, docs.size());

  // unknown field name degrades to literal text against the fields: no error
  docs = search("price:10", {"title_wl", "body_wl"});
  EXPECT_EQ(0u, docs.size());
}

TEST_F(SimpleQueryTest, numericFieldExactMatch) {
  // popularity:10 style: field:value on a numeric column is an exact match
  // (a degenerate [10,10] range), lowered to a NumericRangeQuery
  helper.index(flatdoc("id", "n1", "title_wl", "alpha", "pop_i", "10"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id", "n2", "title_wl", "beta", "pop_i", "20"), UpdateMessage::NO_COMMIT);
  helper.index(flatdoc("id", "n3", "title_wl", "gamma", "pop_i", "10"), UpdateMessage::COMMIT);

  auto docs = search("pop_i:10", {"title_wl"});
  EXPECT_EQ(2u, docs.size());
  EXPECT_TRUE(hasId(docs, "n1"));
  EXPECT_TRUE(hasId(docs, "n3"));

  // a quoted value is the same exact match (quotes are just delimiters)
  EXPECT_EQ(2u, search("pop_i:\"10\"", {"title_wl"}).size());

  // combines with text clauses like any other leaf
  docs = search("pop_i:10 +alpha", {"title_wl"});
  ASSERT_EQ(1u, docs.size());
  EXPECT_TRUE(hasId(docs, "n1"));
}

TEST_F(SimpleQueryTest, numericFieldSyntaxDegradesWithWarning) {
  // wildcard/fuzzy have no numeric meaning, and a non-numeric value cannot be
  // a numeric query: both degrade to text (matching nothing over the text
  // fields here) and declare why, rather than failing the request
  auto run = [&](std::string_view q, std::string_view warnCode) {
    auto req = localReq(helper.getSearchEngine());
    req->collection("main").topDocs("q").simpleQuery(q, {"title_wl"}).fields({"id"}).limit(-1);
    req->execute();
    ASSERT_TRUE(req->ok()) << req->errorMsg();
    EXPECT_TRUE(req->hasWarning(warnCode)) << q;
  };
  run("pop_i:1*", "numeric_field_syntax");
  run("pop_i:10~1", "numeric_field_syntax");
  run("pop_i:abc", "numeric_field_value");
}

TEST_F(SimpleQueryTest, phraseAndPrefix) {
  auto docs = search("\"blade runner\"", {"title_wl"});
  ASSERT_EQ(1u, docs.size());
  EXPECT_TRUE(hasId(docs, "d1"));

  // phrase must not match reordered terms
  EXPECT_EQ(0u, search("\"runner blade\"", {"title_wl"}).size());

  // last-token prefix, explicit: runn* matches runner/running/runs
  docs = search("runn*", {"title_wl", "body_wl"});
  EXPECT_EQ(2u, docs.size());  // d1 (runner), d2 (running, runs)
}

TEST_F(SimpleQueryTest, fuzzyTerm) {
  // "bladee" is one edit from both "blade" (deletion) and "bladed" (substitution)
  auto docs = search("bladee~1", {"title_wl"});
  EXPECT_EQ(2u, docs.size());
  EXPECT_TRUE(hasId(docs, "d1"));
  EXPECT_TRUE(hasId(docs, "d3"));
}

TEST_F(SimpleQueryTest, operatorAndMinMatch) {
  // default operator AND: every clause must match
  auto docs = search("blade replicant", {"title_wl", "body_wl"},
                     [](solux::api::SimpleQuery& sq) { sq.operator_ = Operator::AND; });
  ASSERT_EQ(1u, docs.size());
  EXPECT_TRUE(hasId(docs, "d1"));

  // min_match: at least 2 of 3 optional clauses
  docs = search("blade replicant swords", {"title_wl", "body_wl"},
                [](solux::api::SimpleQuery& sq) { sq.min_match = 2; });
  ASSERT_EQ(1u, docs.size());
  EXPECT_TRUE(hasId(docs, "d1"));
}

TEST_F(SimpleQueryTest, warningsRideTheResponse) {
  auto req = localReq(helper.getSearchEngine());
  req->collection("main").topDocs("q")
      .simpleQuery("blade~9", {"title_wl"})
      .fields({"id"}).limit(-1);
  req->execute();
  ASSERT_TRUE(req->ok()) << req->errorMsg();
  EXPECT_TRUE(req->hasWarning("fuzzy_clamped"));
  EXPECT_EQ(2u, req->getDocs().size());  // still served (blade, bladed): clamp-and-declare
}

TEST_F(SimpleQueryTest, minMatchNeverBindsToFieldExpansion) {
  // one user clause over two fields with min_match=2: it must not require
  // the term in BOTH fields (the expansion boolean is not a clause list),
  // and inapplicability is silent - it depends on what the user typed
  auto req = localReq(helper.getSearchEngine());
  auto& cur = req->collection("main").topDocs("q");
  cur.simpleQuery("blade", {"title_wl", "body_wl"}).fields({"id"}).limit(-1);
  std::get<solux::api::SimpleQuery>(cur.rawQuery().kind).min_match = 2;
  req->execute();
  ASSERT_TRUE(req->ok()) << req->errorMsg();
  EXPECT_EQ(1u, req->getDocs().size());  // d1 matches via title alone
  EXPECT_TRUE(req->respWarnings().empty());
}

TEST_F(SimpleQueryTest, minMatchCountsMixedFieldTypeClauses) {
  // 2 of 3 clauses: an exact STRING match counts alongside analyzed TEXT ones
  auto docs = search("tag_s:scifi blade swords", {"title_wl", "body_wl"},
                     [](solux::api::SimpleQuery& sq) { sq.min_match = 2; });
  EXPECT_EQ(2u, docs.size());
  EXPECT_TRUE(hasId(docs, "d1"));  // scifi + blade
  EXPECT_TRUE(hasId(docs, "d3"));  // scifi + swords
}

TEST_F(SimpleQueryTest, quotedValueOnStringFieldIsExactMatch) {
  // quoted text against an unanalyzed STRING field is one exact term - it
  // used to lower as a PhraseQuery and error the whole request
  helper.index(flatdoc("id", "d4", "title_wl", "extra doc", "tag_s", "in stock"),
               UpdateMessage::COMMIT);
  auto docs = search("tag_s:\"in stock\"", {"title_wl"});
  ASSERT_EQ(1u, docs.size());
  EXPECT_TRUE(hasId(docs, "d4"));
}

TEST_F(SimpleQueryTest, minMatchOnRequiredTopLevelIsSilentlyInapplicable) {
  auto req = localReq(helper.getSearchEngine());
  auto& cur = req->collection("main").topDocs("q");
  // '+runner' is a required clause, so the top level is not optional-only and
  // min_match cannot apply - that is user-input-contingent, so it costs no
  // warning
  cur.simpleQuery("blade +runner", {"title_wl"}).fields({"id"}).limit(-1);
  std::get<solux::api::SimpleQuery>(cur.rawQuery().kind).min_match = 2;
  req->execute();
  ASSERT_TRUE(req->ok()) << req->errorMsg();
  EXPECT_EQ(1u, req->getDocs().size());  // both required terms: d1
  EXPECT_TRUE(req->respWarnings().empty());
}

TEST_F(SimpleQueryTest, allowedFieldsNarrows) {
  auto req = localReq(helper.getSearchEngine());
  auto& cur = req->collection("main").topDocs("q");
  cur.simpleQuery("tag_s:scifi", {"title_wl"}).fields({"id"}).limit(-1);
  auto& sq = std::get<solux::api::SimpleQuery>(cur.rawQuery().kind);
  auto* allowed = build::allocArray(sq.allowed_fields, 1, cur.mr());
  allowed[0] = build::arenaStr(cur.mr(), "title_wl");
  req->execute();
  ASSERT_TRUE(req->ok()) << req->errorMsg();
  // tag_s is queryable but narrowed away: literal text, declared
  EXPECT_EQ(0u, req->getDocs().size());
  EXPECT_TRUE(req->hasWarning("field_narrowed"));
}

TEST_F(SimpleQueryTest, envelopeErrors) {
  // the never-fails contract covers the string, not the request shape
  auto expectError = [&](std::initializer_list<std::string> fields, std::string_view msgPart) {
    auto req = localReq(helper.getSearchEngine());
    req->collection("main").topDocs("q").simpleQuery("foo", fields).fields({"id"});
    req->execute();
    EXPECT_FALSE(req->ok());
    EXPECT_NE(std::string::npos, req->errorMsg().find(msgPart)) << req->errorMsg();
  };
  expectError({}, "non-empty 'fields'");
  expectError({"popularity_i"}, "not a queryable");   // numeric column field
  expectError({"zzz_no_such"}, "not a queryable");    // unresolvable name
}

TEST_F(SimpleQueryTest, emptyStringMatchesNothing) {
  EXPECT_EQ(0u, search("", {"title_wl"}).size());
  EXPECT_EQ(0u, search("+ | -", {"title_wl"}).size());
}

TEST_F(SimpleQueryTest, wholeInputStarMatchesAll) {
  EXPECT_EQ(3u, search("*", {"title_wl"}).size());
}

TEST_F(SimpleQueryTest, expansionSplicesIntoRequestTree) {
  // after execution the simple_query arm has been replaced by its structured
  // expansion (same request-storage lifetime), so serializing the request
  // shows the canonical equivalent - the echo-mode contract, same as expr
  auto req = localReq(helper.getSearchEngine());
  auto& cur = req->collection("main").topDocs("q");
  cur.simpleQuery("tag_s:scifi", {"title_wl"}).fields({"id"}).limit(-1);
  req->execute();
  ASSERT_TRUE(req->ok()) << req->errorMsg();
  EXPECT_TRUE(std::holds_alternative<solux::api::Match>(cur.rawQuery().kind));

  // a q that parses to nothing has no structured equivalent to splice; the
  // string arm stays put and the query matches no documents
  auto req2 = localReq(helper.getSearchEngine());
  auto& cur2 = req2->collection("main").topDocs("q");
  cur2.simpleQuery("+ | -", {"title_wl"}).fields({"id"}).limit(-1);
  req2->execute();
  ASSERT_TRUE(req2->ok()) << req2->errorMsg();
  EXPECT_TRUE(std::holds_alternative<solux::api::SimpleQuery>(cur2.rawQuery().kind));
  EXPECT_EQ(0u, req2->getDocs().size());
}
