
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
    // channels are thread safe
    // TODO: move channel to somewhere that multiple tests can use it
    channel = grpc::CreateChannel("localhost:50051", grpc::InsecureChannelCredentials());
    greeterStub = solux::Greeter::NewStub(channel);
    indexerStub = solux::Indexer::NewStub(channel);
  }


};


TEST_F(GrpcIndexTest, streaming) {
  solux::HelloRequest req;
  solux::HelloReply result;
  grpc::ClientContext context;  // need a new one for each RPC

  std::unique_ptr<grpc::ClientReaderWriter<HelloRequest,HelloReply>> stream = greeterStub->SayHelloStreaming(&context);

  req.set_name("A");
  bool wrote = stream->Write(req);
  ASSERT_TRUE(wrote);
  req.set_name("B");
  wrote = stream->Write(req);
  ASSERT_TRUE(wrote);

  bool ok1 = stream->WritesDone();  // can replace with WriteLast? is it more efficient?
  ASSERT_TRUE(ok1);

  while (stream->Read(&result)) {
    std::string resStr;
    google::protobuf::TextFormat::PrintToString(result, &resStr);
    std::cout << "CLIENT RESULT:( " << resStr << " )" << std::endl;
  }

  grpc::Status status = stream->Finish();
  std::cout << "CLIENT FINISHED" << std::endl;
  ASSERT_TRUE(status.ok());
}



// Test to see if our generic methods of communication (client objects, grpcserver impl, etc) are thread safe.
// This does not test application logic for thread safety, just the communications infrastructure (and how we use it.)
// TODO: remove Greeter and add no-op index & query flags
TEST_F(GrpcIndexTest, threadsafe) {
  std::vector<std::thread> threads;

  int nthreads = std::max(2u, std::thread::hardware_concurrency());
  threads.reserve(nthreads);

  for (int i=0; i<nthreads; i++) {
    threads.emplace_back(
            [this,i]{
              std::cout << "STARTED TEST THREAD " << i <<  std::endl;

              std::string name = "Name_" + std::to_string(i) + "_";
              auto namelen = name.size();

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
  solux::proto::UpdateResponse response;

  ureq.mutable_collection()->add_name("main");

  auto& fields = *ureq.add_docs()->mutable_fields();
  fields["text1_w"].set_s("my first field value");
  fields["text2_w"].set_s("my second field value");

  auto& fields2 = *ureq.add_docs()->mutable_fields();
  fields2["text1_w"].set_s("my first field value of 2nd doc");
  fields2["text1_w"].set_s("my second field value of 2nd doc");
  fields2["text2_w"].set_s("third field of 2nd doc");

  std::string reqStr;
  google::protobuf::TextFormat::PrintToString(ureq, &reqStr);
  std::cout << "CLIENT REQ:( " << reqStr << " )" << std::endl;

  grpc::ClientContext context;
  grpc::Status status = indexerStub->Update(&context, ureq , &response);

  if (!status.ok()) {
    std::cout << "grpc call failed!: " << status.error_code() << " " << status.error_message() << std::endl;
  } else {
    std::cout << "I got id:" << response.responses(0).request_id() << std::endl;
  }

}

TEST_F(GrpcIndexTest, addDocsStream) {
  // Setup request
  solux::proto::UpdateRequest req;
  solux::proto::UpdateResponse response;
  grpc::ClientContext context;  // need a new one for each RPC

  req.mutable_collection()->add_name("main");

  auto& fields = *req.add_docs()->mutable_fields();
  fields["text1_w"].set_s("x1");
  fields["text2_w"].set_s("x2");


  std::string reqStr;
  google::protobuf::TextFormat::PrintToString(req, &reqStr);
  std::cout << "CLIENT REQ:( " << reqStr << " )" << std::endl;

  std::unique_ptr<grpc::ClientReaderWriter<solux::proto::UpdateRequest, solux::proto::UpdateResponse>> stream = indexerStub->UpdateStream(&context);
  bool wrote = stream->Write(req);
  ASSERT_TRUE(wrote);

  solux::proto::UpdateRequest req2;
  auto& fields2 = *req2.add_docs()->mutable_fields();
  fields2["text1_w"].set_s("x3");
  fields2["text2_w"].set_s("x4");
  fields2["text3_w"].set_s("x5");

  wrote = stream->Write(req2);
  ASSERT_TRUE(wrote);

  bool ok = stream->WritesDone();  // can replace with WriteLast? is it more efficient?
  ASSERT_TRUE(ok);

  while (stream->Read(&response)) {
    std::string resStr;
    google::protobuf::TextFormat::PrintToString(response, &resStr);
    std::cout << "CLIENT RESULT:( " << resStr << " )" << std::endl;
  }

  grpc::Status status = stream->Finish();
  std::cout << "STREAMING UPDATE CLIENT FINISHED" << std::endl;
  ASSERT_TRUE(status.ok());
}

