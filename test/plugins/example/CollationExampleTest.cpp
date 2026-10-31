// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory_resource>
#include <string>
#include <vector>

#include "luxir/api/luxir_index.hpp"
#include "luxir/plugin/Registries.h"
#include "luxir/schema/Schema.h"
#include "luxir/server/Collection.h"
#include "luxir/store/FSDirectory.h"
#include "luxir/store/Manifest.h"
#include "test/CollectionHelper.h"
#include "test/DurableIndexInfo.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/QueryBuild.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

namespace {

// A sort representation of `title` ordered by the example_ci collation, bound
// as the field's value, so sorting and facets on `title` use it.
std::string schemaWith(std::string_view collation) {
  return R"({"fields":{"title":{"type":"string","variants":{"sort":{"type":"string","index":"none","column":true,)"
         R"("collation":)" + std::string(collation) + R"(}},"defaults":{"value":"sort"}}}})";
}

std::string schemaError(std::string_view json) {
  std::pmr::monotonic_buffer_resource arena;
  api::SchemaDef def;
  std::string error;
  if (!api::read_json(def, json, arena, &error)) return "parse: " + error;
  try {
    Schema::fromProto(def);
  } catch (const SchemaError& e) {
    return e.what();
  }
  return "";
}

}  // namespace

class CollationExampleTest : public LuxirTest {
protected:
  std::filesystem::path path = std::filesystem::temp_directory_path() /
      ("luxir-collation-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

  void TearDown() override { std::filesystem::remove_all(path); }

  static void putSchema(Collection& collection, std::string_view json) {
    std::pmr::monotonic_buffer_resource arena;
    api::SchemaDef def;
    std::string error;
    ASSERT_TRUE(api::read_json(def, json, arena, &error)) << error;
    collection.updateSchema(def, api::SchemaRequest_::Mode::SET);
  }

  static std::vector<std::string> sorted(CollectionHelper& helper, std::string_view field, qb::SortDir dir,
                                         std::string_view collection = "main") {
    auto req = localReq(helper.getSearchEngine());
    req->collection(collection);
    auto& top = req->topDocs("q").allQuery().limit(10).fields({"id"});
    qb::sort(top, field, dir);
    req->execute(false);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    const auto* docs = req->docList("q");
    if (docs == nullptr) return {};
    const auto& values = std::get<api::ColStr>(docs->columns.at("id").kind).v;
    return {values.begin(), values.end()};
  }

  // Docs d1..d5 in index order.
  static void indexTitles(CollectionHelper& helper) {
    const char* titles[] = {"b", "A", "a", "B", "c"};
    for (int i = 0; i < 5; i++) {
      ASSERT_TRUE(helper.index(flatdoc("id", "d" + std::to_string(i + 1), "title", titles[i]),
                               i == 4 ? UpdateMessage::COMMIT : UpdateMessage::NO_COMMIT).success);
    }
  }
};

TEST_F(CollationExampleTest, registeredAndFrozen) {
  auto& collation = Registries::global().collation;
  EXPECT_TRUE(collation.frozen());
  EXPECT_EQ((std::vector<std::string>{"example_ci"}), collation.names());
  api::AnalyzerComponent def;
  def.name = "example_ci";
  auto made = collation.make(def);
  EXPECT_EQ("example_ci", made->name);
  EXPECT_EQ(R"("example_ci")", made->canonical);
  EXPECT_THROW(collation.add("late", [](const api::AnalyzerComponent&) -> std::unique_ptr<const CollatorFactory> {
    return nullptr;
  }), std::logic_error);
}

TEST_F(CollationExampleTest, makerValidatesParamsAndNamesTeach) {
  auto error = [](std::string_view json) {
    std::pmr::monotonic_buffer_resource arena;
    api::AnalyzerComponent def;
    std::string err;
    EXPECT_TRUE(api::read_json(def, json, arena, &err)) << err;
    try {
      Registries::global().collation.make(def);
    } catch (const std::invalid_argument& e) {
      return std::string(e.what());
    }
    return std::string();
  };
  EXPECT_EQ("unknown collation 'fr'; valid collations: example_ci", error(R"("fr")"));
  EXPECT_EQ("collation 'example_ci': unknown parameter 'locale'; valid parameters: strength",
            error(R"({"name":"example_ci","params":{"locale":"fr"}})"));
  EXPECT_NE(std::string::npos, error(R"({"name":"example_ci","params":{"strength":"loud"}})").find("must be"));
  EXPECT_EQ("", error(R"({"name":"example_ci","params":{"strength":"primary"}})"));
  // The canonical form does not depend on how the parameters were written.
  std::pmr::monotonic_buffer_resource arena;
  api::AnalyzerComponent a, b;
  ASSERT_TRUE(api::read_json(a, R"({"name":"example_ci","params":{"strength":"primary"}})", arena));
  ASSERT_TRUE(api::read_json(b, R"({"params":{"strength":"primary"},"name":"example_ci"})", arena));
  EXPECT_EQ(Registries::global().collation.make(a)->canonical, Registries::global().collation.make(b)->canonical);
}

TEST_F(CollationExampleTest, collatorAppendsComparableKeys) {
  api::AnalyzerComponent def;
  def.name = "example_ci";
  auto collator = Registries::global().collation.make(def)->create();
  std::string key = "prefix:";
  collator->appendKey("Ab", key);
  EXPECT_EQ(std::string("prefix:ab\0\1\0", 12), key);
}

TEST_F(CollationExampleTest, schemaRejectsMisplacedCollations) {
  EXPECT_NE(std::string::npos,
            schemaError(R"({"fields":{"n":{"type":"int","collation":"example_ci"}}})")
                .find("collation is only valid for string fields (field: n)"));
  EXPECT_NE(std::string::npos,
            schemaError(R"({"fields":{"s":{"type":"string","collation":"example_ci"}}})")
                .find("needs index=none and column=true"));
  EXPECT_EQ("unknown collation 'fr'; valid collations: example_ci (collation on field: title__sort)",
            schemaError(schemaWith(R"("fr")")));
  EXPECT_NE(std::string::npos,
            schemaError(schemaWith(R"({"name":"example_ci","params":{"strength":"x"}})"))
                .find("(collation on field: title__sort)"));
  EXPECT_EQ("", schemaError(schemaWith(R"("example_ci")")));
}

// Sorting a field bound to a collated representation orders by the collation
// key, not by the raw bytes, in both directions.
TEST_F(CollationExampleTest, sortOrdersByCollationKey) {
  CollectionHelper helper;
  putSchema(helper.collection(), schemaWith(R"("example_ci")"));
  indexTitles(helper);
  // Raw byte order would put "A" and "B" before every lowercase letter.
  EXPECT_EQ((std::vector<std::string>{"d3", "d2", "d1", "d4", "d5"}), sorted(helper, "title", qb::ASC));
  EXPECT_EQ((std::vector<std::string>{"d5", "d4", "d1", "d2", "d3"}), sorted(helper, "title", qb::DESC));
  // The representation can also be addressed directly.
  EXPECT_EQ((std::vector<std::string>{"d3", "d2", "d1", "d4", "d5"}), sorted(helper, "title__sort", qb::ASC));
}

TEST_F(CollationExampleTest, collationParamsChangeTheOrder) {
  CollectionHelper helper;
  putSchema(helper.collection(), schemaWith(R"({"name":"example_ci","params":{"strength":"primary"}})"));
  indexTitles(helper);
  // primary strength ties "a"/"A" and "b"/"B", which then come in index order.
  EXPECT_EQ((std::vector<std::string>{"d2", "d3", "d1", "d4", "d5"}), sorted(helper, "title", qb::ASC));
}

// The unsorted primary keeps raw order, so a collation changes only the field naming it.
TEST_F(CollationExampleTest, fieldsWithoutCollationKeepByteOrder) {
  CollectionHelper helper;
  putSchema(helper.collection(), schemaWith(R"("example_ci")"));
  indexTitles(helper);
  EXPECT_EQ((std::vector<std::string>{"d2", "d4", "d3", "d1", "d5"}), sorted(helper, "title__self", qb::ASC));
}

// A changed collation is a different representation: its signature differs, so
// the schema history does not treat old segments as holding the new keys.
TEST_F(CollationExampleTest, signatureCarriesTheCollation) {
  auto build = [](std::string_view collation) {
    std::pmr::monotonic_buffer_resource arena;
    api::SchemaDef def;
    std::string json = schemaWith(collation);
    EXPECT_TRUE(api::read_json(def, json, arena));
    return Schema::fromProto(def);
  };
  auto tertiary = build(R"("example_ci")");
  auto primary = build(R"({"name":"example_ci","params":{"strength":"primary"}})");
  auto signature = [](const std::shared_ptr<Schema>& schema) {
    return schema->signatures().at("title__sort").properties.at("collation");
  };
  EXPECT_EQ(R"("example_ci")", signature(tertiary));
  EXPECT_EQ(R"({"name":"example_ci","params":{"strength":"primary"}})", signature(primary));
  auto first = tertiary->withHistory(1, nullptr);
  EXPECT_EQ(2u, primary->withHistory(2, first.get())->introduction("title__sort"));  // new keys, new introduction
  EXPECT_EQ(1u, build(R"("example_ci")")->withHistory(2, first.get())->introduction("title__sort"));  // unchanged
}

// The authored schema reads back with the collation, and a stored primary that
// names one still returns its source text rather than the keys in its column.
TEST_F(CollationExampleTest, authoredSchemaEchoesAndStoredTextIsNotKeys) {
  CollectionHelper helper;
  putSchema(helper.collection(),
            R"({"fields":{"t":{"type":"string","index":"none","column":true,"stored":true,"collation":"example_ci"}}})");
  ASSERT_TRUE(helper.index(flatdoc("id", "d1", "t", "MiXed"), UpdateMessage::COMMIT).success);
  std::pmr::monotonic_buffer_resource arena;
  api::SchemaDef def;
  helper.collection().getSchema()->toProto(&def, arena);
  std::string json;
  ASSERT_TRUE(api::write_json(def, json));
  EXPECT_NE(std::string::npos, json.find(R"("collation":"example_ci")")) << json;
  auto req = localReq(helper.getSearchEngine());
  req->collection("main").topDocs("q").allQuery().limit(1).fields({"t"});
  req->execute(false);
  ASSERT_TRUE(req->ok()) << req->errorMsg();
  EXPECT_EQ("MiXed", std::string(std::get<api::ColStr>(req->docList("q")->columns.at("t").kind).v.at(0)));
}

// The collation is part of the persisted schema: a reopened node sorts the
// same, and a binary without the collation refuses the collection with the
// "unknown ..." message while its other collections keep serving.
TEST_F(CollationExampleTest, persistedAndMissingCollationFailsTheCollectionOnly) {
  LuxirConfig config;
  config.store.backend = "fs";
  config.store.data_dir = path.string();
  {
    LuxirNode node(config);
    for (const char* name : {"good", "bad"}) {
      CollectionHelper helper(node, name);
      putSchema(helper.collection(), schemaWith(R"("example_ci")"));
      indexTitles(helper);
    }
  }
  {
    LuxirNode node(config);
    CollectionHelper good(node, "good");
    EXPECT_EQ((std::vector<std::string>{"d3", "d2", "d1", "d4", "d5"}), sorted(good, "title", qb::ASC, "good"));
  }
  // Rewrite the committed schema of "bad" as if the plugin were absent.
  FSDirectory container(path / "t" / "t0" / "bad");
  FSDirectory dir(path / "t" / "t0" / "bad" / *CollectionStorage::current(container));
  auto loaded = readDurableIndexInfo(dir);
  api::SchemaDef missing;
  std::string error;
  std::string missingJson = schemaWith(R"("from_a_missing_plugin")");  // the views point into it
  ASSERT_TRUE(api::read_json(missing, missingJson, *loaded.arena, &error)) << error;
  std::vector<std::byte> schemaBytes, manifest;
  ASSERT_TRUE(api::encode(missing, schemaBytes));
  loaded.info.schema->source_def = std::span<const std::byte>(schemaBytes);
  ASSERT_TRUE(api::encode(loaded.info, manifest));
  Manifest::write(dir, loaded.info.index_gen, manifest);

  LuxirNode node(config);
  CollectionHelper good(node, "good");
  EXPECT_EQ(5u, sorted(good, "title", qb::ASC, "good").size());
  try {
    node.getCollection("bad");
    FAIL() << "the collection with a missing collation loaded";
  } catch (const CollectionResolutionError& e) {
    std::string message = e.what();
    EXPECT_NE(std::string::npos, message.find("failed to load")) << message;
    EXPECT_NE(std::string::npos, message.find("unknown collation 'from_a_missing_plugin'; valid collations: example_ci"))
        << message;
  }
}
