// Tests for the Val <-> FieldType coercion contract: one implementation shared
// by ingest and query build, so a doc ingested via one rule is findable by
// querying the same literal (the findability invariant).

#include <gtest/gtest.h>

#include "solux/schema/FieldType.h"
#include "solux/schema/ValCoerce.h"
#include "solux/util/DateTime.h"
#include "solux/util/NumericUtils.h"

#include "test/SoluxTest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/QueryBuild.h"
#include "test/TestUtils.h"

using namespace std;
using namespace solux;
using namespace solux::test;

using ResponseStatus = solux::api::UpdateResponse_::Status;

class ValCoerceTest : public SoluxTest {
public:
  static api::Val sval(std::string_view s) { return coerce::scalarVal(s); }

  // docs matching a match query on (field, word)
  static std::vector<Doc> matchDocs(CollectionHelper& helper, std::string_view field,
                                    std::string_view word) {
    auto req = localReq(helper.getSearchEngine());
    req->collection("main").topDocs("q").matchQuery(field, word).fields({"id"}).limit(-1);
    req->execute();
    return req->getDocs();
  }
};

// ---- shared core: strict whole-token parses ----

TEST_F(ValCoerceTest, parseInt64Strict) {
  EXPECT_EQ(10, coerce::parseInt64("10"));
  EXPECT_EQ(-5, coerce::parseInt64("-5"));
  EXPECT_EQ(std::numeric_limits<int64_t>::max(), coerce::parseInt64("9223372036854775807"));
  EXPECT_EQ(std::numeric_limits<int64_t>::min(), coerce::parseInt64("-9223372036854775808"));

  EXPECT_FALSE(coerce::parseInt64(""));
  EXPECT_FALSE(coerce::parseInt64(" 10"));
  EXPECT_FALSE(coerce::parseInt64("10 "));
  EXPECT_FALSE(coerce::parseInt64("10x"));
  EXPECT_FALSE(coerce::parseInt64("1.5"));
  EXPECT_FALSE(coerce::parseInt64("1.0"));                     // strict: no float syntax
  EXPECT_FALSE(coerce::parseInt64("+5"));                      // from_chars takes no '+'
  EXPECT_FALSE(coerce::parseInt64("9223372036854775808"));     // overflow
}

TEST_F(ValCoerceTest, parseDoubleStrict) {
  EXPECT_EQ(1.5, coerce::parseDouble("1.5"));
  EXPECT_EQ(1000.0, coerce::parseDouble("1e3"));
  EXPECT_EQ(-0.25, coerce::parseDouble("-0.25"));
  EXPECT_FALSE(coerce::parseDouble(""));
  EXPECT_FALSE(coerce::parseDouble("abc"));
  EXPECT_FALSE(coerce::parseDouble("1.5x"));
  EXPECT_FALSE(coerce::parseDouble(" 1.5"));
}

TEST_F(ValCoerceTest, toInt64Arms) {
  EXPECT_EQ(7, coerce::toInt64(coerce::scalarVal((int64_t)7), "f"));
  EXPECT_EQ(42, coerce::toInt64(sval("42"), "f"));
  EXPECT_EQ(3, coerce::toInt64(coerce::scalarVal(3.0), "f"));    // integral double narrows
  EXPECT_EQ(2, coerce::toInt64(coerce::scalarVal(2.0f), "f"));

  EXPECT_THROW(coerce::toInt64(coerce::scalarVal(3.5), "f"), std::runtime_error);
  EXPECT_THROW(coerce::toInt64(coerce::scalarVal(std::nan("")), "f"), std::runtime_error);
  EXPECT_THROW(coerce::toInt64(coerce::scalarVal(1e19), "f"), std::runtime_error);  // out of range
  EXPECT_THROW(coerce::toInt64(coerce::scalarVal(true), "f"), std::runtime_error);
  EXPECT_THROW(coerce::toInt64(sval("abc"), "f"), std::runtime_error);
}

TEST_F(ValCoerceTest, toDoubleArms) {
  EXPECT_EQ(1.5, coerce::toDouble(coerce::scalarVal(1.5), "f"));
  EXPECT_EQ(2.5, coerce::toDouble(coerce::scalarVal(2.5f), "f"));
  EXPECT_EQ(10.0, coerce::toDouble(coerce::scalarVal((int64_t)10), "f"));  // widen
  EXPECT_EQ(0.5, coerce::toDouble(sval("0.5"), "f"));
  EXPECT_THROW(coerce::toDouble(sval("abc"), "f"), std::runtime_error);
  EXPECT_THROW(coerce::toDouble(coerce::scalarVal(true), "f"), std::runtime_error);
}

TEST_F(ValCoerceTest, toTextRendersCanonically) {
  char buf[coerce::TEXT_BUF_SIZE];
  EXPECT_EQ("42", coerce::toText(coerce::scalarVal((int64_t)42), "f", buf));
  EXPECT_EQ("-7", coerce::toText(coerce::scalarVal((int64_t)-7), "f", buf));
  EXPECT_EQ("3", coerce::toText(coerce::scalarVal(3.0), "f", buf));    // canonical: "3.0" -> "3"
  EXPECT_EQ("1.5", coerce::toText(coerce::scalarVal(1.5), "f", buf));
  EXPECT_EQ("true", coerce::toText(coerce::scalarVal(true), "f", buf));

  // string arm passes through as a view of the ORIGINAL bytes, not buf
  std::string_view s = "hello";
  EXPECT_EQ(s.data(), coerce::toText(sval(s), "f", buf).data());

  EXPECT_THROW(coerce::toText(api::Val{}, "f", buf), std::runtime_error);
}

// ---- FieldType virtuals: the per-type encodings ----

TEST_F(ValCoerceTest, fieldTypeColInt64Encodings) {
  IntFieldType it("_i");
  FloatFieldType ft("_f");
  DoubleFieldType dt("_d");
  DateFieldType dat("_dt");

  EXPECT_EQ(10, it.coerceColInt64(sval("10"), "f"));
  EXPECT_EQ((int64_t)floatToSortableInt32(1.5f), ft.coerceColInt64(coerce::scalarVal(1.5), "f"));
  EXPECT_EQ(doubleToSortableInt64(2.5), dt.coerceColInt64(sval("2.5"), "f"));

  auto ms = parseDateToEpochMillis("2020-01-01T00:00:00Z");
  ASSERT_TRUE(ms.has_value());
  EXPECT_EQ(*ms, dat.coerceColInt64(sval("2020-01-01T00:00:00Z"), "f"));
  EXPECT_EQ(1234, dat.coerceColInt64(coerce::scalarVal((int64_t)1234), "f"));
  EXPECT_THROW(dat.coerceColInt64(coerce::scalarVal(1.5), "f"), std::runtime_error);
  EXPECT_THROW(dat.coerceColInt64(sval("not a date"), "f"), std::runtime_error);
}

TEST_F(ValCoerceTest, fieldTypeDefaultsThrow) {
  IntFieldType it("_i");
  StrFieldType st("_s");
  char buf[coerce::TEXT_BUF_SIZE];
  EXPECT_THROW(it.coerceTerm(sval("x"), "f", buf), std::runtime_error);       // INT is not term-backed
  EXPECT_THROW(st.coerceColInt64(sval("1"), "f"), std::runtime_error);        // STRING has no int column
  EXPECT_EQ("42", st.coerceTerm(coerce::scalarVal((int64_t)42), "f", buf));
}

// ---- ingest: quoted numbers index (used to be silently dropped) ----

TEST_F(ValCoerceTest, ingestCoercesQuotedNumbers) {
  CollectionHelper helper("main");
  helper.clear();

  helper.index(flatdoc("id", "d1", "popularity_i", "10", "score_d", "2.5"),
               UpdateMessage::COMMIT);

  auto req = localReq(helper.getSearchEngine());
  req->collection("main").topDocs("q").allQuery()
      .fields({"id", "popularity_i", "score_d"}).limit(-1);
  req->execute();
  ASSERT_OK(req);
  auto docs = req->getDocs();
  ASSERT_EQ(1u, docs.size());
  EXPECT_TRUE(containsDoc(docs, flatdoc("id", "d1", "popularity_i", (int64_t)10,
                                        "score_d", 2.5)));
  helper.clear();
}

TEST_F(ValCoerceTest, ingestCoercesIntegralDoubleToIntCol) {
  CollectionHelper helper("main");
  helper.clear();

  helper.index(flatdoc("id", "d1", "popularity_i", 10.0), UpdateMessage::COMMIT);

  auto req = localReq(helper.getSearchEngine());
  req->collection("main").topDocs("q").allQuery().fields({"id", "popularity_i"}).limit(-1);
  req->execute();
  ASSERT_OK(req);
  EXPECT_TRUE(containsDoc(req->getDocs(), flatdoc("id", "d1", "popularity_i", (int64_t)10)));
  helper.clear();
}

TEST_F(ValCoerceTest, ingestCoercesElementWiseArrays) {
  CollectionHelper helper("main");
  helper.clear();

  helper.index(flatdoc("id", "d1", "nums_is", std::vector<std::string>{"1", "2"}),
               UpdateMessage::COMMIT);

  auto req = localReq(helper.getSearchEngine());
  req->collection("main").topDocs("q").allQuery().fields({"id", "nums_is"}).limit(-1);
  req->execute();
  ASSERT_OK(req);
  EXPECT_TRUE(containsDoc(req->getDocs(),
                          flatdoc("id", "d1", "nums_is", std::vector<int64_t>{1, 2})));
  helper.clear();
}

TEST_F(ValCoerceTest, ingestBadValueFailsTheDocOnly) {
  CollectionHelper helper("main");
  helper.clear();

  CollectionHelper::UpdateBuilder b;
  b.add(flatdoc("id", "g1", "popularity_i", (int64_t)1));
  b.add(flatdoc("id", "b1", "popularity_i", "abc"));
  b.add(flatdoc("id", "b2", "popularity_i", 10.5));  // non-integral
  b.add(flatdoc("id", "g2", "popularity_i", "3"));
  b.commit();
  auto result = helper.submit(b);

  ASSERT_EQ(ResponseStatus::PARTIAL, result.status);
  ASSERT_EQ(2u, result.errors.size());
  EXPECT_EQ("b1", result.errors[0].id);
  EXPECT_NE(std::string::npos, result.errors[0].error_message.find("cannot parse"));
  EXPECT_EQ("b2", result.errors[1].id);

  auto req = localReq(helper.getSearchEngine());
  req->collection("main").topDocs("q").allQuery().fields({"id"}).limit(-1);
  req->execute();
  auto docs = req->getDocs();
  ASSERT_EQ(2u, docs.size());
  EXPECT_TRUE(containsDoc(docs, flatdoc("id", "g1")));
  EXPECT_TRUE(containsDoc(docs, flatdoc("id", "g2")));
  helper.clear();
}

TEST_F(ValCoerceTest, ingestBadArrayElementFailsBeforeAnyAppend) {
  CollectionHelper helper("main");
  helper.clear();

  CollectionHelper::UpdateBuilder b;
  b.add(flatdoc("id", "b1", "nums_is", std::vector<std::string>{"1", "x"}));
  b.add(flatdoc("id", "g1", "nums_is", std::vector<std::string>{"5"}));
  b.commit();
  auto result = helper.submit(b);
  ASSERT_EQ(ResponseStatus::PARTIAL, result.status);

  // the good doc's column data must be intact after the failed doc
  auto req = localReq(helper.getSearchEngine());
  req->collection("main").topDocs("q").allQuery().fields({"id", "nums_is"}).limit(-1);
  req->execute();
  auto docs = req->getDocs();
  ASSERT_EQ(1u, docs.size());
  EXPECT_TRUE(containsDoc(docs, flatdoc("id", "g1", "nums_is", std::vector<int64_t>{5})));
  helper.clear();
}

// ---- the findability invariant, both directions ----

TEST_F(ValCoerceTest, numericIngestFindableByString) {
  CollectionHelper helper("main");
  helper.clear();

  // {"tag_s": 42} used to index an EMPTY term; {"body_w": 42} likewise
  helper.index(flatdoc("id", "d1", "tag_s", (int64_t)42, "body_w", (int64_t)42),
               UpdateMessage::COMMIT);

  EXPECT_EQ(1u, matchDocs(helper, "tag_s", "42").size());
  EXPECT_EQ(1u, matchDocs(helper, "body_w", "42").size());
  helper.clear();
}

TEST_F(ValCoerceTest, stringIngestFindableByNumericVal) {
  CollectionHelper helper("main");
  helper.clear();

  helper.index(flatdoc("id", "d1", "tag_s", "42"), UpdateMessage::COMMIT);

  // query with an int Val against the string field (proto client picking the arm)
  auto req = localReq(helper.getSearchEngine());
  auto& cur = req->collection("main").topDocs("q");
  api::Query q = qb::match(cur.mr(), "tag_s", "overwritten");
  auto& m = std::get<api::Match>(q.kind);
  auto* v = (api::Val*)cur.mr().allocate(sizeof(api::Val), alignof(api::Val));
  new (v) api::Val();
  v->kind = (int64_t)42;
  m.val = v;
  qb::setQuery(cur, q);
  cur.fields({"id"}).limit(-1);
  req->execute();
  ASSERT_OK(req);
  EXPECT_EQ(1u, req->getDocs().size());
  helper.clear();
}

TEST_F(ValCoerceTest, numericIdIndexesItsRendering) {
  CollectionHelper helper("main");
  helper.clear();

  // a numeric id used to index NOTHING (doc had no id, so not overwritable)
  helper.index(flatdoc("id", (int64_t)123, "body_w", "one"), UpdateMessage::COMMIT, true);
  helper.index(flatdoc("id", "123", "body_w", "two"), UpdateMessage::COMMIT, true);

  auto req = localReq(helper.getSearchEngine());
  req->collection("main").topDocs("q").allQuery().fields({"id"}).limit(-1);
  req->execute();
  auto docs = req->getDocs();
  ASSERT_EQ(1u, docs.size());  // same id: second write overwrote the first
  EXPECT_TRUE(containsDoc(docs, flatdoc("id", "123")));
  helper.clear();
}

// ---- multi-valued text: arrays are now analyzed and searchable ----

TEST_F(ValCoerceTest, multiValuedTextIsSearchable) {
  CollectionHelper helper("main");
  helper.clear();

  auto schema = Schema::createDefaultSchema();
  schema->fieldTypeMap["tags"] = std::make_shared<TextFieldType>(
      "tags", FieldType::INDEX_DOCS_FREQS_POSITIONS | FieldType::MULTI_VALUED, "whitespace");
  helper.collection().setSchema(schema);

  helper.index(flatdoc("id", "d1", "tags", std::vector<std::string>{"red fish", "blue fish"}),
               UpdateMessage::COMMIT);

  // terms from BOTH values match (they used to index an empty string)
  EXPECT_EQ(1u, matchDocs(helper, "tags", "red").size());
  EXPECT_EQ(1u, matchDocs(helper, "tags", "blue").size());

  // phrases match within a value but not across the value boundary
  auto phraseCount = [&](std::string_view text) {
    auto req = localReq(helper.getSearchEngine());
    req->collection("main").topDocs("q").phraseText("tags", text).withStats();
    req->execute();
    EXPECT_TRUE(req->ok());
    return req->getMatchCount();
  };
  EXPECT_EQ(1, phraseCount("red fish"));
  EXPECT_EQ(1, phraseCount("blue fish"));
  EXPECT_EQ(0, phraseCount("fish blue"));  // crosses the boundary: the gap forbids it

  helper.clear();
  helper.collection().setSchema(Schema::createDefaultSchema());
}
