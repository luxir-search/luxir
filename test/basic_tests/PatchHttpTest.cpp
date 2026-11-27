// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// Partial updates (UpdateRequest.patches) through HTTP: the patched document
// replaces the current one in the index and in its source, in version order
// with other updates of the same id.

#include <gtest/gtest.h>

#include "luxir/server/HttpServer.h"
#include "luxir/server/LuxirNode.h"
#include "test/HttpReq.h"
#include "test/LuxirTest.h"

namespace luxir::test {

namespace {

class PatchHttpTest : public LuxirTest {
protected:
  std::optional<HttpServer> server;

  void SetUp() override {
    server.emplace(*LuxirTest::luxirNode, 2, 0);
    server->start();
  }

  void TearDown() override {
    if (server) server->shutdown();
  }

  http::response<http::string_body> request(std::string_view path, std::string body,
                                            std::string_view type = "application/json") {
    return httpRequest(server->getPort(), http::verb::post, path, std::move(body), type);
  }

  std::string post(std::string_view path, std::string body, std::string_view type = "application/json") {
    auto res = request(path, std::move(body), type);
    EXPECT_EQ(200, res.result_int()) << path << ": " << res.body();
    return res.body();
  }

  void create(const std::string& coll) {
    post("/collections/_create", R"({"name":")" + coll + R"(","schema":{"stored_resources":{"_source_":{}}}})");
  }

  std::string update(const std::string& coll, std::string body) {
    return post("/collections/" + coll + "/_update", std::move(body));
  }

  // The sources of every live document, one per line, in id order.
  std::string sources(const std::string& coll) {
    return post("/collections/" + coll + "/_search?format=docs&source=true",
                R"({"query":{"all":true},"fields":["_source"],"limit":-1,"sort":[{"field":"id"}]})");
  }

  // Ids matching an expression query, comma separated, in id order.
  std::string ids(const std::string& coll, std::string_view expr) {
    auto body = post("/collections/" + coll + "/_search?format=docs",
                     R"({"query":{"expr":")" + std::string(expr) + R"("},"fields":["id"],"limit":-1,"sort":[{"field":"id"}]})");
    std::string out;
    size_t at = 0;
    while ((at = body.find(R"("id":")", at)) != std::string::npos) {
      at += 6;
      size_t end = body.find('"', at);
      if (!out.empty()) out += ',';
      out += body.substr(at, end - at);
    }
    return out;
  }
};

bool has(const std::string& text, std::string_view part) { return text.find(part) != std::string::npos; }

}  // namespace

TEST_F(PatchHttpTest, patchesReindexAndKeepTheSource) {
  create("p_basic");
  update("p_basic", R"({"docs":[{"id":"a","title_t":"red shirt","price_i":10,"tags_ss":["x"],"desc_t":"long text"},)"
                    R"({"id":"b","title_t":"blue shirt","price_i":20}],"commit":{}})");
  auto res = update("p_basic", R"({"patches":[)"
                               R"({"id":"a","ops":[{"op":"set","path":"title_t","value":"green shirt"},)"
                               R"({"op":"increment","path":"price_i","value":5},{"op":"append","path":"tags_ss","value":"y"},)"
                               R"({"op":"unset","path":"desc_t"}]},)"
                               R"({"id":"b","ops":[{"op":"set","path":"price_i","value":20}]},)"
                               R"({"id":"c","ops":[{"op":"set","path":"price_i","value":1}]}]})");
  EXPECT_TRUE(has(res, R"("status":"updated")")) << res;
  EXPECT_TRUE(has(res, R"("status":"noop")")) << res;
  EXPECT_TRUE(has(res, R"("status":"not_found")")) << res;
  EXPECT_EQ(R"({"id":"a","title_t":"green shirt","price_i":15,"tags_ss":["x","y"]})" "\n"
            R"({"id":"b","title_t":"blue shirt","price_i":20})" "\n", sources("p_basic"));
  // The index follows: the old text and price are gone, the new ones match.
  EXPECT_EQ("a", ids("p_basic", "title_t:green"));
  EXPECT_EQ("", ids("p_basic", "title_t:red"));
  EXPECT_EQ("a", ids("p_basic", "price_i:15"));
  EXPECT_EQ("a", ids("p_basic", "tags_ss:y"));
}

TEST_F(PatchHttpTest, patchesOfOneIdChainAndLaterWritesWin) {
  create("p_order");
  update("p_order", R"({"docs":[{"id":"a","n_i":1}],"commit":{}})");
  update("p_order", R"({"patches":[{"id":"a","ops":[{"op":"increment","path":"n_i","value":1}]},)"
                    R"({"id":"a","ops":[{"op":"increment","path":"n_i","value":10}]}]})");
  EXPECT_EQ(R"({"id":"a","n_i":12})" "\n", sources("p_order"));
  // An uncommitted overwrite before the patch is its base.
  update("p_order", R"({"docs":[{"id":"a","n_i":100}]})");
  update("p_order", R"({"patches":[{"id":"a","ops":[{"op":"increment","path":"n_i","value":1}]}]})");
  EXPECT_EQ(R"({"id":"a","n_i":101})" "\n", sources("p_order"));
  // A later overwrite replaces the patched document, and a later delete removes it.
  update("p_order", R"({"docs":[{"id":"a","n_i":7}],"commit":{}})");
  EXPECT_EQ(R"({"id":"a","n_i":7})" "\n", sources("p_order"));
  update("p_order", R"({"patches":[{"id":"a","ops":[{"op":"increment","path":"n_i","value":1}]}]})");
  update("p_order", R"({"delete_ids":["a"],"commit":{}})");
  EXPECT_EQ("", sources("p_order"));
  EXPECT_EQ("", ids("p_order", "n_i:8"));
}

TEST_F(PatchHttpTest, upsertConditionsAndVersions) {
  create("p_cond");
  auto res = update("p_cond", R"({"patches":[{"id":"a","upsert":true,"ops":[{"op":"increment","path":"n_i","value":1}]}]})");
  EXPECT_TRUE(has(res, R"("status":"created")")) << res;
  EXPECT_EQ(R"({"id":"a","n_i":1})" "\n", sources("p_cond"));
  // The version a patch reports is the document's _version_, and if_version checks it.
  auto version = res.substr(res.find(R"("version":)") + 10);
  version = version.substr(0, version.find_first_not_of("0123456789"));
  res = update("p_cond", R"({"patches":[{"id":"a","if_version":)" + version + R"(,"ops":[{"op":"set","path":"s_s","value":"x"}]},)"
                         R"({"id":"a","if_version":)" + version + R"(,"ops":[{"op":"set","path":"t_s","value":"y"}]}]})");
  EXPECT_TRUE(has(res, R"("status":"updated")")) << res;
  EXPECT_TRUE(has(res, R"("status":"conflict")")) << res;
  EXPECT_EQ(R"({"id":"a","n_i":1,"s_s":"x"})" "\n", sources("p_cond"));
  res = update("p_cond", R"({"patches":[{"id":"a","when":[{"path":"n_i","op":"gt","value":5}],)"
                         R"("ops":[{"op":"set","path":"n_i","value":0}]}]})");
  EXPECT_TRUE(has(res, R"("status":"skipped")")) << res;
  res = update("p_cond", R"({"patches":[{"id":"a","delete_doc":true,"when":[{"path":"s_s","op":"eq","value":"x"}]}]})");
  EXPECT_TRUE(has(res, R"("status":"deleted")")) << res;
  EXPECT_EQ("", sources("p_cond"));
}

TEST_F(PatchHttpTest, rejectsWhatCannotResolve) {
  post("/collections/_create", R"({"name":"p_nosource"})");
  auto res = request("/collections/p_nosource/_update", R"({"patches":[{"id":"a","upsert":true}]})");
  EXPECT_TRUE(has(res.body(), "_source_")) << res.body();
  create("p_reject");
  for (const char* body : {R"({"patches":[{"id":"a","ops":[{"op":"set","path":"id","value":"b"}]}]})",
                           R"({"patches":[{"id":"a","ops":[{"op":"set","path":"_version_","value":1}]}]})",
                           R"({"patches":[{"id":"a"}],"docs":[{"id":"a"}]})",
                           R"({"patches":[{"id":"a"}],"delete_ids":["a"]})",
                           R"({"patches":[{"id":"a"}],"all_or_none":true})"}) {
    res = request("/collections/p_reject/_update", body);
    EXPECT_TRUE(has(res.body(), R"("status":"error")")) << body << ": " << res.body();
  }
  // A document whose source was narrowed cannot be patched.
  update("p_reject", R"({"docs":[{"id":"n","a_i":1,"b_i":2}],"source_includes":["a_i"],"commit":{}})");
  res = request("/collections/p_reject/_update", R"({"patches":[{"id":"n","ops":[{"op":"set","path":"a_i","value":3}]}]})");
  EXPECT_TRUE(has(res.body(), R"("status":"error")")) << res.body();
  EXPECT_TRUE(has(res.body(), "narrowed")) << res.body();
}

TEST_F(PatchHttpTest, ndjsonStreamsRejectPatches) {
  create("p_ndjson");
  auto res = request("/collections/p_ndjson/_update?commit=true",
                     R"({"_update_":{"patches":[{"id":"a","upsert":true}]}})" "\n", "application/x-ndjson");
  EXPECT_TRUE(has(res.body(), "not supported in an NDJSON stream")) << res.body();
  EXPECT_EQ("", sources("p_ndjson"));
}

}  // namespace luxir::test
