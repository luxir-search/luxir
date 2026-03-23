#pragma once
#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>

#include "SoluxTest.h"
#include "solux/server/GRPCServer.h"

namespace solux {

class GrpcSoluxTest : public SoluxTest {
  static GRPCServer *server;
  static std::thread serverThread;
  static std::shared_ptr<grpc::Channel> channel;
public:
  // Not thread safe
  static GRPCServer* startServer(int nThreads = -1) {
    if (server != nullptr) {
      return server;
    }

    assert(soluxNode != nullptr);
    auto& config = soluxNode->getConfig();
    int threads = nThreads >= 0 ? nThreads : config.resolveThreads();
    server = new GRPCServer(*soluxNode, threads, config.port);

    serverThread = std::thread([](){server->run();});
    server->waitForStart();
    return server;
  }


  // Stops the server but does not delete it by default since
  // the destructor currently causes issues with running under valgrind
  static void stopServer(bool deleteServer = false) {
    if (server) {
      server->shutdown();
      serverThread.join();
      if (deleteServer) {
        delete server;
        server = nullptr;
        channel = nullptr;
      }
    }
  }

  // Get a shared channel to the test server
  // This will start the server if not already started
  static std::shared_ptr<grpc::Channel> getChannel() {
    if (!channel) {
      auto* grpcServer = startServer();
      std::string serverAddress = "localhost:" + std::to_string(grpcServer->getPort());
      channel = grpc::CreateChannel(serverAddress, grpc::InsecureChannelCredentials());
    }
    return channel;
  }

};

} // end namespace
