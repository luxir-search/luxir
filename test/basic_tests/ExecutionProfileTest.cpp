#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <future>
#include <memory_resource>
#include <string>
#include <thread>
#include <vector>

#include "luxir/api/padded_input.h"
#include "luxir/query/BooleanQuery.h"
#include "luxir/reader/SkipStats.h"
#include "luxir/search/SearchOverrides.h"
#include "luxir/server/JsonResponse.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/QueryBuild.h"
#include "test/LuxirTest.h"
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
            renderSearchResponseLine(req->responses.back()->proto).find("\"profile\""));

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

  std::string json = renderSearchResponseLine(req->responses.back()->proto);
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
  auto& q = req->topDocs("q").allQuery().limit(0).matchFilter("f", "sel_s", "t");
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
    EXPECT_NE(std::string_view::npos,
              req->responses.back()->proto.error.find("max_parallel")) << maxParallel;
  }
}

TEST_F(ExecutionProfileTest, maxParallelMinusOneRunsInlineOnCallingThread) {
  CollectionHelper helper("profile_inline");
  std::vector<Doc> docs;
  for (int i = 0; i < 32; i++) {
    docs.push_back(flatdoc("id", std::to_string(i), "cat_s", "v" + std::to_string(i % 4)));
    if (i % 16 == 15) helper.indexAll(docs, UpdateMessage::COMMIT), docs.clear();
  }

  auto req = localReq(helper.getSearchEngine());
  req->collection("profile_inline").profile().facet("cats", "cat_s").limit(-1);
  helper.getSearchEngine().dispatch(*req.get(), -1);
  // -1 = the calling thread: the whole request completed inside dispatch(), so
  // the response is readable with no wait/synchronization at all.
  ASSERT_OK(req);
  const auto& pieces = profileOp(*req).pieces;
  ASSERT_GT(pieces.size(), 1u);
  for (const auto& piece : pieces) {
    EXPECT_EQ(pieces[0].thread_id, piece.thread_id);
  }
}

namespace {

// dispatch() is asynchronous for max_parallel >= 0; record the replying thread
// and use the promise as a completion latch (reply() runs before set_value, so
// the future's readiness orders `responses` for the test thread).
class DispatchReq : public LocalReq {
public:
  std::promise<std::thread::id> replied;
  using LocalReq::LocalReq;
  ReplyStatus reply(SearchResponse& response) override {
    ReplyStatus status = LocalReq::reply(response);
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

} // namespace luxir::test
