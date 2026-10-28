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
  static inline std::atomic<int> startedNodes{0};
  static inline std::atomic<int> stoppedNodes{0};
  static inline std::atomic<int> startedPullNodes{0};
  static inline std::atomic<int> stoppedPullNodes{0};
  static inline std::atomic<bool> throwOnNodeStart{false};

  luxir::PluginInfo info() const override { return {"example", "1.0", "exercises the plugin framework"}; }
  void registerComponents(luxir::Registries& r) override {
    r.query.add("doc_ids", example::docIdsDef());
    r.query.add("field_value", example::fieldValueDef());
    r.query.add("field_value_alias", example::fieldValueDef());
    r.query.add("unscoped_value", example::unscopedDef());
    r.searchOps.add("sample_docs", example::makeSampleDocs);
    r.http.add("GET", "/_plugins/example/ping", [](luxir::HttpPluginRequest& request, luxir::HttpPluginResponse& response) {
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
  void nodeStarted(luxir::LuxirNode& node) override {
    if (throwOnNodeStart.load()) throw std::runtime_error("example node start failed");
    startedNodes++;
    if (node.getMode() == luxir::LuxirNode::Mode::PULL) startedPullNodes++;
  }
  void nodeStopping(luxir::LuxirNode& node) noexcept override {
    stoppedNodes++;
    if (node.getMode() == luxir::LuxirNode::Mode::PULL) stoppedPullNodes++;
  }
};

}  // namespace

// For PluginTest: how many nodes the example plugin saw start and stop.
int exampleNodesStarted() { return ExamplePlugin::startedNodes.load(); }
int exampleNodesStopped() { return ExamplePlugin::stoppedNodes.load(); }
int examplePullNodesStarted() { return ExamplePlugin::startedPullNodes.load(); }
int examplePullNodesStopped() { return ExamplePlugin::stoppedPullNodes.load(); }
void exampleThrowOnNodeStart(bool value) { ExamplePlugin::throwOnNodeStart.store(value); }

LUXIR_PLUGIN(example, ExamplePlugin)
