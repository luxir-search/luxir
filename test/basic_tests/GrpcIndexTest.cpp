
#include <iostream>
#include <gtest/gtest.h>
#include <google/protobuf/text_format.h>
#include "oneapi/tbb/task_group.h"
#include "test/SoluxTest.h"
#include "solux/server/GRPCServer.h"
#include "test/CollectionHelper.h"

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
  std::unique_ptr<solux::Searcher::Stub> searchStub;

  GrpcIndexTest() {
    // channels are thread safe
    // TODO: move channel to somewhere that multiple tests can use it
    channel = grpc::CreateChannel("localhost:50051", grpc::InsecureChannelCredentials());
    greeterStub = solux::Greeter::NewStub(channel);
    indexerStub = solux::Indexer::NewStub(channel);
    searchStub = solux::Searcher::NewStub(channel);
  }

  constexpr static std::array<const char*, 4> retrieveFields = {"id", "id_i", "i256_50_i", "s3_256_50_ss"};

  // fill in the protobuf for a document based on the document number in a completely deterministic way
  void fillDoc(int64_t docnum, solux::proto::Map& doc) {
    Rng r(rng_seed + docnum);
    doc.clear_fields();
    auto sid = std::to_string(docnum);
    auto& fields = *doc.mutable_fields();
    fields["id"].set_s(sid);
    fields["id_i"].set_i(docnum);
    if (r.rbool()) {
      fields["i256_50_i"].set_i(r() & 0xff);  // 50% of the time as a value between 0 and 255
    }
    if (r.rbool()) {
      size_t n = r.rint(1,4); // 1-3 values
      // up to 3 values with 256 unique values 50% of the time.
      auto& arr = *fields["s3_256_50_ss"].mutable_arr_s()->mutable_v();
      arr.Reserve(n);
      // for sorted-set, we don't want repeated values.
      int curr = 0;
      for (size_t i=0; i<n; i++) {
        curr += r.rint(0, 255/3);
        std::string v;
        v += '0'+i;  // this makes sure the terms will be sorted.  Not a requirement for indexing, but for testing (without sorting).
        v += std::to_string(curr);
        arr.Add(std::move(v));
      }
      if (n == 2) {
        std::swap(arr[0], arr[1]); // mix it up some, just to test that indexing order doesn't matter.
      }
    }
    fields["t_w"].set_s(std::format("{} {}", sid, "common"));  // unique term + common term
    fields["t2_w"].set_s(std::format("{} {} {}", r.rint(0,10), r.rint(0,100), r.rint(0,1000)));
  }

  //         verifyDoc(startDoc + nReads, docList.columns());
  void verifyDoc(int64_t docnum, const ::google::protobuf::Map<std::string, ::solux::proto::Column>& fields, int col=0) {
    // if docnum wasn't passed in, get it from the column.
    if (docnum == -1) {
      auto& id_i = fields.at("id_i");
      docnum = id_i.col_i().v(col);
    }  else {
      auto& id_i = fields.at("id_i");
      ASSERT_EQ(docnum, id_i.col_i().v(col));
    }

    Rng r(rng_seed + docnum);
    auto sid = std::to_string(docnum);
    auto& id = fields.at("id");
    ASSERT_EQ(sid, id.col_s().v(col));

    auto has_i256_50_i = r.rbool();
    if (fields.contains("i256_50_i")) {
      auto& i256_50_i = fields.at("i256_50_i");
      if (has_i256_50_i) {
        ASSERT_EQ(r() & 0xff, i256_50_i.col_i().v(col));
      } else {
        auto missingVal = std::numeric_limits<int64_t>::min();
        ASSERT_EQ(missingVal, i256_50_i.col_i().v(col));
      }
    } else {
      assert(!has_i256_50_i);
    }

    auto has_s3_256_50ss = r.rbool();
    if (fields.contains("s3_256_50_ss")) {
      size_t n = has_s3_256_50ss ? r.rint(1,4) : 0; // expected number of values

      auto& s3_256_50ss = fields.at("s3_256_50_ss");
      auto& arr = s3_256_50ss.multi_s().v(col).v();
      ASSERT_EQ(n, arr.size());
      int curr = 0;
      for (size_t i=0; i<n; i++) {
        curr += r.rint(0, 255/3);
        std::string v;
        v += '0'+i;  // this makes sure the terms will be sorted.  Not a requirement for indexing, but for testing (without sorting).
        v += std::to_string(curr);
        ASSERT_EQ(v, arr[i]);
      }
    } else {
      assert(!has_s3_256_50ss);
    }

  }


  void doSingleUpdate(bool commit, int64_t docnum=0) {
    solux::proto::UpdateRequest ureq;
    solux::proto::UpdateResponse response;
    grpc::ClientContext context;

    ureq.mutable_collection()->add_name("main");
    ureq.set_commit(commit ? solux::proto::UpdateRequest::COMMIT : solux::proto::UpdateRequest::NO_COMMIT);

    fillDoc(docnum, *ureq.add_docs());

    grpc::Status status = indexerStub->Update(&context, ureq , &response);

    ASSERT_TRUE(status.ok());
  }

  int64_t getDocCount() {
    // Read Stream
    grpc::ClientContext rcontext;  // need a new one for each RPC
    std::unique_ptr<grpc::ClientReaderWriter<solux::proto::SearchRequest, solux::proto::SearchResponse>> rstream = searchStub->Search(&rcontext);

    solux::proto::SearchRequest sreq;
    sreq.mutable_collection()->add_name("main");
    auto& ops = *sreq.mutable_ops();
    ops["q"].mutable_top_docs()->mutable_query()->set_all(true);
    ops["q"].mutable_top_docs()->set_get_number(true);

    bool wrote = rstream->Write(sreq);
    EXPECT_TRUE(wrote);

    solux::proto::SearchResponse sresponse;
    bool read = rstream->Read(&sresponse);
    EXPECT_TRUE(read);
    return sresponse.ops().at("q").docs().matches();
  }

  // How we do things here in the client isn't actually ok depending on how the server is implemented and could
  // lead to deadlock if we are insisting on writing more messages and the server is waiting for us to read more.
  // Ideally, a separate thread is used for reading the responses.  This should also increase efficiency/throughput.
  void doStreamingUpdates(Rng& r, int64_t nMessages, int commitPercent, int64_t docnum=-1) {
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

        solux::proto::UpdateRequest req;
        req.mutable_collection()->add_name("main");
        if (r.rint(0, 100) < commitPercent) {
          req.set_commit(solux::proto::UpdateRequest::COMMIT);
        }

        fillDoc(docnum + nWrites, *req.add_docs());
        nWrites++;

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


  void doThreadSafeIndex(int nThreads, int64_t nDocs, int streamingPercent, int commitPercent) {

    // we use threads here instead of tasks because there was an issue with task_group::wait
    // stealing work that somehow led to a deadlock.
    std::vector<std::unique_ptr<std::thread>> threads(nThreads);

    std::atomic_int64_t docsIndexed = 0;

    for (int i=0; i<nThreads; i++) {
      threads[i] = std::make_unique<std::thread>(
              [=,&docsIndexed,this]{
                Rng r(rng_seed + i);
                // std::cout << "STARTED TEST THREAD " << i <<  " worker=" << exec.this_worker_id() << std::endl;
                for (;;) {
                  bool streaming = r.rint(0, 100) < streamingPercent;
                  // first, reserve our document numbers so we know exactly what will be indexed
                  int64_t sz = streaming ? r.rint(1, 10) : 1;
                  int64_t docnumStart = 0;
                  do {
                    int64_t docnumStart = docsIndexed.load(std::memory_order_relaxed);
                    if (docnumStart + sz >= nDocs) {
                      sz = nDocs - docnumStart;
                      if (sz <= 0) {
                        return;
                      }
                    }
                  } while (!docsIndexed.compare_exchange_weak(docnumStart, docnumStart + sz, std::memory_order_relaxed));

                  if (streaming) {
                    doStreamingUpdates(r, sz, commitPercent, docnumStart);
                  } else {
                    ASSERT_EQ(sz, 1);
                    doSingleUpdate(r.rint(100) < commitPercent, docnumStart);
                  }
                }
              }
      );
    }

    // wait for all the threads
    for (int i=0; i<nThreads; i++) {
      threads[i]->join();
    }

    // do a final commit
    solux::test::CollectionHelper ch("main");
    ch.commit();
  }

  using RequestCreator = std::function<void(int64_t docid, solux::proto::SearchRequest&)>;
  using ResponseChecker = std::function<void(int64_t docid, const solux::proto::SearchResponse&)>;

  void doStreamingSearches(Rng& r, int64_t startDoc, int64_t nMessages, int64_t nDocs, RequestCreator& reqCreator, ResponseChecker& respChecker) {
    solux::proto::SearchRequest req;
    solux::proto::SearchResponse response;
    grpc::ClientContext context;  // need a new one for each RPC

    std::unique_ptr<grpc::ClientReaderWriter<solux::proto::SearchRequest, solux::proto::SearchResponse>> stream = searchStub->Search(&context);

    int nWrites=0;
    int nReads=0;
    int additionalReads = 0;

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

      // if writes are far enough ahead of reads, do a read regardless of the random choice above.
      // If the server side implements throttling, we may need to reduce the number of outstanding here.
      if (nWrites - nReads > 10) {
      // if (nWrites - nReads > 0) {  // do completely single-threaded to see if basic code is working
        doRead = true;
        doWrite = false;
      }

      if (doWrite) {
        solux::proto::SearchRequest req;
        auto docId = (startDoc + nWrites) % nDocs;
        reqCreator(docId, req);
        nWrites++;

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

      while (doRead) {
        bool read = stream->Read(&response);
        ASSERT_TRUE(read);
        auto docId = (startDoc + nReads) % nDocs;
        respChecker(docId, response);
        if (response.more()) {
          additionalReads++;
          if (r.rint(0, 100) < 25) {  // 75% of the time, do another read if there are more
            break;
          }
        } else {
          // only increment nReads on final responses
          nReads++;
          break;
        }
      }

    } // end for(;;)
    unused(additionalReads);
  }


  void doThreadSafeSearch(int nThreads, int64_t nQueries, int64_t nDocs, RequestCreator& reqCreator, ResponseChecker& respChecker) {
    // we use threads here instead of tasks because there was an issue with task_group::wait
    // stealing work that somehow led to a deadlock.
    std::vector<std::unique_ptr<std::thread>> threads(nThreads);

    std::atomic_int64_t queries = 0;

    for (int i=0; i<nThreads; i++) {
      threads[i] = std::make_unique<std::thread>(
              [&,i,this]{
                Rng r(rng_seed + i);
                // std::cout << "STARTED TEST THREAD " << i <<  " worker=" << exec.this_worker_id() << std::endl;
                for (;;) {
                  auto sz = r.rint(1, 20);  // number of queries in the streaming search

                  auto qnum = queries.fetch_add(sz, std::memory_order_relaxed);
                  if (qnum >= nQueries) break;
                  doStreamingSearches(r, qnum, sz, nDocs, reqCreator, respChecker);
                }
              }
      );
    }

    // wait for all the threads
    for (int i=0; i<nThreads; i++) {
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
  int nTasks = 50; // concurrency will be limited by TBB
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
// Stress test multi-threaded indexing and searching.
//
TEST_F(GrpcIndexTest, threadsafeIndex) {
  int nThreads = 32;
  int64_t nDocs = 100;  // pump this up for good stress testing.
  int streamingPercent = 50;  // percent of the requests that use streaming
  int commitPercent = 10;
  uint32_t percentFacet = 50; // percent of the requests that use facets

  // clear the index
  solux::test::CollectionHelper ch("main");
  ch.clear();

  doThreadSafeIndex(nThreads, nDocs, streamingPercent, commitPercent);

  RequestCreator requestCreator = [&](int64_t docid, solux::proto::SearchRequest& req) {
    req.mutable_collection()->add_name("main");
    // try to retrieve the document we just indexed
    auto& topDocs = *(*req.mutable_ops())["q"].mutable_top_docs();
    topDocs.set_get_number(true);
    for (auto& field : retrieveFields) {
      topDocs.mutable_fields()->Add(field);
    }
    auto& topQuery = *topDocs.mutable_query()->mutable_match();
    topQuery.set_field("id");
    topQuery.mutable_val()->set_s(std::to_string(docid));
    req.set_request_id(std::to_string(docid));  // set request id to the id so we know what doc we are looking for

#ifdef REMOVED
    // FIXME
    // Sometimes add integer facet request for id_i
    if (SplitMix64(docid)() % 100 < percentFacet) {
      auto& facet = *(*req.mutable_ops())["f"].mutable_field_facet();
      facet.set_field("id_i");
      facet.set_limit(10);
    }
#endif
  };

  ResponseChecker responseChecker = [&](int64_t docid, const solux::proto::SearchResponse& response) {
    auto& docList = response.ops().at("q").docs();
    ASSERT_EQ(1, docList.matches());  // FIXME!  this comes up as "2" now sometimes with nDocs=100????
    ASSERT_EQ(docList.columns_size(), retrieveFields.size()); // this might change in the future.
    verifyDoc(docid, docList.columns());

#ifdef REMOVED
    // FIXME
    // do same calculation to see if facet was requested
    if (SplitMix64(docid)() % 100 < percentFacet) {
      // verify the facet response.  It should be a single bucket with id_i=docid and count=1
      auto& facetResult = response.ops().at("f").facet();
      ASSERT_EQ(1, facetResult.bucket_ids().col_i().v().size());
      ASSERT_EQ(docid, facetResult.bucket_ids().col_i().v()[0]);
      ASSERT_EQ(1, facetResult.counts().size());
      ASSERT_EQ(1, facetResult.counts()[0]);
    }
#endif
  };

  doThreadSafeSearch(nThreads, nDocs, nDocs, requestCreator, responseChecker);

  // Now let's do a test designed to uncover non-thread-safety of the codecs in SIMDCompressionLib
  // unpacking blocks of docids, frequencies, or positions concurrently should do it.

  RequestCreator reqc2 = [&](int64_t docid, solux::proto::SearchRequest& req) {
    req.mutable_collection()->add_name("main");
    auto& topDocs = *(*req.mutable_ops())["q"].mutable_top_docs();
    topDocs.set_get_number(true);
    topDocs.set_limit(1);
    auto& topQuery = *topDocs.mutable_query()->mutable_match();
    // the field t2_w contains integers from 1-10, 1-100, and 1-1000.  So if we search for
    // something like "0" it should match > 11% of the docs and use block compression in the codec
    // provided there are enough docs in the index (otherwise tail compression is used).
    topQuery.set_field("t2_w");
    topQuery.mutable_val()->set_s(std::to_string(docid % 10));
    req.set_request_id(std::to_string(docid));
  };

  // record the number of hits per query in a boost flat unordered map
  boost::unordered::unordered_flat_map<int64_t, int64_t> hits;

  ResponseChecker respc2 = [&](int64_t docid, const solux::proto::SearchResponse& response) {
    auto& docList = response.ops().at("q").docs();
    // not too much to check here... just record the hits we got
    hits[docid % 10] = docList.matches();
  };

  // this pass records the number of hits per t2_w:[0 - 10]
  doThreadSafeSearch(1, 10, nDocs, reqc2, respc2);

  // now we can check the hits to see if we got the expected number of hits for each t2_w:[0 - 10]
  ResponseChecker respc2verify = [&](int64_t docid, const solux::proto::SearchResponse& response) {
    auto& docList = response.ops().at("q").docs();
    ASSERT_EQ(hits[docid % 10], docList.matches());
  };

  // With 10K docs and 32 threads, this reliably fails when using the non-thread-safe SIMDCompressionLib codecs.
  // 1000 docs is enough to get it to fail sometimes, often with ASAN detecting a double-free in SIMDCompressionLib.
  // Failures were fixed by using thread_locals for those specific codecs.
  doThreadSafeSearch(nThreads, nDocs, nDocs, reqc2, respc2verify);

  //
  // Now test streaming back multiple responses per request
  //
  int64_t limit=50;
  int32_t batchSize = 5;
  RequestCreator reqc3 = [&](int64_t docid, solux::proto::SearchRequest& req) {
    req.mutable_collection()->add_name("main");
    auto& topDocs = *(*req.mutable_ops())["q"].mutable_top_docs();
    for (auto& field : retrieveFields) {
      topDocs.mutable_fields()->Add(field);
    }
    topDocs.set_get_number(true);
    topDocs.set_limit(limit);
    topDocs.set_batch_size(batchSize);
    auto& topQuery = *topDocs.mutable_query()->mutable_match();
    topQuery.set_field("t2_w");
    topQuery.mutable_val()->set_s(std::to_string(docid % 10));
    req.set_request_id(std::to_string(docid));
  };

  ResponseChecker respc3 = [&](int64_t docid, const solux::proto::SearchResponse& response) {
    unused(docid);
    auto& docList = response.ops().at("q").docs();
    // because responses are streaming and not necessarily in order across different logical requests,
    // we need to get the number used to generate the query from the request id
    long long reqid;
    std::from_chars(response.request_id().data(), response.request_id().data() + response.request_id().size(), reqid);

    ASSERT_EQ(hits[reqid % 10], docList.matches());
    if (docList.matches() == 0) return;

    auto max = std::min(limit, (int64_t)docList.matches());
    auto expectedColSize = response.more() ? batchSize : max % batchSize;
    if (expectedColSize == 0) expectedColSize = batchSize;

    // check the id field
    if (docList.columns().at("id").col_s().v_size() != expectedColSize) {
      // print out the whole message
      std::string resStr;
      google::protobuf::TextFormat::PrintToString(response, &resStr);
      LOG_ERROR("CLIENT RESULT:( {} )", resStr);
    }
    ASSERT_EQ(expectedColSize, docList.columns().at("id").col_s().v_size());

    for (int i=0; i<expectedColSize; i++) {
      verifyDoc(-1, docList.columns(), i);
    }
  };

  doThreadSafeSearch(nThreads, nDocs, nDocs, reqc3, respc3);
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
  doStreamingUpdates(rng, 10,50);
}

TEST_F(GrpcIndexTest, visibility) {
   clearCollection();
   ASSERT_EQ(0, getDocCount());

  // Write Stream
  grpc::ClientContext wcontext;  // need a new one for each RPC
  std::unique_ptr<grpc::ClientReaderWriter<solux::proto::UpdateRequest, solux::proto::UpdateResponse>> wstream = indexerStub->UpdateStream(&wcontext);

  {
    solux::proto::UpdateRequest req;
    solux::proto::UpdateResponse response;

    req.mutable_collection()->add_name("main");

    auto& fields = *req.add_docs()->mutable_fields();
    fields["text1_w"].set_s("x1");
    fields["text2_w"].set_s("x2");

    std::string reqStr;
    google::protobuf::TextFormat::PrintToString(req, &reqStr);
            GRPC_DEBUG("CLIENT REQ:( {} )", reqStr);

    bool wrote = wstream->Write(req);
    ASSERT_TRUE(wrote);

    // wait for the write response
    bool read = wstream->Read(&response);
    ASSERT_TRUE(read);
  }

  ASSERT_EQ(0, getDocCount());

  {
    solux::proto::UpdateRequest req;
    solux::proto::UpdateResponse response;

    req.mutable_collection()->add_name("main");
    req.set_commit(solux::proto::UpdateRequest::COMMIT);

    auto& fields = *req.add_docs()->mutable_fields();
    fields["text1_w"].set_s("x3");
    fields["text2_w"].set_s("x4");

    std::string reqStr;
    google::protobuf::TextFormat::PrintToString(req, &reqStr);
    GRPC_DEBUG("CLIENT REQ:( {} )", reqStr);

    bool wrote = wstream->Write(req);
    ASSERT_TRUE(wrote);

    // wait for the write response
    bool read = wstream->Read(&response);
    ASSERT_TRUE(read);
  }

  ASSERT_EQ(2, getDocCount());

  bool ok = wstream->WritesDone();
  ASSERT_TRUE(ok);

  solux::proto::UpdateResponse response;
  while (wstream->Read(&response)) {
    std::string resStr;
    google::protobuf::TextFormat::PrintToString(response, &resStr);
    GRPC_DEBUG("CLIENT RESULT:( {} )", resStr);
  }

  grpc::Status status = wstream->Finish();
  GRPC_DEBUG("STREAMING UPDATE CLIENT FINISHED");
  ASSERT_TRUE(status.ok());
}
