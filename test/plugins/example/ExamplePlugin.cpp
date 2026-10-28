// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "ExampleSearch.h"
#include "ExampleQuery.h"
#include "luxir/plugin/Registries.h"
#include "luxir/server/LuxirNode.h"

#include <stdexcept>

namespace {

// Test-only plugin that exercises the plugin framework: a query type
// (doc_ids), a search op (sample_docs), HTTP routes and the node lifecycle.
// Later steps register analysis components and storage backends here.
class ExamplePlugin : public luxir::Plugin {
public:
  static inline std::atomic<bool> throwOnNodeStateCreation{false};

  luxir::PluginInfo info() const override { return {"example", "1.0", "exercises the plugin framework"}; }
  void registerComponents(luxir::Registries& r) override {
    r.query.add("doc_ids", example::docIdsDef());
    r.query.add("field_value", example::fieldValueDef());
    r.query.add("field_value_alias", example::fieldValueDef());
    r.query.add("unscoped_value", example::unscopedDef());
    r.searchOps.add("sample_docs", example::makeSampleDocs);
    r.http.add("GET", "/_plugins/example/ping", [](luxir::HttpPluginRequest& request, luxir::HttpPluginResponse& response) {
      request.node.pluginState<example::NodeState>("example")->routes++;
      const std::string* echo = request.queryParam("echo");
      response.body = R"({"pong":")" + (echo ? *echo : std::string()) + R"("})";
    });
    // A collection route answered by a search: POST {"ids": [...]} runs sample_docs.
    r.http.add("POST", "/collections/{collection}/_sample",
               [](luxir::HttpPluginRequest& request, luxir::HttpPluginResponse& response) {
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
