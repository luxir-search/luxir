// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <cctype>
#include <gtest/gtest.h>
#include "luxir/server/HttpServer.h"
#include "test/CollectionHelper.h"
#include "test/HttpReq.h"
#include "test/LuxirTest.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

namespace {

class CanMatchApiTest : public LuxirTest {
protected:
  std::optional<HttpServer> server;
  CollectionHelper helper{"canmatch"};

  void SetUp() override {
    LuxirTest::SetUp();
    helper.clear();
    server.emplace(*LuxirTest::luxirNode, 2, 0);
    server->start();
  }

  void TearDown() override {
    if (server) server->shutdown();
  }

  // The lowercased body without whitespace outside strings, so assertions
  // hold whether the server pretty-prints or not.
  std::string post(std::string_view target, std::string body) {
    auto res = httpRequest(server->getPort(), http::verb::post, target, std::move(body));
    EXPECT_EQ(200, res.result_int()) << res.body();
    std::string compact;
    bool inString = false;
    for (size_t i = 0; i < res.body().size(); i++) {
      char c = res.body()[i];
      if (c == '"' && (i == 0 || res.body()[i - 1] != '\\')) inString = !inString;
      if (!inString && std::isspace((unsigned char)c)) continue;
      compact.push_back((char)std::tolower((unsigned char)c));
    }
    return compact;
  }
};

} // namespace

TEST_F(CanMatchApiTest, verdictFromManifestSummaries) {
  ASSERT_TRUE(helper.indexAll({flatdoc("id", "a", "ts_i", (int64_t)100, "tenant_s", "acme"),
                               flatdoc("id", "b", "ts_i", (int64_t)200, "tenant_s", "acme")},
                              UpdateMessage::COMMIT).success);
  std::string hit = post("/collections/canmatch/_can_match?segments=true",
                         R"({"ops":{"q":{"top_docs":{"query":{"range":{"field":"ts_i","gte":150}}}}}})");
  // MAY_MATCH is the enum default, which JSON leaves out.
  EXPECT_EQ(std::string::npos, hit.find("no_match")) << hit;
  EXPECT_NE(std::string::npos, hit.find("\"summary_complete\":true")) << hit;
  EXPECT_NE(std::string::npos, hit.find("\"commit\":")) << hit;

  std::string miss = post("/collections/canmatch/_can_match",
                          R"({"ops":{"q":{"top_docs":{"query":{"range":{"field":"ts_i","gte":1000}}}}}})");
  EXPECT_NE(std::string::npos, miss.find("no_match")) << miss;
  EXPECT_NE(std::string::npos, miss.find("numeric bounds disjoint")) << miss;

  std::string tenant = post("/collections/canmatch/_can_match",
                            R"({"ops":{"q":{"top_docs":{"query":{"match":{"tenant_s":"globex"}}}}}})");
  EXPECT_NE(std::string::npos, tenant.find("no_match")) << tenant;

  std::string filtered = post("/collections/canmatch/_can_match",
                              R"({"ops":{"q":{"top_docs":{"query":{"all":true},"filter":[{"range":{"field":"ts_i","lt":50}}]}}}})");
  EXPECT_NE(std::string::npos, filtered.find("no_match")) << filtered;

  auto bad = httpRequest(server->getPort(), http::verb::post, "/collections/canmatch/_can_match", "{not json");
  EXPECT_EQ(400, bad.result_int()) << bad.body();
}
