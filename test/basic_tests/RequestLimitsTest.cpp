// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "luxir/LuxirConfig.h"

using namespace luxir;
using namespace luxir::test;

// Node-configured bounds on one request's work and memory (SearchConfig).
class RequestLimitsTest : public LuxirTest {
protected:
  static constexpr int DOCS = 8;

  // DOCS docs: id d0..d7, n_i 0..7, and a tag_s of five distinct values.
  static void indexDocs(CollectionHelper& helper, int docs = DOCS) {
    std::vector<Doc> batch;
    for (int i = 0; i < docs; i++) {
      batch.push_back(flatdoc("id", "d" + std::to_string(i), "n_i", i,
                              "tag_s", "t" + std::to_string(i % 5)));
    }
    ASSERT_TRUE(helper.indexAll(batch, UpdateMessage::COMMIT).success);
  }

  static auto run(CollectionHelper& helper, std::string_view body) {
    auto request = localReq(helper.getSearchEngine());
    std::string json = R"({"collection":"main",)" + std::string(body.substr(1));
    EXPECT_TRUE(api::read_json(request->rawRequest(), json, request->mr)) << json;
    request->execute(1);
    return request;
  }

  static size_t buckets(const LocalReq& request, std::string_view name) {
    return request.responses[0]->proto.ops.at(name)->facetResult()->counts.size();
  }
};

TEST_F(RequestLimitsTest, defaultsBoundEveryRequest) {
  LuxirConfig config;
  EXPECT_EQ(10000, config.search.max_result_window);
  EXPECT_EQ(65536, config.search.max_facet_buckets);
  EXPECT_EQ(1000, config.search.max_ops);
  EXPECT_EQ(10000, config.search.multiterm_max_expansions);
  EXPECT_GT(config.search.multiterm_max_terms_examined, 0);
  EXPECT_GT(config.search.multiterm_max_postings, 0);

  // The breaker is an eighth of the node budget, with a floor, and a fixed
  // ceiling when the node budget is unlimited. An explicit value is kept.
  auto resolved = [](int64_t nodeMb, int64_t explicitBytes = -1) {
    LuxirConfig c;
    c.max_ram_mb = nodeMb;
    c.search.request_memory_max_bytes = explicitBytes;
    c.resolveRamBudgets();
    return c.search.request_memory_max_bytes;
  };
  EXPECT_EQ(2048LL << 20, resolved(16384));
  EXPECT_EQ(SearchConfig::REQUEST_MEMORY_MIN_AUTO_BYTES, resolved(1024));
  EXPECT_EQ(SearchConfig::REQUEST_MEMORY_UNLIMITED_NODE_BYTES, resolved(0));
  EXPECT_EQ(0, resolved(16384, 0));
  EXPECT_EQ(12345, resolved(16384, 12345));
  EXPECT_GT(luxirNode->getConfig().search.request_memory_max_bytes, 0);
}

TEST_F(RequestLimitsTest, resultWindowRejectsDeepPagesAndCapsAll) {
  LuxirConfig config;
  config.search.max_result_window = 5;
  LuxirNode node(config);
  CollectionHelper helper(node);
  indexDocs(helper);

  auto page = run(helper, R"({"ops":{"q":{"top_docs":{"limit":3,"offset":2}}}})");
  ASSERT_TRUE(page->ok()) << page->errorMsg();
  EXPECT_EQ(3u, page->getDocs().size());
  EXPECT_FALSE(page->hasWarning("result_window_capped"));

  for (const char* body : {R"({"ops":{"q":{"top_docs":{"limit":6}}}})",
                           R"({"ops":{"q":{"top_docs":{"limit":3,"offset":3}}}})",
                           R"({"ops":{"q":{"top_docs":{"limit":-1,"offset":5}}}})",
                           R"({"ops":{"f":{"fusion":{"rrf":{},"limit":6,"sources":{"a":{}}}}}})",
                           R"({"ops":{"f":{"fusion":{"rrf":{},"sources":{"a":{"limit":6}}}}}})"}) {
    auto rejected = run(helper, body);
    EXPECT_FALSE(rejected->ok()) << body;
    EXPECT_EQ(ErrorKind::INVALID_REQUEST, rejected->errorKind()) << body;
    EXPECT_EQ("result_window_exceeded", rejected->errorCode()) << rejected->errorMsg();
  }

  // limit -1 is everything up to the window, and says when it stopped there.
  auto all = run(helper, R"({"ops":{"q":{"top_docs":{"limit":-1,"offset":1,"sort":"n_i asc","fields":["id"]}}}})");
  ASSERT_TRUE(all->ok()) << all->errorMsg();
  EXPECT_EQ(4u, all->getDocs().size());
  EXPECT_TRUE(all->hasWarning("result_window_capped"));
  auto few = run(helper, R"({"ops":{"q":{"top_docs":{"query":"n_i:[0 TO 1]","limit":-1}}}})");
  ASSERT_TRUE(few->ok()) << few->errorMsg();
  EXPECT_EQ(2u, few->getDocs().size());
  EXPECT_FALSE(few->hasWarning("result_window_capped"));
  auto fused = run(helper, R"({"ops":{"f":{"fusion":{"rrf":{},"limit":-1,"sources":{"a":{"limit":-1}}}}}})");
  ASSERT_TRUE(fused->ok()) << fused->errorMsg();
  EXPECT_EQ(5u, fused->getDocs("f").size());
  EXPECT_TRUE(fused->hasWarning("result_window_capped"));

  // The docs format is an export: it streams every match past the window.
  auto exported = run(helper,
      R"({"response_format":"docs","ops":{"q":{"top_docs":{"limit":-1,"offset":6}}}})");
  ASSERT_TRUE(exported->ok()) << exported->errorMsg();
  EXPECT_EQ(2u, exported->getDocs().size());
  EXPECT_FALSE(exported->hasWarning("result_window_capped"));
}

TEST_F(RequestLimitsTest, facetBucketCapRejectsLargeLimitsAndAll) {
  LuxirConfig config;
  config.search.max_facet_buckets = 3;
  LuxirNode node(config);
  CollectionHelper helper(node);
  indexDocs(helper);

  auto atCap = run(helper, R"({"ops":{"f":{"field_facet":{"field":"tag_s","limit":3}}}})");
  ASSERT_TRUE(atCap->ok()) << atCap->errorMsg();
  EXPECT_EQ(3u, buckets(*atCap, "f"));

  auto over = run(helper, R"({"ops":{"f":{"field_facet":{"field":"n_i","limit":4}}}})");
  EXPECT_EQ(ErrorKind::INVALID_REQUEST, over->errorKind());
  EXPECT_EQ("facet_limit_exceeded", over->errorCode()) << over->errorMsg();

  // limit -1 is all buckets, and fails when all is more than the cap.
  for (const char* field : {"tag_s", "n_i"}) {
    auto all = run(helper, R"({"ops":{"f":{"field_facet":{"field":")" + std::string(field)
        + R"(","limit":-1}}}})");
    EXPECT_EQ(ErrorKind::INVALID_REQUEST, all->errorKind()) << field;
    EXPECT_EQ("facet_limit_exceeded", all->errorCode()) << all->errorMsg();
  }
  auto few = run(helper,
      R"({"ops":{"q":{"top_docs":{"query":"n_i:[0 TO 2]","limit":0,"ops":{"f":{"field_facet":{"field":"tag_s","limit":-1}}}}}}})");
  ASSERT_TRUE(few->ok()) << few->errorMsg();
  EXPECT_EQ(3u, few->docList("q")->ops.at("f")->facetResult()->counts.size());
}

TEST_F(RequestLimitsTest, maxOpsCountsEveryNestingLevel) {
  LuxirConfig config;
  config.search.max_ops = 3;
  LuxirNode node(config);
  CollectionHelper helper(node);
  indexDocs(helper);

  auto atCap = run(helper, R"j({"ops":{"q":{"top_docs":{"ops":{
      "f":{"field_facet":{"field":"tag_s","ops":{"m":"max(n_i)"}}}}}}}})j");
  ASSERT_TRUE(atCap->ok()) << atCap->errorMsg();

  for (const char* body : {
           R"j({"ops":{"q":{"top_docs":{"ops":{"f":{"field_facet":{"field":"tag_s","ops":{
               "m":"max(n_i)","s":"sum(n_i)"}}}}}}}})j",
           R"({"ops":{"f":{"fusion":{"rrf":{},"sources":{"a":{},"b":{},"c":{}}}}}})"}) {
    auto rejected = run(helper, body);
    EXPECT_EQ(ErrorKind::INVALID_REQUEST, rejected->errorKind()) << body;
    EXPECT_EQ("too_many_ops", rejected->errorCode()) << rejected->errorMsg();
  }
}

TEST_F(RequestLimitsTest, topDocsHeapsAndSortSlotsChargeTheBreaker) {
  LuxirConfig config;
  config.search.max_result_window = 0;
  config.search.request_memory_max_bytes = 16 * 1024;
  LuxirNode node(config);
  CollectionHelper helper(node);
  indexDocs(helper, 2000);

  for (const char* sort : {"", R"(,"sort":"n_i desc")"}) {
    auto shallow = run(helper, R"({"ops":{"q":{"top_docs":{"limit":10)" + std::string(sort) + "}}}}");
    ASSERT_TRUE(shallow->ok()) << shallow->errorMsg();
    EXPECT_EQ(10u, shallow->getDocs().size());

    auto deep = run(helper, R"({"ops":{"q":{"top_docs":{"limit":-1)" + std::string(sort) + "}}}}");
    EXPECT_EQ(ErrorKind::RESOURCE_EXHAUSTED, deep->errorKind()) << sort;
    EXPECT_EQ("request_memory_exceeded", deep->errorCode()) << deep->errorMsg();
    EXPECT_NE(std::string::npos, deep->errorMsg().find("'top docs heap'")) << deep->errorMsg();
    EXPECT_NE(std::string::npos, deep->errorMsg().find("top_docs 'q'")) << deep->errorMsg();
  }
}
