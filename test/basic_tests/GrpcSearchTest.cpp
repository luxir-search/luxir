
#include <iostream>
#include <memory>
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
