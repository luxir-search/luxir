#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <future>
#include <memory_resource>
#include <string>
#include <thread>
#include <vector>

#include "solux/api/padded_input.h"
#include "solux/server/JsonResponse.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"
#include "test/SoluxTest.h"
#include "test/TestUtils.h"

namespace solux::test {

class ExecutionProfileTest : public SoluxTest {};

namespace {

std::string_view expectedStrategy(int64_t domainSize, int64_t cardinality) {
  if ((domainSize >> 8) >= cardinality) return "vector";
  if ((cardinality >> 6) >= domainSize) return "hash";
  return "skinny";
}

const api::ExecutionProfileOp& profileOp(const LocalReq& req) {
  const auto& response = req.responses.back()->proto;
  EXPECT_TRUE(response.profile.has_value());
  EXPECT_EQ(1u, response.profile->ops.size());
  return response.profile->ops[0];
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
  helper.indexAll(std::array{
      flatdoc("id", "2", "tags_ss", vecs("v0")),
      flatdoc("id", "3", "tags_ss", vecs("v1")),
  }, UpdateMessage::COMMIT);
  helper.index(flatdoc("id", "4", "tags_ss", vecs("v2")), UpdateMessage::COMMIT);
  ASSERT_EQ(3u, helper.durableSegmentCount());

  auto req = localReq(helper.getSearchEngine());
  req->collection("main").profile().facet("tags", "tags_ss").limit(-1);
  req->execute(false);
  ASSERT_OK(req);

  const auto& op = profileOp(*req);
  EXPECT_EQ("tags", op.name);
  ASSERT_EQ(3u, op.pieces.size());
  for (size_t i = 0; i < op.pieces.size(); i++) {
    const auto& piece = op.pieces[i];
    EXPECT_EQ("segment", piece.kind);
    EXPECT_EQ((int32_t)i, piece.segment);
    ASSERT_TRUE(piece.cardinality.has_value());
    ASSERT_TRUE(piece.domain_size.has_value());
    EXPECT_EQ(64, *piece.cardinality);
    EXPECT_EQ(piece.max_doc, *piece.domain_size);
    EXPECT_EQ(expectedStrategy(*piece.domain_size, *piece.cardinality), piece.strategy);
    // Unfiltered facets retain fixed bulk decoding for both rank-indexed and
    // docid-indexed columns.
    ASSERT_GE(piece.details.size(), 1u);
    EXPECT_EQ("all-docs domain, bulk ord loads", piece.details[0]);
    EXPECT_GT(piece.thread_id, 0);
    EXPECT_LT(piece.elapsed_us, 60'000'000u);
  }
  // Sequential contribution order: piece 0 creates the hash accumulator,
  // piece 1 wants skinny and pays the upgrade, piece 2 wants hash but rides
  // the already-upgraded skinny (no-downgrade rule) - all spelled out in the
  // human-readable details rather than typed fields.
  EXPECT_EQ("hash", op.pieces[0].strategy);
  ASSERT_EQ(1u, op.pieces[0].details.size());
  EXPECT_EQ("skinny", op.pieces[1].strategy);
  ASSERT_EQ(2u, op.pieces[1].details.size());
  EXPECT_EQ("upgraded shared counters to skinny", op.pieces[1].details[1]);
  EXPECT_EQ("hash", op.pieces[2].strategy);
  ASSERT_EQ(2u, op.pieces[2].details.size());
  EXPECT_EQ("want=hash, found=skinny", op.pieces[2].details[1]);

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

TEST_F(ExecutionProfileTest, reportsVectorAtStrategyBoundary) {
  CollectionHelper helper("profile-vector");
  std::vector<Doc> docs;
  for (int i = 0; i < 256; i++) {
    docs.push_back(flatdoc("id", std::to_string(i), "cat_s", "only"));
  }
  helper.indexAll(docs, UpdateMessage::COMMIT);

  auto req = localReq(helper.getSearchEngine());
  req->collection("profile-vector").profile().facet("cats", "cat_s").limit(-1);
  req->execute(false);
  ASSERT_OK(req);

  const auto& pieces = profileOp(*req).pieces;
  ASSERT_EQ(1u, pieces.size());
  EXPECT_EQ(256, *pieces[0].domain_size);
  EXPECT_EQ(1, *pieces[0].cardinality);
  EXPECT_EQ("vector", pieces[0].strategy);
  ASSERT_EQ(1u, pieces[0].details.size());  // no upgrade, no divergence note
}

TEST_F(ExecutionProfileTest, reportsPointOrdLoadsForArrayDomains) {
  CollectionHelper helper("profile-sparse");
  std::vector<Doc> docs;
  for (int i = 0; i < 256; i++) {
    docs.push_back(flatdoc("id", std::to_string(i), "cat_s", "only",
                           "sel_s", i < 3 ? "t" : "f"));
  }
  helper.indexAll(docs, UpdateMessage::COMMIT);

  auto req = localReq(helper.getSearchEngine());
  req->collection("profile-sparse").profile();
  auto& q = req->topDocs("q").allQuery().limit(0).matchFilter("f", "sel_s", "t");
  q.facet("cats", "cat_s").limit(-1);
  req->execute(false);
  ASSERT_OK(req);

  // A 3-doc filtered domain materializes as an ArrDocSet: the ord column walk
  // point-selects each domain doc, and the profile must say so.
  const auto& pieces = profileOp(*req).pieces;
  ASSERT_EQ(1u, pieces.size());
  EXPECT_EQ(3, *pieces[0].domain_size);
  ASSERT_GE(pieces[0].details.size(), 1u);
  EXPECT_EQ("array domain, point ord loads", pieces[0].details[0]);
}

TEST_F(ExecutionProfileTest, maxParallelOneRunsSingleThreaded) {
  CollectionHelper helper("profile-serial");
  std::vector<Doc> docs;
  for (int i = 0; i < 64; i++) {
    docs.push_back(flatdoc("id", std::to_string(i), "cat_s", "v" + std::to_string(i % 8)));
    if (i % 16 == 15) helper.indexAll(docs, UpdateMessage::COMMIT), docs.clear();
  }

  auto req = localReq(helper.getSearchEngine());
  req->collection("profile-serial").profile().facet("cats", "cat_s").limit(-1);
  req->execute((int32_t)1);
  ASSERT_OK(req);

  const auto& pieces = profileOp(*req).pieces;
  ASSERT_GT(pieces.size(), 1u);
  for (const auto& piece : pieces) {
    EXPECT_EQ(pieces[0].thread_id, piece.thread_id);
  }
}

TEST_F(ExecutionProfileTest, maxParallelOutOfRangeIsRejected) {
  CollectionHelper helper("profile-reject");
  helper.indexAll(std::array{flatdoc("id", "1", "cat_s", "a")}, UpdateMessage::COMMIT);

  for (int32_t maxParallel : {2, -2}) {
    auto req = localReq(helper.getSearchEngine());
    req->collection("profile-reject").facet("cats", "cat_s").limit(-1);
    req->execute(maxParallel);
    ASSERT_FALSE(req->responses.empty());
    EXPECT_NE(std::string_view::npos,
              req->responses.back()->proto.error.find("max_parallel")) << maxParallel;
  }
}

TEST_F(ExecutionProfileTest, maxParallelMinusOneRunsInlineOnCallingThread) {
  CollectionHelper helper("profile-inline");
  std::vector<Doc> docs;
  for (int i = 0; i < 32; i++) {
    docs.push_back(flatdoc("id", std::to_string(i), "cat_s", "v" + std::to_string(i % 4)));
    if (i % 16 == 15) helper.indexAll(docs, UpdateMessage::COMMIT), docs.clear();
  }

  auto req = localReq(helper.getSearchEngine());
  req->collection("profile-inline").profile().facet("cats", "cat_s").limit(-1);
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
  CollectionHelper helper("profile-pool");
  helper.indexAll(std::array{flatdoc("id", "1", "cat_s", "a")}, UpdateMessage::COMMIT);

  auto* arena = createArena();
  auto req = LocalReqHandle(
      solux::arenaCreate<DispatchReq>(*arena, helper.getSearchEngine(), *arena));
  auto repliedOn = static_cast<DispatchReq*>(req.get())->replied.get_future();
  req->collection("profile-pool").facet("cats", "cat_s").limit(-1);
  helper.getSearchEngine().dispatch(*req.get(), 1);
  ASSERT_EQ(std::future_status::ready, repliedOn.wait_for(std::chrono::seconds(60)));
  EXPECT_NE(std::this_thread::get_id(), repliedOn.get());
  ASSERT_OK(req);
}

} // namespace solux::test
