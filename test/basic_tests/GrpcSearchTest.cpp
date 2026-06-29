
#include <iostream>
#include <memory>
#include <variant>
#include <gtest/gtest.h>
#include "test/GrpcClient.h"
#include "test/GrpcSoluxTest.h"
#include "test/LocalReq.h"
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
