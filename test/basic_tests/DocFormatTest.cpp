#include <gtest/gtest.h>

#include <array>
#include <string>

#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/SoluxTest.h"
#include "test/TestUtils.h"

namespace solux::test {

// DocList's flat columns+docs pair.  document_format=ROWS places every
// returned field in per-document maps (DocList.docs; missing = key absent);
// COLUMNS (the engine default) keeps dense columns.  Document i is always
// the merge of the two, so both formats must decode to identical Docs
// (convertResultsToDocs is the reference decode loop).
class DocFormatTest : public SoluxTest {};

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

  // No fields requested but hits returned: row_count still says how many,
  // and the rows are present (empty maps), not dropped.
  auto req2 = localReq(ch.getSearchEngine());
  req2->collection("main").topDocs("q").allQuery()
      .fields({})
      .documentFormat(api::DocFormat::ROWS)
      .limit(-1);
  req2->execute();
  ASSERT_OK(req2);
  const auto* dl2 = req2->docList("q");
  ASSERT_NE(dl2, nullptr);
  EXPECT_EQ(3, dl2->row_count);
  ASSERT_EQ(3u, dl2->docs.size());
  for (const auto& row : dl2->docs) {
    EXPECT_TRUE(row.fields.empty());
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

} // namespace solux::test
