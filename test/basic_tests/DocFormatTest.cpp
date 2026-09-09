// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <array>
#include <string>

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
    EXPECT_FALSE(hasError(req->responses[0]->proto)) << req->toString();
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
    EXPECT_TRUE(dl->columns.empty());
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
  ASSERT_EQ(2u, dl->columns.size());
  EXPECT_NE(nullptr, dl->columns.find("author"));
  EXPECT_NE(nullptr, dl->columns.find("author__s"));
  for (const auto& row : dl->docs) {
    EXPECT_EQ(nullptr, row.fields.find("author"));
    EXPECT_EQ(nullptr, row.fields.find("author__s"));
  }
  EXPECT_CONTAINS_DOC(req->getDocs(), flatdoc("id", "a", "author", "Le Guin",
      "author__s", "le guin", "authors", vecs("Zed", "Ada", "Zed"),
      "edition", (int64_t)42, "editor_name", "Other Editor"));
}

TEST_F(FieldVariantsProjectionTest, invalidSelectorsTeachExactAndSourceForms) {
  for (const char* pattern : {"author__*", "*__s", "author*__self"}) {
    auto req = project({pattern});
    ASSERT_FALSE(req->ok());
    EXPECT_NE(std::string::npos, req->errorMsg().find("exact selector"));
  }
  for (const char* field : {"author__words", "author_hidden", "author_hidden__self"}) {
    auto req = project({field});
    ASSERT_FALSE(req->ok());
    EXPECT_NE(std::string::npos, req->errorMsg().find("logical root"));
    EXPECT_NE(std::string::npos, req->errorMsg().find("source text"));
  }
  auto unknown = project({"author__missing"});
  ASSERT_FALSE(unknown->ok());
  EXPECT_NE(std::string::npos, unknown->errorMsg().find("Unknown variant label"));
}

TEST_F(FieldVariantsProjectionTest, logicalCatalogUsesSchemaIdentityAndPrimaryPresence) {
  auto reader = helper.getIndexWriter()->getIndexReader();
  auto schema = helper.collection().getSchema();
  auto catalog = reader->logicalProjectableFields(schema);
  EXPECT_EQ(catalog, reader->logicalProjectableFields(schema));

  SchemaBuilder b;
  auto& hidden = b.field("author_hidden");
  hidden.parent = "author";
  hidden.stored = true;
  auto& author = b.field("author");
  author.type = api::FieldDef::FieldClass::TEXT;
  author.stored = false;
  auto other = b.build(schema.get());
  other->gen_ = schema->gen_;  // identity must distinguish equal generations
  auto changed = reader->logicalProjectableFields(other);
  EXPECT_NE(catalog, changed);
  EXPECT_EQ(changed, reader->logicalProjectableFields(other));
  EXPECT_EQ(changed->end(), std::ranges::find(*changed, std::string_view("author")));
  // author_hidden has only a sibling column in the physical catalog. Merely
  // enabling source storage in another schema cannot discover absent source.
  EXPECT_EQ(changed->end(), std::ranges::find(*changed, std::string_view("author_hidden")));
  EXPECT_NE(catalog->end(), std::ranges::find(*catalog, std::string_view("author")));
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
  auto reader = ch.getIndexWriter()->getIndexReader();
  auto original = reader->logicalProjectableFields(ch.collection().getSchema());

  // Both names occur in the old store. Only STRING has a usable fallback
  // when a different schema selects a resource this reader does not have.
  text.stored_resource = "_stored_cold_";
  string.stored_resource = "_stored_cold_";
  auto schema = b.build(ch.collection().getSchema().get());
  ch.collection().setSchema(schema);
  auto current = reader->logicalProjectableFields(schema);
  EXPECT_NE(original, current);
  EXPECT_EQ((std::vector<std::string_view>{"id", "string"}), *current);
  auto req = localReq(ch.getSearchEngine());
  req->collection("main").topDocs("q").allQuery().fields({"*", "string__self"});
  req->execute();
  ASSERT_OK(req);
  EXPECT_CONTAINS_DOC(req->getDocs(), flatdoc("id", "a", "string", "Value", "string__self", "Value"));
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
