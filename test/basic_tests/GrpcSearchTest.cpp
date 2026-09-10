// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0


#include <chrono>
#include <iostream>
#include <memory>
#include <thread>
#include <variant>
#include <gtest/gtest.h>
#include "test/CollectionHelper.h"
#include "test/GrpcClient.h"
#include "test/GrpcLuxirTest.h"
#include "test/LocalReq.h"
#include "test/QueryBuild.h"
#include "test/TestUtils.h"
#include "luxir/server/GRPCServer.h"

using namespace luxir;
using namespace luxir::test;  // HppClientReaderWriter, Reply, rpc::

// TODO - use a different logger for RPC stuff some point
// redefine DEBUG to TRACE level which shouldn't currently be logged!
#define GRPC_DEBUG LOG_TRACE

class GrpcSearchTest : public GrpcLuxirTest {
public:
  std::shared_ptr<grpc::Channel> channel;

  GrpcSearchTest() {
    channel = getChannel();
  }
};



TEST_F(GrpcSearchTest, basic) {
  grpc::ClientContext context;  // need a new one for each RPC

  // Build a concrete SearchRequest with the OpCursor builder (used only as a builder here;
  // we serialize its `view`, we do not execute locally).
  auto lreq = localReq(luxirNode->getSearchEngine());
  lreq->collection("main").topDocs("q").allQuery();

  GRPC_DEBUG("CLIENT REQ: key=q");

  HppClientReaderWriter<luxir::api::SearchRequest, luxir::api::SearchResponse> stream(
    channel.get(), rpc::Search, &context);
  bool wrote = stream.Write(lreq->proto);  // lreq->proto is the built (non-owning) SearchRequest view
  ASSERT_TRUE(wrote);
  bool ok = stream.WritesDone();  // can replace with WriteLast? is it more efficient?
  ASSERT_TRUE(ok);

  Reply<luxir::api::SearchResponse> response;
  while (stream.Read(&response)) {
    GRPC_DEBUG("CLIENT RESULT: ops={}", response.msg.ops.size());
    auto& rsp = *response.msg.ops.at("q");  // make sure the key was unadulterated
    ASSERT_TRUE(std::holds_alternative<luxir::api::DocList>(rsp.kind));
  }

  grpc::Status status = stream.Finish();
  GRPC_DEBUG("STREAMING SEARCH CLIENT FINISHED");
  ASSERT_TRUE(status.ok());
}

TEST_F(GrpcSearchTest, statsUnary) {
  luxir::api::StatsRequest request;
  request.segments = true;
  grpc::ClientContext context;
  Reply<luxir::api::StatsResponse> response;
  auto status = hppUnaryCall(channel.get(), rpc::Stats, &context, request, &response);
  ASSERT_TRUE(status.ok()) << status.error_message();
  EXPECT_GT(response.msg.totals.collections, 0);
  EXPECT_FALSE(response.msg.collections.empty());
}

TEST_F(GrpcSearchTest, collectionCreateDeleteUnary) {
  luxir::api::CreateCollectionRequest create;
  create.name = "grpc_admin_lifecycle";
  grpc::ClientContext createContext;
  Reply<luxir::api::CreateCollectionResponse> createResponse;
  auto createStatus = hppUnaryCall(
      channel.get(), rpc::CreateCollection, &createContext, create, &createResponse);
  ASSERT_TRUE(createStatus.ok()) << createStatus.error_message();
  EXPECT_EQ("grpc_admin_lifecycle", createResponse.msg.name);
  EXPECT_NO_THROW(luxirNode->getCollection("grpc_admin_lifecycle"));

  luxir::api::DeleteCollectionRequest remove;
  remove.name = "grpc_admin_lifecycle";
  grpc::ClientContext deleteContext;
  Reply<luxir::api::DeleteCollectionResponse> deleteResponse;
  auto deleteStatus = hppUnaryCall(
      channel.get(), rpc::DeleteCollection, &deleteContext, remove, &deleteResponse);
  ASSERT_TRUE(deleteStatus.ok()) << deleteStatus.error_message();
  EXPECT_EQ("grpc_admin_lifecycle", deleteResponse.msg.name);
  EXPECT_THROW(luxirNode->getCollection("grpc_admin_lifecycle"), CollectionNotFoundError);
}

// A small batch_size forces emitDocsResponse to stream multiple responses over
// one RPC, exercising the server's pending-write queue: response arenas are
// freed at reply() time, so the queued ByteBuffers must own their bytes.
TEST_F(GrpcSearchTest, multiBatchStreaming) {
  CollectionHelper ch;
  for (int i = 0; i < 9; i++) {
    auto commit = i == 8 ? UpdateMessage::COMMIT : UpdateMessage::NO_COMMIT;
    ch.index(flatdoc("id", std::string("d") + std::to_string(i)), commit);
  }

  auto lreq = localReq(luxirNode->getSearchEngine());
  lreq->collection("main").topDocs("q").allQuery()
      .fields({"id"}).batchSize(2).limit(-1);

  grpc::ClientContext context;
  HppClientReaderWriter<luxir::api::SearchRequest, luxir::api::SearchResponse> stream(
    channel.get(), rpc::Search, &context);
  ASSERT_TRUE(stream.Write(lreq->proto));
  ASSERT_TRUE(stream.WritesDone());

  int64_t totalDocs = 0;
  int responses = 0;
  bool sawLast = false;
  Reply<luxir::api::SearchResponse> response;
  while (stream.Read(&response)) {
    responses++;
    EXPECT_FALSE(sawLast);  // nothing after the first response without more
    sawLast = !response.msg.more;
    auto& rsp = *response.msg.ops.at("q");
    auto& docList = std::get<luxir::api::DocList>(rsp.kind);
    EXPECT_EQ(response.msg.more, docList.more);
    EXPECT_EQ(totalDocs, docList.offset);
    totalDocs += docList.row_count;
  }
  ASSERT_TRUE(stream.Finish().ok());

  EXPECT_EQ(9, totalDocs);
  EXPECT_EQ(5, responses);  // 4 batches of 2 + final batch of 1
  EXPECT_TRUE(sawLast);
}

// Flow control: a client that stops reading must not cause unbounded
// server-side buffering.  A private server with a tiny stream buffer forces
// the emitter to pause (observable via streamPauseCount) while the client
// withholds reads; draining the stream resumes it and every doc arrives.
TEST_F(GrpcSearchTest, backpressurePausesEmitter) {
  LuxirTest::clearCollection("grpc_bp");
  CollectionHelper ch("grpc_bp");
  std::string pad(400, 'x');
  std::vector<Doc> docs;
  for (int i = 0; i < 1000; i++) {
    docs.push_back(flatdoc("id", "bp" + std::to_string(i), "pad_s", pad));
  }
  ASSERT_TRUE(ch.indexAll(docs, UpdateMessage::COMMIT).success);

  GRPCServer server(*luxirNode, 2, 0, /*streamBufferBytes=*/4096);
  std::thread serverThread([&] { server.run(); });
  ASSERT_TRUE(server.waitForStart());
  auto channel = grpc::CreateChannel("localhost:" + std::to_string(server.getPort()),
                                     grpc::InsecureChannelCredentials());

  int64_t pausesBefore = streamPauseCount.load();

  auto lreq = localReq(luxirNode->getSearchEngine());
  lreq->collection("grpc_bp").topDocs("q").allQuery()
      .fields({"id", "pad_s"}).batchSize(100).limit(-1);

  grpc::ClientContext context;
  HppClientReaderWriter<luxir::api::SearchRequest, luxir::api::SearchResponse> stream(
      channel.get(), rpc::Search, &context);
  ASSERT_TRUE(stream.Write(lreq->proto));
  ASSERT_TRUE(stream.WritesDone());

  // Withhold reads until the transport backs up and the emitter parks.
  bool paused = false;
  for (int i = 0; i < 400 && !paused; i++) {
    paused = streamPauseCount.load() > pausesBefore;
    if (!paused) std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  EXPECT_TRUE(paused);

  int64_t totalDocs = 0;
  Reply<luxir::api::SearchResponse> response;
  while (stream.Read(&response)) {
    auto& rsp = *response.msg.ops.at("q");
    auto& docList = std::get<luxir::api::DocList>(rsp.kind);
    EXPECT_EQ(totalDocs, docList.offset);
    totalDocs += docList.row_count;
  }
  ASSERT_TRUE(stream.Finish().ok());
  EXPECT_EQ(1000, totalDocs);

  server.shutdown();
  serverThread.join();
}

// Doc-line framing is an HTTP/NDJSON concept; gRPC rejects it explicitly
// rather than silently returning envelope-framed batches.
TEST_F(GrpcSearchTest, responseFormatDocsIsRejected) {
  auto lreq = localReq(luxirNode->getSearchEngine());
  lreq->collection("main").responseFormat(luxir::api::ResponseFormat::DOCS)
      .topDocs("q").allQuery().fields({"id"});

  grpc::ClientContext context;
  HppClientReaderWriter<luxir::api::SearchRequest, luxir::api::SearchResponse> stream(
      channel.get(), rpc::Search, &context);
  ASSERT_TRUE(stream.Write(lreq->proto));
  stream.WritesDone();
  Reply<luxir::api::SearchResponse> response;
  while (stream.Read(&response)) {}
  auto status = stream.Finish();
  EXPECT_EQ(grpc::StatusCode::INVALID_ARGUMENT, status.error_code());
}

TEST_F(GrpcSearchTest, routedFilterValidation) {
  CollectionHelper helper;
  helper.index(flatdoc("brand_s", "acme"), UpdateMessage::COMMIT);

  auto finish = [&](const luxir::api::SearchRequest& request) {
    grpc::ClientContext context;
    HppClientReaderWriter<luxir::api::SearchRequest, luxir::api::SearchResponse> stream(
        channel.get(), rpc::Search, &context);
    EXPECT_TRUE(stream.Write(request));
    EXPECT_TRUE(stream.WritesDone());
    std::string error;
    Reply<luxir::api::SearchResponse> response;
    while (stream.Read(&response)) {
      if (response.msg.error) error = std::string(response.msg.error->message);
    }
    return std::pair{stream.Finish(), error};
  };

  auto routed = localReq(luxirNode->getSearchEngine());
  routed->collection("main");
  auto& top = routed->topDocs("q").allQuery();
  top.facet("brands", "brand_s");
  top.filter(qb::match(top.mr(), "brand_s", "acme"), {"brands"});
  auto [routedStatus, routedError] = finish(routed->proto);
  EXPECT_TRUE(routedStatus.ok());
  EXPECT_TRUE(routedError.empty()) << routedError;

  auto missing = localReq(luxirNode->getSearchEngine());
  missing->collection("main");
  auto& missingTop = missing->topDocs("q").allQuery();
  auto& missingProto = std::get<luxir::api::TopDocs>(missingTop.rawOp().kind);
  luxir::api::build::allocArray(missingProto.filter, 1, missingTop.mr());
  auto [missingStatus, missingError] = finish(missing->proto);
  EXPECT_TRUE(missingStatus.ok());
  EXPECT_NE(std::string::npos,
            missingError.find(
                "top_docs.filter[0].query requires a query kind"));
}

// A client that cancels the RPC while the emitter is paused must not strand
// the request: the failed writes wake the parked emitter, whose next reply()
// observes CANCEL and completes the call.  A stranded call would hang
// server.shutdown() here (grpc::Server::Shutdown waits for in-flight RPCs).
TEST_F(GrpcSearchTest, disconnectWhilePausedCancelsEmitter) {
  LuxirTest::clearCollection("grpc_bp2");
  CollectionHelper ch("grpc_bp2");
  std::string pad(400, 'x');
  std::vector<Doc> docs;
  for (int i = 0; i < 1000; i++) {
    docs.push_back(flatdoc("id", "bp" + std::to_string(i), "pad_s", pad));
  }
  ASSERT_TRUE(ch.indexAll(docs, UpdateMessage::COMMIT).success);

  GRPCServer server(*luxirNode, 2, 0, /*streamBufferBytes=*/4096);
  std::thread serverThread([&] { server.run(); });
  ASSERT_TRUE(server.waitForStart());
  auto channel = grpc::CreateChannel("localhost:" + std::to_string(server.getPort()),
                                     grpc::InsecureChannelCredentials());

  int64_t pausesBefore = streamPauseCount.load();

  auto lreq = localReq(luxirNode->getSearchEngine());
  lreq->collection("grpc_bp2").topDocs("q").allQuery()
      .fields({"id", "pad_s"}).batchSize(100).limit(-1);

  {
    grpc::ClientContext context;
    HppClientReaderWriter<luxir::api::SearchRequest, luxir::api::SearchResponse> stream(
        channel.get(), rpc::Search, &context);
    ASSERT_TRUE(stream.Write(lreq->proto));
    ASSERT_TRUE(stream.WritesDone());

    bool paused = false;
    for (int i = 0; i < 400 && !paused; i++) {
      paused = streamPauseCount.load() > pausesBefore;
      if (!paused) std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    ASSERT_TRUE(paused);

    context.TryCancel();
    Reply<luxir::api::SearchResponse> response;
    while (stream.Read(&response)) {}
    stream.Finish();  // status is CANCELLED; only completion matters here
  }

  server.shutdown();  // hangs if the paused request was stranded
  serverThread.join();
}
