// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <string>
#include <utility>
#include <vector>

#include "luxir/reader/SkipStats.h"
#include "luxir/search/FilterCache.h"
#include "luxir/server/JsonRequest.h"
#include "luxir/server/JsonResponse.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

class BoundedCountTest : public LuxirTest {
protected:
  size_t cacheBytes = 0;
  std::shared_ptr<FilterCache> cache;

  // Four segments; varying term frequencies and lengths keep ranking nonuniform.
  void SetUp() override {
    clearCollection("bounded");
    CollectionHelper helper("bounded");
    cache = std::make_shared<FilterCache>(FilterCacheConfig{
        .maxBytes = cacheBytes, .minSegmentDocs = 0, .admissionThreshold = 1});
    helper.getIndexWriter()->snapshots.readers.filterCache = cache;
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

  static auto ranked(const LocalReq& request) {
    std::vector<std::pair<std::string, float>> out;
    const auto* docs = request.docList();
    const auto& ids = std::get<api::ColStr>(docs->columns.at("id").kind);
    const auto& scores = std::get<api::ColFloat>(docs->columns.at("_score_").kind);
    for (size_t i = 0; i < ids.v.size(); i++) out.emplace_back(std::string(ids.v[i]), scores.v[i]);
    return out;
  }

  static std::string search(const std::string& query, int64_t countLimit,
                            const std::string& extra = "") {
    return R"({"query":")" + query
        + R"(","fields":["id"],"get_scores":true,"document_format":"columns","limit":10,"count_limit":)"
        + std::to_string(countLimit) + extra + "}";
  }

  static void expectCount(const LocalReq& request, int64_t exact, int64_t limit) {
    ASSERT_TRUE(request.docList()->found.has_value());
    if (request.docList()->found_gte) {
      EXPECT_EQ(limit, request.getMatchCount());
      EXPECT_GE(exact, limit);
    } else EXPECT_EQ(exact, request.getMatchCount());
  }
};

TEST_F(BoundedCountTest, countsAndRanking) {
  std::string query = "body_w:beta OR body_w:gamma";
  for (int parallel : {1, -1}) {
    auto exact = run(search(query, -1), parallel);
    auto uncounted = run(search(query, 0), parallel);
    EXPECT_EQ(1334, exact->getMatchCount());
    EXPECT_FALSE(exact->docList()->found_gte);
    EXPECT_FALSE(uncounted->docList()->found.has_value());
    for (int64_t limit : {1000, 1334, 5000}) {
      auto bounded = run(search(query, limit), parallel);
      expectCount(*bounded, 1334, limit);
      EXPECT_EQ(ranked(*uncounted), ranked(*bounded));
      if (limit >= 1334) { EXPECT_FALSE(bounded->docList()->found_gte); }
    }
  }
}

TEST_F(BoundedCountTest, metadataAndDeletedTermFloor) {
  for (int parallel : {1, -1}) {
    for (auto [query, count] : {std::pair{"*:*", 2000}, {"body_w:beta", 667},
                               {"body_w:missing AND body_w:alpha", 0}}) {
      auto result = run(search(query, 100), parallel);
      EXPECT_EQ(count, result->getMatchCount());
      EXPECT_FALSE(result->docList()->found_gte);
    }
  }
  ASSERT_TRUE(CollectionHelper("bounded").deleteByIds({"0", "1", "2"}, UpdateMessage::COMMIT).success);
  for (int parallel : {1, -1}) {
    auto proven = run(search("body_w:alpha", 1997), parallel);
    expectCount(*proven, 1997, 1997);
    auto exact = run(search("body_w:alpha", 1998), parallel);
    EXPECT_EQ(1997, exact->getMatchCount());
    EXPECT_FALSE(exact->docList()->found_gte);
  }
}

TEST_F(BoundedCountTest, constantDrainsAndSmallBound) {
  for (const std::string& filter : {std::string("body_w:beta AND color_s:red"),
                                   std::string(R"(body_w:\"alpha beta\")")}) {
    for (int parallel : {1, -1}) {
      std::string extra = R"(,"filter":[")" + filter + R"("])";
      auto exact = run(search("*:*", -1, extra), parallel);
      for (int64_t limit : {1, 100}) {
        auto bounded = run(search("*:*", limit, extra), parallel);
        EXPECT_EQ(limit, bounded->getMatchCount());
        EXPECT_TRUE(bounded->docList()->found_gte);
        EXPECT_EQ(ranked(*exact), ranked(*bounded));
      }
      // The final flush can open the threshold without discarding any matches.
      auto last = run(search("*:*", exact->getMatchCount(), extra), parallel);
      EXPECT_EQ(exact->getMatchCount(), last->getMatchCount());
      EXPECT_FALSE(last->docList()->found_gte);
    }
  }
}

TEST_F(BoundedCountTest, provenConstantLimitOnly) {
  std::string extra = R"(,"filter":["body_w:beta OR body_w:filler"])";
  for (int parallel : {1, -1}) {
    auto exact = run(search("*:*", -1, extra), parallel);
    SkipStatsScope stats;
    SkipStats::enabled = parallel == 1;
    auto bounded = run(search("*:*", 100, extra), parallel);
    EXPECT_EQ(100, bounded->getMatchCount());
    EXPECT_TRUE(bounded->docList()->found_gte);
    EXPECT_EQ(ranked(*exact), ranked(*bounded));
    if (parallel == 1) { EXPECT_EQ(4, SkipStats::constantLimitOnlyCollections); }
  }
}

TEST_F(BoundedCountTest, proofAloneKeepsExactCount) {
  for (int parallel : {1, -1}) {
    SkipStatsScope stats;
    SkipStats::enabled = parallel == 1;
    auto result = run(search("*:*", 1, R"(,"filter":["body_w:beta OR body_w:gamma"],"limit":-1)"), parallel);
    EXPECT_EQ(1334, result->getMatchCount());
    EXPECT_FALSE(result->docList()->found_gte);
    if (parallel == 1) { EXPECT_GT(SkipStats::constantLimitOnlyCollections, 0); }
  }
}

TEST_F(BoundedCountTest, threeClauseCountInvariants) {
  std::string query = "body_w:beta OR body_w:gamma OR body_w:filler";
  for (int parallel : {1, -1}) {
    auto exact = run(search(query, -1), parallel);
    int64_t count = exact->getMatchCount();
    for (int64_t limit : {(int64_t)1, (int64_t)100, (int64_t)1000, count - 1, count, count + 1}) {
      auto bounded = run(search(query, limit), parallel);
      expectCount(*bounded, count, limit);
    }
  }
}

TEST_F(BoundedCountTest, exactFallbacksAndValidation) {
  std::string query = "body_w:beta OR body_w:gamma";
  for (std::string extra : {R"(,"ops":{"c":{"field_facet":{"field":"color_s"}}})",
                            R"(,"sort":"n_i asc")", R"(,"limit":0)"}) {
    auto result = run(search(query, 10, extra), -1);
    EXPECT_EQ(1334, result->getMatchCount());
    EXPECT_FALSE(result->docList()->found_gte);
  }
  auto bad = localReq(luxirNode->getSearchEngine());
  parseQueryRequest(search(query, -2), bad->rawRequest(), bad->mr);
  bad->collection("bounded");
  ExpectLog quiet("Search request rejected");
  bad->execute(1);
  EXPECT_FALSE(bad->ok());
  EXPECT_NE(std::string::npos, bad->errorMsg().find("count_limit must be >= -1"));
}

TEST_F(BoundedCountTest, explicitDomainCountsExactly) {
  for (int parallel : {1, -1}) {
    auto result = run(R"({"ops":{"q":{"top_docs":{"limit":1,"count_limit":100,
      "filter":["body_w:beta OR body_w:filler"]},"domain":{"filter":["n_i:<10"]}}}})", parallel);
    EXPECT_EQ(9, result->getMatchCount());
    EXPECT_FALSE(result->docList()->found_gte);
  }
}

class BoundedCountCachedTest : public BoundedCountTest {
protected:
  void SetUp() override {
    cacheBytes = 64 << 20;
    BoundedCountTest::SetUp();
  }
};

TEST_F(BoundedCountCachedTest, residentMembershipCountsExactly) {
  std::string query = "body_w:beta OR body_w:gamma";
  for (int i = 0; i < 4; i++) {
    auto bounded = run(search(query, 1000), 1);
    expectCount(*bounded, 1334, 1000);
  }
  EXPECT_EQ(0u, cache->entryCountForTest());
  EXPECT_EQ(0u, cache->counters().admissions);
  EXPECT_EQ(0u, cache->counters().builds);
  auto warm = run(search(query, -1, R"(,"limit":0)"), 1);
  EXPECT_EQ(1334, warm->getMatchCount());
  ASSERT_GT(cache->counters().builds, 0u);
  for (int parallel : {1, -1}) {
    SkipStatsScope stats;
    SkipStats::enabled = parallel == 1;
    auto bounded = run(search(query, 1000), parallel);
    EXPECT_EQ(1334, bounded->getMatchCount());
    EXPECT_FALSE(bounded->docList()->found_gte);
    if (parallel == 1) { EXPECT_EQ(4, SkipStats::wholeTopKCountHits); }
  }
}

TEST_F(BoundedCountTest, jsonAndDocHeaders) {
  std::string extra = R"(,"filter":["body_w:beta AND color_s:red"])";
  auto bounded = run(search("*:*", 100, extra), 1);
  auto body = renderSearchResponseBody(bounded->responses[0]->proto, true);
  EXPECT_NE(std::string::npos, body.find(R"("found":100,"found_gte":true)")) << body;
  auto runs = renderDocRuns(bounded->responses[0]->proto);
  ASSERT_FALSE(runs.empty());
  DocLinesState framing;
  std::string header;
  ASSERT_TRUE(frameDocRun(runs[0], framing, {}, header));
  EXPECT_NE(std::string::npos, header.find(R"("found":100,"found_gte":true)")) << header;
  auto exact = run(search("*:*", 100), 1);
  auto exactBody = renderSearchResponseBody(exact->responses[0]->proto, true);
  EXPECT_EQ(std::string::npos, exactBody.find("found_gte")) << exactBody;
}
