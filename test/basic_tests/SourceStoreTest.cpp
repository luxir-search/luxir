// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// The optional _source store end to end over HTTP: capture on the buffered and
// NDJSON paths, retrieval and export, merges (reference and relocation),
// restart, and the node-wide default.

#include <gtest/gtest.h>

#include <filesystem>

#include "luxir/server/HttpServer.h"
#include "luxir/server/LuxirNode.h"
#include "test/HttpReq.h"
#include "test/LuxirTest.h"

namespace luxir::test {

namespace {

class SourceStoreTest : public LuxirTest {
protected:
  std::filesystem::path dir = std::filesystem::temp_directory_path() /
      ("luxir_source_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

  void TearDown() override { std::filesystem::remove_all(dir); }

  LuxirConfig config(bool sourceDefault = false) const {
    LuxirConfig c;
    c.store.backend = "fs";
    c.store.data_dir = dir.string();
    c.index.source_default = sourceDefault;
    return c;
  }

  static std::string post(HttpServer& server, std::string_view path, std::string body,
                          std::string_view type = "application/json") {
    auto res = httpRequest(server.getPort(), http::verb::post, path, std::move(body), type);
    EXPECT_EQ(200, res.result_int()) << res.body();
    return res.body();
  }

  static std::string sources(HttpServer& server, const std::string& coll, bool exportMode = false) {
    return post(server, "/collections/" + coll + "/_search" + (exportMode ? "?format=docs&source=true" : "?format=docs"),
                R"({"query":{"all":true},"fields":["_source"],"limit":-1})");
  }
};

constexpr const char* kDoc2 = R"({"id":"b","title_t":"hello  world"})";

}  // namespace

TEST_F(SourceStoreTest, keepsDocumentsAsSubmittedAcrossMergesAndRestart) {
  // The first doc has spelling a typed field would normalize away: a padded
  // number, key order, a duplicate key.
  std::string first = R"({"docs":[ {"id":"a", "n_i":"0042", "z_i":1, "b_i":2, "b_i":3}, )" + std::string(kDoc2) + "]}";
  {
    LuxirNode node(config());
    HttpServer server(node, 2, 0);
    server.start();
    post(server, "/collections/_create", R"({"name":"s","schema":{"source":{"enabled":true,"min_file_bytes":0}}})");
    post(server, "/collections/s/_update?commit=true", first);
    // Second segment over NDJSON, then a merge that rewrites nothing (min_file_bytes 0).
    post(server, "/collections/s/_update?commit=true",
         "{\"id\":\"c\",\"k_is\": [1, 2]}\n{\"id\":\"d\"}\n", "application/x-ndjson");
    post(server, "/collections/s/_update", R"({"docs":[],"commit":{"max_segments":1,"wait_for_merges":true}})");

    auto lines = sources(server, "s");
    EXPECT_NE(lines.find(R"({"_source":{"id":"a","n_i":"0042","z_i":1,"b_i":2,"b_i":3}})"), std::string::npos) << lines;
    EXPECT_NE(lines.find(R"({"_source":{"id":"b","title_t":"hello  world"}})"), std::string::npos) << lines;
    EXPECT_NE(lines.find(R"({"_source":{"id":"c","k_is":[1,2]}})"), std::string::npos) << lines;
    server.shutdown();
  }
  {
    // Restart: the source files and maps are found again.
    LuxirNode node(config());
    HttpServer server(node, 2, 0);
    server.start();
    auto lines = sources(server, "s", true);
    EXPECT_NE(lines.find(R"({"id":"a","n_i":"0042","z_i":1,"b_i":2,"b_i":3})" "\n"), std::string::npos) << lines;
    EXPECT_EQ(std::string::npos, lines.find("_source")) << lines;
    server.shutdown();
  }
}

TEST_F(SourceStoreTest, mergeDropsDeletedDocsAndRelocatesSmallFiles) {
  LuxirNode node(config());
  HttpServer server(node, 2, 0);
  server.start();
  // Default thresholds: every flushed file is small, so a merge relocates.
  post(server, "/collections/_create", R"({"name":"m","schema":{"source":{"enabled":true}}})");
  post(server, "/collections/m/_update?commit=true", R"({"docs":[{"id":"1","x_i":1},{"id":"2","x_i":2}]})");
  post(server, "/collections/m/_update?commit=true", R"({"docs":[{"id":"3","x_i":3}],"delete_ids":["1"]})");
  post(server, "/collections/m/_update", R"({"docs":[],"commit":{"max_segments":1,"wait_for_merges":true}})");
  auto lines = sources(server, "m", true);
  EXPECT_EQ(R"({"id":"2","x_i":2})" "\n" R"({"id":"3","x_i":3})" "\n", lines);
  server.shutdown();
}

TEST_F(SourceStoreTest, offByDefaultAndRequestedFieldIsAnError) {
  LuxirNode node(config());
  HttpServer server(node, 2, 0);
  server.start();
  post(server, "/collections/_create", R"({"name":"off"})");
  post(server, "/collections/off/_update?commit=true", R"({"docs":[{"id":"1"}]})");
  auto res = httpRequest(server.getPort(), http::verb::post, "/collections/off/_search",
                         R"({"query":{"all":true},"fields":["_source"]})");
  EXPECT_EQ(400, res.result_int()) << res.body();
  EXPECT_NE(res.body().find("_source is not stored"), std::string::npos) << res.body();
  server.shutdown();
}

TEST_F(SourceStoreTest, nodeDefaultAppliesUnlessTheCreateRequestOptsOut) {
  LuxirNode node(config(true));
  HttpServer server(node, 2, 0);
  server.start();
  post(server, "/collections/_create", R"({"name":"on"})");
  post(server, "/collections/_create", R"({"name":"out","schema":{"source":{"enabled":false}}})");
  post(server, "/collections/auto/_update?commit=true", R"({"docs":[{"id":"1"}]})");  // auto-created
  post(server, "/collections/on/_update?commit=true", R"({"docs":[{"id":"1"}]})");
  post(server, "/collections/out/_update?commit=true", R"({"docs":[{"id":"1"}]})");

  EXPECT_NE(sources(server, "on").find(R"({"_source":{"id":"1"}})"), std::string::npos);
  EXPECT_NE(sources(server, "auto").find(R"({"_source":{"id":"1"}})"), std::string::npos);
  auto res = httpRequest(server.getPort(), http::verb::post, "/collections/out/_search",
                         R"({"query":{"all":true},"fields":["_source"]})");
  EXPECT_EQ(400, res.result_int());
  auto schema = httpRequest(server.getPort(), http::verb::get, "/collections/on/_schema");
  std::string compact;
  std::ranges::copy_if(schema.body(), std::back_inserter(compact), [](char c) { return c != ' ' && c != '\n'; });
  EXPECT_NE(compact.find(R"("source":{"enabled":true})"), std::string::npos) << schema.body();
  server.shutdown();
}

TEST_F(SourceStoreTest, exportRequiresTheSourceField) {
  LuxirNode node(config());
  HttpServer server(node, 2, 0);
  server.start();
  post(server, "/collections/_create", R"({"name":"e","schema":{"source":{"enabled":true}}})");
  post(server, "/collections/e/_update?commit=true", R"({"docs":[{"id":"1"}]})");
  auto res = httpRequest(server.getPort(), http::verb::post, "/collections/e/_search?format=docs&source=true",
                         R"({"query":{"all":true},"fields":["id"]})");
  EXPECT_NE(res.body().find("source=true requires"), std::string::npos) << res.body();
  auto wrong = httpRequest(server.getPort(), http::verb::post, "/collections/e/_search?source=true",
                           R"({"query":{"all":true}})");
  EXPECT_EQ(400, wrong.result_int());
  server.shutdown();
}

TEST_F(SourceStoreTest, replacingTheSchemaKeepsTheStoreAndEnablingLaterSkipsOlderDocs) {
  LuxirNode node(config());
  HttpServer server(node, 2, 0);
  server.start();
  post(server, "/collections/_create", R"({"name":"r"})");
  post(server, "/collections/r/_update?commit=true", R"({"docs":[{"id":"old"}]})");
  post(server, "/collections/r/_schema", R"({"source":{"enabled":true}})");
  post(server, "/collections/r/_update?commit=true", R"({"docs":[{"id":"new"}]})");
  // A replacement that says nothing about the store must not turn it off.
  post(server, "/collections/r/_schema?mode=replace_all", R"({"fields":{"id":{"type":"id"}}})");
  post(server, "/collections/r/_update", R"({"docs":[],"commit":{"max_segments":1,"wait_for_merges":true}})");
  // The doc indexed before the store was on has no line to export.
  EXPECT_EQ(R"({"id":"new"})" "\n", sources(server, "r", true));
  server.shutdown();
}

}  // namespace luxir::test
