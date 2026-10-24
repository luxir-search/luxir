// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>

#include "CountingStorage.h"
#include "ExampleSearch.h"
#include "luxir/plugin/Registries.h"
#include "luxir/server/LuxirNode.h"
#include "luxir/store/DelegatingDirectory.h"

namespace luxir::example {

namespace {

// Filter: reverses the bytes of every token (ASCII only, which is all the
// tests feed it). Rewrites into its own buffer per the token borrow contract.
class ReverseFilter : public TokenFilter {
  std::string buf;

public:
  explicit ReverseFilter(std::unique_ptr<TokenStream> source) : TokenFilter(std::move(source)) {}

  bool incrementToken() override {
    if (!source().incrementToken()) return false;
    buf.assign(token.text.rbegin(), token.text.rend());
    token.text = buf;
    return true;
  }

  void normalizeTerm(std::string& term) override {
    TokenFilter::normalizeTerm(term);
    std::reverse(term.begin(), term.end());
  }
};

// Filter: sets positionLength to 2 on tokens that start with 'x', as a filter
// emitting a two-word synonym would.
class Span2Filter : public TokenFilter {
public:
  explicit Span2Filter(std::unique_ptr<TokenStream> source) : TokenFilter(std::move(source)) {}

  bool incrementToken() override {
    if (!source().incrementToken()) return false;
    if (!token.text.empty() && token.text[0] == 'x') token.positionLength = 2;
    return true;
  }
};

// Filter: rewrites every token to "text|type|script|positionLength", what a
// downstream plugin filter can read of the attributes upstream stages set.
class DescribeFilter : public TokenFilter {
  std::string buf;

public:
  explicit DescribeFilter(std::unique_ptr<TokenStream> source) : TokenFilter(std::move(source)) {}

  bool incrementToken() override {
    if (!source().incrementToken()) return false;
    buf.assign(token.text);
    buf += '|';
    buf += tokenTypeName(token.type);
    buf += '|';
    buf += scriptName(token.script);
    buf += '|';
    buf += std::to_string(token.positionLength);
    token.text = buf;
    return true;
  }
};

// Char filter: removes every 'x'. The base class records each deletion, so
// token offsets map back to the source.
class StripXFilter : public CharFilter {
  void filter() override {
    for (size_t i = input.find('x'); i != std::string_view::npos; i = input.find('x', i + 1)) replace(i, i + 1, "");
  }
};

// Collation: ASCII case-insensitive order. strength=primary keys are the
// lowercased value, so "a" and "A" tie; strength=tertiary (default) appends the
// case of each letter, lowercase first, as ICU's default does.
class CaseCollator : public Collator {
  bool tertiary;

public:
  explicit CaseCollator(bool tertiary) : tertiary(tertiary) {}

  void appendKey(std::string_view value, std::string& key) override {
    for (char c : value) key.push_back((c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c);
    if (!tertiary) return;
    key.push_back('\0');
    for (char c : value) key.push_back((c >= 'A' && c <= 'Z') ? '\1' : '\0');
  }
};

void noParams(const api::AnalyzerComponent& def) {
  if (!def.params.empty()) throw std::invalid_argument("'" + std::string(def.name) + "' takes no parameters");
}

std::mutex countsMutex;
std::map<std::string, uint64_t>& counts() {
  static std::map<std::string, uint64_t> instance;
  return instance;
}
std::string key(std::string_view source, std::string_view op, std::string_view collection) {
  return std::string(source) + "/" + std::string(op) + "/" + std::string(collection);
}

// Counts the files opened and created through it, under its collection's label.
class CountingDirectory : public DelegatingDirectory {
  std::string collection;

public:
  CountingDirectory(std::shared_ptr<Directory> inner, std::string collection)
      : DelegatingDirectory(std::move(inner)), collection(std::move(collection)) {}

  std::shared_ptr<InputFile> openFile(std::string_view name, bool expectSynced = false) override {
    CountingStorage::add("layer", "open", collection);
    return delegate->openFile(name, expectSynced);
  }
  std::unique_ptr<File> createFile(std::string_view name) override {
    CountingStorage::add("layer", "create", collection);
    return delegate->createFile(name);
  }
  std::unique_ptr<File> createFile(std::string_view name, FileCreateOptions options) override {
    CountingStorage::add("layer", "create", collection);
    return delegate->createFile(name, std::move(options));
  }
};

// The counting layer: wraps every directory it hands out.
class CountingLayer : public DelegatingDirFactory {
public:
  using DelegatingDirFactory::DelegatingDirFactory;

  std::shared_ptr<Directory> container(const CollectionId& id, bool create) override {
    return std::make_shared<CountingDirectory>(delegate->container(id, create), id.label());
  }
  std::shared_ptr<Directory> incarnation(const CollectionId& id, std::string_view incarnation, bool create) override {
    return std::make_shared<CountingDirectory>(delegate->incarnation(id, incarnation, create), id.label());
  }
  std::string describe() const override { return "counting(" + delegate->describe() + ")"; }
};

// The counting_ram backend: RAM storage that counts directory opens and creates.
class CountingRamBackend : public DelegatingDirFactory {
  static void count(const CollectionId& id, bool create) {
    CountingStorage::add("backend", create ? "create" : "open", id.label());
  }

public:
  explicit CountingRamBackend(const StorageContext& ctx)
      : DelegatingDirFactory(std::make_unique<RAMDirFactory>(0, ctx.reclaim)) {}

  std::shared_ptr<Directory> container(const CollectionId& id, bool create) override {
    count(id, create);
    return delegate->container(id, create);
  }
  std::shared_ptr<Directory> incarnation(const CollectionId& id, std::string_view incarnation, bool create) override {
    count(id, create);
    return delegate->incarnation(id, incarnation, create);
  }
  std::string describe() const override { return "counting_ram"; }
};

}  // namespace

void CountingStorage::reset() {
  std::lock_guard lock(countsMutex);
  counts().clear();
}

void CountingStorage::add(std::string_view source, std::string_view op, std::string_view collection) {
  std::lock_guard lock(countsMutex);
  counts()[key(source, op, collection)]++;
  if (!collection.empty()) counts()[key(source, op, "")]++;
}

uint64_t CountingStorage::count(std::string_view source, std::string_view op, std::string_view collection) {
  std::lock_guard lock(countsMutex);
  auto it = counts().find(key(source, op, collection));
  return it == counts().end() ? 0 : it->second;
}

namespace {

// Test-only plugin that exercises the plugin framework: analysis components,
// collations, query types, search ops, HTTP routes, the node lifecycle, and
// storage backends and layers.
class ExamplePlugin : public Plugin {
  static void registerAnalysis(Registries& r) {
    r.analysis.addFilter("reverse", [](const api::AnalyzerComponent& def) {
      noParams(def);
      struct Factory : TokenFilterFactory {
        std::unique_ptr<TokenStream> create(std::unique_ptr<TokenStream> source) const override {
          return std::make_unique<ReverseFilter>(std::move(source));
        }
      };
      auto factory = std::make_unique<Factory>();
      factory->name = "reverse";
      factory->normalizing = true;
      return std::unique_ptr<TokenFilterFactory>(std::move(factory));
    });
    r.analysis.addCharFilter("strip_x", [](const api::AnalyzerComponent& def) {
      noParams(def);
      struct Factory : CharFilterFactory {
        std::unique_ptr<CharFilter> create() const override { return std::make_unique<StripXFilter>(); }
      };
      auto factory = std::make_unique<Factory>();
      factory->name = "strip_x";
      factory->normalizing = true;
      return std::unique_ptr<const CharFilterFactory>(std::move(factory));
    });

    api::AnalyzerComponent whitespace, stripX, lowercase, reverse;
    whitespace.name = "whitespace";
    stripX.name = "strip_x";
    lowercase.name = "lowercase";
    reverse.name = "reverse";
    api::AnalyzerDef example;
    example.tokenizer = whitespace;
    const api::AnalyzerComponent charFilters[] = {stripX};
    const api::AnalyzerComponent filters[] = {lowercase, reverse};
    example.char_filters = charFilters;
    example.filters = filters;
    r.analysis.addAnalyzer("example", example);
  }

  static void registerFilter(Registries& r, std::string name,
                             std::unique_ptr<TokenStream> (*make)(std::unique_ptr<TokenStream>)) {
    r.analysis.addFilter(name, [name, make](const api::AnalyzerComponent& def) {
      noParams(def);
      struct Factory : TokenFilterFactory {
        std::unique_ptr<TokenStream> (*make)(std::unique_ptr<TokenStream>);
        std::unique_ptr<TokenStream> create(std::unique_ptr<TokenStream> source) const override {
          return make(std::move(source));
        }
      };
      auto factory = std::make_unique<Factory>();
      factory->name = name;
      factory->make = make;
      return std::unique_ptr<TokenFilterFactory>(std::move(factory));
    });
  }

  static void registerCollation(Registries& r) {
    r.collation.add("example_ci", [](const api::AnalyzerComponent& def) {
      bool tertiary = true;
      for (const auto& [key, value] : def.params) {
        auto* text = std::get_if<std::string_view>(&value->kind);
        if (key != "strength") {
          throw std::invalid_argument("collation 'example_ci': unknown parameter '" + std::string(key) +
                                      "'; valid parameters: strength");
        }
        if (!text || (*text != "primary" && *text != "tertiary")) {
          throw std::invalid_argument("collation 'example_ci': parameter 'strength' must be \"primary\" or \"tertiary\"");
        }
        tertiary = *text == "tertiary";
      }
      struct Factory : CollatorFactory {
        bool tertiary = true;
        std::unique_ptr<Collator> create() const override { return std::make_unique<CaseCollator>(tertiary); }
      };
      auto factory = std::make_unique<Factory>();
      factory->tertiary = tertiary;
      return std::unique_ptr<const CollatorFactory>(std::move(factory));
    });
  }

  static void registerStorage(Registries& r) {
    r.storage.addBackend("counting_ram", [](const StorageContext& ctx) -> std::unique_ptr<DirectoryFactory> {
      return std::make_unique<CountingRamBackend>(ctx);
    });
    r.storage.addLayer("counting", [](std::unique_ptr<DirectoryFactory> inner, const StorageContext&)
                                       -> std::unique_ptr<DirectoryFactory> {
      return std::make_unique<CountingLayer>(std::move(inner));
    });
  }

  static void registerSearch(Registries& r) {
    r.query.add("doc_ids", ::example::docIdsDef());
    r.searchOps.add("sample_docs", ::example::makeSampleDocs);
    r.http.add("GET", "/_plugins/example/ping", [](HttpPluginRequest& request, HttpPluginResponse& response) {
      const std::string* echo = request.queryParam("echo");
      response.body = R"({"pong":")" + (echo ? *echo : std::string()) + R"("})";
    });
    // A collection route answered by a search: POST {"ids": [...]} runs sample_docs.
    r.http.add("POST", "/collections/{collection}/_sample", [](HttpPluginRequest& request, HttpPluginResponse& response) {
      response.searchCollection = *request.pathParam("collection");
      response.searchBody = R"({"ops":{"sample":{"sample_docs":)" + std::string(request.body) + "}}}";
    });
  }

public:
  static inline std::atomic<int> startedNodes{0};
  static inline std::atomic<int> stoppedNodes{0};

  PluginInfo info() const override { return {"example", "1.0", "exercises the plugin framework"}; }
  void registerComponents(Registries& r) override {
    registerAnalysis(r);
    registerSearch(r);
    registerFilter(r, "span2", [](std::unique_ptr<TokenStream> source) -> std::unique_ptr<TokenStream> {
      return std::make_unique<Span2Filter>(std::move(source));
    });
    registerFilter(r, "describe", [](std::unique_ptr<TokenStream> source) -> std::unique_ptr<TokenStream> {
      return std::make_unique<DescribeFilter>(std::move(source));
    });
    registerCollation(r);
    registerStorage(r);
  }
  void nodeStarted(LuxirNode&) override { startedNodes++; }
  void nodeStopping(LuxirNode&) noexcept override { stoppedNodes++; }
};

}  // namespace

}  // namespace luxir::example

// For PluginExtensionTest: how many nodes the example plugin saw start and stop.
int exampleNodesStarted() { return luxir::example::ExamplePlugin::startedNodes.load(); }
int exampleNodesStopped() { return luxir::example::ExamplePlugin::stoppedNodes.load(); }

LUXIR_PLUGIN(example, luxir::example::ExamplePlugin)
