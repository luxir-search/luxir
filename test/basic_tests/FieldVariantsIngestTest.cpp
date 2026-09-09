// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "test/LuxirTest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/SchemaBuilder.h"
#include "test/QueryBuild.h"
#include "test/TestUtils.h"
#include "luxir/reader/StoredFieldsReader.h"

using namespace luxir;
using namespace luxir::test;
using FieldClass = api::FieldDef::FieldClass;
using Status = api::UpdateResponse_::Status;

class FieldVariantsIngestTest : public LuxirTest {
protected:
  static void authorSchema(CollectionHelper& helper, bool multi = false) {
    SchemaBuilder b;
    auto& author = b.field("author");
    author.type = FieldClass::TEXT;
    author.multi = multi;
    auto& s = b.variant(author, "s");
    s.type = FieldClass::STRING;
    b.normalizer(s, {"nfkc_cf", "fold"});
    b.set(helper.collection());
  }

  static std::vector<Doc> match(CollectionHelper& helper, std::string_view field, std::string_view value,
                                 std::initializer_list<std::string> fields = {"id"}) {
    auto req = localReq(helper.getSearchEngine());
    req->collection("main").topDocs("q").matchQuery(field, value).fields(fields).limit(-1);
    req->execute();
    return req->getDocs();
  }
};

TEST_F(FieldVariantsIngestTest, authorFanoutAndOneStoredSource) {
  CollectionHelper helper("main");
  authorSchema(helper);
  ASSERT_TRUE(helper.index(flatdoc("id", "a", "author", "Ursula K. Le Guin"), UpdateMessage::COMMIT).success);
  EXPECT_EQ(1u, match(helper, "author", "Ursula").size());
  auto docs = match(helper, "author__s", "ursula k. le guin", {"author"});
  ASSERT_EQ(1u, docs.size());
  EXPECT_TRUE(containsDoc(docs, flatdoc("author", "Ursula K. Le Guin")));
  EXPECT_TRUE(match(helper, "author__s", "ursula").empty());

  auto reader = helper.getIndexWriter()->getIndexReader();
  ASSERT_EQ(1u, reader->segments().size());
  FieldReader fields(reader->segments()[0].postingsReader());
  ASSERT_TRUE(fields.seek("author__s"));
  SegFieldInfo info;
  fields.readFieldInfo(info);
  EXPECT_EQ(0, info.flags & (FieldType::ABSTRACT | FieldType::DERIVED));
  auto stored = StoredFieldsReader::open(reader->segments()[0].postingsReader());
  ASSERT_NE(nullptr, stored);
  int copies = 0;
  stored->readDoc(0, [&](std::string_view name, std::span<const std::string_view> values) {
    EXPECT_EQ("author", name);
    ASSERT_EQ(1u, values.size());
    EXPECT_EQ("Ursula K. Le Guin", values[0]);
    copies++;
  });
  EXPECT_EQ(1, copies);
}

TEST_F(FieldVariantsIngestTest, onlyStoredPrimariesWriteSourceValues) {
  for (bool keepSource : {false, true}) {
    SCOPED_TRACE(keepSource);
    CollectionHelper helper("main");
    helper.clear();
    SchemaBuilder b;
    b.field("exact").type = FieldClass::STRING;
    auto& text = b.field("text");
    text.type = FieldClass::TEXT;
    text.stored = false;
    auto& kept = b.field("kept");
    kept.type = FieldClass::TEXT;
    kept.stored = keepSource;
    b.set(helper.collection());
    ASSERT_TRUE(helper.index(flatdoc("id", "a", "exact", "exact value", "text", "text value",
                                     "dynamic_s", "dynamic value", "kept", "kept value"),
                             UpdateMessage::COMMIT).success);
    auto docs = match(helper, "id", "a");
    ASSERT_EQ(1u, docs.size());
    EXPECT_TRUE(containsDoc(docs, flatdoc("id", "a")));

    auto reader = helper.getIndexWriter()->getIndexReader();
    ASSERT_EQ(1u, reader->segments().size());
    auto stored = StoredFieldsReader::open(reader->segments()[0].postingsReader());
    if (!keepSource) {
      EXPECT_EQ(nullptr, stored);
      continue;
    }
    ASSERT_NE(nullptr, stored);
    for (auto name : {"id", "exact", "text", "dynamic_s"}) EXPECT_FALSE(stored->hasField(name)) << name;
    int copies = 0;
    stored->readDoc(0, [&](std::string_view name, std::span<const std::string_view> values) {
      EXPECT_EQ("kept", name);
      ASSERT_EQ(1u, values.size());
      EXPECT_EQ("kept value", values[0]);
      copies++;
    });
    EXPECT_EQ(1, copies);
  }
}

TEST_F(FieldVariantsIngestTest, editionPreservesSubmittedLexicalValue) {
  CollectionHelper helper("main");
  SchemaBuilder b;
  auto& edition = b.field("edition");
  edition.type = FieldClass::INT;
  edition.index = api::FieldDef::IndexMode::RANGE;
  b.variant(edition, "label").type = FieldClass::STRING;
  b.set(helper.collection());
  ASSERT_TRUE(helper.index(flatdoc("id", "e", "edition", "0042"), UpdateMessage::COMMIT).success);
  EXPECT_EQ(1u, match(helper, "edition", "42").size());
  auto docs = match(helper, "edition__label", "0042", {"edition", "edition__label"});
  ASSERT_EQ(1u, docs.size());
  EXPECT_TRUE(containsDoc(docs, flatdoc("edition", 42, "edition__label", "0042")));
  EXPECT_TRUE(match(helper, "edition__label", "42").empty());
}

TEST_F(FieldVariantsIngestTest, dynamicTemplateUsesVariantPrototype) {
  CollectionHelper helper("main");
  SchemaBuilder b;
  auto& text = b.templ("_t");
  text.type = FieldClass::TEXT;
  b.variant(text, "s").type = FieldClass::INT;
  b.set(helper.collection());
  ASSERT_TRUE(helper.index(flatdoc("id", "d", "book_t", "0042"), UpdateMessage::COMMIT).success);
  EXPECT_EQ(1u, match(helper, "book_t", "0042").size());
  auto docs = match(helper, "book_t__s", "42", {"book_t", "book_t__s"});
  ASSERT_EQ(1u, docs.size());
  EXPECT_TRUE(containsDoc(docs, flatdoc("book_t", "0042", "book_t__s", 42)));
  auto reader = helper.getIndexWriter()->getIndexReader();
  ASSERT_EQ(1u, reader->segments().size());
  FieldReader fields(reader->segments()[0].postingsReader());
  ASSERT_TRUE(fields.seek("book_t__s"));
  SegFieldInfo info;
  fields.readFieldInfo(info);
  EXPECT_EQ(0, info.flags & (FieldType::ABSTRACT | FieldType::DERIVED));
}

TEST_F(FieldVariantsIngestTest, multiFansOutAndPreservesStoredOrder) {
  CollectionHelper helper("main");
  authorSchema(helper, true);
  std::vector<std::string> authors{"Le Guin", "TOLKIEN", "Le Guin"};
  ASSERT_TRUE(helper.index(flatdoc("id", "m", "author", authors), UpdateMessage::COMMIT).success);
  EXPECT_EQ(1u, match(helper, "author", "TOLKIEN").size());
  EXPECT_EQ(1u, match(helper, "author__s", "le guin").size());
  auto docs = match(helper, "author__s", "tolkien", {"author", "author__s"});
  ASSERT_EQ(1u, docs.size());
  EXPECT_TRUE(containsDoc(docs, flatdoc("author", authors, "author__s", std::vector<std::string>{"le guin", "tolkien"})));
}

TEST_F(FieldVariantsIngestTest, laterBranchFailureRollsBackAndReusesInverter) {
  for (bool allOrNone : {false, true}) {
    SCOPED_TRACE(allOrNone);
    CollectionHelper helper("main");
    SchemaBuilder b;
    auto& value = b.field("value");
    value.type = FieldClass::TEXT;
    b.variant(value, "a").type = FieldClass::STRING;
    b.variant(value, "z").type = FieldClass::INT;
    b.set(helper.collection());
    auto writer = helper.getIndexWriter();
    auto& inverter = writer->obtainInverter();
    auto seg = inverter.getSegId();
    writer->releaseInverter(inverter, false);

    CollectionHelper::UpdateBuilder batch;
    batch.add(flatdoc("id", "old", "value", "10"));
    batch.add(flatdoc("id", "retry", "value", "badnumber"));
    batch.add(flatdoc("id", "tail", "value", "20"));
    batch.allOrNone(allOrNone).overwrite();
    auto result = helper.submit(batch);
    ASSERT_EQ(allOrNone ? Status::ERROR : Status::PARTIAL, result.status);
    ASSERT_EQ(1u, result.errors.size());
    EXPECT_EQ("invalid_value", result.errors[0].code);
    EXPECT_NE(std::string::npos, result.errors[0].error_message.find("Field 'value' branch 'z'"));
    EXPECT_NE(std::string::npos, result.errors[0].error_message.find("badnumber"));

    auto& reused = writer->obtainInverter();
    EXPECT_EQ(seg, reused.getSegId());
    EXPECT_EQ(allOrNone ? 2 : 3, reused.getMaxDoc());
    writer->releaseInverter(reused, false);
    ASSERT_TRUE(helper.index(flatdoc("id", "retry", "value", "30"), UpdateMessage::COMMIT, true).success);
    EXPECT_TRUE(match(helper, "value", "badnumber").empty());
    EXPECT_TRUE(match(helper, "value__a", "badnumber").empty());
    EXPECT_EQ(allOrNone ? 0u : 1u, match(helper, "value__z", "10").size());
    EXPECT_EQ(allOrNone ? 0u : 1u, match(helper, "value__z", "20").size());
    EXPECT_EQ(1u, match(helper, "value", "30").size());
    EXPECT_EQ(1u, match(helper, "value__a", "30").size());
    EXPECT_EQ(1u, match(helper, "value__z", "30").size());
    helper.clear();
  }
}

TEST_F(FieldVariantsIngestTest, derivedKeysAreValidatedAfterMappingBeforeCachedLookup) {
  CollectionHelper helper("main");
  authorSchema(helper);
  ASSERT_TRUE(helper.index(flatdoc("id", "seed", "author", "seed"), UpdateMessage::NO_COMMIT).success);
  auto bad = helper.index(flatdoc("id", "bad", "author__s", "bad"), UpdateMessage::NO_COMMIT);
  ASSERT_EQ(Status::ERROR, bad.status);
  ASSERT_EQ(1u, bad.errors.size());
  EXPECT_EQ("invalid_field_name", bad.errors[0].code);
  EXPECT_TRUE(bad.error_message.empty());

  CollectionHelper::UpdateBuilder badMap;
  badMap.add(flatdoc("id", "badmap", "author", "badmap")).fieldMap({{"external", "author__s"}});
  auto result = helper.submit(badMap);
  EXPECT_EQ(Status::ERROR, result.status);
  EXPECT_TRUE(result.errors.empty());
  EXPECT_NE(std::string::npos, result.error_message.find("field_map target"));

  CollectionHelper::UpdateBuilder goodMap;
  goodMap.add(flatdoc("id", "mapped", "first", "loser", "external__author", "Winner", "drop__me", "ignored"))
      .fieldMap({{"first", "author"}, {"external__author", "author"}, {"drop__me", ""}}).commit();
  ASSERT_EQ(Status::OK, helper.submit(goodMap).status);
  EXPECT_TRUE(match(helper, "author", "loser").empty());
  EXPECT_TRUE(match(helper, "author__s", "loser").empty());
  auto docs = match(helper, "author__s", "winner", {"author"});
  ASSERT_EQ(1u, docs.size());
  EXPECT_TRUE(containsDoc(docs, flatdoc("author", "Winner")));
}

TEST_F(FieldVariantsIngestTest, normalizedLengthLimitAndRecoveryForBothStringLayouts) {
  for (bool indexed : {false, true}) {
    SCOPED_TRACE(indexed);
    CollectionHelper helper("main");
    SchemaBuilder b;
    auto& value = b.field("value");
    value.type = FieldClass::STRING;
    value.multi = true;
    value.index = indexed ? api::FieldDef::IndexMode::MATCH : api::FieldDef::IndexMode::NONE;
    b.normalizer(value, {"nfkc_cf", "fold"});
    b.set(helper.collection());
    // UTF-8 e-acute folds from 256 source bytes to 128 bytes.
    std::string accents;
    for (int i = 0; i < 128; i++) accents += "\xc3\xa9";
    std::string shrinking(254, 'x');
    shrinking += "\xc4\xb0"; // dotted capital I becomes i + combining dot, then fold removes the dot
    CollectionHelper::UpdateBuilder batch;
    batch.add(flatdoc("id", "bad", "value", std::vector<std::string>{"first", std::string(256, 'x')}));
    batch.add(flatdoc("id", "good", "value", std::vector<std::string>{"CAFE", accents, std::string(255, 'x'), shrinking}));
    batch.commit();
    auto result = helper.submit(batch);
    ASSERT_EQ(Status::PARTIAL, result.status);
    ASSERT_EQ(1u, result.errors.size());
    EXPECT_EQ("invalid_value", result.errors[0].code);
    EXPECT_NE(std::string::npos, result.errors[0].error_message.find("maximum is 255"));
    auto req = localReq(helper.getSearchEngine());
    req->collection("main").topDocs("q").allQuery().fields({"id", "value"});
    req->execute();
    auto docs = req->getDocs();
    ASSERT_EQ(1u, docs.size());
    auto expected = std::vector<std::string>{"cafe", std::string(128, 'e'), std::string(255, 'x'), std::string(254, 'x') + "i"};
    if (indexed) std::sort(expected.begin(), expected.end());
    EXPECT_TRUE(containsDoc(docs, flatdoc("id", "good", "value", expected)));
    helper.clear();
  }
}

TEST_F(FieldVariantsIngestTest, normalizationCanExpandPastLimit) {
  CollectionHelper helper("main");
  SchemaBuilder b;
  for (auto name : {"indexed", "column"}) {
    auto& field = b.field(name);
    field.type = FieldClass::STRING;
    if (std::string_view(name) == "column") field.index = api::FieldDef::IndexMode::NONE;
    b.normalizer(field, {"nfkc_cf"});
  }
  b.set(helper.collection());
  std::string value = std::string(253, 'x') + "\xc4\xb0"; // 255 bytes -> 256 after folding
  for (auto name : {"indexed", "column"}) {
    auto result = helper.index(flatdoc(name, value), UpdateMessage::NO_COMMIT);
    ASSERT_EQ(Status::ERROR, result.status);
    ASSERT_EQ(1u, result.errors.size());
    EXPECT_NE(std::string::npos, result.errors[0].error_message.find("256 bytes after normalization"));
  }
  ASSERT_TRUE(helper.index(flatdoc("indexed", "OK", "column", "OK"), UpdateMessage::COMMIT).success);
  EXPECT_EQ(1u, match(helper, "indexed", "ok").size());
}

TEST_F(FieldVariantsIngestTest, directOverloadsUseBothRegistriesAndGlobalPhysicalOrder) {
  CollectionHelper helper("main");
  SchemaBuilder b;
  auto& a = b.field("a");
  a.type = FieldClass::TEXT;
  a.multi = true;
  b.variant(a, "s").type = FieldClass::STRING;
  auto& col = b.variant(a, "col");
  col.type = FieldClass::STRING;
  col.index = api::FieldDef::IndexMode::NONE;
  b.field("aB").type = FieldClass::INT; // sorts between the primary and its variants
  auto& point = b.field("point");
  point.type = FieldClass::GEO_POINT;
  point.multi = true;
  b.variant(point, "copy").type = FieldClass::GEO_POINT;
  b.set(helper.collection());
  auto writer = helper.getIndexWriter();
  auto& inv = writer->obtainInverter(1);
  inv.overwrite = true;
  auto& input = inv.getIndexHandler("a");
  EXPECT_EQ(&input, &inv.getIndexHandler("a"));
  EXPECT_EQ(1u, inv.inputHandlers.size());
  EXPECT_EQ(3u, inv.indexHandlers.size());
  const std::string_view strings[]{"0042", "0043"};
  const int64_t ints[]{44, 45};
  const GeoPoint points[]{{10, 20}, {30, 40}};
  for (int i = 0; i < 4; i++) {
    inv.startDoc();
    inv.getIndexHandler("id").index(inv, std::to_string(i));
    inv.getIndexHandler("aB").index(inv, (int64_t)i);
    if (i == 0) input.index(inv, std::string_view("0041"));
    if (i == 1) input.index(inv, std::span<const std::string_view>(strings));
    if (i == 2) input.index(inv, (int64_t)46);
    if (i == 3) input.index(inv, std::span<const int64_t>(ints));
    auto& geo = inv.getIndexHandler("point");
    if (i % 2) geo.index(inv, std::span<const GeoPoint>(points));
    else geo.index(inv, 10.0, 20.0);
    inv.finishDoc();
  }
  EXPECT_TRUE(inv.inputHandlers.contains("_version_"));
  EXPECT_TRUE(inv.indexHandlers.contains("_version_"));
  EXPECT_FALSE(inv.inputHandlers.contains("a__s"));
  writer->releaseInverter(inv, true);
  helper.commit();
  for (auto val : {"0041", "0042", "0043", "44", "45", "46"}) {
    EXPECT_EQ(1u, match(helper, "a", val).size());
    EXPECT_EQ(1u, match(helper, "a__s", val).size());
  }
  auto req = localReq(helper.getSearchEngine());
  req->collection("main").topDocs("q").allQuery().fields({"a", "a__col", "_version_"});
  req->execute();
  auto docs = req->getDocs();
  ASSERT_EQ(4u, docs.size());
  for (const auto& doc : docs) {
    ASSERT_NE(nullptr, find(doc, "a"));
    ASSERT_NE(nullptr, find(doc, "a__col"));
    EXPECT_EQ(*find(doc, "a"), *find(doc, "a__col"));
    EXPECT_EQ((int64_t)1, std::get<int64_t>(*find(doc, "_version_")));
  }
  EXPECT_EQ(1u, match(helper, "aB", "3").size());
  for (auto name : {"point", "point__copy"}) {
    auto geoReq = localReq(helper.getSearchEngine());
    auto& op = geoReq->collection("main").topDocs("q");
    op.rawQuery() = qb::geoBox(op.mr(), name, 29, 31, 39, 41);
    op.fields({"id"});
    geoReq->execute();
    EXPECT_EQ(2u, geoReq->getDocs().size());
  }

}

TEST_F(FieldVariantsIngestTest, logicalCardinalityIsCheckedBeforeStorageAndFanout) {
  CollectionHelper helper("main");
  SchemaBuilder b;
  auto& author = b.field("author");
  author.type = FieldClass::TEXT;
  auto& col = b.variant(author, "col");
  col.type = FieldClass::STRING;
  col.index = api::FieldDef::IndexMode::NONE;
  b.set(helper.collection());
  auto writer = helper.getIndexWriter();
  auto& inv = writer->obtainInverter();
  auto& input = inv.getIndexHandler("author");
  const std::string_view tooMany[]{"first", "second"};
  inv.startDoc();
  EXPECT_THROW(input.index(inv, std::span<const std::string_view>(tooMany)), DocumentError);
  // No append occurred: the same doc can still receive one valid value.
  input.index(inv, std::span<const std::string_view>(tooMany, 1));
  inv.finishDoc();
  inv.startDoc();
  input.index(inv, std::span<const std::string_view>());
  inv.finishDoc();
  writer->releaseInverter(inv, true);
  helper.commit();
  auto docs = match(helper, "author", "first", {"author", "author__col"});
  EXPECT_TRUE(containsDoc(docs, flatdoc("author", "first", "author__col", "first")));
}

TEST_F(FieldVariantsIngestTest, storedAuthorSurvivesLaterStringBranchFailure) {
  CollectionHelper helper("main");
  authorSchema(helper);
  CollectionHelper::UpdateBuilder batch;
  batch.add(flatdoc("id", "same", "author", std::string(256, 'x')));
  batch.add(flatdoc("id", "same", "author", "Valid Author"));
  batch.overwrite().commit();
  auto result = helper.submit(batch);
  ASSERT_EQ(Status::PARTIAL, result.status);
  ASSERT_EQ(1u, result.errors.size());
  EXPECT_NE(std::string::npos, result.errors[0].error_message.find("Field 'author' branch 's'"));
  auto docs = match(helper, "author__s", "valid author", {"author"});
  EXPECT_TRUE(containsDoc(docs, flatdoc("author", "Valid Author")));
  EXPECT_TRUE(match(helper, "author", std::string(255, 'x')).empty());
}

TEST_F(FieldVariantsIngestTest, lazySchemaRefreshKeepsExistingDispatchersAlive) {
  RAMDir dir;
  auto schema = Schema::createDefaultSchema();
  int acquisitions = 0;
  Inverter inv(dir, 1, [&] { acquisitions++; return schema; });
  EXPECT_EQ(0, acquisitions);
  auto& existing = inv.getIndexHandler("old_s");
  EXPECT_EQ(1, acquisitions);
  EXPECT_EQ(&existing, &inv.getIndexHandler("old_s"));
  EXPECT_EQ(1, acquisitions);
  SchemaBuilder b;
  auto& added = b.field("added");
  added.type = FieldClass::TEXT;
  b.variant(added, "s").type = FieldClass::STRING;
  schema = b.build(schema.get());
  auto& addedInput = inv.getIndexHandler("added");
  EXPECT_EQ(2, acquisitions);
  inv.startDoc();
  existing.index(inv, "old");
  addedInput.index(inv, "new");
  inv.finishDoc();
  EXPECT_EQ(2u, inv.inputHandlers.size());
  EXPECT_EQ(3u, inv.indexHandlers.size());
  EXPECT_TRUE(inv.flush());
}

TEST_F(FieldVariantsIngestTest, vectorBranchesFinishTheirRawColumns) {
  CollectionHelper helper("main");
  SchemaBuilder b;
  auto& vec = b.field("vec");
  vec.type = FieldClass::VECTOR;
  vec.dims = 80;
  auto& copy = b.variant(vec, "copy");
  copy.type = FieldClass::VECTOR;
  copy.dims = 80;
  b.set(helper.collection());
  // Each raw column value exceeds the STRING byte limit.
  std::vector<float> values(80, 1.0f);
  ASSERT_TRUE(helper.index(flatdoc("vec", values), UpdateMessage::COMMIT).success);
  auto req = localReq(helper.getSearchEngine());
  req->collection("main").topDocs("q").allQuery().fields({"vec", "vec__copy"});
  req->execute();
  EXPECT_TRUE(containsDoc(req->getDocs(), flatdoc("vec", values, "vec__copy", values)));
}
