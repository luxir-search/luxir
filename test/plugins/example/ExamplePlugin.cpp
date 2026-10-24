// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>

#include "CountingStorage.h"
#include "luxir/plugin/Registries.h"
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

// Char filter: removes every 'x'. The base class records each deletion, so
// token offsets map back to the source.
class StripXFilter : public CharFilter {
  void filter() override {
    for (size_t i = input.find('x'); i != std::string_view::npos; i = input.find('x', i + 1)) replace(i, i + 1, "");
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

// Test-only plugin that exercises the plugin framework: analysis components and
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

  static void registerStorage(Registries& r) {
    r.storage.addBackend("counting_ram", [](const StorageContext& ctx) -> std::unique_ptr<DirectoryFactory> {
      return std::make_unique<CountingRamBackend>(ctx);
    });
    r.storage.addLayer("counting", [](std::unique_ptr<DirectoryFactory> inner, const StorageContext&)
                                       -> std::unique_ptr<DirectoryFactory> {
      return std::make_unique<CountingLayer>(std::move(inner));
    });
  }

public:
  PluginInfo info() const override { return {"example", "1.0", "exercises the plugin framework"}; }
  void registerComponents(Registries& r) override {
    registerAnalysis(r);
    registerStorage(r);
  }
};

}  // namespace

}  // namespace luxir::example

LUXIR_PLUGIN(example, luxir::example::ExamplePlugin)
