#pragma once
#include <gtest/gtest.h>

#include "SoluxTest.h"
#include "solux/server/GRPCServer.h"

namespace solux {

class GrpcSoluxTest : public SoluxTest {
  static GRPCServer *server;
  static std::thread serverThread;
public:
  // Not thread safe
  static GRPCServer* startServer(int nThreads = -1) {
    if (server != nullptr) {
      return server;
    }

    assert(soluxNode != nullptr);
    if (nThreads < 0) {
      server = new GRPCServer(*soluxNode);
    } else {
      server = new GRPCServer(*soluxNode, nThreads);
    }

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
      }
    }
  }

};

} // end namespace
