#include <gtest/gtest.h>

#include <array>
#include <string>

#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/TestUtils.h"

namespace luxir::test {

// DocList's flat columns+docs pair.  document_format=ROWS places every
// returned field in per-document maps (DocList.docs; missing = key absent);
// COLUMNS (the engine default) keeps dense columns.  Document i is always
// the merge of the two, so both formats must decode to identical Docs
// (convertResultsToDocs is the reference decode loop).
class DocFormatTest : public LuxirTest {};

namespace {

void indexBooks(CollectionHelper& ch) {
  ch.index(flatdoc("id", std::string("b1"), "title_t", std::string("dune novel"),
                   "year_i", (int64_t)1965, "rating_f", 4.5f, "price_d", 9.99,
                   "tags_ss", vecs("scifi", "classic")),
           UpdateMessage::NO_COMMIT);
  // b2 has only id + title: every other requested field is missing.
  ch.index(flatdoc("id", std::string("b2"), "title_t", std::string("dune messiah")),
           UpdateMessage::NO_COMMIT);
  ch.index(flatdoc("id", std::string("b3"), "title_t", std::string("foundation"),
                   "year_i", (int64_t)1951),
           UpdateMessage::COMMIT);
}

} // namespace

TEST_F(DocFormatTest, rowsPlacesFieldsInDocs) {
  CollectionHelper ch;
  indexBooks(ch);

  auto req = localReq(ch.getSearchEngine());
  req->collection("main").topDocs("q").allQuery()
      .fields({"id", "title_t", "year_i", "rating_f", "price_d", "tags_ss"})
      .documentFormat(api::DocFormat::ROWS)
      .limit(-1);
  req->execute();
  ASSERT_OK(req);

  const auto* dl = req->docList("q");
  ASSERT_NE(dl, nullptr);
  EXPECT_EQ(3, dl->row_count);
  EXPECT_TRUE(dl->columns.empty());
  ASSERT_EQ(3u, dl->docs.size());

  auto docs = req->getDocs();
  ASSERT_EQ(3u, docs.size());
  // tags_ss comes back in term-ord (sorted) order: multi-valued STRING
  // columns read by ord, same as the COLUMNS format has always returned.
  EXPECT_CONTAINS_DOC(docs, flatdoc("id", std::string("b1"),
                                        "title_t", std::string("dune novel"),
                                        "year_i", (int64_t)1965,
                                        "rating_f", 4.5f, "price_d", 9.99,
                                        "tags_ss", vecs("classic", "scifi")));
  // Missing fields are absent keys, never placeholders.
  EXPECT_CONTAINS_DOC(docs, flatdoc("id", std::string("b2"),
                                        "title_t", std::string("dune messiah")));
  EXPECT_CONTAINS_DOC(docs, flatdoc("id", std::string("b3"),
                                        "title_t", std::string("foundation"),
                                        "year_i", (int64_t)1951));
}

TEST_F(DocFormatTest, rowsAndColumnsDecodeIdentically) {
  CollectionHelper ch;
  indexBooks(ch);

  auto run = [&](api::DocFormat fmt) {
    auto req = localReq(ch.getSearchEngine());
    req->collection("main").topDocs("q").matchQuery("title_t", "dune")
        .fields({"id", "title_t", "year_i", "rating_f", "tags_ss"})
        .documentFormat(fmt)
        .withStats()
        .limit(-1);
    req->execute();
    EXPECT_TRUE(req->responses[0]->proto.error.empty()) << req->toString();
    return std::pair{req->getDocs(), req->getMatchCount()};
  };

  auto [rowDocs, rowMatches] = run(api::DocFormat::ROWS);
  auto [colDocs, colMatches] = run(api::DocFormat::COLUMNS);

  EXPECT_EQ(rowMatches, colMatches);
  ASSERT_EQ(rowDocs.size(), colDocs.size());
  for (const auto& doc : rowDocs) {
    EXPECT_TRUE(containsDoc(colDocs, doc));
  }
}

TEST_F(DocFormatTest, rowsCarryScorePerDocument) {
  CollectionHelper ch;
  indexBooks(ch);

  auto req = localReq(ch.getSearchEngine());
  req->collection("main").topDocs("q").matchQuery("title_t", "dune")
      .fields({"id"})
      .documentFormat(api::DocFormat::ROWS)
      .getScores()
      .limit(-1);
  req->execute();
  ASSERT_OK(req);

  const auto* dl = req->docList("q");
  ASSERT_NE(dl, nullptr);
  ASSERT_EQ(2, dl->row_count);
  for (const auto& row : dl->docs) {
    const auto* score = row.fields.find("_score_");
    ASSERT_NE(score, nullptr);
    EXPECT_GT(std::get<float>((**score).kind), 0.0f);
  }
}

TEST_F(DocFormatTest, rowCountWithoutColumnsOrDocs) {
  CollectionHelper ch;
  indexBooks(ch);

  // Count-only: no batch rows at all, but the count is exact.
  auto req = localReq(ch.getSearchEngine());
  req->collection("main").topDocs("q").allQuery()
      .documentFormat(api::DocFormat::ROWS)
      .getNumber()
      .limit(0);
  req->execute();
  ASSERT_OK(req);
  const auto* dl = req->docList("q");
  ASSERT_NE(dl, nullptr);
  EXPECT_EQ(0, dl->row_count);
  EXPECT_TRUE(dl->docs.empty());
  EXPECT_EQ(3, req->getMatchCount());

}

// No fields named: the default projection returns every retrievable field -
// id, stored text, string and numeric columns - as per-document rows whatever
// document_format says (discovered fields are never dense columns).  Engine
// fields (_version_) are left out unless named.
TEST_F(DocFormatTest, defaultProjectionReturnsEveryRetrievableField) {
  CollectionHelper ch;
  indexBooks(ch);

  for (auto fmt : {api::DocFormat::DEFAULT, api::DocFormat::ROWS, api::DocFormat::COLUMNS}) {
    auto req = localReq(ch.getSearchEngine());
    req->collection("main").topDocs("q").allQuery().documentFormat(fmt).limit(-1);
    req->execute();
    ASSERT_OK(req);
    const auto* dl = req->docList("q");
    ASSERT_NE(dl, nullptr);
    EXPECT_EQ(3, dl->row_count);
    EXPECT_TRUE(dl->columns.empty());
    ASSERT_EQ(3u, dl->docs.size());
    for (const auto& row : dl->docs) {
      EXPECT_EQ(nullptr, row.fields.find("_version_"));
    }
    auto docs = req->getDocs();
    EXPECT_CONTAINS_DOC(docs, flatdoc("id", std::string("b1"),
                                      "title_t", std::string("dune novel"),
                                      "year_i", (int64_t)1965,
                                      "rating_f", 4.5f, "price_d", 9.99,
                                      "tags_ss", vecs("classic", "scifi")));
    EXPECT_CONTAINS_DOC(docs, flatdoc("id", std::string("b2"),
                                      "title_t", std::string("dune messiah")));
    EXPECT_CONTAINS_DOC(docs, flatdoc("id", std::string("b3"),
                                      "title_t", std::string("foundation"),
                                      "year_i", (int64_t)1951));
  }

  // _version_ is written for documents indexed with overwrite.  The default
  // projection still leaves it out; named, it comes back like any column.
  ch.index(flatdoc("id", std::string("b4"), "title_t", std::string("versioned")),
           UpdateMessage::COMMIT, /*overwrite=*/true);
  auto req = localReq(ch.getSearchEngine());
  req->collection("main").topDocs("q").matchQuery("title_t", "versioned").limit(-1);
  req->execute();
  ASSERT_OK(req);
  auto docs = req->getDocs();
  ASSERT_EQ(1u, docs.size());
  EXPECT_CONTAINS_DOC(docs, flatdoc("id", std::string("b4"), "title_t", std::string("versioned")));

  auto named = localReq(ch.getSearchEngine());
  named->collection("main").topDocs("q").matchQuery("title_t", "versioned")
      .fields({"id", "_version_"}).limit(-1);
  named->execute();
  ASSERT_OK(named);
  auto namedDocs = named->getDocs();
  ASSERT_EQ(1u, namedDocs.size());
  EXPECT_NE(nullptr, find(namedDocs[0], "_version_"));
}

// _score_ is asked for explicitly (get_scores), so it keeps the
// document_format placement even when the fields are discovered: a dense
// column under columns format, beside the discovered rows; a row key under
// rows format.
TEST_F(DocFormatTest, defaultProjectionKeepsScoreColumnUnderColumnsFormat) {
  CollectionHelper ch;
  indexBooks(ch);

  auto run = [&](api::DocFormat fmt) {
    auto req = localReq(ch.getSearchEngine());
    req->collection("main").topDocs("q").matchQuery("title_t", "dune")
        .getScores().documentFormat(fmt).limit(-1);
    req->execute();
    EXPECT_TRUE(req->responses[0]->proto.error.empty()) << req->toString();
    return req;
  };

  auto cols = run(api::DocFormat::COLUMNS);
  const auto* dl = cols->docList("q");
  ASSERT_NE(dl, nullptr);
  EXPECT_EQ(1u, dl->columns.size());
  EXPECT_NE(nullptr, dl->columns.find("_score_"));
  ASSERT_EQ(2u, dl->docs.size());
  for (const auto& row : dl->docs) {
    EXPECT_EQ(nullptr, row.fields.find("_score_"));
    EXPECT_NE(nullptr, row.fields.find("title_t"));
  }

  auto rows = run(api::DocFormat::ROWS);
  const auto* dl2 = rows->docList("q");
  ASSERT_NE(dl2, nullptr);
  EXPECT_TRUE(dl2->columns.empty());
  ASSERT_EQ(2u, dl2->docs.size());
  for (const auto& row : dl2->docs) {
    EXPECT_NE(nullptr, row.fields.find("_score_"));
    EXPECT_NE(nullptr, row.fields.find("title_t"));
  }
}

// Discovery reads each segment's own metadata, so a dynamic field held by
// only one segment appears only on that segment's documents.  Vector columns
// are never part of the default projection; naming one returns it.
TEST_F(DocFormatTest, defaultProjectionDiscoversPerSegmentAndSkipsVectors) {
  CollectionHelper ch;
  ch.index(flatdoc("id", std::string("s1"), "first_s", std::string("one"),
                   "emb_v", std::vector<float>{1, 0}),
           UpdateMessage::COMMIT);
  ch.index(flatdoc("id", std::string("s2"), "second_i", (int64_t)2),
           UpdateMessage::COMMIT);

  auto req = localReq(ch.getSearchEngine());
  req->collection("main").topDocs("q").allQuery().limit(-1);
  req->execute();
  ASSERT_OK(req);
  auto docs = req->getDocs();
  ASSERT_EQ(2u, docs.size());
  EXPECT_CONTAINS_DOC(docs, flatdoc("id", std::string("s1"), "first_s", std::string("one")));
  EXPECT_CONTAINS_DOC(docs, flatdoc("id", std::string("s2"), "second_i", (int64_t)2));

  auto req2 = localReq(ch.getSearchEngine());
  req2->collection("main").topDocs("q").allQuery().fields({"id", "emb_v"}).limit(-1);
  req2->execute();
  ASSERT_OK(req2);
  EXPECT_CONTAINS_DOC(req2->getDocs(), flatdoc("id", std::string("s1"),
                                               "emb_v", std::vector<float>{1, 0}));
}

// A fields entry containing '*' expands against the index's field catalog
// through the default-projection filter.  Matches are placed in rows whatever
// document_format says; explicitly named fields keep dense columns.  Engine
// fields and vectors are never matched by a pattern, and a name given
// explicitly is never duplicated by one.
TEST_F(DocFormatTest, wildcardFieldsDiscoverIntoRows) {
  CollectionHelper ch;
  indexBooks(ch);
  ch.index(flatdoc("id", std::string("b5"), "title_t", std::string("wildcards"),
                   "emb_v", std::vector<float>{1, 0}),
           UpdateMessage::COMMIT, /*overwrite=*/true);  // overwrite writes _version_

  auto req = localReq(ch.getSearchEngine());
  req->collection("main").topDocs("q").allQuery()
      .fields({"id", "*"})
      .documentFormat(api::DocFormat::COLUMNS)
      .limit(-1);
  req->execute();
  ASSERT_OK(req);

  const auto* dl = req->docList("q");
  ASSERT_NE(dl, nullptr);
  EXPECT_EQ(4, dl->row_count);
  ASSERT_EQ(1u, dl->columns.size());
  EXPECT_NE(nullptr, dl->columns.find("id"));
  ASSERT_EQ(4u, dl->docs.size());
  for (const auto& row : dl->docs) {
    EXPECT_EQ(nullptr, row.fields.find("id"));         // explicit wins on collision
    EXPECT_EQ(nullptr, row.fields.find("_version_"));  // engine fields need naming
    EXPECT_EQ(nullptr, row.fields.find("emb_v"));      // vectors need naming
  }
  auto docs = req->getDocs();
  EXPECT_CONTAINS_DOC(docs, flatdoc("id", std::string("b1"),
                                    "title_t", std::string("dune novel"),
                                    "year_i", (int64_t)1965,
                                    "rating_f", 4.5f, "price_d", 9.99,
                                    "tags_ss", vecs("classic", "scifi")));
  EXPECT_CONTAINS_DOC(docs, flatdoc("id", std::string("b5"),
                                    "title_t", std::string("wildcards")));
}

// Patterns with literal prefix/suffix parts select just the matching fields;
// a pattern matching nothing contributes nothing and is never an error.
TEST_F(DocFormatTest, wildcardPatternSelectsMatchingFields) {
  CollectionHelper ch;
  indexBooks(ch);

  auto req = localReq(ch.getSearchEngine());
  req->collection("main").topDocs("q").allQuery()
      .fields({"id", "*_i", "t*s", "nosuch*"})
      .documentFormat(api::DocFormat::ROWS)
      .limit(-1);
  req->execute();
  ASSERT_OK(req);
  EXPECT_TRUE(req->docList("q")->columns.empty());  // ROWS places explicit fields in rows too
  auto docs = req->getDocs();
  ASSERT_EQ(3u, docs.size());
  EXPECT_CONTAINS_DOC(docs, flatdoc("id", std::string("b1"), "year_i", (int64_t)1965,
                                    "tags_ss", vecs("classic", "scifi")));
  EXPECT_CONTAINS_DOC(docs, flatdoc("id", std::string("b2")));
  EXPECT_CONTAINS_DOC(docs, flatdoc("id", std::string("b3"), "year_i", (int64_t)1951));
}

// Explicit wins on collision regardless of position: a field named after a
// pattern that also matches it stays a dense column, appearing exactly once.
TEST_F(DocFormatTest, wildcardExplicitWinsOnCollision) {
  CollectionHelper ch;
  indexBooks(ch);

  auto req = localReq(ch.getSearchEngine());
  req->collection("main").topDocs("q").allQuery()
      .fields({"t*", "title_t"})
      .documentFormat(api::DocFormat::COLUMNS)
      .limit(-1);
  req->execute();
  ASSERT_OK(req);
  const auto* dl = req->docList("q");
  ASSERT_NE(dl, nullptr);
  ASSERT_EQ(1u, dl->columns.size());
  EXPECT_NE(nullptr, dl->columns.find("title_t"));
  ASSERT_EQ(3u, dl->docs.size());
  for (const auto& row : dl->docs) {
    EXPECT_EQ(nullptr, row.fields.find("title_t"));
  }
  EXPECT_CONTAINS_DOC(req->getDocs(), flatdoc("title_t", std::string("dune novel"),
                                              "tags_ss", vecs("classic", "scifi")));
}

// A repeated explicit name collapses to its first occurrence.  Regression:
// the duplicate used to re-emplace the same output Column and orphan the
// first stored-field target, so the attached column came back empty.
TEST_F(DocFormatTest, duplicateExplicitFieldReturnsOnce) {
  CollectionHelper ch;
  indexBooks(ch);

  for (auto fmt : {api::DocFormat::COLUMNS, api::DocFormat::ROWS}) {
    auto req = localReq(ch.getSearchEngine());
    req->collection("main").topDocs("q").matchQuery("title_t", "dune")
        .fields({"title_t", "title_t", "id"})
        .documentFormat(fmt)
        .limit(-1);
    req->execute();
    ASSERT_OK(req);
    auto docs = req->getDocs();
    ASSERT_EQ(2u, docs.size());
    EXPECT_CONTAINS_DOC(docs, flatdoc("id", std::string("b1"),
                                      "title_t", std::string("dune novel")));
    EXPECT_CONTAINS_DOC(docs, flatdoc("id", std::string("b2"),
                                      "title_t", std::string("dune messiah")));
  }
}

// COLUMNS stays the wire shape it always was, now with the authoritative
// row_count alongside.
TEST_F(DocFormatTest, columnsFormatSetsRowCount) {
  CollectionHelper ch;
  indexBooks(ch);

  auto req = localReq(ch.getSearchEngine());
  req->collection("main").topDocs("q").allQuery()
      .fields({"id", "year_i"})
      .documentFormat(api::DocFormat::COLUMNS)
      .limit(-1);
  req->execute();
  ASSERT_OK(req);

  const auto* dl = req->docList("q");
  ASSERT_NE(dl, nullptr);
  EXPECT_EQ(3, dl->row_count);
  EXPECT_TRUE(dl->docs.empty());
  EXPECT_EQ(2u, dl->columns.size());
}

} // namespace luxir::test
