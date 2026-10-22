// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <vector>

#include "luxir/api/build.h"
#include "luxir/util/Signal.h"

#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/SchemaBuilder.h"
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
    EXPECT_FALSE(hasError(req->responses[0]->proto)) << req->toString();
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

TEST_F(DocFormatTest, mixedReturnFieldKindsPreserveSelectors) {
  CollectionHelper ch;
  auto indexed = ch.index(flatdoc("id", "one"), UpdateMessage::COMMIT);
  ASSERT_TRUE(indexed.success);
  for (auto fmt : {api::DocFormat::ROWS, api::DocFormat::COLUMNS}) {
    auto req = localReq(ch.getSearchEngine());
    req->collection("main").topDocs("q").allQuery().documentFormat(fmt)
        .fields({"id", "_score_", "_version_", "_version___self",
                 "id", "_score_", "_version_", "_version___self"});
    req->execute();
    ASSERT_OK(req);
    const auto* dl = req->docList("q");
    EXPECT_EQ(fmt == api::DocFormat::ROWS ? 0u : 4u, dl->columns.size());
    if (fmt == api::DocFormat::ROWS) {
      ASSERT_EQ(1u, dl->docs.size());
      EXPECT_EQ(4u, dl->docs[0].fields.size());
      EXPECT_NE(nullptr, dl->docs[0].fields.find("_score_"));
    } else {
      EXPECT_NE(nullptr, dl->columns.find("_score_"));
    }
    auto docs = req->getDocs();
    ASSERT_EQ(1u, docs.size());
    EXPECT_CONTAINS_DOC(docs, flatdoc("id", "one", "_version_", (int64_t)indexed.updateVersion,
                                    "_version___self", (int64_t)indexed.updateVersion));
  }
}

TEST_F(DocFormatTest, rowCountWithoutColumnsOrDocs) {
  CollectionHelper ch;
  indexBooks(ch);

  // Count-only: no batch rows at all, but the count is exact.
  auto req = localReq(ch.getSearchEngine());
  req->collection("main").topDocs("q").allQuery()
      .documentFormat(api::DocFormat::ROWS)
      .countLimit()
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
// id, stored text, string and numeric columns - placed per document_format
// (the gRPC default is COLUMNS).  Engine fields (_version_) are left out
// unless named.
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
    bool rows = fmt == api::DocFormat::ROWS;
    EXPECT_EQ(rows, dl->columns.empty());
    EXPECT_EQ(rows ? 3u : 0u, dl->docs.size());
    EXPECT_EQ(nullptr, dl->columns.find("_version_"));
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

  // Every document has a request birth. The default projection leaves it out;
  // an explicit name retrieves it.
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

// _score_ (get_scores) is a dense column under COLUMNS and ADAPTIVE, even
// beside fields ADAPTIVE moves to docs, and a row key under ROWS.
TEST_F(DocFormatTest, defaultProjectionScorePlacementFollowsFormat) {
  CollectionHelper ch;
  indexBooks(ch);

  auto run = [&](api::DocFormat fmt) {
    auto req = localReq(ch.getSearchEngine());
    req->collection("main").topDocs("q").matchQuery("title_t", "dune")
        .getScores().documentFormat(fmt).limit(-1);
    req->execute();
    EXPECT_FALSE(hasError(req->responses[0]->proto)) << req->toString();
    return req;
  };

  for (auto fmt : {api::DocFormat::COLUMNS, api::DocFormat::ADAPTIVE}) {
    auto cols = run(fmt);
    const auto* dl = cols->docList("q");
    ASSERT_NE(dl, nullptr);
    EXPECT_NE(nullptr, dl->columns.find("_score_"));
    EXPECT_NE(nullptr, dl->columns.find("title_t"));
    for (const auto& row : dl->docs) EXPECT_EQ(nullptr, row.fields.find("_score_"));
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
// through the default-projection filter.  Matches are placed per
// document_format like named fields.  Engine fields and vectors are never
// matched by a pattern, and a name given explicitly is never duplicated by one.
TEST_F(DocFormatTest, wildcardFieldsFollowDocumentFormat) {
  CollectionHelper ch;
  indexBooks(ch);
  ch.index(flatdoc("id", std::string("b5"), "title_t", std::string("wildcards"),
                   "emb_v", std::vector<float>{1, 0}),
           UpdateMessage::COMMIT, /*overwrite=*/true);

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
  EXPECT_TRUE(dl->docs.empty());
  for (auto name : {"id", "title_t", "year_i", "rating_f", "price_d", "tags_ss"}) {
    EXPECT_NE(nullptr, dl->columns.find(name)) << name;
  }
  EXPECT_EQ(nullptr, dl->columns.find("_version_"));  // engine fields need naming
  EXPECT_EQ(nullptr, dl->columns.find("emb_v"));      // vectors need naming
  auto docs = req->getDocs();
  EXPECT_CONTAINS_DOC(docs, flatdoc("id", std::string("b1"),
                                    "title_t", std::string("dune novel"),
                                    "year_i", (int64_t)1965,
                                    "rating_f", 4.5f, "price_d", 9.99,
                                    "tags_ss", vecs("classic", "scifi")));
  EXPECT_CONTAINS_DOC(docs, flatdoc("id", std::string("b5"),
                                    "title_t", std::string("wildcards")));
}

// COLUMNS leaves a discovered column with no value in the batch out of it; a
// named column stays even when every slot is missing.
TEST_F(DocFormatTest, columnsLeavesOutAllMissingDiscoveredColumns) {
  CollectionHelper ch;
  indexBooks(ch);

  auto req = localReq(ch.getSearchEngine());
  req->collection("main").topDocs("q").matchQuery("title_t", "messiah")
      .fields({"price_d", "*"})
      .documentFormat(api::DocFormat::COLUMNS).limit(-1);
  req->execute();
  ASSERT_OK(req);
  const auto* dl = req->docList("q");
  ASSERT_NE(dl, nullptr);
  EXPECT_EQ(1, dl->row_count);
  EXPECT_TRUE(dl->docs.empty());
  EXPECT_NE(nullptr, dl->columns.find("price_d"));  // named: always present
  EXPECT_NE(nullptr, dl->columns.find("id"));
  EXPECT_NE(nullptr, dl->columns.find("title_t"));
  for (auto name : {"year_i", "rating_f", "tags_ss"}) {
    EXPECT_EQ(nullptr, dl->columns.find(name)) << name;
  }
}

// ADAPTIVE puts a field present in 1 of 20 documents in docs and dense fields
// in columns; the merged documents equal the ROWS result.
TEST_F(DocFormatTest, adaptivePlacesSparseFieldsInDocs) {
  CollectionHelper ch;
  std::vector<Doc> corpus{flatdoc("id", "d0", "year_i", (int64_t)2000, "rare_s", "x")};
  for (int i = 1; i < 20; i++) {
    corpus.push_back(flatdoc("id", "d" + std::to_string(i), "year_i", (int64_t)(2000 + i)));
  }
  ASSERT_TRUE(ch.indexAll(std::span<const Doc>(corpus), UpdateMessage::COMMIT).success);

  auto run = [&](api::DocFormat fmt) {
    auto req = localReq(ch.getSearchEngine());
    req->collection("main").topDocs("q").allQuery().documentFormat(fmt).limit(-1);
    req->execute();
    EXPECT_FALSE(hasError(req->responses[0]->proto)) << req->toString();
    return req;
  };
  auto adaptive = run(api::DocFormat::ADAPTIVE);
  const auto* dl = adaptive->docList("q");
  ASSERT_NE(dl, nullptr);
  EXPECT_NE(nullptr, dl->columns.find("id"));
  EXPECT_NE(nullptr, dl->columns.find("year_i"));
  EXPECT_EQ(nullptr, dl->columns.find("rare_s"));
  ASSERT_EQ(20u, dl->docs.size());
  size_t withRare = 0;
  for (const auto& row : dl->docs) withRare += row.fields.find("rare_s") ? 1 : 0;
  EXPECT_EQ(1u, withRare);

  auto rowDocs = run(api::DocFormat::ROWS)->getDocs();
  auto adaptiveDocs = adaptive->getDocs();
  ASSERT_EQ(rowDocs.size(), adaptiveDocs.size());
  for (const auto& doc : rowDocs) EXPECT_TRUE(containsDoc(adaptiveDocs, doc));
}

// ADAPTIVE leaves a named field out of a batch where no document has a value.
TEST_F(DocFormatTest, adaptiveLeavesOutNamedFieldWithoutValues) {
  CollectionHelper ch;
  indexBooks(ch);

  auto req = localReq(ch.getSearchEngine());
  req->collection("main").topDocs("q").matchQuery("title_t", "messiah")
      .fields({"id", "price_d"})
      .documentFormat(api::DocFormat::ADAPTIVE).limit(-1);
  req->execute();
  ASSERT_OK(req);
  const auto* dl = req->docList("q");
  ASSERT_NE(dl, nullptr);
  EXPECT_EQ(1, dl->row_count);
  EXPECT_NE(nullptr, dl->columns.find("id"));
  EXPECT_EQ(nullptr, dl->columns.find("price_d"));
  for (const auto& row : dl->docs) EXPECT_EQ(nullptr, row.fields.find("price_d"));
}

// COLUMNS settles discovered columns per batch: one with no value in a batch
// is left out of that batch and present in the next; a named column is in
// every batch.
TEST_F(DocFormatTest, columnsDiscoveredColumnsVaryPerBatch) {
  CollectionHelper ch;
  ASSERT_TRUE(ch.indexAll({flatdoc("id", "d0", "n_i", (int64_t)0),
                           flatdoc("id", "d1", "n_i", (int64_t)1),
                           flatdoc("id", "d2", "n_i", (int64_t)2, "extra_s", "x"),
                           flatdoc("id", "d3", "n_i", (int64_t)3, "rating_f", 1.5f)},
                          UpdateMessage::COMMIT).success);
  auto req = localReq(ch.getSearchEngine());
  auto& cursor = req->collection("main").topDocs("q").allQuery().fields({"rating_f", "*"})
      .documentFormat(api::DocFormat::COLUMNS).batchSize(2).limit(-1);
  auto* sorts = api::build::allocArray(std::get<api::TopDocs>(cursor.rawOp().kind).sort, 1, cursor.mr());
  sorts[0].expr = "n_i";
  sorts[0].dir = api::SortSpec::SortDir::ASC;
  req->execute();
  ASSERT_OK(req);
  ASSERT_EQ(2u, req->responses.size());
  for (size_t b = 0; b < 2; b++) {
    const auto* op = req->responses[b]->proto.ops.find("q");
    ASSERT_NE(nullptr, op);
    const auto* dl = (**op).docList();
    ASSERT_NE(nullptr, dl);
    EXPECT_EQ(2, dl->row_count);
    EXPECT_TRUE(dl->docs.empty());
    EXPECT_NE(nullptr, dl->columns.find("rating_f")) << b;  // named: every batch
    EXPECT_EQ(b == 1, dl->columns.find("extra_s") != nullptr) << b;
  }
}

// ADAPTIVE also moves sparse multi-valued and stored text fields to docs;
// the merged documents equal the ROWS result.
TEST_F(DocFormatTest, adaptiveMovesMultiValuedAndStoredFields) {
  CollectionHelper ch;
  std::vector<Doc> corpus{flatdoc("id", "d0", "year_i", (int64_t)2000,
                                  "tags_ss", vecs("scifi", "classic"), "title_t", "dune novel")};
  for (int i = 1; i < 20; i++) {
    corpus.push_back(flatdoc("id", "d" + std::to_string(i), "year_i", (int64_t)(2000 + i)));
  }
  ASSERT_TRUE(ch.indexAll(std::span<const Doc>(corpus), UpdateMessage::COMMIT).success);

  auto run = [&](api::DocFormat fmt) {
    auto req = localReq(ch.getSearchEngine());
    req->collection("main").topDocs("q").allQuery()
        .fields({"id", "year_i", "tags_ss", "title_t"}).documentFormat(fmt).limit(-1);
    req->execute();
    EXPECT_FALSE(hasError(req->responses[0]->proto)) << req->toString();
    return req;
  };
  auto adaptive = run(api::DocFormat::ADAPTIVE);
  const auto* dl = adaptive->docList("q");
  ASSERT_NE(dl, nullptr);
  EXPECT_NE(nullptr, dl->columns.find("year_i"));
  EXPECT_EQ(nullptr, dl->columns.find("tags_ss"));
  EXPECT_EQ(nullptr, dl->columns.find("title_t"));
  ASSERT_EQ(20u, dl->docs.size());

  auto rowDocs = run(api::DocFormat::ROWS)->getDocs();
  auto adaptiveDocs = adaptive->getDocs();
  ASSERT_EQ(rowDocs.size(), adaptiveDocs.size());
  for (const auto& doc : rowDocs) EXPECT_TRUE(containsDoc(adaptiveDocs, doc));
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
// pattern that also matches it appears exactly once.
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
  ASSERT_EQ(2u, dl->columns.size());
  EXPECT_NE(nullptr, dl->columns.find("title_t"));
  EXPECT_NE(nullptr, dl->columns.find("tags_ss"));
  EXPECT_TRUE(dl->docs.empty());
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

class FieldVariantsProjectionTest : public LuxirTest {
protected:
  CollectionHelper helper{"main"};

  void SetUp() override {
    SchemaBuilder b;
    auto& author = b.field("author");
    author.type = api::FieldDef::FieldClass::TEXT;
    auto& s = b.variant(author, "s");
    s.type = api::FieldDef::FieldClass::STRING;
    b.normalizer(s, {"nfkc_cf"});
    b.variant(author, "words").type = api::FieldDef::FieldClass::TEXT;
    author.defaults.emplace().value = "s";
    auto& hidden = b.field("author_hidden");
    hidden.parent = "author";
    hidden.stored = false;
    auto& multi = b.field("authors");
    multi.parent = "author";
    multi.multi = true;
    auto& edition = b.field("edition");
    edition.type = api::FieldDef::FieldClass::INT;
    b.variant(edition, "label").type = api::FieldDef::FieldClass::STRING;
    edition.defaults.emplace().value = "label";
    auto& dynamic = b.templ("_name");
    dynamic.parent = "author";
    b.set(helper.collection());
    ASSERT_TRUE(helper.index(flatdoc("id", "a", "author", "Le Guin",
        "author_hidden", "Invisible", "authors", vecs("Zed", "Ada", "Zed"),
        "edition", "0042", "editor_name", "Other Editor"), UpdateMessage::COMMIT).success);
    ASSERT_TRUE(helper.index(flatdoc("id", "b", "translator_name", "A Translator"),
                             UpdateMessage::COMMIT).success);
  }

  LocalReqHandle project(std::initializer_list<std::string> fields,
                         api::DocFormat format = api::DocFormat::DEFAULT) {
    auto req = localReq(helper.getSearchEngine());
    req->collection("main").topDocs("q").allQuery().fields(fields)
        .documentFormat(format).limit(-1);
    req->execute(false);
    return req;
  }
};

TEST_F(FieldVariantsProjectionTest, discoveryReturnsConcreteLogicalRootsOnce) {
  for (auto fields : {std::initializer_list<std::string>{}, {"*"}}) {
    auto req = project(fields);
    ASSERT_OK(req);
    auto docs = req->getDocs();
    ASSERT_EQ(2u, docs.size());
    EXPECT_CONTAINS_DOC(docs, flatdoc("id", "a", "author", "Le Guin",
        "authors", vecs("Zed", "Ada", "Zed"), "edition", (int64_t)42,
        "editor_name", "Other Editor"));
    EXPECT_CONTAINS_DOC(docs, flatdoc("id", "b", "translator_name", "A Translator"));
    const auto* dl = req->docList("q");
    ASSERT_NE(dl, nullptr);
    EXPECT_TRUE(dl->docs.empty());  // the gRPC default is COLUMNS
    if (fields.size() == 0) {
      EXPECT_EQ("id", docs[0][0].name);
    }
  }
  auto prefix = project({"id", "author*", "*name", "editor*"});
  ASSERT_OK(prefix);
  EXPECT_CONTAINS_DOC(prefix->getDocs(), flatdoc("id", "a", "author", "Le Guin",
      "authors", vecs("Zed", "Ada", "Zed"), "editor_name", "Other Editor"));
}

TEST_F(FieldVariantsProjectionTest, selectorsKeepOutputKeysAndRepresentationValues) {
  for (auto format : {api::DocFormat::ROWS, api::DocFormat::COLUMNS}) {
    auto req = project({"id", "author", "author__s", "author__self", "author_hidden__s",
                        "edition", "edition__label", "edition__self",
                        "authors", "authors__s", "authors__self", "editor_name__s",
                        "editor_name__self"}, format);
    ASSERT_OK(req);
    EXPECT_CONTAINS_DOC(req->getDocs(), flatdoc("id", "a", "author", "Le Guin",
        "author__s", "le guin", "author__self", "Le Guin", "author_hidden__s", "invisible",
        "edition", (int64_t)42, "edition__label", "0042", "edition__self", (int64_t)42,
        "authors", vecs("Zed", "Ada", "Zed"), "authors__s", vecs("ada", "zed"),
        "authors__self", vecs("Zed", "Ada", "Zed"), "editor_name__s", "other editor",
        "editor_name__self", "Other Editor"));
    EXPECT_CONTAINS_DOC(req->getDocs(), flatdoc("id", "b"));
    const auto* dl = req->docList("q");
    ASSERT_NE(dl, nullptr);
    if (format == api::DocFormat::ROWS) {
      EXPECT_TRUE(dl->columns.empty());
    } else {
      EXPECT_TRUE(dl->docs.empty());
      EXPECT_NE(nullptr, dl->columns.find("author__self"));
      EXPECT_NE(nullptr, dl->columns.find("edition__label"));
    }
  }
}

TEST_F(FieldVariantsProjectionTest, explicitKeysWinWildcardPlacementAndDeduplicate) {
  auto req = project({"*", "author", "author__s", "author", "author__s", "author*"});
  ASSERT_OK(req);
  const auto* dl = req->docList("q");
  ASSERT_NE(dl, nullptr);
  EXPECT_TRUE(dl->docs.empty());
  for (std::string_view name : {"author", "author__s"}) {
    size_t n = 0;
    for (const auto& [key, col] : dl->columns) n += key == name ? 1 : 0;
    EXPECT_EQ(1u, n) << name;
  }
  EXPECT_CONTAINS_DOC(req->getDocs(), flatdoc("id", "a", "author", "Le Guin",
      "author__s", "le guin", "authors", vecs("Zed", "Ada", "Zed"),
      "edition", (int64_t)42, "editor_name", "Other Editor"));
}

// A pattern containing "__" expands over variants with a column: the TEXT
// variant author__words is skipped, a stored: false primary does not hide its
// variant, "__self" is a selector alias that no pattern reaches, and a
// derived pattern never surfaces roots.
TEST_F(FieldVariantsProjectionTest, derivedWildcardsExpandVariantsWithColumns) {
  auto req = project({"id", "author__*", "*__label", "author*__self"});
  ASSERT_OK(req);
  EXPECT_CONTAINS_DOC(req->getDocs(), flatdoc("id", "a", "author__s", "le guin",
      "edition__label", "0042"));
  EXPECT_CONTAINS_DOC(req->getDocs(), flatdoc("id", "b"));
  auto all = project({"id", "*__s"});
  ASSERT_OK(all);
  EXPECT_CONTAINS_DOC(all->getDocs(), flatdoc("id", "a", "author__s", "le guin",
      "author_hidden__s", "invisible", "authors__s", vecs("ada", "zed"),
      "editor_name__s", "other editor"));
  EXPECT_CONTAINS_DOC(all->getDocs(), flatdoc("id", "b", "translator_name__s", "a translator"));
}

TEST_F(FieldVariantsProjectionTest, invalidSelectorsTeachSourceForms) {
  auto stored = project({"author__words"});
  ASSERT_FALSE(stored->ok());
  EXPECT_EQ("Field 'author__words' has no retrievable value; retrieve logical root 'author' instead",
            stored->errorMsg());
  for (const char* field : {"author_hidden", "author_hidden__self", "author_hidden__words"}) {
    auto req = project({field});
    ASSERT_FALSE(req->ok());
    EXPECT_NE(std::string::npos, req->errorMsg().find("logical root 'author_hidden' must enable stored"));
  }
  SchemaBuilder b;
  auto& genre = b.field("genre");
  genre.type = api::FieldDef::FieldClass::STRING;
  b.variant(genre, "t").type = api::FieldDef::FieldClass::TEXT;
  auto& number = b.field("number");
  number.type = api::FieldDef::FieldClass::INT;
  b.variant(number, "t").type = api::FieldDef::FieldClass::TEXT;
  auto& unavailable = b.field("unavailable");
  unavailable.type = api::FieldDef::FieldClass::INT;
  unavailable.column = false;
  unavailable.stored = true;  // ignored for numeric primaries
  b.variant(unavailable, "t").type = api::FieldDef::FieldClass::TEXT;
  b.set(helper.collection());
  ASSERT_TRUE(helper.index(flatdoc("id", "c", "genre", "Science Fiction", "number", 42),
                           UpdateMessage::COMMIT).success);
  auto roots = project({"genre", "number"});
  ASSERT_OK(roots);
  EXPECT_CONTAINS_DOC(roots->getDocs(), flatdoc("genre", "Science Fiction", "number", (int64_t)42));
  for (const char* root : {"genre", "number"}) {
    auto req = project({std::string(root) + "__t"});
    ASSERT_FALSE(req->ok());
    EXPECT_EQ(std::format("Field '{}__t' has no retrievable value; retrieve logical root '{}' instead", root, root),
              req->errorMsg());
  }
  auto noSource = project({"unavailable__t"});
  ASSERT_FALSE(noSource->ok());
  EXPECT_EQ("Field 'unavailable__t' has no retrievable value", noSource->errorMsg());
  auto unknown = project({"author__missing"});
  ASSERT_FALSE(unknown->ok());
  EXPECT_NE(std::string::npos, unknown->errorMsg().find("Unknown variant label"));
}

TEST_F(FieldVariantsProjectionTest, retrievableCatalogUsesSchemaIdentityAndPrimaryPresence) {
  auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
  auto schema = helper.collection().getSchema();
  auto catalog = reader->retrievableFields();
  EXPECT_EQ(catalog.data(), reader->retrievableFields().data());

  SchemaBuilder b;
  auto& hidden = b.field("author_hidden");
  hidden.parent = "author";
  hidden.stored = true;
  auto& author = b.field("author");
  author.type = api::FieldDef::FieldClass::TEXT;
  author.stored = false;
  auto other = b.build(schema.get());
  other->gen_ = schema->gen_;  // publication must assign a fresh generation to a copy
  helper.getIndexWriter()->setSchema(other);
  auto replacement = helper.getIndexWriter()->snapshots.readers.getReader(UINT64_MAX);
  EXPECT_EQ(schema, reader->schema());
  EXPECT_EQ(helper.collection().getSchema(), replacement->schema());
  EXPECT_NE(other, replacement->schema());
  EXPECT_EQ(schema->gen_, other->gen_);
  EXPECT_GT(replacement->schema()->gen_, schema->gen_);
  auto changed = replacement->retrievableFields();
  EXPECT_NE(catalog.data(), changed.data());
  EXPECT_EQ(changed.data(), replacement->retrievableFields().data());
  EXPECT_EQ(changed.end(), std::ranges::find(changed, std::string_view("author"),
                                          &IndexReader::RetrievableField::name));
  // author_hidden has only a sibling column in the physical catalog. Merely
  // enabling source storage in another schema cannot discover absent source.
  EXPECT_EQ(changed.end(), std::ranges::find(changed, std::string_view("author_hidden"),
                                          &IndexReader::RetrievableField::name));
  EXPECT_NE(catalog.end(), std::ranges::find(catalog, std::string_view("author"),
                                          &IndexReader::RetrievableField::name));
}

TEST_F(DocFormatTest, storedPlainAndNormalizedStringsReturnSourceValues) {
  CollectionHelper ch;
  SchemaBuilder b;
  auto& plain = b.field("plain");
  plain.type = api::FieldDef::FieldClass::STRING;
  plain.stored = true;
  auto& normalized = b.field("normalized");
  normalized.type = api::FieldDef::FieldClass::STRING;
  normalized.stored = true;
  b.normalizer(normalized, {"nfkc_cf"});
  b.set(ch.collection());
  ASSERT_TRUE(ch.index(flatdoc("id", "a", "plain", "Le Guin", "normalized", "LE GUIN"),
                       UpdateMessage::COMMIT).success);

  // Plain STRING alone permits the column shortcut, including both aliases.
  auto plainReq = localReq(ch.getSearchEngine());
  plainReq->collection("main").topDocs("q").allQuery().fields({"plain", "plain__self"});
  plainReq->execute();
  ASSERT_OK(plainReq);
  EXPECT_CONTAINS_DOC(plainReq->getDocs(), flatdoc("plain", "Le Guin", "plain__self", "Le Guin"));

  // The normalized peer forces their shared resource through stored retrieval.
  auto mixedReq = localReq(ch.getSearchEngine());
  mixedReq->collection("main").topDocs("q").allQuery()
      .fields({"plain", "normalized", "plain__self", "normalized__self"});
  mixedReq->execute();
  ASSERT_OK(mixedReq);
  EXPECT_CONTAINS_DOC(mixedReq->getDocs(), flatdoc("plain", "Le Guin", "normalized", "LE GUIN",
      "plain__self", "Le Guin", "normalized__self", "LE GUIN"));
}

TEST_F(DocFormatTest, storedStringPrimaryPreservesSourceWithOnlyColumnBackedOutputs) {
  CollectionHelper ch;
  SchemaBuilder b;
  auto& names = b.field("names_");
  names.type = api::FieldDef::FieldClass::STRING;
  names.multi = true;
  names.stored = true;
  auto& variant = b.variant(names, "s");
  variant.type = api::FieldDef::FieldClass::STRING;
  b.normalizer(variant, {"nfkc_cf"});
  b.set(ch.collection());
  ASSERT_TRUE(ch.index(flatdoc("id", "a", "names_", vecs("Zed", "Ada", "Zed")),
                       UpdateMessage::COMMIT).success);
  auto req = localReq(ch.getSearchEngine());
  req->collection("main").topDocs("q").allQuery()
      .fields({"names_", "names___self", "names___s"});
  req->execute();
  ASSERT_OK(req);
  EXPECT_CONTAINS_DOC(req->getDocs(), flatdoc("names_", vecs("Zed", "Ada", "Zed"),
      "names___self", vecs("Zed", "Ada", "Zed"), "names___s", vecs("ada", "zed")));
}

TEST_F(DocFormatTest, discoveryRequiresConfiguredStoreOrPrimaryColumnFallback) {
  CollectionHelper ch;
  SchemaBuilder b;
  auto& text = b.field("text");
  text.type = api::FieldDef::FieldClass::TEXT;
  auto& string = b.field("string");
  string.type = api::FieldDef::FieldClass::STRING;
  string.stored = true;
  b.set(ch.collection());
  ASSERT_TRUE(ch.index(flatdoc("id", "a", "text", "Source", "string", "Value"),
                       UpdateMessage::COMMIT).success);
  auto reader = ch.getIndexWriter()->snapshots.readers.getReader();
  auto original = reader->retrievableFields();

  // Both names occur in the old store. Only STRING has a usable fallback
  // when a different schema selects a resource this reader does not have.
  b.storedResource("_stored_cold_");
  text.stored_resource = "_stored_cold_";
  string.stored_resource = "_stored_cold_";
  auto schema = b.build(ch.collection().getSchema().get());
  ch.collection().setSchema(schema);
  auto replacement = ch.getIndexWriter()->snapshots.readers.getReader();
  auto current = replacement->retrievableFields();
  EXPECT_NE(original.data(), current.data());
  ASSERT_EQ(2u, current.size());
  EXPECT_EQ("id", current[0].name);
  EXPECT_EQ("string", current[1].name);
  auto req = localReq(ch.getSearchEngine());
  req->collection("main").topDocs("q").allQuery().fields({"*", "string__self"});
  req->execute();
  ASSERT_OK(req);
  EXPECT_CONTAINS_DOC(req->getDocs(), flatdoc("id", "a", "string", "Value", "string__self", "Value"));
}

TEST_F(DocFormatTest, schemaOnlyPublicationUpdatesSearchAndProjection) {
  CollectionHelper ch;
  SchemaBuilder b;
  auto& title = b.field("title");
  title.type = api::FieldDef::FieldClass::TEXT;
  b.set(ch.collection());
  ASSERT_TRUE(ch.index(flatdoc("id", "a", "title", "Source", "year_i", 2026),
                       UpdateMessage::COMMIT).success);
  auto project = [&](std::initializer_list<std::string> fields, uint64_t freshness = 0) {
    auto req = localReq(ch.getSearchEngine());
    req->rawRequest().freshness_ms = freshness;
    req->collection("main").topDocs("q").allQuery().fields(fields).limit(-1);
    req->execute(false);
    return req;
  };
  auto before = project({});
  ASSERT_OK(before);
  auto reader = before->reader;
  auto oldSchema = reader->schema();

  auto& added = b.field("added");
  added.type = api::FieldDef::FieldClass::STRING;
  b.variant(added, "words").type = api::FieldDef::FieldClass::TEXT;
  title.stored = false;
  auto schema = b.set(ch.collection());
  EXPECT_EQ(oldSchema, reader->schema());
  EXPECT_EQ(nullptr, oldSchema->getFieldTypePtr("added"));

  auto resolved = project({"added"}, UINT64_MAX);
  ASSERT_OK(resolved);
  EXPECT_EQ(schema, resolved->schema);
  EXPECT_EQ(resolved->reader->schema(), resolved->schema);
  EXPECT_NE(nullptr, resolved->schema->getFieldTypePtr("added__words"));
  EXPECT_LT(reader->commitTime(), resolved->reader->commitTime());

  auto checkProjection = [&] {
    for (auto fields : {std::initializer_list<std::string>{}, {"*"}, {"id", "year*"}}) {
      auto req = project(fields, UINT64_MAX);
      ASSERT_OK(req);
      EXPECT_EQ(schema, req->schema);
      for (const auto& doc : req->getDocs()) EXPECT_EQ(nullptr, find(doc, "title"));
      EXPECT_CONTAINS_DOC(req->getDocs(), flatdoc("id", "a", "year_i", (int64_t)2026));
    }
  };
  checkProjection();
  EXPECT_EQ(oldSchema, before->schema);
  EXPECT_CONTAINS_DOC(before->getDocs(), flatdoc("id", "a", "title", "Source", "year_i", (int64_t)2026));
  EXPECT_NE(reader->retrievableFields().end(), std::ranges::find(
      reader->retrievableFields(), std::string_view("title"),
      &IndexReader::RetrievableField::name));

  ASSERT_TRUE(ch.index(flatdoc("id", "b", "added", "New Value"), UpdateMessage::COMMIT).success);
  auto fresh = project({"added"});
  ASSERT_OK(fresh);
  EXPECT_GT(fresh->reader->commitTime(), reader->commitTime());
  EXPECT_CONTAINS_DOC(fresh->getDocs(), flatdoc("added", "New Value"));
  checkProjection();
}

TEST_F(DocFormatTest, readerCapturesSchemaAfterPhysicalOpen) {
  CollectionHelper ch;
  indexBooks(ch);
  auto writer = ch.getIndexWriter();
  auto before = writer->snapshots.readers.getReader();
  ASSERT_TRUE(ch.index(flatdoc("id", "new"), UpdateMessage::COMMIT).success);
  std::shared_ptr<Schema> published;
  Signal::listen("indexReaderOpened", [&](void* source, void*, void*) -> void* {
    if (source == &writer->snapshots.readers) {
      SchemaBuilder b;
      b.field("added").type = api::FieldDef::FieldClass::STRING;
      published = b.set(ch.collection());
    }
    return nullptr;
  });
  auto cleanup = scope_guard([] { Signal::unlisten("indexReaderOpened"); });
  auto after = writer->snapshots.readers.getReader();
  ASSERT_NE(nullptr, published);
  EXPECT_EQ(published, after->schema());
  EXPECT_NE(published, before->schema());
  EXPECT_GT(after->commitTime(), before->commitTime());
  EXPECT_EQ(after, writer->snapshots.readers.getReader());
}

TEST_F(DocFormatTest, emptyStringAndVectorSelectorsKeepColumnsEmpty) {
  CollectionHelper ch;
  indexBooks(ch);
  auto req = localReq(ch.getSearchEngine());
  req->collection("main").topDocs("q").allQuery().offset(10)
      .fields({"id__self", "emb_v__self"});
  req->execute();
  ASSERT_OK(req);
  const auto* dl = req->docList("q");
  ASSERT_NE(dl, nullptr);
  EXPECT_EQ(0, dl->row_count);
  EXPECT_TRUE(dl->columns.empty());
}

} // namespace luxir::test
