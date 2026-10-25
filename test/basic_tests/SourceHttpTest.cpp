// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// The _source_ store over HTTP, where the bytes come from: byte-exact capture
// on the buffered, NDJSON and inline _update_ paths, source filters from the
// body, URL and group, export, the node default, and a restart.

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>

#include "luxir/server/HttpServer.h"
#include "luxir/server/LuxirNode.h"
#include "test/HttpReq.h"
#include "test/LuxirTest.h"

namespace luxir::test {

namespace {

std::string post(HttpServer& server, std::string_view path, std::string body,
                 std::string_view type = "application/json") {
  auto res = httpRequest(server.getPort(), http::verb::post, path, std::move(body), type);
  EXPECT_EQ(200, res.result_int()) << path << ": " << res.body();
  return res.body();
}

std::string exportSources(HttpServer& server, const std::string& coll) {
  return post(server, "/collections/" + coll + "/_search?format=docs&source=true",
              R"({"query":{"all":true},"fields":["_source"],"limit":-1})");
}

std::string compact(std::string text) {
  std::erase_if(text, [](char c) { return c == ' ' || c == '\n'; });
  return text;
}

class SourceHttpTest : public LuxirTest {
protected:
  std::optional<HttpServer> server;

  void SetUp() override {
    server.emplace(*LuxirTest::luxirNode, 2, 0);
    server->start();
  }

  void TearDown() override {
    if (server) server->shutdown();
  }

  std::string post(std::string_view path, std::string body, std::string_view type = "application/json") {
    return test::post(*server, path, std::move(body), type);
  }

  std::string exportSources(const std::string& coll) { return test::exportSources(*server, coll); }

  void createWithSource(const std::string& coll, std::string_view settings = "{}") {
    post("/collections/_create",
         R"({"name":")" + coll + R"(","schema":{"stored_resources":{"_source_":)" + std::string(settings) + "}}}");
  }
};

constexpr const char* kOdd = R"({"id":"a", "n_i":"0042", "z_i":1, "b_i":2, "b_i":3})";

}  // namespace

TEST_F(SourceHttpTest, keepsTheBytesOfBufferedNdjsonAndInlineDocs) {
  createWithSource("src_capture");
  post("/collections/src_capture/_update?commit=true", std::string(R"({"docs":[ )") + kOdd + R"(, {"id":"b","title_t":"hello  world"}]})");
  post("/collections/src_capture/_update?commit=true",
       "{\"id\":\"c\",\"k_is\": [1, 2]}\n"
       R"({"_update_":{"docs":[{"id":"d", "y_i":2, "x_i":1}]}})" "\n",
       "application/x-ndjson");
  auto lines = exportSources("src_capture");
  EXPECT_EQ(R"({"id":"a","n_i":"0042","z_i":1,"b_i":2,"b_i":3})" "\n"
            R"({"id":"b","title_t":"hello  world"})" "\n"
            R"({"id":"c","k_is":[1,2]})" "\n"
            R"({"id":"d","y_i":2,"x_i":1})" "\n", lines);
  // The projection splices the same bytes into the response, unquoted.
  auto projected = post("/collections/src_capture/_search?format=docs",
                        R"({"query":{"match":{"id":"a"}},"fields":["_source"]})");
  EXPECT_EQ(R"({"_source":{"id":"a","n_i":"0042","z_i":1,"b_i":2,"b_i":3}})" "\n", projected);
  auto schema = httpRequest(server->getPort(), http::verb::get, "/collections/src_capture/_schema");
  EXPECT_NE(std::string::npos, compact(schema.body()).find(R"("stored_resources":{"_source_":{}})")) << schema.body();
}

TEST_F(SourceHttpTest, keepsSpellingAndEscapesOfSmallDocs) {
  createWithSource("src_spell");
  // "meta" is mapped away from the index, so only the source keeps it.
  post("/collections/src_spell/_update?commit=true",
       R"({"docs":[{"id":"a","x_s":"caf\u00e9 \"q\"","p_i":1.0,"q_i":1e3,"r_i":-0,"meta":{"k":{}}},)"
       R"({"id":"b","zz_s":"b"},{"id":"d","e_s":[]}],"field_map":{"meta":""}})");
  EXPECT_EQ(R"({"id":"a","x_s":"caf\u00e9 \"q\"","p_i":1.0,"q_i":1e3,"r_i":-0,"meta":{"k":{}}})" "\n"
            R"({"id":"b","zz_s":"b"})" "\n"
            R"({"id":"d","e_s":[]})" "\n", exportSources("src_spell"));
}

TEST_F(SourceHttpTest, keepsLargeDocsAcrossBlocks) {
  createWithSource("src_big", R"({"chunk_size_bytes":1024,"max_docs_per_chunk":2})");
  const std::string big(70000, 'q');
  post("/collections/src_big/_update?commit=true",
       R"({"docs":[{"id":"a"},{"id":"c","big_s":")" + big + R"("},{"id":"d"},{"id":"e"}]})");
  EXPECT_EQ(R"({"id":"a"})" "\n" R"({"id":"c","big_s":")" + big + R"("})" "\n"
            R"({"id":"d"})" "\n" R"({"id":"e"})" "\n", exportSources("src_big"));
  auto one = post("/collections/src_big/_search?format=docs",
                  R"({"query":{"match":{"id":"c"}},"fields":["_source"]})");
  EXPECT_EQ(R"({"_source":{"id":"c","big_s":")" + big + R"("}})" "\n", one);
}

TEST_F(SourceHttpTest, exportFailsOnAHitWithoutSourceAndWrapsControlLookingOnes) {
  post("/collections/_create", R"({"name":"src_export"})");
  post("/collections/src_export/_update?commit=true", R"({"docs":[{"id":"old"}]})");
  post("/collections/src_export/_schema", R"({"stored_resources":{"_source_":{}}})");
  post("/collections/src_export/_update?commit=true", R"({"docs":[{"id":"new"}]})");

  auto failed = httpRequest(server->getPort(), http::verb::post, "/collections/src_export/_search?format=docs&source=true",
                            R"({"query":{"all":true},"fields":["_source"],"limit":-1})");
  EXPECT_EQ(400, failed.result_int()) << failed.body();
  EXPECT_NE(std::string::npos, failed.body().find("missing_source")) << failed.body();
  // Without source=true the document comes back with a null source instead.
  auto partial = post("/collections/src_export/_search?format=docs",
                      R"({"query":{"match":{"id":"old"}},"fields":["_source"]})");
  EXPECT_EQ(R"({"_source":null})" "\n", partial);

  post("/collections/src_export/_update?commit=true", R"({"delete_ids":["old"]})");
  EXPECT_EQ(R"({"id":"new"})" "\n", exportSources("src_export"));

  // A source NDJSON ingest would read as a control record is wrapped so it
  // comes back as a document: nothing kept, or a lone "_name_" object (a key
  // the request's field_map dropped from the index but the source kept).
  post("/collections/src_export/_update?commit=true&source_includes=none_s",
       R"({"docs":[{"id":"empty","v_i":1}]})");
  post("/collections/src_export/_update?commit=true",
       R"({"docs":[{"id":"meta","_meta_":{"a":1}}],"field_map":{"_meta_":""},"source_includes":["_meta_"]})");
  auto lines = exportSources("src_export");
  EXPECT_EQ(R"({"id":"new"})" "\n" R"({"_update_":{"docs":[{}]}})" "\n"
            R"({"_update_":{"docs":[{"_meta_":{"a":1}}]}})" "\n", lines);
  // The wrapped lines reindex as documents rather than being read as controls.
  createWithSource("src_reimport");
  post("/collections/src_reimport/_update?commit=true", R"({"id":"new"})" "\n" R"({"_update_":{"docs":[{}]}})" "\n",
       "application/x-ndjson");
  EXPECT_EQ(R"({"id":"new"})" "\n" R"({"_update_":{"docs":[{}]}})" "\n", exportSources("src_reimport"));
}

TEST_F(SourceHttpTest, includesAndExcludesNarrowTheSourceButNotTheIndex) {
  createWithSource("src_filter", R"({"excludes":["secret_s","pw*"]})");
  post("/collections/src_filter/_update?commit=true",
       R"({"docs":[{"id":"1","secret_s":"k","name_s":"n","pw_s":"p"}]})");
  // A request's own list replaces the collection's, in the body or the URL (NDJSON too).
  post("/collections/src_filter/_update?commit=true",
       R"({"docs":[{"id":"2","secret_s":"k","title_s":"t"}],"source_includes":["id","title_s"]})");
  // An NDJSON _update_ group's list wins over the URL's.
  post("/collections/src_filter/_update?commit=true&source_excludes=title_s",
       R"({"id":"3","secret_s":"k","title_s":"t"})" "\n"
       R"({"_update_":{"source_includes":["id"]}})" "\n" R"({"id":"4","title_s":"t"})" "\n"
       R"({"_end_":{}})" "\n", "application/x-ndjson");
  EXPECT_EQ(R"({"id":"1","name_s":"n"})" "\n" R"({"id":"2","title_s":"t"})" "\n"
            R"({"id":"3","secret_s":"k"})" "\n" R"({"id":"4"})" "\n", exportSources("src_filter"));
  // The dropped field is still indexed.
  auto hits = post("/collections/src_filter/_search?format=docs",
                   R"({"query":{"match":{"secret_s":"k"}},"fields":["id"],"limit":-1})");
  EXPECT_EQ(3, std::ranges::count(hits, '\n')) << hits;

  auto both = httpRequest(server->getPort(), http::verb::post,
                          "/collections/src_filter/_update?source_includes=a&source_excludes=b", R"({"docs":[]})");
  EXPECT_EQ(400, both.result_int()) << both.body();
  auto bodyBoth = httpRequest(server->getPort(), http::verb::post, "/collections/src_filter/_update",
                              R"({"docs":[{"id":"5"}],"source_includes":["a"],"source_excludes":["b"]})");
  EXPECT_NE(bodyBoth.body().find("mutually exclusive"), std::string::npos) << bodyBoth.body();
  auto schemaBoth = httpRequest(server->getPort(), http::verb::post, "/collections/_create",
      R"({"name":"src_bad","schema":{"stored_resources":{"_source_":{"includes":["a"],"excludes":["b"]}}}})");
  EXPECT_EQ(400, schemaBoth.result_int()) << schemaBoth.body();
}

TEST_F(SourceHttpTest, sourceParamIsValidated) {
  createWithSource("src_param");
  post("/collections/src_param/_update?commit=true", R"({"docs":[{"id":"1"}]})");
  auto res = httpRequest(server->getPort(), http::verb::post, "/collections/src_param/_search?format=docs&source=true",
                         R"({"query":{"all":true},"fields":["id"]})");
  EXPECT_NE(res.body().find("source=true requires"), std::string::npos) << res.body();
  auto wrong = httpRequest(server->getPort(), http::verb::post, "/collections/src_param/_search?source=true",
                           R"({"query":{"all":true}})");
  EXPECT_EQ(400, wrong.result_int());
  auto off = httpRequest(server->getPort(), http::verb::post, "/collections/_create", R"({"name":"src_off"})");
  EXPECT_EQ(200, off.result_int());
  post("/collections/src_off/_update?commit=true", R"({"docs":[{"id":"1"}]})");
  auto notStored = httpRequest(server->getPort(), http::verb::post, "/collections/src_off/_search",
                               R"({"query":{"all":true},"fields":["_source"]})");
  EXPECT_EQ(400, notStored.result_int()) << notStored.body();
  EXPECT_NE(notStored.body().find("_source is not stored"), std::string::npos) << notStored.body();
}

namespace {

class SourceNodeTest : public LuxirTest {
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
};

}  // namespace

TEST_F(SourceNodeTest, nodeDefaultAppliesUnlessTheCreateRequestOptsOut) {
  LuxirNode node(config(true));
  HttpServer server(node, 2, 0);
  server.start();
  post(server, "/collections/_create", R"({"name":"on"})");
  post(server, "/collections/_create",
                       R"({"name":"out","schema":{"stored_resources":{"_source_":{"enabled":false}}}})");
  post(server, "/collections/auto/_update?commit=true", R"({"docs":[{"id":"1"}]})");  // auto-created
  post(server, "/collections/on/_update?commit=true", R"({"docs":[{"id":"1"}]})");
  post(server, "/collections/out/_update?commit=true", R"({"docs":[{"id":"1"}]})");

  EXPECT_EQ(R"({"id":"1"})" "\n", exportSources(server, "on"));
  EXPECT_EQ(R"({"id":"1"})" "\n", exportSources(server, "auto"));
  auto res = httpRequest(server.getPort(), http::verb::post, "/collections/out/_search",
                         R"({"query":{"all":true},"fields":["_source"]})");
  EXPECT_EQ(400, res.result_int()) << res.body();
  auto schema = httpRequest(server.getPort(), http::verb::get, "/collections/on/_schema");
  EXPECT_NE(std::string::npos, compact(schema.body()).find(R"("_source_":{})")) << schema.body();
  server.shutdown();
}

TEST_F(SourceNodeTest, sourcesSurviveARestart) {
  {
    LuxirNode node(config());
    HttpServer server(node, 2, 0);
    server.start();
    post(server, "/collections/_create",
                         R"({"name":"s","schema":{"stored_resources":{"_source_":{"min_file_bytes":0,"max_files":100}}}})");
    post(server, "/collections/s/_update?commit=true", std::string(R"({"docs":[)") + kOdd + "]}");
    post(server, "/collections/s/_update?commit=true", R"({"docs":[{"id":"b"}]})");
    // A merge that references both files rather than rewriting them.
    post(server, "/collections/s/_update",
                         R"({"docs":[],"commit":{"max_segments":1,"wait_for_merges":true}})");
    server.shutdown();
  }
  {
    LuxirNode node(config());
    HttpServer server(node, 2, 0);
    server.start();
    EXPECT_EQ(R"({"id":"a","n_i":"0042","z_i":1,"b_i":2,"b_i":3})" "\n" R"({"id":"b"})" "\n",
              exportSources(server, "s"));
    server.shutdown();
  }
}

}  // namespace luxir::test
