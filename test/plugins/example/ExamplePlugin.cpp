// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <stdexcept>

#include "ExampleSearch.h"
#include "ExampleQuery.h"
#include "luxir/plugin/Registries.h"
#include "luxir/server/LuxirNode.h"

namespace {

using namespace luxir;

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

// Test-only plugin that exercises the plugin framework: analysis components,
// a query type (doc_ids), a search op (sample_docs), HTTP routes and the node
// lifecycle here, storage backends in a later step.
class ExamplePlugin : public Plugin {
public:
  static inline std::atomic<bool> throwOnNodeStateCreation{false};

  PluginInfo info() const override { return {"example", "1.0", "exercises the plugin framework"}; }

  void registerComponents(Registries& r) override {
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

    r.query.add("doc_ids", example::docIdsDef());
    r.query.add("field_value", example::fieldValueDef());
    r.query.add("field_value_alias", example::fieldValueDef());
    r.query.add("unscoped_value", example::unscopedDef());
    r.searchOps.add("sample_docs", example::makeSampleDocs);
    r.http.add("GET", "/_plugins/example/ping", [](HttpPluginRequest& request, HttpPluginResponse& response) {
      request.node.pluginState<example::NodeState>("example")->routes++;
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
    return std::make_unique<example::NodeState>(node);
  }
};

}  // namespace

// For the node-construction failure test.
void exampleThrowOnNodeStateCreation(bool value) { ExamplePlugin::throwOnNodeStateCreation.store(value); }

LUXIR_PLUGIN(example, ExamplePlugin)
