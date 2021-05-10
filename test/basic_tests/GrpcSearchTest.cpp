
#include <iostream>
#include <gtest/gtest.h>
#include <google/protobuf/text_format.h>
#include "test/SoluxTest.h"
#include "solux/server/GRPCServer.h"

using namespace solux;

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
  ops["q"].mutable_top_docs()->mutable_query()->set_all(true);

  std::string reqStr;
  google::protobuf::TextFormat::PrintToString(req, &reqStr);
  std::cout << "CLIENT REQ:( " << reqStr << " )" << std::endl;

  std::unique_ptr<grpc::ClientReaderWriter<solux::proto::SearchRequest, solux::proto::SearchResponse>> stream = searchStub->Search(&context);
  bool wrote = stream->Write(req);
  ASSERT_TRUE(wrote);
  bool ok = stream->WritesDone();  // can replace with WriteLast? is it more efficient?
  ASSERT_TRUE(ok);

  while (stream->Read(&response)) {
    std::string resStr;
    google::protobuf::TextFormat::PrintToString(response, &resStr);
    std::cout << "CLIENT RESULT:( " << resStr << " )" << std::endl;
  }

  grpc::Status status = stream->Finish();
  std::cout << "STREAMING SEARCH CLIENT FINISHED" << std::endl;
  ASSERT_TRUE(status.ok());
}

