#pragma once

#include <thread>
#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>
#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include "protos/solux.grpc.pb.h"
#include <latch>
#include "SoluxNode.h"

namespace solux {

class GRPCServer {
public:
  // gRPC performance guidelines suggest having numcpu threads and 2 threads per completion queue.
  // Are those real cpu cores, or the hyper-threaded cores that hardware_concurrency reports?
  // https://grpc.io/docs/guides/performance/
  GRPCServer(int nthreads = std::max(1u, std::thread::hardware_concurrency() / 2));
  void run(); // this blocks the current thread
  void shutdown();
  bool waitForStart(); // wait for the server to come up, returns false on failure

  /// Get the associated SoluxNode.  Only valid for the lifetime of this GRPCServer.
  SoluxNode& getSoluxNode() {
    return soluxNode;
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

  SoluxNode soluxNode;  // TODO: this may be passed in later rather than exclusively owned?
  std::latch startLatch; // triggered when the gRPC server has started (but not the serving threads yet)
  std::latch startLatchThreads;  // count_down when grpc server thread has finished registering listeners
  std::unique_ptr<grpc::Server> server;
  std::vector<std::thread> threads;
  std::vector<ThreadInfo> threadInfos;
  int nthreads;


  // this is run for each thread
  void runThread(ThreadInfo& threadInfo);

  friend class CallData;
};

}