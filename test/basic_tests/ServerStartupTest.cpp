// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <boost/asio.hpp>
#include "test/LuxirTest.h"
#include "luxir/server/GRPCServer.h"

using namespace luxir;

class ServerStartupTest : public LuxirTest {};

TEST_F(ServerStartupTest, bindFailureIsReportedToCaller) {
  boost::asio::io_context context;
  boost::asio::ip::tcp::acceptor occupied(context,
      {boost::asio::ip::make_address("127.0.0.1"), 0});
  GRPCServer server(*luxirNode, 1, occupied.local_endpoint().port());
  EXPECT_FALSE(server.run());
  EXPECT_FALSE(server.waitForStart());
  server.shutdown();
}

TEST_F(ServerStartupTest, partialWorkerStartupFailureJoinsStartedThreads) {
  Signal::listen("grpcWorkerStart", [](void* index, void*, void*) -> void* {
    if (*(int*)index == 1) throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
    return nullptr;
  });
  GRPCServer server(*luxirNode, 2, 0);
  EXPECT_FALSE(server.run());
  EXPECT_FALSE(server.waitForStart());
  server.shutdown();
}
