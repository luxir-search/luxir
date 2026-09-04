#include "oneapi/tbb/task_group.h"
#include "bench/luxir_bench.h"
#include "test/GrpcLuxirTest.h"
#include "test/GrpcClient.h"


using namespace luxir;
using namespace luxir::test;  // HppClientReaderWriter, Reply, rpc::*

// about 1.2% slower when not ommitting frame pointer
// adding term hashes (without using them) resulted in a slowdown of ~1%
// med is about 5% slower than small (before any optimizations like using hashes or pulling out prefixes from block starts)
//

static void BM_Req(benchmark::State& state, int writers, int readers) {
  unused(writers,readers);
  
  auto channel = GrpcLuxirTest::getChannel();
  if (GrpcLuxirTest::startFailed()) {
    state.SkipWithError("gRPC test server failed to start in this environment");
    return;
  }

  TrivialSearchRequest fixture;
  auto& req = fixture.request;
  Reply<luxir::api::SearchResponse> result;
  grpc::ClientContext context;  // need a new one for each RPC
  HppClientReaderWriter<luxir::api::SearchRequest, luxir::api::SearchResponse> stream(
    channel.get(), rpc::Search, &context);

  oneapi::tbb::task_group tasks;

  const int32_t requestsPerLoop = 200;
  int64_t totalReads = 0;
  int64_t totalWrites = 0;
  int64_t loops = 0;

  auto start = std::chrono::high_resolution_clock::now();
  // int64_t inside_duration = 0;
  for (auto _ : state) {
    // auto inside_start = std::chrono::high_resolution_clock::now();

    std::atomic_int32_t expectedResponses{0};
    std::atomic_bool writesDone(false);

    // moving this outside the loop (and using a latch to wait) made less than 2% difference.
    tasks.run(
            [&] {
              int32_t numResponses = 0;
              while (stream.Read(&result)) {
                ASSERT_FALSE(result.msg.more);
                ASSERT_FALSE(result.msg.request_id.empty());
                numResponses++;
                if (writesDone && numResponses >= expectedResponses) {
                  break;
                }
                /*
                std::string resStr;
                google::protobuf::TextFormat::PrintToString(result, &resStr);
                GRPC_DEBUG("CLIENT RESULT:( {} )", resStr);
                 */
              }
            });

    for (int i=0; i<requestsPerLoop; i++) {
      std::string requestId = std::to_string(totalWrites + i);
      req.request_id = requestId;
      expectedResponses++;

      // set writesDone *before* we actually write to avoid a race condition where
      // the reader can read the last response before I set writesDone (and then blocks on the Read())
      if (i == requestsPerLoop-1) {
        writesDone = true;
      }

      bool wrote = stream.Write(req);
      ASSERT_TRUE(wrote);
    }

    // Wait to read all responses.  How to do a timeout if one never comes?
    tasks.wait();

    totalWrites += requestsPerLoop;
    totalReads += expectedResponses;
    loops++;

    // auto inside_end = std::chrono::high_resolution_clock::now();
    // inside_duration += std::chrono::duration_cast<std::chrono::nanoseconds>(inside_end - inside_start).count();
  }

  // get wallclock end time and find the duration in nanoseconds
  auto end = std::chrono::high_resolution_clock::now();
  auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();

  bool ok = stream.WritesDone();
  ASSERT_TRUE(ok);

  grpc::Status status = stream.Finish();
  ASSERT_TRUE(status.ok());

  state.counters["writeRate"] = benchmark::Counter((double)totalWrites/(double)loops, benchmark::Counter::kIsIterationInvariantRate);
  state.counters["readRate"] = benchmark::Counter((double)totalReads/(double)loops, benchmark::Counter::kIsIterationInvariantRate);

  // Sanity checking google test... we need to use real time UseRealTime() or else the cpu time from just the main thread will be used.
  // Also, measuring the wall time from outside of the loop and inside of the loop is very different... gtest must be doing a bunch of stuff for the setup.
  // Measuring duration from inside the loop matches the Time column in gtest output that is seemingly used when UseRealTime() is set.
  // LOG_INFO("totalReads: {} totalWrites: {} loops: {} myduration={} inside_duration={}", totalReads, totalWrites, loops, duration, inside_duration/loops);
  unused(duration);
}


BENCHMARK_CAPTURE(BM_Req, searchStream, 1, 1)->UseRealTime();
