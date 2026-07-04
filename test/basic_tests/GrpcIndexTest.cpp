
#include <array>
#include <atomic>
#include <charconv>
#include <cstddef>
#include <format>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <variant>
#include <vector>
#include <gtest/gtest.h>
#include "test/GrpcClient.h"
#include "oneapi/tbb/task_group.h"
#include "test/GrpcSoluxTest.h"
#include "test/CollectionHelper.h"
#include "test/LocalReq.h"

// TODO - use a different logger for RPC stuff some point
// redefine DEBUG to TRACE level which shouldn't currently be logged!
#define GRPC_DEBUG LOG_TRACE
// #define GRPC_DEBUG LOG_DEBUG

using namespace solux;
using namespace solux::test;  // HppClientReaderWriter, hppUnaryCall, Reply, rpc::

namespace {

static std::string idString(std::string_view id) {  // request_id, now a proto string
  return std::string(id);
}

} // namespace

class GrpcIndexTest : public GrpcSoluxTest {
public:
  std::shared_ptr<grpc::Channel> channel;

  GrpcIndexTest() {
    channel = getChannel();
  }

  constexpr static std::array<const char*, 6> retrieveFields = {"id", "id_i", "i256_50_i", "s3_256_50_ss", "t_w", "t2_w"};

  // Build a test Doc based on the document number in a completely deterministic way.
  Doc makeDoc(int64_t docnum) {
    Rng r(rng_seed + docnum);
    auto sid = std::to_string(docnum);
    Doc doc;
    doc.push_back({"id", sid});
    doc.push_back({"id_i", docnum});
    if (r.rbool()) {
      doc.push_back({"i256_50_i", (int64_t)(r() & 0xff)});  // 50% of the time as a value between 0 and 255
    }
    if (r.rbool()) {
      size_t n = (size_t)r.rint(1,4); // 1-3 values
      // up to 3 values with 256 unique values 50% of the time.
      std::vector<std::string> arr;
      arr.reserve(n);
      // for sorted-set, we don't want repeated values.
      int curr = 0;
      for (size_t i=0; i<n; i++) {
        curr += r.rint(0, 255/3);
        std::string v;
        v += '0'+i;  // this makes sure the terms will be sorted.  Not a requirement for indexing, but for testing (without sorting).
        v += std::to_string(curr);
        arr.emplace_back(std::move(v));
      }
      if (n == 2) {
        std::swap(arr[0], arr[1]); // mix it up some, just to test that indexing order doesn't matter.
      }
      doc.push_back({"s3_256_50_ss", std::move(arr)});
    }
    doc.push_back({"t_w", std::format("{} {}", sid, "common")});  // unique term + common term
    doc.push_back({"t2_w", std::format("{} {} {}", r.rint(0,10), r.rint(0,100), r.rint(0,1000))});
    return doc;
  }

  //         verifyDoc(startDoc + nReads, docList.columns);
  template <typename FieldMap>
  void verifyDoc(int64_t docnum, const FieldMap& fields, int col=0) {
    size_t row = (size_t)col;
    // if docnum wasn't passed in, get it from the column.
    if (docnum == -1) {
      const auto& id_i = std::get<solux::api::ColInt>(fields.at("id_i").kind);
      docnum = id_i.v[row];
    }  else {
      const auto& id_i = std::get<solux::api::ColInt>(fields.at("id_i").kind);
      ASSERT_EQ(docnum, id_i.v[row]);
    }

    Rng r(rng_seed + docnum);
    auto sid = std::to_string(docnum);
    const auto& id = std::get<solux::api::ColStr>(fields.at("id").kind);
    ASSERT_EQ(sid, id.v[row]);

    auto has_i256_50_i = r.rbool();
    if (fields.contains("i256_50_i")) {
      const auto& i256_50_i = std::get<solux::api::ColInt>(fields.at("i256_50_i").kind);
      if (has_i256_50_i) {
        ASSERT_EQ((int64_t)(r() & 0xff), i256_50_i.v[row]);
      } else {
        // missing slots hold the column's batch-chosen filler
        ASSERT_EQ(i256_50_i.missing_val, i256_50_i.v[row]);
      }
    } else {
      assert(!has_i256_50_i);
    }

    auto has_s3_256_50ss = r.rbool();
    if (fields.contains("s3_256_50_ss")) {
      size_t n = has_s3_256_50ss ? (size_t)r.rint(1,4) : 0; // expected number of values

      const auto& s3_256_50ss = std::get<solux::api::ArrArrStr>(fields.at("s3_256_50_ss").kind);
      const auto& arr = s3_256_50ss.v[row].v;
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

    // t_w and t2_w are TEXT fields, stored by default, returned as col_s.
    if (fields.contains("t_w")) {
      const auto& t_w = std::get<solux::api::ColStr>(fields.at("t_w").kind);
      ASSERT_EQ(std::format("{} {}", sid, "common"), t_w.v[row]);
    }
    if (fields.contains("t2_w")) {
      auto expected = std::format("{} {} {}", r.rint(0,10), r.rint(0,100), r.rint(0,1000));
      const auto& t2_w = std::get<solux::api::ColStr>(fields.at("t2_w").kind);
      ASSERT_EQ(expected, t2_w.v[row]);
    }
  }


  void doSingleUpdate(bool commit, int64_t docnum=0, bool waitForMerges=false) {
    Reply<solux::api::UpdateResponse> response;
    grpc::ClientContext context;

    CollectionHelper::UpdateBuilder b;
    b.collection("main").requestId(std::to_string(docnum));
    if (commit) {
      b.commit(waitForMerges);
    }
    b.add(makeDoc(docnum));

    grpc::Status status = hppUnaryCall(channel.get(), rpc::Update, &context, b.finish(), &response);

    ASSERT_TRUE(status.ok());
    // The unary path fills a caller-supplied response: it must get the same
    // initialization (status, request_id) as an arena-created one.
    ASSERT_EQ(solux::api::UpdateResponse_::Status::OK, response.msg.status);
    ASSERT_EQ(std::to_string(docnum), idString(response.msg.request_id));
    ASSERT_GT(response.msg.update_version, 0u);
  }

  int64_t getDocCount() {
    // Read Stream
    grpc::ClientContext rcontext;  // need a new one for each RPC
    HppClientReaderWriter<solux::api::SearchRequest, solux::api::SearchResponse> rstream(
      channel.get(), rpc::Search, &rcontext);

    // Build the SearchRequest with the LocalReq builder (used only as a builder; we serialize
    // its `proto`, we do not execute locally).
    auto lreq = localReq(soluxNode->getSearchEngine());
    lreq->collection("main").topDocs("q").allQuery().getNumber();

    bool wrote = rstream.Write(lreq->proto);  // lreq stays alive through Write
    EXPECT_TRUE(wrote);

    Reply<solux::api::SearchResponse> sresponse;
    bool read = rstream.Read(&sresponse);
    EXPECT_TRUE(read);
    const auto& docs = std::get<solux::api::DocList>(sresponse.msg.ops.at("q")->kind);
    return docs.matches ? *docs.matches : 0;
  }

  // How we do things here in the client isn't actually ok depending on how the server is implemented and could
  // lead to deadlock if we are insisting on writing more messages and the server is waiting for us to read more.
  // Ideally, a separate thread is used for reading the responses.  This should also increase efficiency/throughput.
  void doStreamingUpdates(Rng& r, int64_t nMessages, int commitPercent, int64_t docnum=-1, int waitForMergesPercent=0) {
    Reply<solux::api::UpdateResponse> response;
    grpc::ClientContext context;  // need a new one for each RPC

    HppClientReaderWriter<solux::api::UpdateRequest, solux::api::UpdateResponse> stream(
      channel.get(), rpc::UpdateStream, &context);

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

        CollectionHelper::UpdateBuilder req;
        req.collection("main");
        if (r.rint(0, 100) < commitPercent) {
          bool waitForMerges = r.rint(0, 100) < waitForMergesPercent;
          req.commit(waitForMerges);
        }

        req.add(makeDoc(docnum + nWrites));
        nWrites++;

        /* dump message
        std::cout << "CLIENT REQ docs=1" << std::endl;
         */

        // on last message, randomly use WriteLast or WritesDone
        if (nWrites == nMessages && r.rbool()) {
           stream.WriteLast(req.finish(), grpc::WriteOptions());
           // hmmm, return type of WriteLast is void
        } else {
          bool wrote = stream.Write(req.finish());
          ASSERT_TRUE(wrote);

          // I could also test delaying this call (doing a read inbetween sometimes)
          if (nWrites == nMessages) {
            bool ok = stream.WritesDone();
            ASSERT_TRUE(ok);
          }
        }
      }

      if (doRead) {
        nReads++;
        bool read = stream.Read(&response);
        ASSERT_TRUE(read);
        /*
        std::cout << "CLIENT RESULT status=" << (int)response.msg.status << std::endl;
        */
      }

    } // end for(;;)
  }


  // This version of streaming updates uses a separate reader thread to avoid deadlock that can happen above if we insist on
  // writing more messages and the server is waiting for us to read more.
  void doStreamingUpdates2(Rng& r, int nMessages) {
    Reply<solux::api::UpdateResponse> response;
    grpc::ClientContext context;  // need a new one for each RPC

    HppClientReaderWriter<solux::api::UpdateRequest, solux::api::UpdateResponse> stream(
      channel.get(), rpc::UpdateStream, &context);

    int nWrites=0;
    int nReads=0;

    oneapi::tbb::task_group tg;
    tg.run(
            [&] {
              while (stream.Read(&response)) {
                nReads++;
                if (nReads == nWrites) break;
                /*
                std::cout << "CLIENT RESULT status=" << (int)response.msg.status << std::endl;
                 */
              }
            });

    do {
        nWrites++;

        CollectionHelper::UpdateBuilder req;
        req.collection("main");
        req.add(flatdoc("text1_w", std::string("val1"), "text2_w", std::string("val2"), "int1_i", (int64_t)42));

        /* dump message
        std::cout << "CLIENT REQ docs=1" << std::endl;
         */

        // on last message, randomly use WriteLast or WritesDone
        if (nWrites == nMessages && r.rbool()) {
          stream.WriteLast(req.finish(), grpc::WriteOptions());
          // hmmm, return type of WriteLast is void
        } else {
          bool wrote = stream.Write(req.finish());
          ASSERT_TRUE(wrote);

          // I could also test delaying this call (doing a read inbetween sometimes)
          if (nWrites == nMessages) {
            bool ok = stream.WritesDone();
            ASSERT_TRUE(ok);
          }
        }
    } while (nWrites < nMessages);

    tg.wait();
    EXPECT_EQ(nWrites, nReads);
  }


  void doThreadSafeIndex(int nThreads, int64_t nDocs, int streamingPercent, int commitPercent, int waitForMergesPercent=0) {

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
                    doStreamingUpdates(r, sz, commitPercent, docnumStart, waitForMergesPercent);
                  } else {
                    ASSERT_EQ(sz, 1);
                    bool commit = r.rint(100) < commitPercent;
                    bool waitForMerges = commit && r.rint(100) < waitForMergesPercent;
                    doSingleUpdate(commit, docnumStart, waitForMerges);
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

  using RequestCreator = std::function<void(int64_t docid, solux::test::LocalReq& req)>;
  using ResponseChecker = std::function<void(int64_t docid, const solux::api::SearchResponse& response)>;

  void doStreamingSearches(Rng& r, int64_t startDoc, int64_t nMessages, int64_t nDocs, RequestCreator& reqCreator, ResponseChecker& respChecker) {
    Reply<solux::api::SearchResponse> response;
    grpc::ClientContext context;  // need a new one for each RPC

    HppClientReaderWriter<solux::api::SearchRequest, solux::api::SearchResponse> stream(
      channel.get(), rpc::Search, &context);

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
        // Build the request with a fresh LocalReq builder; it must live through the Write.
        auto lreq = localReq(soluxNode->getSearchEngine());
        auto docId = (startDoc + nWrites) % nDocs;
        reqCreator(docId, *lreq);
        nWrites++;

        // on last message, randomly use WriteLast or WritesDone
        if (nWrites == nMessages && r.rbool()) {
          stream.WriteLast(lreq->proto, grpc::WriteOptions());
          // hmmm, return type of WriteLast is void
        } else {
          bool wrote = stream.Write(lreq->proto);
          ASSERT_TRUE(wrote);

          // I could also test delaying this call (doing a read inbetween sometimes)
          if (nWrites == nMessages) {
            bool ok = stream.WritesDone();
            ASSERT_TRUE(ok);
          }
        }
      }

      while (doRead) {
        bool read = stream.Read(&response);
        ASSERT_TRUE(read);
        auto docId = (startDoc + nReads) % nDocs;
        respChecker(docId, response.msg);
        if (response.msg.more) {
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
  solux::api::HelloRequest req;
  Reply<solux::api::HelloReply> result;
  grpc::ClientContext context;  // need a new one for each RPC

  // The grpc write and read interfaces are specified to be thread-safe with respect to each other, which should mean
  // that we can have a separate thread reading responses while the main thread is writing requests.
  HppClientReaderWriter<solux::api::HelloRequest, solux::api::HelloReply> stream(
    channel.get(), rpc::SayHelloStreaming, &context);

  req.async = false;
  req.min_sleep_us = 1;
  req.max_sleep_us = 100;

  int numRequests = 0;
  req.name = "A";
  bool wrote = stream.Write(req);
  ASSERT_TRUE(wrote);
  numRequests++;

  req.name = "B";
  wrote = stream.Write(req);
  ASSERT_TRUE(wrote);
  numRequests++;

  req.name = "C";
  req.response_count = 2;
  wrote = stream.Write(req);
  ASSERT_TRUE(wrote);
  numRequests += 2;

  bool ok1 = stream.WritesDone();  // can replace with WriteLast? is it more efficient?
  ASSERT_TRUE(ok1);

  int numResponses = 0;
  while (stream.Read(&result)) {
    numResponses++;
    GRPC_DEBUG("CLIENT RESULT: {}", result.msg.message);
  }

  ASSERT_EQ(numRequests, numResponses);

  grpc::Status status = stream.Finish();
  GRPC_DEBUG("CLIENT FINISHED");

  ASSERT_TRUE(status.ok());
}


// Single streaming request with many requests + multiple responses per request over that stream.
// Commenting out the lock guard in BiStreamingRequest::respond() should cause this test to fail sometimes.
TEST_F(GrpcIndexTest, streamingHello2) {
  Rng r = SoluxTest::rng;

  solux::api::HelloRequest req;
  Reply<solux::api::HelloReply> result;
  grpc::ClientContext context;  // need a new one for each RPC

  HppClientReaderWriter<solux::api::HelloRequest, solux::api::HelloReply> stream(
    channel.get(), rpc::SayHelloStreaming, &context);

  oneapi::tbb::task_group tasks;

  const int64_t numRequests = 100;
  int64_t numResponsesExpected = 0;
  int64_t numResponses = 0;

  tasks.run(
          [&] {
            while (stream.Read(&result)) {
              numResponses++;
              /*
              GRPC_DEBUG("CLIENT RESULT: {}", result.msg.message);
               */
            }
          });

  int sleepMin = 1;
  int sleepMax = 20;
  int maxResponsesPerRequest = 4;

  req.name = "A";
  for (int i=0; i<numRequests; i++) {
    req.async = true;
    if (r.rbool()) {
      req.min_sleep_us = sleepMin;
      req.max_sleep_us = sleepMax;
    } else {
      req.min_sleep_us = 0;
      req.max_sleep_us = 0;
    }
    req.min_sleep_us = sleepMin;
    req.max_sleep_us = sleepMax;
    int responseCount = r.rint(maxResponsesPerRequest) + 1;
    req.response_count = responseCount;
    numResponsesExpected += responseCount;

    bool wrote = stream.Write(req);
    ASSERT_TRUE(wrote);
  }

  bool ok = stream.WritesDone();
  ASSERT_TRUE(ok);

  // Wait to read all responses.  How to do a timeout if one never comes?
  tasks.wait();

  ASSERT_EQ(numResponsesExpected, numResponses);

  grpc::Status status = stream.Finish();
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
                solux::api::HelloRequest req;
                Reply<solux::api::HelloReply> result;
                grpc::ClientContext context;  // need a new one for each RPC


                name.resize(namelen);
                name.append(std::to_string(j)); // TODO: this may still create another string

                req.name = name;

                grpc::Status status;
                if ((i+j)%2 == 0) {
                  status = hppUnaryCall(channel.get(), rpc::SayHello, &context, req, &result);
                } else {
                  status = hppUnaryCall(channel.get(), rpc::SayHello2, &context, req, &result);
                }
                assert(status.ok());

                // std::cout << "Got response " << result.msg.message <<  std::endl;

                ASSERT_TRUE(result.msg.message.ends_with(name));
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
  int waitForMergesPercent = 30;  // of commits, fraction that wait for in-flight merges before publishing
  uint32_t percentFacet = 50; // percent of the requests that use facets

  // clear the index
  solux::test::CollectionHelper ch("main");
  ch.clear();

  doThreadSafeIndex(nThreads, nDocs, streamingPercent, commitPercent, waitForMergesPercent);

  RequestCreator requestCreator = [&](int64_t docid, solux::test::LocalReq& req) {
    req.collection("main").requestId(std::to_string(docid));  // set request id to the id so we know what doc we are looking for
    // try to retrieve the document we just indexed
    auto& q = req.topDocs("q").getNumber().matchQuery("id", std::to_string(docid));
    for (auto& field : retrieveFields) q.fields({std::string(field)});

    // Sometimes add integer facet request for id_i
    if (SplitMix64(docid)() % 100 < percentFacet) {
      q.facet("f", "id_i").limit(10);
    }
  };

  ResponseChecker responseChecker = [&](int64_t docid, const solux::api::SearchResponse& response) {
    const auto& docList = std::get<solux::api::DocList>(response.ops.at("q")->kind);
    ASSERT_TRUE(docList.matches.has_value());
    ASSERT_EQ(1, *docList.matches);  // FIXME!  this comes up as "2" now sometimes with nDocs=100????
    ASSERT_EQ(docList.columns.size(), retrieveFields.size()); // this might change in the future.
    verifyDoc(docid, docList.columns);

    // do same calculation to see if facet was requested
    if (SplitMix64(docid)() % 100 < percentFacet) {
      // verify the facet response.  It should be a single bucket with id_i=docid and count=1
      const auto& facetResult = std::get<solux::api::FacetResult>(docList.ops.at("f")->kind);
      ASSERT_TRUE(facetResult.bucket_ids.has_value());
      const auto& bucketIds = std::get<solux::api::ColInt>(facetResult.bucket_ids->kind);
      ASSERT_EQ(1u, bucketIds.v.size());
      ASSERT_EQ(docid, bucketIds.v[0]);
      ASSERT_EQ(1u, facetResult.counts.size());
      ASSERT_EQ(1, facetResult.counts[0]);
    }
  };

  doThreadSafeSearch(nThreads, nDocs, nDocs, requestCreator, responseChecker);

  // Now let's do a test designed to uncover codec thread-safety bugs -
  // unpacking blocks of docids, frequencies, or positions concurrently should do it.

  RequestCreator reqc2 = [&](int64_t docid, solux::test::LocalReq& req) {
    req.collection("main").requestId(std::to_string(docid));
    // the field t2_w contains integers from 1-10, 1-100, and 1-1000.  So if we search for
    // something like "0" it should match > 11% of the docs and use block compression in the codec
    // provided there are enough docs in the index (otherwise tail compression is used).
    req.topDocs("q").getNumber().limit(1).matchQuery("t2_w", std::to_string(docid % 10));
  };

  // record the number of hits per query in a boost flat unordered map
  boost::unordered::unordered_flat_map<int64_t, int64_t> hits;

  ResponseChecker respc2 = [&](int64_t docid, const solux::api::SearchResponse& response) {
    const auto& docList = std::get<solux::api::DocList>(response.ops.at("q")->kind);
    // not too much to check here... just record the hits we got
    ASSERT_TRUE(docList.matches.has_value());
    hits[docid % 10] = *docList.matches;
  };

  // this pass records the number of hits per t2_w:[0 - 10]
  doThreadSafeSearch(1, 10, nDocs, reqc2, respc2);

  // now we can check the hits to see if we got the expected number of hits for each t2_w:[0 - 10]
  ResponseChecker respc2verify = [&](int64_t docid, const solux::api::SearchResponse& response) {
    const auto& docList = std::get<solux::api::DocList>(response.ops.at("q")->kind);
    ASSERT_TRUE(docList.matches.has_value());
    ASSERT_EQ(hits[docid % 10], *docList.matches);
  };

  // With 10K docs and 32 threads, this reliably failed with the old non-thread-safe
  // SIMDCompressionLib codecs (ASan often caught a double-free). The FastPFOR codecs
  // we use now are thread-safe by construction (stack-resident bit packers); this guards it.
  doThreadSafeSearch(nThreads, nDocs, nDocs, reqc2, respc2verify);

  //
  // Now test streaming back multiple responses per request
  //
  int64_t limit=50;
  int32_t batchSize = 5;
  RequestCreator reqc3 = [&](int64_t docid, solux::test::LocalReq& req) {
    req.collection("main").requestId(std::to_string(docid));
    auto& q = req.topDocs("q").getNumber().limit(limit).batchSize(batchSize)
                 .matchQuery("t2_w", std::to_string(docid % 10));
    for (auto& field : retrieveFields) q.fields({std::string(field)});
  };

  ResponseChecker respc3 = [&](int64_t docid, const solux::api::SearchResponse& response) {
    unused(docid);
    const auto& docList = std::get<solux::api::DocList>(response.ops.at("q")->kind);
    // because responses are streaming and not necessarily in order across different logical requests,
    // we need to get the number used to generate the query from the request id
    std::string requestId = idString(response.request_id);
    long long reqid = 0;
    auto [ptr, ec] = std::from_chars(requestId.data(), requestId.data() + requestId.size(), reqid);
    ASSERT_EQ(std::errc(), ec);
    unused(ptr);

    ASSERT_TRUE(docList.matches.has_value());
    ASSERT_EQ(hits[reqid % 10], *docList.matches);
    if (*docList.matches == 0) return;

    auto max = std::min(limit, (int64_t)*docList.matches);
    auto expectedColSize = response.more ? batchSize : max % batchSize;
    if (expectedColSize == 0) expectedColSize = batchSize;
    size_t expectedSize = (size_t)expectedColSize;

    // check the id field
    const auto& idColumn = std::get<solux::api::ColStr>(docList.columns.at("id").kind);
    if (idColumn.v.size() != expectedSize) {
      LOG_ERROR("CLIENT RESULT: request_id={} more={} matches={}", requestId, response.more, *docList.matches);
    }
    ASSERT_EQ(expectedSize, idColumn.v.size());

    for (size_t i=0; i<expectedSize; i++) {
      verifyDoc(-1, docList.columns, (int)i);
    }
  };

  doThreadSafeSearch(nThreads, nDocs, nDocs, reqc3, respc3);
}


TEST_F(GrpcIndexTest, addDocs) {

  // Setup request
  CollectionHelper::UpdateBuilder b;
  Reply<solux::api::UpdateResponse> response;

  b.collection("main");

  b.add(flatdoc("text1_w", std::string("my first field value"),
                "text2_w", std::string("my second field value")));

  b.add(flatdoc("text1_w", std::string("my first field value of 2nd doc"),
                "text1_w", std::string("my second field value of 2nd doc"),
                "text2_w", std::string("third field of 2nd doc")));

  GRPC_DEBUG("CLIENT REQ: docs={}", 2);

  grpc::ClientContext context;
  grpc::Status status = hppUnaryCall(channel.get(), rpc::Update, &context, b.finish(), &response);

  if (!status.ok()) {
    LOG_ERROR("grpc call failed!: code={} msg={}", (int)status.error_code(), status.error_message());
  } else {
    GRPC_DEBUG("I got id:{}", idString(response.msg.request_id));
  }

}

TEST_F(GrpcIndexTest, unsafeCollectionNameReturnsNotFound) {
  CollectionHelper::UpdateBuilder b;
  Reply<solux::api::UpdateResponse> response;
  b.collection("../bad");
  b.add(flatdoc("id", "grpc-bad-name", "title_w", "badname token"));

  grpc::ClientContext context;
  grpc::Status status = hppUnaryCall(channel.get(), rpc::Update, &context, b.finish(), &response);

  EXPECT_EQ(grpc::StatusCode::NOT_FOUND, status.error_code());
  EXPECT_NE(status.error_message().find("single path component"), std::string::npos)
      << status.error_message();
}

TEST_F(GrpcIndexTest, addDocsStream) {
  // Setup request
  Reply<solux::api::UpdateResponse> response;
  grpc::ClientContext context;  // need a new one for each RPC

  CollectionHelper::UpdateBuilder req;
  req.collection("main");

  req.add(flatdoc("text1_w", std::string("x1"), "text2_w", std::string("x2")));

  GRPC_DEBUG("CLIENT REQ: docs={}", 1);

  HppClientReaderWriter<solux::api::UpdateRequest, solux::api::UpdateResponse> stream(
    channel.get(), rpc::UpdateStream, &context);
  bool wrote = stream.Write(req.finish());
  ASSERT_TRUE(wrote);

  CollectionHelper::UpdateBuilder req2;
  req2.collection("main");  // TODO: allow this to not be set if same as last message!

  req2.add(flatdoc("text1_w", std::string("x3"), "text2_w", std::string("x4"), "text3_w", std::string("x5")));

  wrote = stream.Write(req2.finish());
  ASSERT_TRUE(wrote);

  bool ok = stream.WritesDone();  // can replace with WriteLast? is it more efficient?
  ASSERT_TRUE(ok);

  while (stream.Read(&response)) {
    GRPC_DEBUG("CLIENT RESULT: status={}", (int)response.msg.status);
  }

  grpc::Status status = stream.Finish();
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
  HppClientReaderWriter<solux::api::UpdateRequest, solux::api::UpdateResponse> wstream(
    channel.get(), rpc::UpdateStream, &wcontext);

  {
    CollectionHelper::UpdateBuilder req;
    Reply<solux::api::UpdateResponse> response;

    req.collection("main");

    req.add(flatdoc("text1_w", std::string("x1"), "text2_w", std::string("x2")));

    GRPC_DEBUG("CLIENT REQ: docs={}", 1);

    bool wrote = wstream.Write(req.finish());
    ASSERT_TRUE(wrote);

    // wait for the write response
    bool read = wstream.Read(&response);
    ASSERT_TRUE(read);
  }

  ASSERT_EQ(0, getDocCount());

  {
    CollectionHelper::UpdateBuilder req;
    Reply<solux::api::UpdateResponse> response;

    req.collection("main");
    req.commit();

    req.add(flatdoc("text1_w", std::string("x3"), "text2_w", std::string("x4")));

    GRPC_DEBUG("CLIENT REQ: docs={}", 1);

    bool wrote = wstream.Write(req.finish());
    ASSERT_TRUE(wrote);

    // wait for the write response
    bool read = wstream.Read(&response);
    ASSERT_TRUE(read);
  }

  ASSERT_EQ(2, getDocCount());

  bool ok = wstream.WritesDone();
  ASSERT_TRUE(ok);

  Reply<solux::api::UpdateResponse> response;
  while (wstream.Read(&response)) {
    GRPC_DEBUG("CLIENT RESULT: status={}", (int)response.msg.status);
  }

  grpc::Status status = wstream.Finish();
  GRPC_DEBUG("STREAMING UPDATE CLIENT FINISHED");
  ASSERT_TRUE(status.ok());
}
