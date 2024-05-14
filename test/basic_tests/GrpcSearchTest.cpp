
#include <iostream>
#include <gtest/gtest.h>
#include <google/protobuf/text_format.h>
#include "test/SoluxTest.h"
#include "solux/server/GRPCServer.h"

using namespace solux;

// TODO - use a different logger for RPC stuff some point
// redefine DEBUG to TRACE level which shouldn't currently be logged!
#define GRPC_DEBUG LOG_TRACE

class GrpcSearchTest : public SoluxTest {
public:
  std::shared_ptr<grpc::Channel> channel;
  std::unique_ptr<solux::Searcher::Stub> searchStub;

  GrpcSearchTest() {
    channel = grpc::CreateChannel("localhost:50051", grpc::InsecureChannelCredentials());
    searchStub = solux::Searcher::NewStub(channel);
  }
};



TEST_F(GrpcSearchTest, basic) {
  // Setup request
  solux::proto::SearchRequest req;
  solux::proto::SearchResponse response;
  grpc::ClientContext context;  // need a new one for each RPC

  req.mutable_collection()->add_name("main");
  auto& ops = *req.mutable_ops();
  // invalid utf8 looks to be validated by both libprotobuf on both serialization and deserialization! Is there a way to stop this?
  // It actually still works and passes the test, but it generates ERROR output to stderr.
  // std::string key = "q\xc3\x01";
  std::string key = "q";
  ops[key].mutable_top_docs()->mutable_query()->set_all(true);

  std::string reqStr;
  google::protobuf::TextFormat::PrintToString(req, &reqStr);
  GRPC_DEBUG("CLIENT REQ:( {} )", reqStr);

  std::unique_ptr<grpc::ClientReaderWriter<solux::proto::SearchRequest, solux::proto::SearchResponse>> stream = searchStub->Search(&context);
  bool wrote = stream->Write(req);
  ASSERT_TRUE(wrote);
  bool ok = stream->WritesDone();  // can replace with WriteLast? is it more efficient?
  ASSERT_TRUE(ok);

  while (stream->Read(&response)) {
    std::string resStr;
    google::protobuf::TextFormat::PrintToString(response, &resStr);
    GRPC_DEBUG("CLIENT RESULT:( {} )", resStr);
    auto& rsp = response.ops().at(key);  // make sure the key was unadulterated
    ASSERT_TRUE(rsp.has_docs());
  }

  grpc::Status status = stream->Finish();
  GRPC_DEBUG("STREAMING SEARCH CLIENT FINISHED");
  ASSERT_TRUE(status.ok());
}

// codecs are not currently thread safe.
// we should create a test that fails before we fix this.
// We should create a big test index (i.e. not "main") that can be reused by multiple tests.
TEST_F(GrpcSearchTest, threadsafe) {
  // idea: do a series of searches in a single thread and record the results.  then use multiple threads and see
  // if the results are the same.


}

