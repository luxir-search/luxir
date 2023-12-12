
#include <iostream>
#include <gtest/gtest.h>
#include <google/protobuf/text_format.h>
#include "oneapi/tbb/task_group.h"
#include "test/SoluxTest.h"
#include "solux/server/GRPCServer.h"

// TODO - use a different logger for RPC stuff some point
// redefine DEBUG to TRACE level which shouldn't currently be logged!
#define GRPC_DEBUG LOG_TRACE
// #define GRPC_DEBUG LOG_DEBUG

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

      // if writes are far enough ahead of reads, do a read regardless of the random choice above
      if (nWrites - nReads > 10) {
        doRead = true;
        doWrite = false;
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


  // This version of streaming updates uses a separate reader thread to avoid deadlock that can happen above if we insist on
  // writing more messages and the server is waiting for us to read more.
  void doStreamingUpdates2(Rng& r, int nMessages) {
    solux::proto::UpdateRequest req;
    solux::proto::UpdateResponse response;
    grpc::ClientContext context;  // need a new one for each RPC

    std::unique_ptr<grpc::ClientReaderWriter<solux::proto::UpdateRequest, solux::proto::UpdateResponse>> stream = indexerStub->UpdateStream(&context);

    int nWrites=0;
    int nReads=0;

    oneapi::tbb::task_group tg;
    tg.run(
            [&] {
              while (stream->Read(&response)) {
                nReads++;
                if (nReads == nWrites) break;
                /*
                std::string resStr;
                google::protobuf::TextFormat::PrintToString(response, &resStr);
                std::cout << "CLIENT RESULT:( " << resStr << " )" << std::endl;
                 */
              }
            });

    do {
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
    } while (nWrites < nMessages);

    tg.wait();
    EXPECT_EQ(nWrites, nReads);
  }

  void doThreadSafeIndex(int nTasks, int callsPerTask, int streamingPercent) {

    tbb::task_group tasks;

    for (int i=0; i<nTasks; i++) {
      tasks.run(
              [=,this]{

                Rng r(rng_seed + i);

                // std::cout << "STARTED TEST THREAD " << i <<  " worker=" << exec.this_worker_id() << std::endl;

                int nCalls = 0;

                while (nCalls < callsPerTask) {
                  if (r.rint(0, 100) < streamingPercent) {
                    int left = callsPerTask - nCalls;
                    int nStream = left == 1 ? 1 : r.rint(left) + 1;
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

  void doThreadSafeIndex2(int nTasks, int callsPerTask, int streamingPercent) {

    std::unique_ptr<std::thread> threads[nTasks];

    for (int i=0; i<nTasks; i++) {
      threads[i] = std::make_unique<std::thread>(
              [=,this]{

                Rng r(rng_seed + i);

                // std::cout << "STARTED TEST THREAD " << i <<  " worker=" << exec.this_worker_id() << std::endl;

                int nCalls = 0;

                while (nCalls < callsPerTask) {
                  if (r.rint(0, 100) < streamingPercent) {
                    int left = callsPerTask - nCalls;
                    int nStream = left == 1 ? 1 : r.rint(left) + 1;
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

    // wait for all the threads
    for (int i=0; i<nTasks; i++) {
      threads[i]->join();
    }
  }


};


TEST_F(GrpcIndexTest, streamingHello) {
  solux::HelloRequest req;
  solux::HelloReply result;
  grpc::ClientContext context;  // need a new one for each RPC

  // The grpc write and read interfaces are specified to be thread-safe with respect to each other, which should mean
  // that we can have a separate thread reading responses while the main thread is writing requests.
  std::unique_ptr<grpc::ClientReaderWriter<HelloRequest,HelloReply>> stream = greeterStub->SayHelloStreaming(&context);

  req.set_async(false);
  req.set_min_sleep_us(1);
  req.set_max_sleep_us(100);

  int numRequests = 0;
  req.set_name("A");
  bool wrote = stream->Write(req);
  ASSERT_TRUE(wrote);
  numRequests++;

  req.set_name("B");
  wrote = stream->Write(req);
  ASSERT_TRUE(wrote);
  numRequests++;

  req.set_name("C");
  req.set_response_count(2);
  wrote = stream->Write(req);
  ASSERT_TRUE(wrote);
  numRequests += 2;

  bool ok1 = stream->WritesDone();  // can replace with WriteLast? is it more efficient?
  ASSERT_TRUE(ok1);

  int numResponses = 0;
  while (stream->Read(&result)) {
    numResponses++;
    std::string resStr;
    google::protobuf::TextFormat::PrintToString(result, &resStr);
    GRPC_DEBUG("CLIENT RESULT:( {} )", resStr);
  }

  ASSERT_EQ(numRequests, numResponses);

  grpc::Status status = stream->Finish();
  GRPC_DEBUG("CLIENT FINISHED");

  ASSERT_TRUE(status.ok());
}


// Single streaming request with many requests + multiple responses per request over that stream.
// Commenting out the lock guard in BiStreamingRequest::respond() should cause this test to fail sometimes.
TEST_F(GrpcIndexTest, streamingHello2) {
  Rng r = SoluxTest::rng;

  solux::HelloRequest req;
  solux::HelloReply result;
  grpc::ClientContext context;  // need a new one for each RPC

  std::unique_ptr<grpc::ClientReaderWriter<HelloRequest,HelloReply>> stream = greeterStub->SayHelloStreaming(&context);

  oneapi::tbb::task_group tasks;

  const int64_t numRequests = 100;
  int64_t numResponsesExpected = 0;
  int64_t numResponses = 0;

  tasks.run(
          [&] {
            while (stream->Read(&result)) {
              numResponses++;
              /*
              std::string resStr;
              google::protobuf::TextFormat::PrintToString(result, &resStr);
              GRPC_DEBUG("CLIENT RESULT:( {} )", resStr);
               */
            }
          });

  int sleepMin = 1;
  int sleepMax = 20;
  int maxResponsesPerRequest = 4;

  req.set_name("A");
  for (int i=0; i<numRequests; i++) {
    req.set_async(true);
    if (r.rbool()) {
      req.set_min_sleep_us(sleepMin);
      req.set_max_sleep_us(sleepMax);
    } else {
      req.set_min_sleep_us(0);
      req.set_max_sleep_us(0);
    }
    req.set_min_sleep_us(sleepMin);
    req.set_max_sleep_us(sleepMax);
    int responseCount = r.rint(maxResponsesPerRequest) + 1;
    req.set_response_count(responseCount);
    numResponsesExpected += responseCount;

    bool wrote = stream->Write(req);
    ASSERT_TRUE(wrote);
  }

  bool ok = stream->WritesDone();
  ASSERT_TRUE(ok);

  // Wait to read all responses.  How to do a timeout if one never comes?
  tasks.wait();

  ASSERT_EQ(numResponsesExpected, numResponses);

  grpc::Status status = stream->Finish();
  GRPC_DEBUG("CLIENT FINISHED");

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
                assert(status.ok());

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
  int nTasks = 100;
  int callsPerTask = 2000;
  int streamingPercent = 0;  // percent of the requests that use streaming

  // doThreadSafeIndex(nTasks, callsPerTask, streamingPercent);

  // too many concurrent requests here will cause deadlock.  It may just be because
  // we do task_group.wait() on the client side, and that can perhaps steal work from our server side?
  // Need a separate process to test higher concurrency levels.
  tbb::task_arena arena(std::thread::hardware_concurrency()/2);
  arena.execute(
          [&,this]{
            // TODO: not sure if this isolate does anything useful here or not.
            tbb::this_task_arena::isolate(
                    [&,this]{
                      doThreadSafeIndex(nTasks, callsPerTask, streamingPercent);
                    }
            );
          }
  );

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
  GRPC_DEBUG("CLIENT REQ:( {} )", reqStr);

  grpc::ClientContext context;
  grpc::Status status = indexerStub->Update(&context, ureq , &response);

  if (!status.ok()) {
    LOG_ERROR("grpc call failed!: code={} msg={}", (int)status.error_code(), status.error_message());
  } else {
    GRPC_DEBUG("I got id:{}", response.responses(0).request_id());
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
  GRPC_DEBUG("CLIENT REQ:( {} )", reqStr);

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
    GRPC_DEBUG("CLIENT RESULT:( {} )", resStr);
  }

  grpc::Status status = stream->Finish();
  GRPC_DEBUG("STREAMING UPDATE CLIENT FINISHED");
  ASSERT_TRUE(status.ok());
}


TEST_F(GrpcIndexTest, addDocsStream2) {
  doStreamingUpdates(rng, 10);
}