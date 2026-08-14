#pragma once
#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>

#include "LuxirTest.h"
#include "luxir/server/GRPCServer.h"

namespace luxir {

class GrpcLuxirTest : public LuxirTest {
  static GRPCServer *server;
  static std::thread serverThread;
  static std::shared_ptr<grpc::Channel> channel;
  static bool serverStartFailed;
public:
  // Not thread safe
  static GRPCServer* startServer(int nThreads = -1) {
    if (server != nullptr) {
      return server;
    }

    assert(luxirNode != nullptr);
    auto& config = luxirNode->getConfig();
    int threads = nThreads >= 0 ? nThreads : config.server.grpc.resolveThreads();
    server = new GRPCServer(*luxirNode, threads, config.server.grpc.port);

    serverThread = std::thread([](){server->run();});
    if (!server->waitForStart()) {
      serverStartFailed = true;
    }
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
        serverStartFailed = false;
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

  static bool startFailed() {
    return serverStartFailed;
  }

  void SetUp() override {
    LuxirTest::SetUp();
    if (serverStartFailed) {
      GTEST_SKIP() << "gRPC test server failed to start in this environment";
    }
  }

};

} // end namespace
