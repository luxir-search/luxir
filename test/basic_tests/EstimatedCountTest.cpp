// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "luxir/api/RequestFlags.h"
#include "luxir/reader/SkipStats.h"
#include "luxir/server/JsonRequest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/LuxirTest.h"
#include "test/QueryBuild.h"
#include "test/TestUtils.h"

using namespace luxir;
using namespace luxir::test;

// Opt-in estimated counts: count_limit -2, or the packed estimate_count flag.
class EstimatedCountTest : public LuxirTest {
protected:
  struct Result {
    std::optional<int64_t> found;
    bool gte = false;
    bool estimated = false;
  };

  static Result resultOf(const LocalReq& req, std::string_view op = "q") {
    EXPECT_TRUE(req.ok()) << req.errorMsg();
    const auto* list = req.docList(op);
    EXPECT_NE(list, nullptr);
    if (list == nullptr) return {};
    // One number with at most one qualifier.
    EXPECT_FALSE(list->found_gte && list->found_estimated);
    return {list->found, list->found_gte, list->found_estimated};
  }

  auto json(std::string_view collection, const std::string& body, int parallel = 0) {
    auto req = localReq(luxirNode->getSearchEngine());
    parseQueryRequest(body, req->rawRequest(), req->mr);
    req->collection(collection);
    req->execute(parallel);
    return req;
  }

  Result match(std::string_view collection, std::string_view text,
               std::optional<int64_t> countLimit, int64_t limit = 10, int parallel = 0) {
    auto req = localReq(luxirNode->getSearchEngine());
    req->collection(collection);
    auto& cur = req->topDocs("q").fields({"id"}).limit(limit);
    if (countLimit) cur.countLimit(*countLimit);
    cur.rawQuery() = qb::match(cur.mr(), "body_w", text);
    req->execute(parallel);
    return resultOf(*req);
  }

  static void expectExact(const Result& result, int64_t expected) {
    EXPECT_EQ(std::optional<int64_t>(expected), result.found);
    EXPECT_FALSE(result.estimated);
    EXPECT_FALSE(result.gte);
  }
};

TEST_F(EstimatedCountTest, sampledUnionLandsNearExactCount) {
  constexpr std::string_view collection = "estimate_union";
  CollectionHelper helper(collection);
  std::vector<Doc> docs;
  for (int32_t doc = 0; doc < 40000; doc++) {
    std::string body = "filler";
    if ((doc % 2) == 0) body += " common";
    if ((doc % 3) == 0) body += " other";
    if (doc < 5) body += " rare";
    docs.push_back(flatdoc("id", "d" + std::to_string(doc), "body_w", body));
  }
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);

  Result exact = match(collection, "common other", -1);
  ASSERT_TRUE(exact.found.has_value());
  EXPECT_FALSE(exact.estimated);
  EXPECT_FALSE(exact.gte);

  Result estimate = match(collection, "common other", -2);
  ASSERT_TRUE(estimate.found.has_value());
  EXPECT_TRUE(estimate.estimated);
  EXPECT_NEAR((double) *exact.found, (double) *estimate.found, 0.1 * (double) *exact.found);

  // Unset and 0 omit the count: estimating is never default work.
  EXPECT_FALSE(match(collection, "common other", std::nullopt).found.has_value());
  EXPECT_FALSE(match(collection, "common other", 0).found.has_value());

  // A term counts from metadata and a sparse query counts everything.
  expectExact(match(collection, "common", -2), 20000);
  expectExact(match(collection, "rare", -2), 5);
}

// Terms cluster at the start of every 4,096 docs, aligned with the sampling
// frame on purpose: the raw sample is badly wrong, and the bounds must hold.
TEST_F(EstimatedCountTest, estimatesRespectKnownBounds) {
  constexpr std::string_view collection = "estimate_bounds";
  CollectionHelper helper(collection);
  std::vector<Doc> docs;
  for (int32_t doc = 0; doc < 65536; doc++) {
    int32_t position = doc % 4096;
    std::string body = "filler";
    if (position < 64) body += " hot hotterm";
    if (position < 1024) body += " warm warmterm";
    if (position >= 64) body += " cold coldterm";
    docs.push_back(flatdoc("id", "d" + std::to_string(doc), "body_w", body));
  }
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);

  // The sample sees only matches and would claim every doc. The df sum caps it.
  Result warm = match(collection, "warm warmterm", -2);
  ASSERT_TRUE(warm.found.has_value());
  EXPECT_TRUE(warm.estimated);
  EXPECT_EQ(32768, *warm.found);

  // The sample undercounts. One clause's df is a floor.
  Result cold = match(collection, "cold coldterm", -2);
  ASSERT_TRUE(cold.found.has_value());
  EXPECT_TRUE(cold.estimated);
  EXPECT_EQ(64512, *cold.found);

  // At most 2,048 matches fit the budget, so they are simply counted.
  expectExact(match(collection, "hot hotterm", -2), 1024);

  // Ranking that visited every match knows the count without any estimator.
  expectExact(match(collection, "warm warmterm", -2, 65536), 16384);
}

TEST_F(EstimatedCountTest, workStaysWithinBudgetOnLowYieldPhrases) {
  constexpr std::string_view collection = "estimate_phrase";
  constexpr int32_t nDocs = 65536;
  CollectionHelper helper(collection);
  std::vector<Doc> docs;
  for (int32_t doc = 0; doc < nDocs; doc++) {
    // Every doc holds both terms; the phrase matches 1 doc in 512.
    docs.push_back(flatdoc("id", "d" + std::to_string(doc), "body_w",
                           (doc % 512) == 0 ? "alpha beta" : "alpha middle beta"));
  }
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);

  auto run = [&](int64_t countLimit, int64_t limit) {
    auto req = localReq(luxirNode->getSearchEngine());
    req->collection(collection);
    auto& cur = req->topDocs("q").fields({"id"}).limit(limit).countLimit(countLimit);
    cur.rawQuery() = qb::phraseText(cur.mr(), "body_w", "alpha beta");
    SkipStatsScope stats;
    req->execute(false);
    return std::pair(resultOf(*req), SkipStats::phraseVerifies);
  };

  // Ranking exhausted the 128 matches, so the estimate adds no verification.
  auto [uncounted, uncountedVerifies] = run(0, 1000);
  auto [exhausted, exhaustedVerifies] = run(-2, 1000);
  EXPECT_FALSE(uncounted.found.has_value());
  expectExact(exhausted, 128);
  EXPECT_EQ(uncountedVerifies, exhaustedVerifies);

  // Pruned ranking leaves the estimator its budget of 1/8 of the segment. The
  // sample is too thin to scale, so the answer is a lower bound of what ranking
  // saw, not a guess. count_limit 1 ranks the same way without an estimator.
  auto [ranked, rankedVerifies] = run(1, 1);
  auto [budgeted, budgetedVerifies] = run(-2, 1);
  EXPECT_TRUE(ranked.gte);
  ASSERT_TRUE(budgeted.found.has_value());
  EXPECT_TRUE(budgeted.gte);
  EXPECT_FALSE(budgeted.estimated);
  EXPECT_GE(*budgeted.found, 1);
  EXPECT_LE(*budgeted.found, 128);
  EXPECT_LE(budgetedVerifies - rankedVerifies, nDocs / 8 + 64);
}

TEST_F(EstimatedCountTest, sumsAcrossSegmentsAndStreams) {
  constexpr std::string_view collection = "estimate_segments";
  CollectionHelper helper(collection);
  for (int32_t segment = 0; segment < 3; segment++) {
    std::vector<Doc> docs;
    for (int32_t doc = 0; doc < 20000; doc++) {
      std::string body = "filler";
      if ((doc % 2) == 0) body += " common";
      if ((doc % 3) == 0) body += " other";
      docs.push_back(flatdoc("id", std::to_string(segment) + "_" + std::to_string(doc),
                             "body_w", body, "cat_s", (doc % 4) == 0 ? "a" : "b"));
    }
    ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
  }
  for (int parallel : {0, -1}) {
    SCOPED_TRACE(parallel);
    Result estimate = match(collection, "common other", -2, 10, parallel);
    ASSERT_TRUE(estimate.found.has_value());
    EXPECT_TRUE(estimate.estimated);
    EXPECT_NEAR(40000.0, (double) *estimate.found, 4000.0);

    // A filter narrows the estimate, and term statistics no longer floor it.
    // Every cat_s:a doc is even, so it matches common.
    auto filtered = json(collection, R"({"ops":{"q":{"top_docs":{"query":"body_w:common body_w:other",
        "filter":["cat_s:a"],"count_limit":-2}}}})", parallel);
    Result narrowed = resultOf(*filtered);
    ASSERT_TRUE(narrowed.found.has_value());
    EXPECT_FALSE(narrowed.gte);
    EXPECT_NEAR(15000.0, (double) *narrowed.found, 1500.0);
  }
}

// A broad prefix ranks through a lazy union; the sample takes the same plan
// rather than materializing every expanded term's postings.
TEST_F(EstimatedCountTest, multiTermSamplesLazily) {
  constexpr std::string_view collection = "estimate_prefix";
  CollectionHelper helper(collection);
  std::vector<Doc> docs;
  for (int32_t doc = 0; doc < 65536; doc++) {
    docs.push_back(flatdoc("id", "d" + std::to_string(doc),
                           "tag_w", "t" + std::to_string(doc % 4)));
  }
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
  auto run = [&](int64_t countLimit) {
    auto req = localReq(luxirNode->getSearchEngine());
    req->collection(collection);
    req->topDocs("q").prefixQuery("tag_w", "t").fields({"id"}).limit(10).countLimit(countLimit);
    SkipStatsScope stats;
    req->execute(false);
    return std::pair(resultOf(*req), SkipStats::docBlocksDecoded);
  };
  auto [bounded, rankedBlocks] = run(1);
  auto [estimate, estimateBlocks] = run(-2);
  auto [exact, exactBlocks] = run(-1);
  EXPECT_TRUE(bounded.gte);
  ASSERT_TRUE(estimate.found.has_value());
  EXPECT_TRUE(estimate.estimated);
  EXPECT_NEAR(65536.0, (double) *estimate.found, 6554.0);
  expectExact(exact, 65536);
  // Each term's postings span many blocks per slice, so a 1/64 sample decodes
  // a fraction of what an exact count, or a materialized union, does.
  EXPECT_LT(estimateBlocks - rankedBlocks, (exactBlocks - rankedBlocks) / 2)
      << rankedBlocks << " " << estimateBlocks << " " << exactBlocks;
}

// With one term per document the accepted terms' docFreqs sum to the exact
// count, so there is nothing to sample.
TEST_F(EstimatedCountTest, multiTermOnOneTermPerDocumentFieldIsExact) {
  constexpr std::string_view collection = "estimate_prefix_exact";
  CollectionHelper helper(collection);
  std::vector<Doc> docs;
  for (int32_t doc = 0; doc < 65536; doc++) {
    docs.push_back(flatdoc("id", "d" + std::to_string(doc),
                           "tag_s", "t" + std::to_string(doc % 16)));
  }
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);
  auto req = localReq(luxirNode->getSearchEngine());
  req->collection(collection);
  req->topDocs("q").prefixQuery("tag_s", "t1").fields({"id"}).limit(10).countLimit(-2);
  req->execute(false);
  // t1 and t10..t15.
  expectExact(resultOf(*req), 7 * 4096);
}

TEST_F(EstimatedCountTest, positiveLimitWithFlagKeepsAtLeastN) {
  constexpr std::string_view collection = "estimate_bounded";
  CollectionHelper helper(collection);
  std::vector<Doc> docs;
  for (int32_t doc = 0; doc < 40000; doc++) {
    std::string body = "filler";
    if ((doc % 2) == 0) body += " common";
    if ((doc % 3) == 0) body += " other";
    docs.push_back(flatdoc("id", "d" + std::to_string(doc), "body_w", body));
  }
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);

  auto run = [&](int64_t countLimit) {
    return resultOf(*json(collection, R"({"flags":["estimate_count"],"ops":{"q":{"top_docs":{
        "query":"body_w:common body_w:other","count_limit":)" + std::to_string(countLimit) + "}}}}"));
  };
  Result estimate = run(1000);
  ASSERT_TRUE(estimate.found.has_value());
  EXPECT_TRUE(estimate.estimated);
  EXPECT_NEAR(26667.0, (double) *estimate.found, 2667.0);
  // N above the match count: the hold never opens, so the count is exact.
  expectExact(run(30000), 26667);

  // When index statistics pin the count, no sample is needed.
  auto pinned = json(collection, R"({"ops":{"q":{"top_docs":{
      "query":"body_w:filler body_w:common","count_limit":-2}}}})");
  expectExact(resultOf(*pinned), 40000);

  // Without the flag, N keeps its plain meaning.
  auto plain = json(collection, R"({"ops":{"q":{"top_docs":{
      "query":"body_w:common body_w:other","count_limit":1000}}}})");
  Result bounded = resultOf(*plain);
  EXPECT_EQ(std::optional<int64_t>(1000), bounded.found);
  EXPECT_TRUE(bounded.gte);
  EXPECT_FALSE(bounded.estimated);
}

TEST_F(EstimatedCountTest, flagPrecedenceAndInheritance) {
  constexpr std::string_view collection = "estimate_flags";
  CollectionHelper helper(collection);
  std::vector<Doc> docs;
  for (int32_t doc = 0; doc < 2000; doc++) {
    docs.push_back(flatdoc("id", "d" + std::to_string(doc), "body_w",
                           (doc % 2) == 0 ? "common other" : "other"));
  }
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);

  // A request-level flag reaches a list that left count_limit unset.
  expectExact(resultOf(*json(collection, R"({"flags":["estimate_count"],"ops":{"q":{"top_docs":{
      "query":"body_w:common body_w:other"}}}})")), 2000);

  // An op-level flag passes down to subops. A nested list cannot estimate, so
  // it counts exactly.
  auto opFlag = json(collection, R"({"ops":{"q":{"flags":["estimate_count"],"top_docs":{
      "query":"body_w:common","limit":1,"ops":{"sub":{"top_docs":{"query":"body_w:other"}}}}}}})");
  EXPECT_TRUE(resultOf(*opFlag).found.has_value());
  ASSERT_NE(opFlag->docList("q")->ops.at("sub")->docList(), nullptr);
  EXPECT_EQ(std::optional<int64_t>(1000), opFlag->docList("q")->ops.at("sub")->docList()->found);

  // count_limit -2 applies to that op alone: its nested list stays uncounted.
  auto shorthand = json(collection, R"({"ops":{"q":{"top_docs":{"query":"body_w:common","limit":1,
      "count_limit":-2,"ops":{"sub":{"top_docs":{"query":"body_w:other"}}}}}}})");
  EXPECT_TRUE(resultOf(*shorthand).found.has_value());
  ASSERT_NE(shorthand->docList("q")->ops.at("sub")->docList(), nullptr);
  EXPECT_FALSE(shorthand->docList("q")->ops.at("sub")->docList()->found.has_value());

  // An explicit 0 or -1 beats an inherited flag.
  EXPECT_FALSE(resultOf(*json(collection, R"({"flags":["estimate_count"],"ops":{"q":{"top_docs":{
      "query":"body_w:common body_w:other","count_limit":0}}}})")).found.has_value());
  expectExact(resultOf(*json(collection, R"({"flags":["estimate_count"],"ops":{"q":{"top_docs":{
      "query":"body_w:common body_w:other","count_limit":-1}}}})")), 2000);

  // Clearing the bit on the op with -2 only stops subops inheriting it.
  auto cleared = localReq(luxirNode->getSearchEngine());
  cleared->collection(collection);
  auto& cur = cleared->topDocs("q").fields({"id"}).countLimit(-2);
  cur.rawQuery() = qb::match(cur.mr(), "body_w", "common");
  cleared->rawRequest().flags = RequestFlags::ESTIMATE_COUNT;
  cleared->rawRequest().flags_set = RequestFlags::ESTIMATE_COUNT;
  cur.rawOp().flags = 0;
  cur.rawOp().flags_set = RequestFlags::ESTIMATE_COUNT;
  cleared->execute(false);
  expectExact(resultOf(*cleared), 1000);
}

// Lists that cannot estimate still honor the request: an exact count is always
// a valid answer, and the same one -1 gives.
TEST_F(EstimatedCountTest, ineligibleListsCountExactly) {
  constexpr std::string_view collection = "estimate_ineligible";
  CollectionHelper helper(collection);
  std::vector<Doc> docs;
  for (int32_t doc = 0; doc < 2000; doc++) {
    docs.push_back(flatdoc("id", "d" + std::to_string(doc), "body_w", "common other",
                           "cat_s", (doc % 4) == 0 ? "a" : "b", "num_i", (int64_t) doc));
  }
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);

  for (std::string_view extra : {
           R"(,"limit":0)",
           R"(,"sort":"num_i asc")",
           R"(,"ops":{"f":{"field_facet":{"field":"cat_s"}}})",
           R"(,"diversify":{"field":"cat_s","max_per_key":1})",
           R"(,"collapse":{"field":"cat_s"})"}) {
    SCOPED_TRACE(extra);
    auto body = [&](std::string_view countLimit) {
      return std::string(R"({"ops":{"q":{"top_docs":{"query":"body_w:common body_w:other","count_limit":)")
          + std::string(countLimit) + std::string(extra) + "}}}}";
    };
    Result exact = resultOf(*json(collection, body("-1")));
    ASSERT_TRUE(exact.found.has_value());
    expectExact(resultOf(*json(collection, body("-2"))), *exact.found);
  }

  // Fusion counts exactly for -2 and for an inherited flag: the union of its
  // sources' lists.
  std::string sources = R"(,"fields":["id"],"rrf":{},"sources":{
      "a":{"query":"body_w:common"},"b":{"query":"body_w:other"}})";
  auto fusion = [&](std::string_view flags, std::string_view countLimit) {
    return resultOf(*json(collection, "{" + std::string(flags) + R"("ops":{"q":{"fusion":{"limit":5)"
        + std::string(countLimit) + sources + "}}}}"));
  };
  Result fusionExact = fusion("", R"(,"count_limit":-1)");
  ASSERT_TRUE(fusionExact.found.has_value());
  expectExact(fusion("", R"(,"count_limit":-2)"), *fusionExact.found);
  expectExact(fusion(R"("flags":["estimate_count"],)", ""), *fusionExact.found);
  EXPECT_FALSE(fusion(R"("flags":["estimate_count"],)", R"(,"count_limit":0)").found.has_value());
  EXPECT_FALSE(fusion("", "").found.has_value());
}

TEST_F(EstimatedCountTest, emptyIndexReportsZero) {
  constexpr std::string_view collection = "estimate_empty";
  CollectionHelper helper(collection);
  expectExact(match(collection, "anything", -2), 0);
}

TEST_F(EstimatedCountTest, rejectsCountLimitBelowMinusTwo) {
  constexpr std::string_view collection = "estimate_validation";
  CollectionHelper helper(collection);
  ASSERT_TRUE(helper.indexAll({flatdoc("id", "1", "body_w", "common")},
                              UpdateMessage::COMMIT).success);
  auto rejected = json(collection, R"({"ops":{"q":{"top_docs":{"query":"body_w:common","count_limit":-3}}}})");
  EXPECT_FALSE(rejected->ok());
  EXPECT_NE(std::string::npos, rejected->errorMsg().find("count_limit must be >= -2"))
      << rejected->errorMsg();
}

TEST_F(EstimatedCountTest, flagNameRoundTrips) {
  std::array<std::string_view, 1> names{"estimate_count"};
  auto parsed = RequestFlags::fromNames(names, RequestFlags::Level::OP);
  EXPECT_EQ(RequestFlags::ESTIMATE_COUNT, parsed.flags);
  EXPECT_EQ(RequestFlags::ESTIMATE_COUNT, parsed.flagsSet);
  auto rendered = RequestFlags::names(parsed.flags, parsed.flagsSet);
  ASSERT_EQ(1u, rendered.size());
  EXPECT_EQ("estimate_count", rendered[0]);
  std::array<std::string_view, 2> twice{"estimate_count", "estimate_count"};
  EXPECT_THROW(RequestFlags::fromNames(twice), RequestError);
}
