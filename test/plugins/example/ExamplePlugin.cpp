// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <stdexcept>
#include <string>

#include "CountingStorage.h"
#include "ExampleSearch.h"
#include "ExampleQuery.h"
#include "luxir/plugin/Registries.h"
#include "luxir/server/LuxirNode.h"
#include "test/StrictStorage.h"

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

// Filter: rewrites every token to "text|wordClass|script|positionLength", what
// a downstream plugin filter can read of a token's attributes. wordClass() and
// script() classify the input word on the first read unless the tokenizer
// already stored them.
class DescribeFilter : public TokenFilter {
  std::string buf;

public:
  explicit DescribeFilter(std::unique_ptr<TokenStream> source) : TokenFilter(std::move(source)) {}

  bool incrementToken() override {
    if (!source().incrementToken()) return false;
    buf.assign(token.text);
    buf += '|';
    buf += wordClassName(token.wordClass());
    buf += '|';
    buf += scriptName(token.script());
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

void noParams(const api::AnalyzerComponent& def) {
  if (!def.params.empty()) throw std::invalid_argument("'" + std::string(def.name) + "' takes no parameters");
}

// Test-only plugin that exercises the plugin framework: analysis components,
// query types, search ops, HTTP routes, the node lifecycle, and storage
// backends and layers.
class ExamplePlugin : public Plugin {
public:
  static inline std::atomic<bool> throwOnNodeStateCreation{false};

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

  PluginInfo info() const override { return {"example", "1.0", "exercises the plugin framework"}; }

  void registerComponents(Registries& r) override {
    r.analysis.addFilter("example_reverse", [](const api::AnalyzerComponent& def) {
      noParams(def);
      struct Factory : TokenFilterFactory {
        std::unique_ptr<TokenStream> create(std::unique_ptr<TokenStream> source) const override {
          return std::make_unique<ReverseFilter>(std::move(source));
        }
      };
      auto factory = std::make_unique<Factory>();
      factory->name = "example_reverse";
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
    reverse.name = "example_reverse";
    api::AnalyzerDef example;
    example.tokenizer = whitespace;
    const api::AnalyzerComponent charFilters[] = {stripX};
    const api::AnalyzerComponent filters[] = {lowercase, reverse};
    example.char_filters = charFilters;
    example.filters = filters;
    r.analysis.addAnalyzer("example", example);

    registerFilter(r, "span2", [](std::unique_ptr<TokenStream> source) -> std::unique_ptr<TokenStream> {
      return std::make_unique<Span2Filter>(std::move(source));
    });
    registerFilter(r, "describe", [](std::unique_ptr<TokenStream> source) -> std::unique_ptr<TokenStream> {
      return std::make_unique<DescribeFilter>(std::move(source));
    });

    r.storage.addBackend("counting_ram", [](const StorageContext& ctx) -> std::unique_ptr<DirectoryFactory> {
      return std::make_unique<CountingRamBackend>(ctx);
    });
    r.storage.addLayer("counting", [](std::unique_ptr<DirectoryFactory> inner, const StorageContext&)
                                       -> std::unique_ptr<DirectoryFactory> {
      return std::make_unique<CountingLayer>(std::move(inner));
    });
    r.storage.addLayer("strict", [](std::unique_ptr<DirectoryFactory> inner, const StorageContext& ctx)
                                     -> std::unique_ptr<DirectoryFactory> {
      auto mode = ctx.params.getString("whole_segment_files", "count");
      if (mode != "count" && mode != "deny") ctx.params.fail("whole_segment_files", "must be count or deny");
      return std::make_unique<test::StrictLayer>(std::move(inner),
          mode == "deny" ? test::StrictWholeFiles::DENY : test::StrictWholeFiles::COUNT);
    });

    r.query.add("doc_ids", ::example::docIdsDef());
    r.query.add("field_value", ::example::fieldValueDef());
    r.query.add("field_value_alias", ::example::fieldValueDef());
    r.query.add("unscoped_value", ::example::unscopedDef());
    r.searchOps.add("sample_docs", ::example::makeSampleDocs);
    r.searchOps.add("query_count", ::example::makeQueryCount);
    // Adds a "label" column holding the configured text to every emitted hit.
    r.hitDecorators.add("label", [](DecoratorContext&, const ParamReader& params) {
      struct Label : HitDecorator {
        std::string text;
        std::string_view columnName() const override { return "label"; }
        void decorate(std::span<const segdoc>, std::span<const uint32_t> rows,
                      std::span<api::Val> out, std::pmr::memory_resource& mr) const override {
          for (uint32_t row : rows) {
            char* copy = (char*)mr.allocate(text.size(), alignof(char));
            std::memcpy(copy, text.data(), text.size());
            out[row].kind = std::string_view(copy, text.size());
          }
        }
      };
      auto label = std::make_shared<Label>();
      label->text = std::string(params.requireString("text"));
      return label;
    });
    // A request-wide term summary, collected once during serial planning.
    r.hitDecorators.add("ranking_terms", [](DecoratorContext&, const ParamReader&) {
      struct Terms : HitDecorator, QueryVisitor {
        std::string text;
        std::string_view columnName() const override { return "ranking_terms"; }
        void bind(std::span<const QuerySource> sources) override {
          for (const auto& source : sources) {
            if (source.query) source.query->visit(*this, source.context);
          }
        }
        QueryVisitor* sub(Occur occur, const Query&) override {
          return occur == Occur::MUST || occur == Occur::SHOULD ? this : nullptr;
        }
        void term(std::string_view field, std::string_view term, float) override {
          if (!text.empty()) text += ",";
          text += field;
          text += ":";
          text += term;
        }
        void decorate(std::span<const segdoc>, std::span<const uint32_t> rows,
                      std::span<api::Val> out, std::pmr::memory_resource& mr) const override {
          for (uint32_t row : rows) {
            char* copy = (char*)mr.allocate(text.size(), alignof(char));
            std::memcpy(copy, text.data(), text.size());
            out[row].kind = std::string_view(copy, text.size());
          }
        }
      };
      return std::make_shared<Terms>();
    });
    r.http.add("GET", "/_plugins/example/ping", [](HttpPluginRequest& request, HttpPluginResponse& response) {
      request.node.pluginState<::example::NodeState>("example")->routes++;
      const std::string* echo = request.queryParam("echo");
      response.body = R"({"pong":")" + (echo ? *echo : std::string()) + R"("})";
    });
    // A collection route answered by a search: POST {"ids": [...]} runs sample_docs.
    r.http.add("POST", "/collections/{collection}/_sample", [](HttpPluginRequest& request, HttpPluginResponse& response) {
      response.searchCollection = *request.pathParam("collection");
      response.searchBody = R"({"ops":{"sample":{"sample_docs":)" + std::string(request.body) + "}}}";
    });
  }
  std::unique_ptr<luxir::PluginNodeState> createNodeState(luxir::LuxirNode& node) override {
    if (throwOnNodeStateCreation.load()) throw std::runtime_error("example node state creation failed");
    return std::make_unique<::example::NodeState>(node);
  }
};

}  // namespace

}  // namespace luxir::example

// For the node-construction failure test.
void exampleThrowOnNodeStateCreation(bool value) { luxir::example::ExamplePlugin::throwOnNodeStateCreation.store(value); }

LUXIR_PLUGIN(example, luxir::example::ExamplePlugin)
