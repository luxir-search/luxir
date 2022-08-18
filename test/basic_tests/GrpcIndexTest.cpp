
#include <iostream>
#include <gtest/gtest.h>
#include <google/protobuf/text_format.h>
#include "tbb/task_group.h"
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

  void doSingleUpdate() {
    solux::proto::UpdateRequest ureq;
    solux::proto::UpdateResponse response;
    grpc::ClientContext context;

    ureq.mutable_collection()->add_name("main");

    auto& fields = *ureq.add_docs()->mutable_fields();
    fields["text1_w"].set_s("a value");
    fields["int1_i"].set_i(42);

    grpc::Status status = indexerStub->Update(&context, ureq , &response);

    ASSERT_TRUE(status.ok());
  }

  // How we do things here in the client isn't actually ok depending on how the server is implemented and could
  // lead to deadlock if we are insisting on writing more messages and the server is waiting for us to read more.
  // Ideally, a separate thread is used for reading the responses.  This should also increase efficiency/throughput.
  void doStreamingUpdates(Rng& r, int nMessages) {
    solux::proto::UpdateRequest req;
    solux::proto::UpdateResponse response;
    grpc::ClientContext context;  // need a new one for each RPC

    std::unique_ptr<grpc::ClientReaderWriter<solux::proto::UpdateRequest, solux::proto::UpdateResponse>> stream = indexerStub->UpdateStream(&context);

    int nWrites=0;
    int nReads=0;

    for(;;) {
      bool doWrite = nWrites < nMessages;
      bool doRead = nReads < nWrites;

      if (!doWrite && !doRead) {
        // we are done
        break;
      }

      // If we could do a read or a write, randomly select which one
      if (doRead && doWrite) {
        if (r.rbool()) {
          doWrite = false;
        } else {
          doRead = false;
        }
      }

      if (doWrite) {
        nWrites++;

        solux::proto::UpdateRequest req;
        req.mutable_collection()->add_name("main");

        auto& fields = *req.add_docs()->mutable_fields();
        fields["text1_w"].set_s("val1");
        fields["text2_w"].set_s("val2");
        fields["int1_i"].set_i(42);

        /* dump message
        std::string reqStr;
        google::protobuf::TextFormat::PrintToString(req, &reqStr);
        std::cout << "CLIENT REQ:( " << reqStr << " )" << std::endl;
         */

        // on last message, randomly use WriteLast or WritesDone
        if (nWrites == nMessages && r.rbool()) {
           stream->WriteLast(req, grpc::WriteOptions());
           // hmmm, return type of WriteLast is void
        } else {
          bool wrote = stream->Write(req);
          ASSERT_TRUE(wrote);

          // I could also test delaying this call (doing a read inbetween sometimes)
          if (nWrites == nMessages) {
            bool ok = stream->WritesDone();
            ASSERT_TRUE(ok);
          }
        }
      }

      if (doRead) {
        nReads++;
        bool read = stream->Read(&response);
        ASSERT_TRUE(read);
        /*
        std::string resStr;
        google::protobuf::TextFormat::PrintToString(response, &resStr);
        std::cout << "CLIENT RESULT:( " << resStr << " )" << std::endl;
        */
      }

    } // end for(;;)
  }

};


TEST_F(GrpcIndexTest, streamingHello) {
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
  int nTasks = 100; // concurrency will be limited by TBB
  int callsPerTask = 10;
  tbb::task_group tasks;


  for (int i=0; i<nTasks; i++) {
    tasks.run(
            [=,this]{
              // std::cout << "STARTED TEST THREAD " << i <<  " worker=" << exec.this_worker_id() << std::endl;

              std::string name = "Name_" + std::to_string(i) + "_";
              auto namelen = name.size();

              for (int j=0; j<callsPerTask; j++) {
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

  tasks.wait();
}

//
// With the first simplistic multi-threading support in IndexWriter (just a single Inverter protected by a mutex)
// this test quickly crashed after the mutex was removed.  When investigating thread safety and indexing, consider
// ramping up callsPerTask to hammer things for longer.
//
TEST_F(GrpcIndexTest, threadsafeIndex) {
  int nTasks = 32; // concurrency will be limited by TBB
  int callsPerTask = 10;
  int streamingPercent = 20;  // percent of the requests that use streaming, lower than 50% since streaming
                              // requests will often consist of a number of update messages.
  tbb::task_group tasks;

  for (int i=0; i<nTasks; i++) {
    tasks.run(
            [=,this]{
              Rng r(rng_seed + i);

              // std::cout << "STARTED TEST THREAD " << i <<  " worker=" << exec.this_worker_id() << std::endl;

              int nCalls = 0;

              while (nCalls < callsPerTask) {
                if (r.rint(0,100) < streamingPercent) {
                  int left = callsPerTask-nCalls;
                  int nStream = left==1 ? 1 : r.rint(left) + 1;
                  doStreamingUpdates(r, nStream);
                  nCalls += nStream;
                } else {
                  doSingleUpdate();
                  nCalls++;
                }
              }
            }
    );
  }

  tasks.wait();
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
  req2.mutable_collection()->add_name("main");  // TODO: allow this to not be set if same as last message!

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


TEST_F(GrpcIndexTest, addDocsStream2) {
  doStreamingUpdates(rng, 10);
}