
#include <iostream>
#include <gtest/gtest.h>
#include <google/protobuf/text_format.h>
#include "test/SoluxTest.h"
#include "solux/server/GRPCServer.h"

using namespace solux;

class GrpcIndexTest : public SoluxTest {
public:
};


TEST_F(GrpcIndexTest, addDocs) {

  // Setup request
  solux::proto::UpdateRequest ureq;
  HelloReply result;

  ureq.mutable_collection()->add_name("main");

  auto& fields = *ureq.add_docs()->mutable_fields();
  fields["text1_w"].set_s("my first field value");
  fields["text2_w"].set_s("my second field value");

  auto& fields2 = *ureq.add_docs()->mutable_fields();
  fields2["textd2_1_w"].set_s("my first field value of 2nd doc");
  fields2["textd2_2_w"].set_s("my second field value of 2nd doc");
  fields2["textd2_2_w"].set_s("third field of 2nd doc");

  std::string reqStr;
  google::protobuf::TextFormat::PrintToString(ureq, &reqStr);
  std::cout << "CLIENT REQ:( " << reqStr << " )" << std::endl;

  // TODO: thread safety of channels? This does return a shared_ptr
  auto channel = grpc::CreateChannel("localhost:50051", grpc::InsecureChannelCredentials());
  std::unique_ptr<solux::Indexer::Stub> stub = solux::Indexer::NewStub(channel);

  grpc::ClientContext context;
  grpc::Status status = stub->Update(&context, ureq , &result);

  if (!status.ok()) {
    std::cout << "grpc call failed!: " << status.error_code() << " " << status.error_message() << std::endl;
  } else {
    std::cout << "I got:" << result.message() << std::endl;
  }

}
