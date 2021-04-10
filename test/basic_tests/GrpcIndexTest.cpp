
#include <iostream>
#include <gtest/gtest.h>
#include <google/protobuf/text_format.h>
#include "test/SoluxTest.h"
#include "solux/server/GRPCServer.h"

using namespace solux;

class GrpcIndexTest : public SoluxTest {
public:
  std::shared_ptr<grpc::Channel> channel;
  std::unique_ptr<solux::Greeter::Stub> greeterStub;
  std::unique_ptr<solux::Indexer::Stub> indexerStub;

  GrpcIndexTest() {
    channel = grpc::CreateChannel("localhost:50051", grpc::InsecureChannelCredentials());
    greeterStub = solux::Greeter::NewStub(channel);
    indexerStub = solux::Indexer::NewStub(channel);
  }


};

// Test to see if our generic methods of communication (client objects, grpcserver impl, etc) are thread safe.
// This does not test application logic for thread safety, just the communications infrastructure (and how we use it.)
TEST_F(GrpcIndexTest, threadsafe) {
  std::vector<std::thread> threads;

  int nthreads = std::max(2u, std::thread::hardware_concurrency());
  threads.reserve(nthreads);

  for (int i=0; i<nthreads; i++) {
    threads.emplace_back(
            [this,i]{
              std::cout << "STARTED TEST THREAD " << i <<  std::endl;

              std::string name = "Name_" + std::to_string(i) + "_";
              int namelen = name.size();

              for (int j=0; j<100; j++) {
                solux::HelloRequest req;
                solux::HelloReply result;
                grpc::ClientContext context;  // need a new one for each RPC


                name.resize(namelen);
                name.append(std::to_string(j)); // TODO: this may still create another string

                req.set_name(name);

                grpc::Status status;
                if ((i+j)%2 == 0) {
                  status = greeterStub->SayHello(&context, req, &result);
                } else {
                  status = greeterStub->SayHello2(&context, req, &result);
                }

                // std::cout << "Got response " << result.message() <<  std::endl;

                ASSERT_TRUE(result.message().ends_with(name));
              }
            }
            );
  }

  for (auto& thread : threads) {
    thread.join();
  }

}


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
  // auto channel = grpc::CreateChannel("localhost:50051", grpc::InsecureChannelCredentials());
  // std::unique_ptr<solux::Indexer::Stub> stub = solux::Indexer::NewStub(channel);

  grpc::ClientContext context;
  grpc::Status status = indexerStub->Update(&context, ureq , &result);

  if (!status.ok()) {
    std::cout << "grpc call failed!: " << status.error_code() << " " << status.error_message() << std::endl;
  } else {
    std::cout << "I got:" << result.message() << std::endl;
  }

}
