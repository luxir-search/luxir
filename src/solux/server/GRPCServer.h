#pragma once

#include <thread>
#include <latch>
#include <grpcpp/grpcpp.h>
#include "protos/solux.grpc.pb.h"
#include "SoluxNode.h"

namespace solux {

class GRPCServer {
public:
  // gRPC performance guidelines suggest having numcpu threads and 2 threads per completion queue.
  // Are those real cpu cores, or the hyper-threaded cores that hardware_concurrency reports?
  // https://grpc.io/docs/guides/performance/
  // port: The port to listen on. Use 0 for dynamic port allocation (useful for testing).
  GRPCServer(SoluxNode& node, int nthreads = std::max(1u, std::thread::hardware_concurrency() / 2), int port = 0);

  /// This starts the server and blocks the current thread until shutdown.
  void run();

  /// wait for the server to come up, returns false on failure
  bool waitForStart();

  /// This stops the server and should cause run() to return
  void shutdown();


  /// Get the associated SoluxNode.
  SoluxNode& getSoluxNode() {
    return soluxNode;
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

  solux::Greeter::AsyncService greeterService;
  solux::Indexer::AsyncService indexerService;
  solux::Searcher::AsyncService searcherService;
  solux::Admin::AsyncService adminService;

  SoluxNode& soluxNode;
  std::latch startLatch; // triggered when the gRPC server has started (but not the serving threads yet)
  std::latch startLatchThreads;  // count_down when grpc server thread has finished registering listeners
  std::unique_ptr<grpc::Server> server;
  std::vector<std::thread> threads;
  std::vector<ThreadInfo> threadInfos;
  int nthreads;
  int serverPort = 0;
  int requestedPort = 0;


  // this is run for each thread
  void runThread(ThreadInfo& threadInfo);

  friend class CallData;
};

}