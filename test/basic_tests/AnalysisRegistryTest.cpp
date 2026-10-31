// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory_resource>
#include <stdexcept>
#include <string>
#include <vector>

#include "luxir/analysis/AnalysisRegistry.h"
#include "luxir/api/luxir_index.hpp"
#include "luxir/plugin/Registries.h"
#include "luxir/server/Collection.h"
#include "luxir/store/DirectoryFactory.h"
#include "luxir/store/FSDirectory.h"
#include "luxir/store/Manifest.h"
#include "test/CollectionHelper.h"
#include "test/DurableIndexInfo.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

namespace {

api::AnalyzerComponent component(std::string_view name) {
  api::AnalyzerComponent c;
  c.name = name;
  return c;
}

std::string compileError(const AnalysisRegistry& registry, std::string_view json) {
  std::pmr::monotonic_buffer_resource arena;
  api::AnalyzerDef def;
  std::string err;
  if (!api::read_json(def, json, arena, &err)) return "parse: " + err;
  try {
    Analyzer::compile(def, registry);
  } catch (const std::invalid_argument& e) {
    return e.what();
  }
  return "";
}

std::shared_ptr<const Analyzer> compileWith(const AnalysisRegistry& registry, std::string_view json) {
  std::pmr::monotonic_buffer_resource arena;
  api::AnalyzerDef def;
  std::string err;
  if (!api::read_json(def, json, arena, &err)) throw std::runtime_error(err);
  return Analyzer::compile(def, registry);
}

}  // namespace

class AnalysisRegistryTest : public LuxirTest {
protected:
  std::filesystem::path path = std::filesystem::temp_directory_path() /
      ("luxir-analysis-registry-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

  void TearDown() override { std::filesystem::remove_all(path); }

  static void putSchema(Collection& collection, std::string_view json) {
    std::pmr::monotonic_buffer_resource arena;
    api::SchemaDef def;
    std::string error;
    ASSERT_TRUE(api::read_json(def, json, arena, &error)) << error;
    collection.updateSchema(def, api::SchemaRequest_::Mode::SET);
  }

  static int64_t matches(CollectionHelper& helper, std::string_view field, std::string_view value,
                         std::string_view collection = "main") {
    auto req = localReq(helper.getSearchEngine());
    req->collection(collection).topDocs("q").matchQuery(field, value).withStats();
    req->execute();
    return req->getMatchCount();
  }
};

TEST_F(AnalysisRegistryTest, builtinNamesAreListedSorted) {
  AnalysisRegistry registry;
  EXPECT_EQ((std::vector<std::string>{"keyword", "unicode_word", "whitespace"}), registry.tokenizerNames());
  EXPECT_EQ((std::vector<std::string>{"english_possessive", "fold", "kstem", "lowercase", "nfkc_cf", "stop"}),
            registry.filterNames());
  EXPECT_EQ((std::vector<std::string>{"html_strip", "mapping", "nfkc_cf"}), registry.charFilterNames());
  EXPECT_EQ((std::vector<std::string>{"english", "standard"}), registry.analyzerNames());
}

TEST_F(AnalysisRegistryTest, duplicatesAndBadNamesThrow) {
  AnalysisRegistry registry;
  auto tokenizer = [](const api::AnalyzerComponent&) -> std::unique_ptr<const TokenizerFactory> { return nullptr; };
  auto filter = [](const api::AnalyzerComponent&) -> std::unique_ptr<TokenFilterFactory> { return nullptr; };
  auto charFilter = [](const api::AnalyzerComponent&) -> std::unique_ptr<const CharFilterFactory> { return nullptr; };
  EXPECT_THROW(registry.addTokenizer("keyword", tokenizer), std::invalid_argument);
  try {
    registry.addFilter("stop", filter);
    FAIL();
  } catch (const std::invalid_argument& e) {
    EXPECT_STREQ("duplicate filter 'stop'", e.what());
  }
  EXPECT_THROW(registry.addCharFilter("html_strip", charFilter), std::invalid_argument);
  EXPECT_THROW(registry.addAnalyzer("english", api::AnalyzerDef{}), std::invalid_argument);
  registry.addCharFilter("mine", charFilter);
  EXPECT_THROW(registry.addCharFilter("mine", charFilter), std::invalid_argument);
  // the same name in different kinds is fine
  EXPECT_NO_THROW(registry.addFilter("mine", filter));
  for (const char* bad : {"", "Mixed", "has-dash", "sp ace"}) {
    EXPECT_THROW(registry.addFilter(bad, filter), std::invalid_argument) << bad;
  }
  EXPECT_THROW(registry.addFilter("empty_maker", nullptr), std::invalid_argument);
}

TEST_F(AnalysisRegistryTest, frozenRegistryRejectsEveryAdd) {
  AnalysisRegistry registry;
  registry.freeze();
  EXPECT_TRUE(registry.frozen());
  auto filter = [](const api::AnalyzerComponent&) -> std::unique_ptr<TokenFilterFactory> { return nullptr; };
  EXPECT_THROW(registry.addFilter("late", filter), std::logic_error);
  EXPECT_THROW(registry.addTokenizer("late", nullptr), std::logic_error);
  EXPECT_THROW(registry.addCharFilter("late", nullptr), std::logic_error);
  EXPECT_THROW(registry.addAnalyzer("late", api::AnalyzerDef{}), std::logic_error);
  // lookups keep working
  EXPECT_NE(nullptr, registry.findAnalyzer("english"));
  EXPECT_NO_THROW(registry.makeFilter(component("lowercase")));
  EXPECT_TRUE(globalAnalysisRegistry().frozen());
  EXPECT_THROW(Registries::global().analysis.addFilter("late", filter), std::logic_error);
}

TEST_F(AnalysisRegistryTest, unknownNamesListValidOnesSorted) {
  AnalysisRegistry registry;
  EXPECT_EQ("unknown filter 'stemmer'; valid filters: english_possessive, fold, kstem, lowercase, nfkc_cf, stop",
            compileError(registry, R"({"filters":["stemmer"]})"));
  EXPECT_EQ("unknown tokenizer 'standard'; valid tokenizers: keyword, unicode_word, whitespace",
            compileError(registry, R"({"tokenizer":"standard"})"));
  EXPECT_EQ("unknown char filter 'html'; valid char filters: html_strip, mapping, nfkc_cf",
            compileError(registry, R"({"char_filters":["html"]})"));
  EXPECT_EQ("unknown analyzer 'french'; valid analyzers: english, standard",
            compileError(registry, R"("french")"));
  EXPECT_NE(std::string::npos, compileError(registry, R"({"filters":[{"params":{}}]})").find("has no name"));
}

TEST_F(AnalysisRegistryTest, namedAnalyzerExpandsToTheAuthoredCanonicalForm) {
  AnalysisRegistry registry;
  auto named = compileWith(registry, R"({"name":"english"})");
  auto bare = compileWith(registry, R"("english")");
  auto authored = compileWith(registry,
      R"({"tokenizer":"unicode_word","filters":["nfkc_cf","fold","kstem"]})");
  EXPECT_EQ("english", named->presetName);
  EXPECT_EQ("english", bare->presetName);
  EXPECT_TRUE(authored->presetName.empty());
  EXPECT_EQ(authored->canonical, named->canonical);  // the persisted form never names the preset
  EXPECT_EQ(authored->canonical, bare->canonical);
  EXPECT_EQ(R"({"char_filters":[],"filters":["nfkc_cf","fold","kstem"],"tokenizer":"unicode_word"})",
            named->canonical);
  EXPECT_TRUE(named->fusedHead);
  EXPECT_TRUE(named->fusedFold);
  EXPECT_EQ(compileWith(registry, R"({"tokenizer":"unicode_word","filters":["nfkc_cf"]})")->canonical,
            compileWith(registry, R"("standard")")->canonical);
  // the example plugin's preset carries a char filter in the canonical form
  auto example = compileWith(globalAnalysisRegistry(), R"("example")");
  EXPECT_EQ(R"({"char_filters":["strip_x"],"filters":["lowercase","example_reverse"],"tokenizer":"whitespace"})",
            example->canonical);
  EXPECT_EQ(example->canonical, compileWith(globalAnalysisRegistry(),
      R"({"char_filters":["strip_x"],"tokenizer":"whitespace","filters":["lowercase","example_reverse"]})")->canonical);
}

TEST_F(AnalysisRegistryTest, namedAnalyzerTakesOnlyCharFilters) {
  AnalysisRegistry registry;
  const std::string expected = "analyzer 'english' is a named analyzer; it takes no tokenizer or filters";
  EXPECT_EQ(expected, compileError(registry, R"({"name":"english","tokenizer":"keyword"})"));
  EXPECT_EQ(expected, compileError(registry, R"({"name":"english","filters":["lowercase"]})"));
  // char filters next to a name replace the preset's
  auto withCharFilters = compileWith(registry, R"({"name":"english","char_filters":["html_strip"]})");
  EXPECT_EQ("english", withCharFilters->presetName);
  EXPECT_EQ(R"({"char_filters":["html_strip"],"filters":["nfkc_cf","fold","kstem"],"tokenizer":"unicode_word"})",
            withCharFilters->canonical);
  EXPECT_EQ(R"({"char_filters":["nfkc_cf"],"filters":["lowercase","example_reverse"],"tokenizer":"whitespace"})",
            compileWith(globalAnalysisRegistry(), R"({"name":"example","char_filters":["nfkc_cf"]})")->canonical);
}

TEST_F(AnalysisRegistryTest, nestedNamedAnalyzerRejectedAtRegistration) {
  AnalysisRegistry registry;
  api::AnalyzerDef alias;
  alias.name = "english";
  EXPECT_THROW(registry.addAnalyzer("alias", alias), std::invalid_argument);
  EXPECT_EQ(nullptr, registry.findAnalyzer("alias"));
}

TEST_F(AnalysisRegistryTest, freezeRejectsPresetWithMissingComponent) {
  AnalysisRegistry registry;
  api::AnalyzerDef def;
  const api::AnalyzerComponent filters[] = {component("lowercase"), component("not_registered")};
  def.filters = filters;
  registry.addAnalyzer("broken", def);  // components may be registered later, so this is accepted
  try {
    registry.freeze();
    FAIL() << "freeze accepted an unresolvable preset";
  } catch (const std::invalid_argument& e) {
    EXPECT_NE(std::string::npos, std::string(e.what()).find("analyzer 'broken': unknown filter 'not_registered'"))
        << e.what();
  }
  EXPECT_FALSE(registry.frozen());
}

TEST_F(AnalysisRegistryTest, addAnalyzerCopiesTheDefinition) {
  AnalysisRegistry registry;
  {
    std::pmr::monotonic_buffer_resource arena;
    api::AnalyzerDef def;
    ASSERT_TRUE(api::read_json(def, R"({"tokenizer":"keyword","filters":[{"name":"stop","params":{"words":["a"]}}]})", arena));
    registry.addAnalyzer("copied", def);
  }  // the authored views are gone
  const api::AnalyzerDef* found = registry.findAnalyzer("copied");
  ASSERT_NE(nullptr, found);
  EXPECT_EQ("keyword", found->tokenizer->name);
  ASSERT_EQ(1u, found->filters.size());
  EXPECT_EQ("stop", found->filters[0].name);
  EXPECT_EQ(R"({"char_filters":[],"filters":[{"name":"stop","params":{"words":["a"]}}],"tokenizer":"keyword"})",
            compileWith(registry, R"("copied")")->canonical);
}

TEST_F(AnalysisRegistryTest, examplePluginComponentsAreRegistered) {
  const auto& analysis = globalAnalysisRegistry();
  auto has = [](const std::vector<std::string>& names, std::string_view name) {
    return std::find(names.begin(), names.end(), name) != names.end();
  };
  EXPECT_TRUE(has(analysis.filterNames(), "example_reverse"));
  EXPECT_TRUE(has(analysis.filterNames(), "stop"));  // built-ins stay alongside plugin components
  EXPECT_TRUE(has(analysis.charFilterNames(), "strip_x"));
  EXPECT_TRUE(has(analysis.analyzerNames(), "example"));
  EXPECT_TRUE(has(analysis.analyzerNames(), "english"));
  api::AnalyzerComponent withParams = component("example_reverse");
  EXPECT_NO_THROW(analysis.makeFilter(withParams));
}

// A named analyzer and a plugin filter work through the whole stack: schema,
// indexing, and query-time analysis (the query text runs the same chain).
TEST_F(AnalysisRegistryTest, pluginComponentsIndexAndMatchEndToEnd) {
  CollectionHelper helper;
  putSchema(helper.collection(), R"({"fields":{
    "ex":{"type":"text","analyzer":{"name":"example"}},
    "rv":{"type":"text","analyzer":{"tokenizer":"whitespace","filters":["example_reverse"]}},
    "en":{"type":"text","analyzer":"english"}
  }})");
  ASSERT_TRUE(helper.index(flatdoc("id", "d1", "ex", "Hxello WORLD", "rv", "abc def", "en", "The Winter's Ponies"),
                           UpdateMessage::NO_COMMIT).success);
  ASSERT_TRUE(helper.index(flatdoc("id", "d2", "ex", "foo", "rv", "cba", "en", "a pony"), UpdateMessage::COMMIT).success);

  // example: strip_x, lowercase, reverse, at index and query time
  EXPECT_EQ(1, matches(helper, "ex", "hello"));
  EXPECT_EQ(1, matches(helper, "ex", "HELLxO"));
  EXPECT_EQ(1, matches(helper, "ex", "world"));
  EXPECT_EQ(1, matches(helper, "ex", "foo"));
  EXPECT_EQ(0, matches(helper, "ex", "olleh"));  // the stored terms are reversed, the query reverses too
  // plain filters-only definition resolved through the plugin
  EXPECT_EQ(1, matches(helper, "rv", "abc"));
  EXPECT_EQ(1, matches(helper, "rv", "cba"));
  EXPECT_EQ(2, matches(helper, "rv", "abc cba"));
  EXPECT_EQ(0, matches(helper, "rv", "abd"));
  // built-in preset by bare name: possessive and plural stemming
  EXPECT_EQ(2, matches(helper, "en", "pony winter"));
  EXPECT_EQ(1, matches(helper, "en", "winter"));
}

// A collection whose schema names a component this binary lacks (a plugin that
// is not compiled in) fails to load with the teaching message; the node keeps
// serving its other collections and the name resolves to the failure, never to
// "does not exist" or a silent re-create.
TEST_F(AnalysisRegistryTest, missingComponentAtOpenFailsTheCollectionOnly) {
  LuxirConfig config;
  config.store.backend = "fs";
  config.store.data_dir = path.string();
  {
    LuxirNode node(config);
    for (const char* name : {"good", "bad"}) {
      CollectionHelper helper(node, name);
      putSchema(helper.collection(), R"({"fields":{"t":{"type":"text","analyzer":"example"}}})");
      ASSERT_TRUE(helper.index(flatdoc("id", "1", "t", "hello"), UpdateMessage::COMMIT).success);
    }
  }
  // Rewrite the committed schema of "bad" as if a plugin's filter were absent.
  FSDirectory container(path / "t" / "t0" / "bad");
  FSDirectory dir(path / "t" / "t0" / "bad" / *CollectionStorage::current(container));
  auto loaded = readDurableIndexInfo(dir);
  api::SchemaDef missing;
  std::string error;
  ASSERT_TRUE(api::read_json(missing,
      R"({"fields":{"t":{"type":"text","analyzer":{"filters":["from_a_missing_plugin"]}}}})", *loaded.arena, &error))
      << error;
  std::vector<std::byte> schemaBytes, manifest;
  ASSERT_TRUE(api::encode(missing, schemaBytes));
  loaded.info.schema->source_def = std::span<const std::byte>(schemaBytes);
  ASSERT_TRUE(api::encode(loaded.info, manifest));
  Manifest::write(dir, loaded.info.index_gen, manifest);

  LuxirNode node(config);
  CollectionHelper good(node, "good");
  EXPECT_EQ(1, matches(good, "t", "hello", "good"));
  try {
    node.getCollection("bad");
    FAIL() << "the collection with a missing component loaded";
  } catch (const CollectionResolutionError& e) {
    std::string message = e.what();
    EXPECT_NE(std::string::npos, message.find("failed to load")) << message;
    EXPECT_NE(std::string::npos, message.find("unknown filter 'from_a_missing_plugin'; valid filters: ")) << message;
  }
}
