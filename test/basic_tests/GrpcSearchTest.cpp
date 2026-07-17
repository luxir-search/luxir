
#include <chrono>
#include <iostream>
#include <memory>
#include <thread>
#include <variant>
#include <gtest/gtest.h>
#include "test/CollectionHelper.h"
#include "test/GrpcClient.h"
#include "test/GrpcSoluxTest.h"
#include "test/LocalReq.h"
#include "test/TestUtils.h"
#include "solux/server/GRPCServer.h"

using namespace solux;
using namespace solux::test;  // HppClientReaderWriter, Reply, rpc::

// TODO - use a different logger for RPC stuff some point
// redefine DEBUG to TRACE level which shouldn't currently be logged!
#define GRPC_DEBUG LOG_TRACE

class GrpcSearchTest : public GrpcSoluxTest {
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
  auto lreq = localReq(soluxNode->getSearchEngine());
  lreq->collection("main").topDocs("q").allQuery();

  GRPC_DEBUG("CLIENT REQ: key=q");

  HppClientReaderWriter<solux::api::SearchRequest, solux::api::SearchResponse> stream(
    channel.get(), rpc::Search, &context);
  bool wrote = stream.Write(lreq->proto);  // lreq->proto is the built (non-owning) SearchRequest view
  ASSERT_TRUE(wrote);
  bool ok = stream.WritesDone();  // can replace with WriteLast? is it more efficient?
  ASSERT_TRUE(ok);

  Reply<solux::api::SearchResponse> response;
  while (stream.Read(&response)) {
    GRPC_DEBUG("CLIENT RESULT: ops={}", response.msg.ops.size());
    auto& rsp = *response.msg.ops.at("q");  // make sure the key was unadulterated
    ASSERT_TRUE(std::holds_alternative<solux::api::DocList>(rsp.kind));
  }

  grpc::Status status = stream.Finish();
  GRPC_DEBUG("STREAMING SEARCH CLIENT FINISHED");
  ASSERT_TRUE(status.ok());
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

  auto lreq = localReq(soluxNode->getSearchEngine());
  lreq->collection("main").topDocs("q").allQuery()
      .fields({"id"}).batchSize(2).limit(-1);

  grpc::ClientContext context;
  HppClientReaderWriter<solux::api::SearchRequest, solux::api::SearchResponse> stream(
    channel.get(), rpc::Search, &context);
  ASSERT_TRUE(stream.Write(lreq->proto));
  ASSERT_TRUE(stream.WritesDone());

  int64_t totalDocs = 0;
  int responses = 0;
  bool sawLast = false;
  Reply<solux::api::SearchResponse> response;
  while (stream.Read(&response)) {
    responses++;
    EXPECT_FALSE(sawLast);  // nothing after the first response without more
    sawLast = !response.msg.more;
    auto& rsp = *response.msg.ops.at("q");
    auto& docList = std::get<solux::api::DocList>(rsp.kind);
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
  SoluxTest::clearCollection("grpc_bp");
  CollectionHelper ch("grpc_bp");
  std::string pad(400, 'x');
  std::vector<Doc> docs;
  for (int i = 0; i < 1000; i++) {
    docs.push_back(flatdoc("id", "bp" + std::to_string(i), "pad_s", pad));
  }
  ch.indexAll(docs, UpdateMessage::COMMIT);

  GRPCServer server(*soluxNode, 2, 0, /*streamBufferBytes=*/4096);
  std::thread serverThread([&] { server.run(); });
  ASSERT_TRUE(server.waitForStart());
  auto channel = grpc::CreateChannel("localhost:" + std::to_string(server.getPort()),
                                     grpc::InsecureChannelCredentials());

  int64_t pausesBefore = streamPauseCount.load();

  auto lreq = localReq(soluxNode->getSearchEngine());
  lreq->collection("grpc_bp").topDocs("q").allQuery()
      .fields({"id", "pad_s"}).batchSize(100).limit(-1);

  grpc::ClientContext context;
  HppClientReaderWriter<solux::api::SearchRequest, solux::api::SearchResponse> stream(
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
  Reply<solux::api::SearchResponse> response;
  while (stream.Read(&response)) {
    auto& rsp = *response.msg.ops.at("q");
    auto& docList = std::get<solux::api::DocList>(rsp.kind);
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
  auto lreq = localReq(soluxNode->getSearchEngine());
  lreq->collection("main").responseFormat(solux::api::ResponseFormat::DOCS)
      .topDocs("q").allQuery().fields({"id"});

  grpc::ClientContext context;
  HppClientReaderWriter<solux::api::SearchRequest, solux::api::SearchResponse> stream(
      channel.get(), rpc::Search, &context);
  ASSERT_TRUE(stream.Write(lreq->proto));
  stream.WritesDone();
  Reply<solux::api::SearchResponse> response;
  while (stream.Read(&response)) {}
  auto status = stream.Finish();
  EXPECT_EQ(grpc::StatusCode::INVALID_ARGUMENT, status.error_code());
}

// A client that cancels the RPC while the emitter is paused must not strand
// the request: the failed writes wake the parked emitter, whose next reply()
// observes CANCEL and completes the call.  A stranded call would hang
// server.shutdown() here (grpc::Server::Shutdown waits for in-flight RPCs).
TEST_F(GrpcSearchTest, disconnectWhilePausedCancelsEmitter) {
  SoluxTest::clearCollection("grpc_bp2");
  CollectionHelper ch("grpc_bp2");
  std::string pad(400, 'x');
  std::vector<Doc> docs;
  for (int i = 0; i < 1000; i++) {
    docs.push_back(flatdoc("id", "bp" + std::to_string(i), "pad_s", pad));
  }
  ch.indexAll(docs, UpdateMessage::COMMIT);

  GRPCServer server(*soluxNode, 2, 0, /*streamBufferBytes=*/4096);
  std::thread serverThread([&] { server.run(); });
  ASSERT_TRUE(server.waitForStart());
  auto channel = grpc::CreateChannel("localhost:" + std::to_string(server.getPort()),
                                     grpc::InsecureChannelCredentials());

  int64_t pausesBefore = streamPauseCount.load();

  auto lreq = localReq(soluxNode->getSearchEngine());
  lreq->collection("grpc_bp2").topDocs("q").allQuery()
      .fields({"id", "pad_s"}).batchSize(100).limit(-1);

  {
    grpc::ClientContext context;
    HppClientReaderWriter<solux::api::SearchRequest, solux::api::SearchResponse> stream(
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
    Reply<solux::api::SearchResponse> response;
    while (stream.Read(&response)) {}
    stream.Finish();  // status is CANCELLED; only completion matters here
  }

  server.shutdown();  // hangs if the paused request was stranded
  serverThread.join();
}
