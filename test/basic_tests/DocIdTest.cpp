// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <glaze/json.hpp>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "luxir/index/IdGenerator.h"
#include "luxir/server/HttpServer.h"
#include "test/CollectionHelper.h"
#include "test/DurableIndexInfo.h"
#include "test/HttpReq.h"
#include "test/LuxirTest.h"
#include "test/SchemaBuilder.h"
#include "test/TestUtils.h"

namespace luxir::test {

using IdMode = api::SchemaDef::IdMode;

class DocIdTest : public LuxirTest {
protected:
  std::optional<HttpServer> server;
  CollectionHelper helper{"main"};

  void SetUp() override {
    server.emplace(*LuxirTest::luxirNode, 2, 0);
    server->start();
  }
  void TearDown() override { server->shutdown(); }

  void idMode(IdMode mode) {
    SchemaBuilder schema;
    schema.idMode(mode);
    schema.set(helper.collection());
  }

  IndexResult submit(std::vector<Doc> docs, bool allOrNone = false) {
    CollectionHelper::UpdateBuilder b;
    for (const auto& doc : docs) b.add(doc);
    b.returnIds().allOrNone(allOrNone).commit();
    return helper.submit(b);
  }

  // The ids of the documents an HTTP get returns, sorted.
  std::multiset<std::string> fetched(http::verb method, const std::string& target, std::string body = {}) {
    auto res = httpRequest(server->getPort(), method, target, std::move(body));
    EXPECT_EQ(200, res.result_int()) << res.body();
    std::multiset<std::string> ids;
    for (auto line : splitLines(res.body())) {
      glz::generic doc;
      EXPECT_FALSE(glz::read_json(doc, line)) << line;
      if (doc.is_object() && doc.get_object().contains("id")) ids.insert(doc["id"].get_string());
    }
    return ids;
  }

  std::multiset<std::string> mget(std::string idsJson) {
    return fetched(http::verb::post, "/collections/main/_mget?format=docs", R"({"ids":)" + idsJson + "}");
  }

  static std::vector<std::string> splitLines(const std::string& body) {
    std::vector<std::string> lines;
    for (size_t at = 0; at < body.size();) {
      auto end = body.find('\n', at);
      if (end == std::string::npos) end = body.size();
      if (end > at) lines.emplace_back(body.substr(at, end - at));
      at = end + 1;
    }
    return lines;
  }
};

TEST_F(DocIdTest, requireRejectsMissingAndNullIds) {
  idMode(IdMode::REQUIRE);
  auto nullId = httpRequest(server->getPort(), http::verb::post, "/collections/main/_update",
                            R"({"docs":[{"id":null,"text_w":"x"}]})");
  EXPECT_NE(std::string::npos, nullId.body().find("missing_id")) << nullId.body();
  auto result = submit({flatdoc("text_w", "x"), flatdoc("id", "a", "text_w", "x")});
  ASSERT_EQ(1u, result.errors.size());
  EXPECT_EQ(0, result.errors[0].index);
  EXPECT_EQ("missing_id", result.errors[0].code);
  EXPECT_EQ(std::vector<std::string>{"a"}, result.ids);

  auto all = submit({flatdoc("id", "b"), flatdoc("text_w", "y")}, true);
  EXPECT_FALSE(all.success);
  EXPECT_EQ((std::multiset<std::string>{"a"}), mget(R"(["a","b"])"));
}

TEST_F(DocIdTest, generateAssignsOrderedIdsAndKeepsExplicitOnes) {
  idMode(IdMode::GENERATE);
  auto result = submit({flatdoc("text_w", "x"), flatdoc("id", (int64_t)123), flatdoc("text_w", "y"),
                        flatdoc("text_w", "z")});
  ASSERT_TRUE(result.success);
  ASSERT_EQ(4u, result.ids.size());
  EXPECT_EQ("123", result.ids[1]);
  std::vector<std::string> generated = {result.ids[0], result.ids[2], result.ids[3]};
  for (const auto& id : generated) EXPECT_EQ(IdGenerator::ENCODED_LEN, id.size()) << id;
  EXPECT_LT(generated[0], generated[1]);
  EXPECT_LT(generated[1], generated[2]);

  // An empty id counts as missing.
  auto empty = submit({flatdoc("id", "", "text_w", "e")});
  EXPECT_EQ(IdGenerator::ENCODED_LEN, empty.ids.at(0).size());

  auto later = submit({flatdoc("text_w", "w")});
  EXPECT_LT(generated[2], later.ids.at(0));
  EXPECT_EQ((std::multiset<std::string>{generated[0], "123", later.ids[0]}),
            mget("[\"" + generated[0] + "\",\"123\",\"" + later.ids[0] + "\"]"));

  // Generated ids never collide, so they leave nothing for delete passes.
  auto iw = helper.getIndexWriter();
  iw->updateGraph->wait_for_all();
  EXPECT_TRUE(iw->pendingDeletes.empty());
  // The commit records the generator's clock for the next writer lifetime.
  auto durable = readDurableIndexInfo(iw->dir);
  EXPECT_EQ(iw->idGenerator.clockMs(), durable->id_clock_ms);
  EXPECT_GT(durable->id_clock_ms, 0u);
}

TEST_F(DocIdTest, allowMissingIndexesWithoutIdentity) {
  auto result = submit({flatdoc("text_w", "noid"), flatdoc("id", (int64_t)7, "text_w", "seven")});
  ASSERT_TRUE(result.success);
  EXPECT_EQ((std::vector<std::string>{"", "7"}), result.ids);
  EXPECT_EQ((std::multiset<std::string>{"7"}), mget(R"(["7",""])"));
  // A failed document reports its numeric id in canonical form.
  auto failed = submit({flatdoc("id", (int64_t)8, "no_such_field", "x")});
  ASSERT_EQ(1u, failed.errors.size());
  EXPECT_EQ("8", failed.errors[0].id);
}

TEST_F(DocIdTest, getAndDeleteById) {
  std::string longId(300, 'L');
  ASSERT_TRUE(submit({flatdoc("id", "a", "text_w", "old"), flatdoc("id", "b"), flatdoc("id", "c"),
                      flatdoc("id", "d/e+f"), flatdoc("id", longId)}).success);
  ASSERT_TRUE(submit({flatdoc("id", "a", "text_w", "new")}).success);

  EXPECT_EQ((std::multiset<std::string>{"a", "b"}), mget(R"(["a","b","a","missing"])"));
  // An id over 255 bytes is indexed (and returned) in its long_terms hashed form.
  EXPECT_EQ(1u, mget("[\"" + longId + "\"]").size());
  EXPECT_EQ((std::multiset<std::string>{"d/e+f"}),
            fetched(http::verb::get, "/collections/main/_doc/d%2Fe+f?format=docs"));
  auto one = httpRequest(server->getPort(), http::verb::get, "/collections/main/_doc/a?fields=text_w&format=docs");
  EXPECT_NE(std::string::npos, one.body().find("new")) << one.body();
  EXPECT_EQ(std::string::npos, one.body().find("old")) << one.body();

  // An explicit field list still returns id; an id may end in a route name.
  EXPECT_EQ((std::multiset<std::string>{"a"}),
            fetched(http::verb::post, "/collections/main/_mget?format=docs", R"({"ids":["a"],"fields":["text_w"]})"));
  ASSERT_TRUE(submit({flatdoc("id", "x/_search")}).success);
  EXPECT_EQ((std::multiset<std::string>{"x/_search"}),
            fetched(http::verb::get, "/collections/main/_doc/x/_search?format=docs"));

  auto del = httpRequest(server->getPort(), http::verb::post, "/collections/main/_delete?commit=true",
                         R"({"ids":["b","nope"]})");
  ASSERT_EQ(200, del.result_int()) << del.body();
  auto delOne = httpRequest(server->getPort(), http::verb::delete_, "/collections/main/_doc/c?commit=true");
  ASSERT_EQ(200, delOne.result_int()) << delOne.body();
  EXPECT_EQ((std::multiset<std::string>{"a"}), mget(R"(["a","b","c"])"));

  auto empty = httpRequest(server->getPort(), http::verb::post, "/collections/main/_mget", R"({"ids":[]})");
  EXPECT_EQ(400, empty.result_int()) << empty.body();
}

TEST(IdGeneratorTest, logicalClockAndEncoding) {
  int64_t t0 = IdGenerator::EPOCH_MS + 1000;
  IdGenerator gen(3, 0);
  uint64_t a = gen.next(t0);
  uint64_t b = gen.next(t0 - 500);  // the wall clock stepped back
  EXPECT_GT(b, a);
  EXPECT_EQ(1000u, gen.clockMs());
  for (int i = 0; i < 5000; i++) gen.next(t0);  // more than one millisecond's sequence
  EXPECT_EQ(1001u, gen.clockMs());
  EXPECT_EQ(3u, (a >> IdGenerator::SEQ_BITS) & IdGenerator::WORKER_MASK);

  // A reopened writer starts at the committed clock even if the wall clock is behind it.
  IdGenerator reopened(4, 5000);
  EXPECT_EQ(5001u, reopened.next(t0) >> (IdGenerator::WORKER_BITS + IdGenerator::SEQ_BITS));

  char x[IdGenerator::ENCODED_LEN], y[IdGenerator::ENCODED_LEN];
  EXPECT_LT(IdGenerator::encode(a, x), IdGenerator::encode(b, y));
  EXPECT_EQ("0000000000000", IdGenerator::encode(0, x));
  EXPECT_EQ("FZZZZZZZZZZZZ", IdGenerator::encode(UINT64_MAX, x));
}

} // namespace luxir::test
