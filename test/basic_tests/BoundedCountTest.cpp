// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// TopDocs.count_limit: a get_number count that may stop at the limit.

#include <string>
#include <utility>
#include <vector>

#include "luxir/search/FilterCache.h"
#include "luxir/server/JsonRequest.h"
#include "luxir/server/JsonResponse.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

namespace {
using Ranked = std::vector<std::pair<std::string, float>>;
}

class BoundedCountTest : public LuxirTest {
protected:
  size_t cacheBytes = 0;

  // 2000 docs over 4 segments; alpha in every doc, beta in i%3==0,
  // gamma in i%3==1. Term frequencies and lengths vary so scores differ.
  // The filter cache is off unless a fixture sizes it, so repeated queries
  // keep the same plan.
  void SetUp() override {
    clearCollection("bounded");
    CollectionHelper helper("bounded");
    helper.getIndexWriter()->snapshots.readers.filterCache = std::make_shared<FilterCache>(
        FilterCacheConfig{.maxBytes = cacheBytes});
    for (int s = 0; s < 4; s++) {
      std::vector<Doc> docs;
      for (int i = s * 500; i < (s + 1) * 500; i++) {
        std::string body = "alpha";
        for (int r = 0; r < i % 5; r++) body += " alpha";
        if (i % 3 == 0) body += " beta";
        if (i % 3 == 1) body += " gamma gamma";
        for (int r = 0; r < i % 7; r++) body += " filler";
        docs.push_back(flatdoc("id", std::to_string(i), "body_w", body, "n_i", (int64_t)i,
                               "color_s", i % 2 ? "red" : "blue"));
      }
      ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
    }
  }

  auto run(const std::string& body, int parallel) {
    auto request = localReq(luxirNode->getSearchEngine());
    parseQueryRequest(body, request->rawRequest(), request->mr);
    request->collection("bounded");
    request->execute(parallel);
    EXPECT_TRUE(request->ok()) << request->errorMsg() << body;
    return request;
  }

  static Ranked ranked(const LocalReq& request) {
    Ranked out;
    const auto* docs = request.docList();
    const auto& ids = std::get<luxir::api::ColStr>(docs->columns.at("id").kind);
    const auto& scores = std::get<luxir::api::ColFloat>(docs->columns.at("_score_").kind);
    for (size_t i = 0; i < ids.v.size(); i++) out.emplace_back(std::string(ids.v[i]), scores.v[i]);
    return out;
  }

  static std::string search(const std::string& query, const std::string& count) {
    return R"({"query":")" + query + R"(","fields":["id"],"get_scores":true,"document_format":"columns","limit":10)"
        + count + "}";
  }
};

TEST_F(BoundedCountTest, limitBelowAndAboveMatchCount) {
  // beta OR gamma: 1334 matches; the largest clause (667) proves no limit
  // above it, so these bounds come from collection.
  std::string query = "body_w:beta OR body_w:gamma";
  for (int parallel : {1, -1}) {
    auto exact = run(search(query, R"(,"get_number":true)"), parallel);
    auto uncounted = run(search(query, ""), parallel);
    EXPECT_EQ(1334, exact->getMatchCount());
    EXPECT_FALSE(exact->docList()->found_is_lower_bound);
    EXPECT_EQ(ranked(*exact), ranked(*uncounted));

    auto below = run(search(query, R"(,"get_number":true,"count_limit":1000)"), parallel);
    EXPECT_EQ(1000, below->getMatchCount()) << parallel;
    EXPECT_TRUE(below->docList()->found_is_lower_bound);
    EXPECT_EQ(ranked(*uncounted), ranked(*below));

    auto above = run(search(query, R"(,"get_number":true,"count_limit":5000)"), parallel);
    EXPECT_EQ(1334, above->getMatchCount());
    EXPECT_FALSE(above->docList()->found_is_lower_bound);
    EXPECT_EQ(ranked(*uncounted), ranked(*above));

    auto equal = run(search(query, R"(,"get_number":true,"count_limit":1334)"), parallel);
    EXPECT_EQ(1334, equal->getMatchCount());
    EXPECT_EQ(ranked(*uncounted), ranked(*equal));
  }
}

TEST_F(BoundedCountTest, termShortcutProvesLimit) {
  CollectionHelper("bounded").deleteByIds({"0", "1", "2"}, UpdateMessage::COMMIT);
  for (int parallel : {1, -1}) {
    // df 2000 minus 3 deletions proves 1997 matches without counting.
    auto proven = run(search("body_w:alpha", R"(,"get_number":true,"count_limit":1997)"), parallel);
    EXPECT_EQ(1997, proven->getMatchCount());
    EXPECT_TRUE(proven->docList()->found_is_lower_bound);
    auto uncounted = run(search("body_w:alpha", ""), parallel);
    EXPECT_EQ(ranked(*uncounted), ranked(*proven));
    // Above the proof the count runs to completion.
    auto exact = run(search("body_w:alpha", R"(,"get_number":true,"count_limit":1998)"), parallel);
    EXPECT_EQ(1997, exact->getMatchCount());
    EXPECT_FALSE(exact->docList()->found_is_lower_bound);
  }
}

TEST_F(BoundedCountTest, exactShapesIgnoreLimit) {
  std::string query = "body_w:beta OR body_w:gamma";
  // Ops need the exact domain, so the count is exact.
  auto withOps = run(R"({"query":")" + query + R"(","limit":10,"get_number":true,"count_limit":10,)"
      R"("ops":{"c":{"field_facet":{"field":"color_s"}}}})", -1);
  EXPECT_EQ(1334, withOps->getMatchCount());
  EXPECT_FALSE(withOps->docList()->found_is_lower_bound);
  // A field sort counts exactly.
  auto sorted = run(R"({"query":")" + query + R"(","limit":10,"get_number":true,"count_limit":10,)"
      R"("sort":"n_i asc"})", -1);
  EXPECT_EQ(1334, sorted->getMatchCount());
  EXPECT_FALSE(sorted->docList()->found_is_lower_bound);
  // A filtered query still bounds through collection.
  auto filtered = run(R"({"query":")" + query + R"(","limit":10,"get_number":true,"count_limit":100,)"
      R"("filter":["color_s:red"]})", -1);
  EXPECT_EQ(100, filtered->getMatchCount());
  EXPECT_TRUE(filtered->docList()->found_is_lower_bound);

  auto bad = localReq(luxirNode->getSearchEngine());
  parseQueryRequest(search(query, R"(,"get_number":true,"count_limit":0)"), bad->rawRequest(), bad->mr);
  bad->collection("bounded");
  bad->execute(1);
  EXPECT_FALSE(bad->ok());
}

// A resident whole-membership cache entry already holds the exact count; the
// bounded plan yields to it and reports the exact count.
class BoundedCountCachedTest : public BoundedCountTest {
protected:
  void SetUp() override {
    cacheBytes = 64 << 20;
    BoundedCountTest::SetUp();
  }
};

TEST_F(BoundedCountCachedTest, residentMembershipCountsExactly) {
  std::string query = "body_w:beta OR body_w:gamma";
  auto uncounted = run(search(query, ""), -1);
  for (int i = 0; i < 4; i++) {
    auto bounded = run(search(query, R"(,"get_number":true,"count_limit":1000)"), -1);
    bool lower = bounded->docList()->found_is_lower_bound;
    EXPECT_EQ(lower ? 1000 : 1334, bounded->getMatchCount()) << i;
    EXPECT_EQ(ranked(*uncounted), ranked(*bounded));
  }
}

TEST_F(BoundedCountTest, trackTotalHitsShorthand) {
  std::string query = R"({"query":"body_w:beta OR body_w:gamma","fields":["id"],"track_total_hits":)";
  auto exact = run(query + "true}", -1);
  EXPECT_EQ(1334, exact->getMatchCount());
  EXPECT_FALSE(exact->docList()->found_is_lower_bound);

  auto none = run(query + "false}", -1);
  EXPECT_FALSE(none->docList()->found.has_value());

  auto bounded = run(query + "100}", -1);
  EXPECT_EQ(100, bounded->getMatchCount());
  auto body = renderSearchResponseBody(bounded->responses[0]->proto, true);
  EXPECT_NE(std::string::npos, body.find(R"("found":100,"found_is_lower_bound":true)")) << body;
  auto runs = renderDocRuns(bounded->responses[0]->proto);
  ASSERT_FALSE(runs.empty());
  DocLinesState framing;
  std::string header;
  ASSERT_TRUE(frameDocRun(runs[0], framing, {}, header));
  EXPECT_NE(std::string::npos, header.find(R"("found":100,"found_is_lower_bound":true)")) << header;
  auto exactBody = renderSearchResponseBody(exact->responses[0]->proto, true);
  EXPECT_EQ(std::string::npos, exactBody.find("found_is_lower_bound")) << exactBody;
}
