// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// Stored TEXT/STRING values served from _source_: left out of stored fields
// for documents whose source holds them, read back as ingest stored them, kept
// stored where the source cannot give them back (filters, field_map, a
// stored_copy field), carried by merges, and moved into stored fields by the
// merge that drops the sources.

#include <gtest/gtest.h>

#include "luxir/index/IndexWriter.h"
#include "luxir/reader/Postings.h"
#include "luxir/reader/StoredFieldsReader.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/SchemaBuilder.h"
#include "test/TestUtils.h"

namespace luxir::test {

namespace {

using Resource = api::StoredResourceDef;

class SourceBackedStoredTest : public LuxirTest {
protected:
  CollectionHelper helper;

  void enableSource(const std::function<void(Resource&)>& configure = {}) {
    SchemaBuilder b;
    auto& source = b.storedResource(Postings::SOURCE_RESOURCE);
    if (configure) configure(source);
    auto& multi = b.field("body_ts");
    multi.parent = "_t";
    multi.multi = api::FieldDef::MultiMode::ARRAY;
    b.set(helper.collection());
  }

  IndexResult forceMerge() {
    CollectionHelper::UpdateBuilder request;
    request.commit(true, 1);
    return helper.submit(request);
  }

  // `field` of every live doc in doc order: a string, values joined with '|',
  // or "-" when the doc returns none.
  std::vector<std::string> values(std::string_view field) {
    auto req = localReq(helper.getSearchEngine());
    req->collection("main").topDocs("q").allQuery().fields({std::string(field)}).limit(-1);
    req->execute();
    EXPECT_TRUE(req->ok()) << req->toString();
    std::vector<std::string> out;
    for (const auto& doc : req->getDocs()) {
      const auto* value = find(doc, field);
      if (const auto* text = value ? std::get_if<std::string>(value) : nullptr) {
        out.push_back(*text);
      } else if (const auto* list = value ? std::get_if<std::vector<std::string>>(value) : nullptr) {
        std::string joined;
        for (const auto& v : *list) joined += (joined.empty() ? "" : "|") + v;
        out.push_back(joined);
      } else {
        out.push_back("-");
      }
    }
    return out;
  }

  // Whether doc `doc` of the first segment has `field` in its stored fields.
  bool storedHas(std::string_view field, int32_t doc) {
    auto reader = helper.getIndexWriter()->snapshots.readers.getReader();
    auto& postings = reader->segments()[0].postingsReader();
    const auto* info = postings.fieldInfo(Postings::STORED_DEFAULT_RESOURCE);
    if (!info) return false;
    StoredFieldsReader stored(postings, *info);
    return stored.readField(doc, field, [](std::span<const std::string_view>) {});
  }
};

}  // namespace

TEST_F(SourceBackedStoredTest, textIsServedFromSourceAsStored) {
  enableSource();
  auto r0 = helper.indexAll({flatdoc("id", "a", "title_t", "say \"hi\" \\ caf\xc3\xa9", "body_ts", vecs("x", "y"),
                                       "n_t", 3.0, "s_s", "k"),
                               flatdoc("id", "b", "title_t", "second", "title_t", "last wins")},
                              UpdateMessage::COMMIT);
  ASSERT_TRUE(r0.success);
  for (auto& e : r0.errors) ADD_FAILURE() << e.index << ": " << e.error_message;
  EXPECT_EQ((std::vector<std::string>{"say \"hi\" \\ caf\xc3\xa9", "last wins"}), values("title_t"));
  EXPECT_EQ((std::vector<std::string>{"x|y", "-"}), values("body_ts"));
  EXPECT_EQ((std::vector<std::string>{"3", "-"}), values("n_t"));  // canonical text, as stored
  EXPECT_FALSE(storedHas("title_t", 0));
  EXPECT_FALSE(storedHas("body_ts", 0));
}

TEST_F(SourceBackedStoredTest, storedCopyKeepsBoth) {
  enableSource();
  SchemaBuilder b;
  auto& title = b.field("title_t");
  title.type = api::FieldDef::FieldClass::TEXT;
  title.stored_copy = true;
  b.set(helper.collection());
  ASSERT_TRUE(helper.index(flatdoc("id", "a", "title_t", "kept", "body_t", "src"), UpdateMessage::COMMIT).success);
  EXPECT_TRUE(storedHas("title_t", 0));
  EXPECT_FALSE(storedHas("body_t", 0));
  EXPECT_EQ((std::vector<std::string>{"kept"}), values("title_t"));
  EXPECT_EQ((std::vector<std::string>{"src"}), values("body_t"));
}

TEST_F(SourceBackedStoredTest, sourcesThatCannotGiveTheValueBackStoreIt) {
  enableSource([](Resource& r) {
    static const std::string_view patterns[] = {"secret_t"};
    r.excludes = patterns;
  });
  ASSERT_TRUE(helper.index(flatdoc("id", "a", "secret_t", "hidden"), UpdateMessage::COMMIT).success);
  EXPECT_TRUE(storedHas("secret_t", 0));
  EXPECT_EQ((std::vector<std::string>{"hidden"}), values("secret_t"));

  helper.clear();
  enableSource();
  CollectionHelper::UpdateBuilder request;
  request.add(flatdoc("id", "b", "raw", "renamed", "title_t", "decoy"));
  request.fieldMap({{"raw", "title_t"}, {"title_t", ""}}).commit();
  ASSERT_TRUE(helper.submit(request).success);
  EXPECT_TRUE(storedHas("title_t", 0));
  EXPECT_EQ((std::vector<std::string>{"renamed"}), values("title_t"));
}

TEST_F(SourceBackedStoredTest, mergesCarrySourceBackedFields) {
  enableSource();
  ASSERT_TRUE(helper.index(flatdoc("id", "a", "title_t", "one"), UpdateMessage::COMMIT).success);
  ASSERT_TRUE(helper.index(flatdoc("id", "b", "title_t", "two"), UpdateMessage::COMMIT).success);
  ASSERT_TRUE(helper.deleteByIds({"a"}, UpdateMessage::COMMIT).success);
  ASSERT_TRUE(helper.index(flatdoc("id", "c", "title_t", "three"), UpdateMessage::COMMIT).success);
  ASSERT_TRUE(forceMerge().success);
  EXPECT_EQ((std::vector<std::string>{"two", "three"}), values("title_t"));
  EXPECT_FALSE(storedHas("title_t", 0));
}

TEST_F(SourceBackedStoredTest, mergeThatDropsSourcesStoresTheirValues) {
  enableSource();
  ASSERT_TRUE(helper.indexAll({flatdoc("id", "a", "title_t", "one", "body_ts", vecs("p", "q")),
                               flatdoc("id", "b", "title_t", "two")},
                              UpdateMessage::COMMIT)
                  .success);
  ASSERT_TRUE(helper.index(flatdoc("id", "c", "title_t", "three"), UpdateMessage::COMMIT).success);
  enableSource([](Resource& r) { r.enabled = false; });
  ASSERT_TRUE(forceMerge().success);
  EXPECT_EQ(nullptr, helper.getIndexWriter()->snapshots.readers.getReader()->segments()[0].source());
  EXPECT_TRUE(storedHas("title_t", 0));
  EXPECT_EQ((std::vector<std::string>{"one", "two", "three"}), values("title_t"));
  EXPECT_EQ((std::vector<std::string>{"p|q", "-", "-"}), values("body_ts"));
}

TEST_F(SourceBackedStoredTest, storedFalseAndSourceOffKeepNeither) {
  SchemaBuilder b;
  auto& title = b.field("title_t");
  title.type = api::FieldDef::FieldClass::TEXT;
  title.stored = false;
  b.set(helper.collection());
  ASSERT_TRUE(helper.index(flatdoc("id", "a", "title_t", "gone", "body_t", "kept"), UpdateMessage::COMMIT).success);
  EXPECT_FALSE(storedHas("title_t", 0));
  EXPECT_TRUE(storedHas("body_t", 0));
}

}  // namespace luxir::test
