#pragma once

#include <thread>
#include <latch>
#include <grpcpp/grpcpp.h>
#include <grpcpp/generic/async_generic_service.h>
#include "LuxirNode.h"
// The server itself serves everything through the generic service in the .cpp.
// This hpp-proto metadata include is kept for callers that still pick it up
// transitively while the client side migration catches up.
#include "luxir/api/luxir.hpp"

namespace luxir {

class GRPCServer {
public:
  // gRPC performance guidelines suggest having numcpu threads and 2 threads per completion queue.
  // Are those real cpu cores, or the hyper-threaded cores that hardware_concurrency reports?
  // https://grpc.io/docs/guides/performance/
  // port: The port to listen on. Use 0 for dynamic port allocation (useful for testing).
  // streamBufferBytes <= 0 means "use server.stream_buffer_bytes from the node
  // config" (per-connection response buffering cap; see ServerConfig).
  GRPCServer(LuxirNode& node, int nthreads = std::max(1u, std::thread::hardware_concurrency() / 2),
             int port = 0, int64_t streamBufferBytes = -1);

  /// Per-call cap on buffered response bytes (flow-control high-water mark).
  int64_t streamBufferBytes() const { return streamBufferBytes_; }

  /// This starts the server and blocks the current thread until shutdown.
  void run();

  /// wait for the server to come up, returns false on failure
  bool waitForStart();

  /// This stops the server and should cause run() to return
  void shutdown();


  /// Get the associated LuxirNode.
  LuxirNode& getLuxirNode() {
    return luxirNode;
  }

  /// Get the port the server is listening on
  int getPort() const {
    return serverPort;
  }


  struct ThreadInfo {
    int threadno;  // the thread number, starting at 0
    std::unique_ptr<grpc::ServerCompletionQueue> cq;  // TODO: allow multiple threads to share a completion queue

    int64_t requests;

    friend std::ostream &operator<<(std::ostream &out, const ThreadInfo &obj) {
      return out << "thread " << obj.threadno << " requests=" << obj.requests;
    }
  };
private:
  // All RPCs are served raw (grpc::ByteBuffer in/out) through a single generic
  // service; method dispatch is by RPC path (see GenericCallData in the .cpp).
  // This replaces the per-service typed AsyncService stubs so the server no longer
  // depends on generated service classes for transport; handlers own hpp-proto
  // read_binpb/write_binpb.
  grpc::AsyncGenericService genericService;

  LuxirNode& luxirNode;
  std::latch startLatch; // triggered when the gRPC server has started (but not the serving threads yet)
  std::latch startLatchThreads;  // count_down when grpc server thread has finished registering listeners
  std::unique_ptr<grpc::Server> server;
  std::vector<std::thread> threads;
  std::vector<ThreadInfo> threadInfos;
  int nthreads;
  int serverPort = 0;
  int requestedPort = 0;
  int64_t streamBufferBytes_;


  // this is run for each thread
  void runThread(ThreadInfo& threadInfo);

  friend class CallData;
  friend class GenericCallData;
};

}
