// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <future>
#include <latch>
#include <memory_resource>
#include <numeric>
#include <string>
#include <thread>
#include <vector>
#include <oneapi/tbb/global_control.h>

#include "luxir/api/padded_input.h"
#include "luxir/query/BooleanWeight.h"
#include "luxir/reader/SkipStats.h"
#include "luxir/search/SearchOverrides.h"
#include "luxir/search/ops/SearchOp.h"
#include "luxir/server/JsonRequest.h"
#include "luxir/server/JsonResponse.h"
#include "luxir/util/Signal.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/QueryBuild.h"
#include "test/LuxirTest.h"
#include "test/TaskSpawnCounter.h"
#include "test/TestUtils.h"

namespace luxir::test {

class ExecutionProfileTest : public LuxirTest {};

namespace {

std::string_view expectedStrategy(int64_t domainSize, int64_t cardinality) {
  if ((domainSize >> 4) >= cardinality) return "vector";
  if ((cardinality >> 5) >= domainSize) return "hash";
  return "skinny";
}

const api::ExecutionProfileOp& profileOp(const LocalReq& req) {
  const auto& response = req.responses.back()->proto;
  EXPECT_TRUE(response.profile.has_value());
  EXPECT_EQ(1u, response.profile->ops.size());
  return response.profile->ops[0];
}

const api::ExecutionProfileOp& profileOp(
    const LocalReq& req, std::string_view name) {
  const auto& response = req.responses.back()->proto;
  EXPECT_TRUE(response.profile.has_value());
  if (response.profile) {
    for (const auto& op : response.profile->ops) {
      if (op.name == name) return op;
    }
  }
  ADD_FAILURE() << "profile op not found: " << name;
  return response.profile->ops.front();
}

// details is free-form prose for humans and NOT an API, but tests ship with the
// code, so matching a marker in it is a fine way to confirm the intended path
// fired.  Match a distinguishing fragment, never a whole rendered line - the
// wording and the ordering are meant to stay free to change.
::testing::AssertionResult detailsMention(
    const api::ExecutionProfilePiece& piece, std::string_view marker) {
  for (const auto& detail : piece.details) {
    if (std::string_view(detail).find(marker) != std::string_view::npos) {
      return ::testing::AssertionSuccess();
    }
  }
  return ::testing::AssertionFailure()
      << "no detail mentions '" << marker << "'; details are ["
      << [&] {
           std::string joined;
           for (const auto& detail : piece.details) {
             if (!joined.empty()) joined += " | ";
             joined += std::string(detail);
           }
           return joined;
         }() << "]";
}

} // namespace

TEST_F(ExecutionProfileTest, absentUnlessRequested) {
  CollectionHelper helper;
  helper.index(flatdoc("id", "1", "cat_s", "a"), UpdateMessage::COMMIT);

  auto req = localReq(helper.getSearchEngine());
  req->collection("main").facet("cats", "cat_s").limit(-1);
  req->execute(false);
  ASSERT_OK(req);
  ASSERT_FALSE(req->responses.back()->proto.profile.has_value());
  EXPECT_EQ(std::string::npos,
            renderSearchResponseBody(req->responses.back()->proto).find("\"profile\""));

  auto offReq = localReq(helper.getSearchEngine());
  offReq->collection("main").profile(false).facet("cats", "cat_s").limit(-1);
  offReq->execute(false);
  ASSERT_OK(offReq);
  EXPECT_FALSE(offReq->responses.back()->proto.profile.has_value());
}

TEST_F(ExecutionProfileTest, reportsMultiSegmentStrategyInputsAndUpgrade) {
  CollectionHelper helper;
  std::vector<std::string> values;
  for (int i = 0; i < 64; i++) values.push_back("v" + std::to_string(i));
  helper.index(flatdoc("id", "1", "tags_ss", values), UpdateMessage::COMMIT);
  // Three docs: enough that this segment's domain clears the hash threshold
  // (cardinality >> 5 = 2 < 3) and wants skinny, driving the upgrade below.
  helper.indexAll(std::array{
      flatdoc("id", "2", "tags_ss", vecs("v0")),
      flatdoc("id", "3", "tags_ss", vecs("v1")),
      flatdoc("id", "4", "tags_ss", vecs("v0")),
  }, UpdateMessage::COMMIT);
  helper.index(flatdoc("id", "5", "tags_ss", vecs("v2")), UpdateMessage::COMMIT);
  ASSERT_EQ(3u, helper.durableSegmentCount());

  auto req = localReq(helper.getSearchEngine());
  req->collection("main").profile().facet("tags", "tags_ss").limit(-1);
  req->execute(false);
  ASSERT_OK(req);

  const auto& op = profileOp(*req);
  EXPECT_EQ("tags", op.name);
  ASSERT_EQ(3u, op.pieces.size());
  constexpr std::array<int64_t, 3> localMaxOrds = {64, 2, 1};
  constexpr std::array<std::string_view, 3> ordMappings = {
      "identity", "identity", "remapped"};
  for (size_t i = 0; i < op.pieces.size(); i++) {
    const auto& piece = op.pieces[i];
    EXPECT_EQ("segment", piece.kind);
    EXPECT_EQ((int32_t)i, piece.segment);
    ASSERT_TRUE(piece.cardinality.has_value());
    ASSERT_TRUE(piece.domain_size.has_value());
    EXPECT_EQ(64, *piece.cardinality);
    EXPECT_EQ(piece.max_doc, *piece.domain_size);
    EXPECT_EQ(expectedStrategy(*piece.domain_size, *piece.cardinality), piece.strategy);
    // An unfiltered facet has an empty domain complement, so every term's count
    // IS its docFreq: the ord column is never read.
    EXPECT_TRUE(detailsMention(piece, "maxOrd=" + std::to_string(localMaxOrds[i])
                                          + " ords=" + std::string(ordMappings[i])));
    EXPECT_TRUE(detailsMention(piece, "docFreq-only"));
    EXPECT_GT(piece.thread_id, 0);
    EXPECT_LT(piece.elapsed_us, 60'000'000u);
  }
  // Sequential contribution order: piece 0 creates the hash accumulator,
  // piece 1 wants skinny and pays the upgrade, piece 2 wants hash but rides
  // the already-upgraded skinny (no-downgrade rule) - all spelled out in the
  // human-readable details rather than typed fields.
  EXPECT_EQ("hash", op.pieces[0].strategy);
  EXPECT_EQ("skinny", op.pieces[1].strategy);
  EXPECT_TRUE(detailsMention(op.pieces[1], "upgraded shared counters to skinny"));
  EXPECT_EQ("hash", op.pieces[2].strategy);
  EXPECT_TRUE(detailsMention(op.pieces[2], "want=hash, found=skinny"));

  std::string json = renderSearchResponseBody(req->responses.back()->proto);
  EXPECT_NE(std::string::npos, json.find(R"("profile":{"ops":[{"name":"tags")"));
  EXPECT_NE(std::string::npos, json.find(R"("upgraded shared counters to skinny")"));

  std::vector<std::byte> wire;
  ASSERT_TRUE(api::encode(req->responses.back()->proto, wire));
  std::pmr::monotonic_buffer_resource arena;
  api::SearchResponse decoded;
  auto padded = api::copyToPaddedInput(std::span<const std::byte>(wire), arena);
  ASSERT_TRUE(api::decode(decoded, padded, arena));
  ASSERT_TRUE(decoded.profile.has_value());
  ASSERT_EQ(3u, decoded.profile->ops[0].pieces.size());
  EXPECT_EQ("skinny", decoded.profile->ops[0].pieces[1].strategy);

  auto parallelReq = localReq(helper.getSearchEngine());
  parallelReq->collection("main").profile().facet("tags", "tags_ss").limit(-1);
  parallelReq->execute(true);
  ASSERT_OK(parallelReq);
  const auto& parallelPieces = profileOp(*parallelReq).pieces;
  ASSERT_EQ(3u, parallelPieces.size());
  for (size_t i = 0; i < parallelPieces.size(); i++) {
    EXPECT_EQ((int32_t)i, parallelPieces[i].segment);
    EXPECT_EQ(expectedStrategy(*parallelPieces[i].domain_size,
                               *parallelPieces[i].cardinality),
              parallelPieces[i].strategy);
  }
}

TEST_F(ExecutionProfileTest, reportsGlobalTopTermsPath) {
  CollectionHelper helper("profile_top_terms");
  helper.indexAll(std::array{
      flatdoc("id", "1", "cat_s", "a", "metric_i", 10),
      flatdoc("id", "2", "cat_s", "b", "metric_i", 20),
  }, UpdateMessage::COMMIT);
  helper.indexAll(std::array{
      flatdoc("id", "3", "cat_s", "a", "metric_i", 30),
      flatdoc("id", "4", "cat_s", "c", "metric_i", 40),
  }, UpdateMessage::COMMIT);

  auto req = localReq(helper.getSearchEngine());
  auto& facet = req->collection("profile_top_terms").profile()
      .facet("cats", "cat_s").limit(2);
  facet.avg("avg", "metric_i");
  req->execute(false);
  ASSERT_OK(req);

  const auto& pieces = profileOp(*req).pieces;
  ASSERT_EQ(2u, pieces.size());
  for (const auto& piece : pieces) {
    EXPECT_EQ("toplist", piece.strategy);
    EXPECT_EQ(3, *piece.cardinality);
    EXPECT_TRUE(detailsMention(piece, "parent-count=top-terms"));
    EXPECT_TRUE(detailsMention(piece, "result-feed=bucket-domains"));
    EXPECT_TRUE(detailsMention(piece, "global docFreq top terms"));
    EXPECT_TRUE(detailsMention(piece, "3 listed"));
  }
}

TEST_F(ExecutionProfileTest, selectedTailKeepsGlobalTopTermsPath) {
  CollectionHelper helper("profile_selected_top_terms");
  std::vector<Doc> docs;
  docs.reserve(128);
  for (int i = 0; i < 128; i++) {
    std::string value = i < 64 ? "head"
        : i == 64 ? "tail" : "other-" + std::to_string(i);
    docs.push_back(flatdoc("id", std::to_string(i), "cat_s", value));
  }
  helper.indexAll(docs, UpdateMessage::COMMIT);

  auto req = localReq(helper.getSearchEngine());
  parseQueryRequest(R"({"ops":{"q":{"top_docs":{"limit":0,
    "ops":{"cats":{"field_facet":{"field":"cat_s","limit":1,
      "selected":["head","tail"]}}}}}}})",
      req->rawRequest(), req->mr);
  req->collection("profile_selected_top_terms").profile();
  req->execute(false);
  ASSERT_OK(req);

  const auto* result = req->docList("q")->ops.at("cats")->facetResult();
  ASSERT_NE(nullptr, result);
  ASSERT_EQ(2u, result->counts.size());
  EXPECT_EQ(64, result->counts[0]);
  EXPECT_EQ(1, result->counts[1]);
  const auto& pieces = profileOp(*req, "cats").pieces;
  ASSERT_EQ(1u, pieces.size());
  EXPECT_EQ("toplist", pieces[0].strategy);
  EXPECT_TRUE(detailsMention(pieces[0], "parent-count=top-terms"));
}

TEST_F(ExecutionProfileTest, nestedAutoSelectsReplayAndFallsBack) {
  CollectionHelper helper("profile_nested_auto");
  std::vector<Doc> docs;
  for (int i = 0; i < 200; i++) {
    docs.push_back(flatdoc(
        "id", std::to_string(i),
        "parent_s", "parent-" + std::to_string(i % 4),
        "child_s", "child-" + std::to_string(i % 7),
        "sel_s", i == 0 ? "yes" : "no"));
  }
  ASSERT_TRUE(helper.indexAll(docs, UpdateMessage::COMMIT).success);

  SearchOverridesGuard guard(forcedFacetFeedStrategy);
  auto run = [&](FacetFeedStrategy feed, bool filtered) {
    forcedFacetFeedStrategy = feed;
    auto req = localReq(helper.getSearchEngine());
    req->collection("profile_nested_auto").profile();
    auto addFacet = [](auto& cursor) {
      cursor.facet("parent", "parent_s").limit(10)
          .facet("child", "child_s").limit(10);
    };
    if (filtered) {
      auto& top = req->topDocs("q");
      top.matchQuery("sel_s", "yes");
      addFacet(top);
    } else {
      addFacet(*req);
    }
    req->execute(false);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    return req;
  };
  auto encodeParent = [](const LocalReq& req, bool filtered) {
    const api::FacetResult* result = filtered
        ? req.docList("q")->ops.at("parent")->facetResult()
        : req.responses[0]->proto.ops.at("parent")->facetResult();
    std::vector<std::byte> encoded;
    EXPECT_NE(nullptr, result);
    if (result != nullptr) {
      EXPECT_TRUE(api::encode(*result, encoded));
    }
    return encoded;
  };

  auto baseline = run(FacetFeedStrategy::BUCKET_DOMAINS, true);
  auto automatic = run(FacetFeedStrategy::AUTO, true);
  EXPECT_EQ(encodeParent(*baseline, true), encodeParent(*automatic, true));
  for (const auto& piece : profileOp(*automatic, "parent").pieces) {
    EXPECT_TRUE(detailsMention(piece, "result-feed=string-column-replay"));
  }

  auto fallback = run(FacetFeedStrategy::AUTO, false);
  for (const auto& piece : profileOp(*fallback, "parent").pieces) {
    EXPECT_TRUE(detailsMention(piece, "result-feed=bucket-domains"));
  }
}

TEST_F(ExecutionProfileTest, reportsVectorAtStrategyBoundary) {
  CollectionHelper helper("profile_vector");
  std::vector<Doc> docs;
  for (int i = 0; i < 256; i++) {
    docs.push_back(flatdoc("id", std::to_string(i), "cat_s", "only",
                           "sel_s", i < 16 ? "yes" : "no"));
  }
  helper.indexAll(docs, UpdateMessage::COMMIT);

  // Unfiltered: the postings-side docFreq path emits one pre-aggregated
  // count per ord, so vector is wanted from average magnitude 256 - the
  // count that would spill skinny's u8 on every add. 256 docs of one term
  // sits exactly on that boundary.
  auto req = localReq(helper.getSearchEngine());
  req->collection("profile_vector").profile().facet("cats", "cat_s").limit(-1);
  req->execute(false);
  ASSERT_OK(req);
  {
    const auto& pieces = profileOp(*req).pieces;
    ASSERT_EQ(1u, pieces.size());
    EXPECT_EQ(256, *pieces[0].domain_size);
    EXPECT_EQ(1, *pieces[0].cardinality);
    EXPECT_EQ("vector", pieces[0].strategy);
    EXPECT_TRUE(detailsMention(pieces[0], "maxOrd=1 ords=identity"));
    EXPECT_TRUE(detailsMention(pieces[0], "docFreq-only"));
    // no upgrade and no divergence note when this piece is the only contributor
    EXPECT_FALSE(detailsMention(pieces[0], "upgraded"));
    EXPECT_FALSE(detailsMention(pieces[0], "want="));
  }

  // Filtered to 16 of 256: the column walk adds once per in-domain doc, so
  // R = 16 sits exactly at the measured vector boundary.
  auto filtered = localReq(helper.getSearchEngine());
  filtered->collection("profile_vector").profile();
  auto& top = filtered->topDocs("q");
  top.matchQuery("sel_s", "yes");
  top.facet("cats", "cat_s").limit(-1);
  filtered->execute(false);
  ASSERT_OK(filtered);
  {
    const auto& pieces = profileOp(*filtered).pieces;
    ASSERT_EQ(1u, pieces.size());
    EXPECT_EQ(16, *pieces[0].domain_size);
    EXPECT_EQ("vector", pieces[0].strategy);
  }
}

TEST_F(ExecutionProfileTest, reportsPointOrdLoadsForArrayDomains) {
  CollectionHelper helper("profile_sparse");
  std::vector<Doc> docs;
  for (int i = 0; i < 256; i++) {
    docs.push_back(flatdoc("id", std::to_string(i), "cat_s", "only",
                           "sel_s", i < 3 ? "t" : "f"));
  }
  helper.indexAll(docs, UpdateMessage::COMMIT);

  auto req = localReq(helper.getSearchEngine());
  req->collection("profile_sparse").profile();
  auto& q = req->topDocs("q").allQuery().limit(0).matchFilter("sel_s", "t");
  q.facet("cats", "cat_s").limit(-1);
  req->execute(false);
  ASSERT_OK(req);

  // A 3-doc filtered domain materializes as an ArrDocSet: the ord column walk
  // point-selects each domain doc, and the profile must say so.
  const auto& pieces = profileOp(*req).pieces;
  ASSERT_EQ(1u, pieces.size());
  EXPECT_EQ(3, *pieces[0].domain_size);
  EXPECT_TRUE(detailsMention(pieces[0], "maxOrd=1 ords=identity"));
  EXPECT_TRUE(detailsMention(pieces[0], "point ord loads"));
}

TEST_F(ExecutionProfileTest, termLeadFirstFillLeapfrogMatchesKillSwitch) {
  constexpr int32_t docCount = 2 * DocsEnumMeta::L1_DOCS;
  CollectionHelper helper("profile_term_lead_leapfrog");
  std::vector<Doc> docs;
  docs.reserve((size_t) docCount);
  for (int32_t doc = 0; doc < docCount; doc++) {
    std::string body = "tail";
    if ((doc % 256) == 0) body += " lead";
    docs.push_back(flatdoc("id", std::to_string(doc), "body_w", body));
  }
  helper.indexAll(docs, UpdateMessage::COMMIT);

  auto count = [&] {
    auto req = localReq(helper.getSearchEngine());
    req->collection("profile_term_lead_leapfrog");
    auto& top = req->topDocs("q").getNumber().limit(0);
    top.rawQuery() = qb::boolean(
        top.mr(), {qb::match(top.mr(), "body_w", "lead"),
                   qb::match(top.mr(), "body_w", "tail")});
    req->execute(false);
    EXPECT_TRUE(req->ok()) << req->errorMsg();
    return req->getMatchCount("q");
  };

  bool savedStats = SkipStats::enabled;
  int32_t savedThreshold =
      BooleanQuery::ConjunctionBulkScorer::
          termLeadLeapfrogThresholdForTests;
  SkipStats::enabled = true;
  SkipStats::reset();
  BooleanQuery::ConjunctionBulkScorer::
      termLeadLeapfrogThresholdForTests = 32;
  int64_t leapfrogCount = count();
  EXPECT_GT(SkipStats::conjTermLeadFirstFillLeapfrogs, 0);

  BooleanQuery::ConjunctionBulkScorer::
      termLeadLeapfrogThresholdForTests = 0;
  int64_t fillCount = count();

  BooleanQuery::ConjunctionBulkScorer::
      termLeadLeapfrogThresholdForTests = savedThreshold;
  SkipStats::enabled = savedStats;
  EXPECT_EQ(leapfrogCount, fillCount);
}

TEST_F(ExecutionProfileTest, maxParallelOneRunsSingleThreaded) {
  CollectionHelper helper("profile_serial");
  std::vector<Doc> docs;
  for (int i = 0; i < 64; i++) {
    docs.push_back(flatdoc("id", std::to_string(i), "cat_s", "v" + std::to_string(i % 8)));
    if (i % 16 == 15) helper.indexAll(docs, UpdateMessage::COMMIT), docs.clear();
  }

  auto req = localReq(helper.getSearchEngine());
  req->collection("profile_serial").profile().facet("cats", "cat_s").limit(-1);
  req->execute((int32_t)1);
  ASSERT_OK(req);

  const auto& pieces = profileOp(*req).pieces;
  ASSERT_GT(pieces.size(), 1u);
  for (const auto& piece : pieces) {
    EXPECT_EQ(pieces[0].thread_id, piece.thread_id);
  }
}

TEST_F(ExecutionProfileTest, maxParallelOutOfRangeIsRejected) {
  CollectionHelper helper("profile_reject");
  helper.indexAll(std::array{flatdoc("id", "1", "cat_s", "a")}, UpdateMessage::COMMIT);

  for (int32_t maxParallel : {2, -2}) {
    auto req = localReq(helper.getSearchEngine());
    req->collection("profile_reject").facet("cats", "cat_s").limit(-1);
    req->execute(maxParallel);
    ASSERT_FALSE(req->responses.empty());
    EXPECT_NE(std::string::npos,
              errorMessage(req->responses.back()->proto).find("max_parallel")) << maxParallel;
  }
}

TEST_F(ExecutionProfileTest, maxParallelZeroRunsInlineOnCallingThread) {
  CollectionHelper helper("profile_inline");
  std::vector<Doc> docs;
  for (int i = 0; i < 32; i++) {
    docs.push_back(flatdoc("id", std::to_string(i), "cat_s", "v" + std::to_string(i % 4)));
    if (i % 16 == 15) helper.indexAll(docs, UpdateMessage::COMMIT), docs.clear();
  }

  auto req = localReq(helper.getSearchEngine());
  req->collection("profile_inline").profile().facet("cats", "cat_s").limit(-1);
  helper.getSearchEngine().dispatch(*req.get(), 0);
  // 0 (the default) = the calling thread: the whole request completed inside
  // dispatch(), so the response is readable with no wait/synchronization.
  ASSERT_OK(req);
  const auto& pieces = profileOp(*req).pieces;
  ASSERT_GT(pieces.size(), 1u);
  for (const auto& piece : pieces) {
    EXPECT_EQ(pieces[0].thread_id, piece.thread_id);
  }
}

namespace {

// dispatch() is asynchronous for max_parallel=1; record the replying thread
// and its arena width, and use the promise as a completion latch (reply() runs
// before set_value, so the future's readiness orders `responses` for the test
// thread).
class DispatchReq : public LocalReq {
public:
  std::promise<std::thread::id> replied;
  int replyArenaConcurrency = 0;
  using LocalReq::LocalReq;
  ReplyStatus reply(SearchResponse& response) override {
    ReplyStatus status = LocalReq::reply(response);
    replyArenaConcurrency = oneapi::tbb::this_task_arena::max_concurrency();
    replied.set_value(std::this_thread::get_id());
    return status;
  }
};

} // namespace

TEST_F(ExecutionProfileTest, maxParallelOneDispatchesOffCallingThread) {
  CollectionHelper helper("profile_pool");
  helper.indexAll(std::array{flatdoc("id", "1", "cat_s", "a")}, UpdateMessage::COMMIT);

  auto* arena = createArena();
  auto req = LocalReqHandle(
      luxir::arenaCreate<DispatchReq>(*arena, helper.getSearchEngine(), *arena));
  auto repliedOn = static_cast<DispatchReq*>(req.get())->replied.get_future();
  req->collection("profile_pool").facet("cats", "cat_s").limit(-1);
  helper.getSearchEngine().dispatch(*req.get(), 1);
  ASSERT_EQ(std::future_status::ready, repliedOn.wait_for(std::chrono::seconds(60)));
  EXPECT_NE(std::this_thread::get_id(), repliedOn.get());
  ASSERT_OK(req);
}

TEST_F(ExecutionProfileTest, maxParallelMinusOneRunsOnCallingThreadInNodeArena) {
  CollectionHelper helper("profile_parallel");
  std::vector<Doc> docs;
  for (int i = 0; i < 32; i++) {
    docs.push_back(flatdoc("id", std::to_string(i), "cat_s", "v" + std::to_string(i % 4)));
    if (i % 16 == 15) helper.indexAll(docs, UpdateMessage::COMMIT), docs.clear();
  }

  auto* arena = createArena();
  auto req = LocalReqHandle(
      luxir::arenaCreate<DispatchReq>(*arena, helper.getSearchEngine(), *arena));
  auto repliedOn = static_cast<DispatchReq*>(req.get())->replied.get_future();
  req->collection("profile_parallel").facet("cats", "cat_s").limit(-1);
  helper.getSearchEngine().dispatch(*req.get(), -1);
  // Completed inside dispatch(), on this thread, inside the (wider than
  // default) node arena rather than this thread's implicit arena.
  ASSERT_EQ(std::future_status::ready, repliedOn.wait_for(std::chrono::seconds(0)));
  EXPECT_EQ(std::this_thread::get_id(), repliedOn.get());
  EXPECT_EQ(helper.getNode().getTaskArena().max_concurrency(),
            static_cast<DispatchReq*>(req.get())->replyArenaConcurrency);
  ASSERT_OK(req);
}

TEST_F(ExecutionProfileTest, facetFanoutSpawnCounts) {
  for (int segments : {1, 4}) {
    CollectionHelper helper("fanout" + std::to_string(segments));
    for (int seg = 0; seg < segments; seg++) {
      std::vector<Doc> docs;
      for (int i = 0; i < 64; i++) {
        docs.push_back(flatdoc("id", std::to_string(seg * 64 + i),
                              "sel01_s", i % 2 ? "t" : "f",
                              "cat_s", "c" + std::to_string(i % 8),
                              "unique_s", std::to_string(seg * 64 + i)));
      }
      helper.indexAll(docs, UpdateMessage::COMMIT);
    }
    ASSERT_EQ((size_t)segments,
              helper.getIndexWriter()->snapshots.readers.getReader()->segments().size());
    for (const char* field : {"cat_s", "unique_s"}) {
      for (bool filtered : {false, true}) {
        for (int repeat = 0; repeat < 2; repeat++) {
          SCOPED_TRACE(::testing::Message() << segments << " segments, " << field
                       << ", filtered=" << filtered << ", repeat=" << repeat);
          auto req = localReq(helper.getSearchEngine());
          req->collection("fanout" + std::to_string(segments));
          auto& top = req->topDocs("q").allQuery().limit(0).fields({"id"});
          if (filtered) top.matchFilter("sel01_s", "t");
          top.facet("facet", field).limit(10);
          int64_t spawns;
          {
            TaskSpawnCounter counter;
            helper.getSearchEngine().dispatch(*req.get(), -1);
            spawns = counter.count();
          }
          ASSERT_OK(req);
          EXPECT_EQ(segments - 1, spawns);
          ASSERT_NE(nullptr, req->docList());
          EXPECT_EQ(0, req->docList()->row_count);
          const auto* facet = req->docList()->ops.at("facet")->facetResult();
          ASSERT_NE(nullptr, facet);
          if (std::string_view(field) == "cat_s") {
            ASSERT_EQ(filtered ? 4u : 8u, facet->counts.size());
            EXPECT_EQ((filtered ? 32 : 64) * segments,
                      std::accumulate(facet->counts.begin(), facet->counts.end(), (int64_t)0));
          } else {
            ASSERT_EQ(10u, facet->counts.size());
            for (auto count : facet->counts) EXPECT_EQ(1, count);
          }
        }
      }
    }
  }
}

TEST_F(ExecutionProfileTest, currentPerColumnFieldLoadSpawnCounts) {
  CollectionHelper helper("fanout_columns");
  helper.index(flatdoc("id", "1", "cat_s", "a", "value_i", (int64_t)7), UpdateMessage::COMMIT);
  for (int columns : {1, 2, 3}) {
    auto req = localReq(helper.getSearchEngine());
    req->collection("fanout_columns");
    std::vector<std::string> fields{"id", "cat_s", "value_i"};
    fields.resize(columns);
    req->topDocs("q").allQuery().limit(1).fields(fields);
    int64_t spawns;
    {
      TaskSpawnCounter counter;
      helper.getSearchEngine().dispatch(*req.get(), -1);
      spawns = counter.count();
    }
    ASSERT_OK(req);
    EXPECT_EQ(columns - 1, spawns);
    EXPECT_EQ(1, req->docList()->row_count);
  }
}

TEST_F(ExecutionProfileTest, topLevelOpsOverlap) {
  CollectionHelper helper("fanout_overlap");
  helper.indexAll(std::array{flatdoc("id", "1", "cat_s", "a"),
                            flatdoc("id", "2", "cat_s", "b")}, UpdateMessage::COMMIT);
  auto req = localReq(helper.getSearchEngine());
  req->collection("fanout_overlap");
  req->facet("a", "cat_s");
  req->facet("b", "cat_s");
  std::promise<void> firstStarted, secondStarted;
  auto first = firstStarted.get_future();
  auto second = secondStarted.get_future();
  std::latch release(1);
  std::atomic<int> arrivals{0};
  Signal::listen("searchRootChildStart", [&](void*, void*, void*) -> void* {
    if (arrivals.fetch_add(1) == 0) {
      firstStarted.set_value();
      release.wait();
    } else {
      secondStarted.set_value();
    }
    return nullptr;
  });
  int64_t spawns;
  {
    TaskSpawnCounter counter;
    std::thread search([&] { helper.getSearchEngine().dispatch(*req.get(), -1); });
    // Timeouts bound a broken dispatch; both start signals must arrive while
    // the first op is still held by the latch.
    EXPECT_EQ(std::future_status::ready, first.wait_for(std::chrono::seconds(10)));
    EXPECT_EQ(std::future_status::ready, second.wait_for(std::chrono::seconds(10)));
    release.count_down();
    search.join();
    spawns = counter.count();
  }
  Signal::unlisten("searchRootChildStart");
  ASSERT_OK(req);
  EXPECT_EQ(1, spawns);
  EXPECT_EQ(2, arrivals.load());
}

TEST_F(ExecutionProfileTest, rankedSubOpRunsOnCaller) {
  CollectionHelper helper("fanout_ranked");
  helper.index(flatdoc("id", "1", "cat_s", "a"), UpdateMessage::COMMIT);
  auto req = localReq(helper.getSearchEngine());
  req->collection("fanout_ranked").topDocs("q").allQuery().limit(1).fields({"id"})
      .facet("facet", "cat_s");
  std::thread::id childThread;
  Signal::listen("searchTopDocsChildStart", [&](void*, void*, void*) -> void* {
    childThread = std::this_thread::get_id();
    return nullptr;
  });
  int64_t spawns;
  {
    TaskSpawnCounter counter;
    helper.getSearchEngine().dispatch(*req.get(), -1);
    spawns = counter.count();
  }
  Signal::unlisten("searchTopDocsChildStart");
  ASSERT_OK(req);
  EXPECT_EQ(std::this_thread::get_id(), childThread);
  EXPECT_EQ(0, spawns);
  EXPECT_EQ(1, req->docList()->row_count);
  EXPECT_EQ(1, req->docList()->ops.at("facet")->facetResult()->counts[0]);
}

TEST_F(ExecutionProfileTest, inlineFailureCancelsPublishedSiblings) {
  CollectionHelper helper("fanout_cancel");
  helper.index(flatdoc("id", "1", "cat_s", "a"), UpdateMessage::COMMIT);
  auto req = localReq(helper.getSearchEngine());
  req->collection("fanout_cancel");
  for (const char* name : {"a", "b", "c"}) req->facet(name, "cat_s");
  oneapi::tbb::global_control serial(oneapi::tbb::global_control::max_allowed_parallelism, 1);
  std::atomic<int> started{0};
  Signal::listen("searchRootChildStart", [&](void*, void*, void*) -> void* {
    if (started.fetch_add(1) == 0) throw RequestError("inline failure", "invalid_request");
    return nullptr;
  });
  helper.getSearchEngine().dispatch(*req.get(), -1);
  Signal::unlisten("searchRootChildStart");
  EXPECT_FALSE(req->ok());
  EXPECT_EQ("invalid_request", req->errorCode());
  EXPECT_EQ("inline failure", req->errorMsg());
  EXPECT_EQ(1, started.load());
}

TEST_F(ExecutionProfileTest, rankedFacetFanoutSpawnCounts) {
  for (int segments : {1, 4}) {
    CollectionHelper helper("ranked" + std::to_string(segments));
    for (int seg = 0; seg < segments; seg++) {
      std::vector<Doc> docs;
      for (int i = 0; i < 64; i++) {
        docs.push_back(flatdoc("id", std::to_string(seg * 64 + i), "cat_s", "a",
                              "rank_i", (int64_t)(i * segments + seg)));
      }
      helper.indexAll(docs, UpdateMessage::COMMIT);
    }
    ASSERT_EQ((size_t)segments,
              helper.getIndexWriter()->snapshots.readers.getReader()->segments().size());
    for (int profile : {0, -1}) {
      for (bool loadFields : {false, true}) {
        for (int64_t offset : {0, 1000}) {
          SCOPED_TRACE(::testing::Message() << segments << " segments, profile=" << profile
                       << ", fields=" << loadFields << ", offset=" << offset);
          auto req = localReq(helper.getSearchEngine());
          req->collection("ranked" + std::to_string(segments));
          auto& top = req->topDocs("q").allQuery().limit(10).offset(offset)
              .fields({loadFields ? "id" : "_score_"});
          qb::sort(top, "rank_i", qb::DESC);
          top.facet("facet", "cat_s").limit(10);
          int64_t spawns;
          {
            TaskSpawnCounter counter;
            helper.getSearchEngine().dispatch(*req.get(), profile);
            spawns = counter.count();
          }
          ASSERT_OK(req);
          int64_t expected = profile == 0 ? 0 : segments - 1;
          if (profile == -1 && loadFields && offset == 0) expected += segments - 1;
          EXPECT_EQ(expected, spawns);
          ASSERT_NE(nullptr, req->docList());
          EXPECT_EQ(offset == 0 ? 10 : 0, req->docList()->row_count);
          const auto* facet = req->docList()->ops.at("facet")->facetResult();
          ASSERT_NE(nullptr, facet);
          ASSERT_EQ(1u, facet->counts.size());
          EXPECT_EQ(64 * segments, facet->counts[0]);
        }
      }
    }
  }
}

} // namespace luxir::test
