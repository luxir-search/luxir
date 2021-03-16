#pragma once

#include <grpcpp/grpcpp.h>
#include <grpcpp/health_check_service_interface.h>
#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include "protos/solux.grpc.pb.h"
#include "boost/thread/latch.hpp"  // replace with std::latch when it's ready (libstdc++11 / gcc11)
#include "SoluxNode.h"

namespace solux {

class GRPCServer {
public:
  GRPCServer();
  void run(); // this blocks the current thread
  void shutdown();
  bool waitForStart(); // wait for the server to come up, returns false on failure

  /// Get the associated SoluxNode.  Only valid for the lifetime of this GRPCServer.
  SoluxNode& getSoluxNode() {
    return soluxNode;
  }

private:
  SoluxNode soluxNode;  // TODO: this may be passed in later rather than exclusively owned?
  boost::latch startLatch;
  std::unique_ptr<grpc::Server> server;
};

}