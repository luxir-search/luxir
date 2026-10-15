// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <atomic>
#include "test/LuxirTest.h"
#include "test/CollectionHelper.h"
#include "test/HttpReq.h"
#include "luxir/server/HttpServer.h"

namespace luxir::test {

class HttpFailureTest : public LuxirTest {
protected:
  std::optional<HttpServer> server;
  CollectionHelper helper{"main"};
  void SetUp() override {
    server.emplace(*luxirNode, 2, 0);
    server->start();
  }
  void TearDown() override { server->shutdown(); }
  int port() { return server->getPort(); }
};

TEST_F(HttpFailureTest, unexpectedDispatchExceptionFinishesRequestAndKeepsServing) {
  ExpectLog diagnostic("request_id=failed code=internal");
  std::atomic<bool> fail{true};
  Signal::listen("httpRequestDispatch", [&](void*, void*, void*) -> void* {
    if (fail.exchange(false)) throw 7;
    return nullptr;
  });
  auto response = httpRequest(port(), http::verb::get, "/health?request_id=failed");
  EXPECT_EQ(500, response.result_int()) << response.body();
  EXPECT_NE(std::string::npos, response.body().find("failed"));
  EXPECT_EQ(200, httpRequest(port(), http::verb::get, "/health").result_int());
  EXPECT_EQ(1u, diagnostic.suppressed());
  server->shutdown();
}

TEST_F(HttpFailureTest, updateCompletionFailureReleasesJsonAndNdjsonWork) {
  for (bool ndjson : {false, true}) {
    std::atomic<bool> fail{true};
    Signal::listen("httpUpdateCompletion", [&](void*, void*, void*) -> void* {
      if (fail.exchange(false)) throw std::bad_alloc();
      return nullptr;
    });
    auto response = httpRequest(port(), http::verb::post, "/collections/main/_update",
        ndjson ? "{\"id\":\"a\"}\n" : R"({"docs":[{"id":"a"}]})",
        ndjson ? "application/x-ndjson" : "application/json");
    EXPECT_EQ(429, response.result_int()) << response.body();
    EXPECT_FALSE(fail.load());
    Signal::unlisten("httpUpdateCompletion");
    EXPECT_EQ(200, httpRequest(port(), http::verb::get, "/health").result_int());
  }
  server->shutdown();
}

TEST_F(HttpFailureTest, searchSchedulingFailureFinishesAndReleasesArena) {
  Signal::listen("searchEnqueue", [](void*, void*, void*) -> void* { throw std::bad_alloc(); });
  auto response = httpRequest(port(), http::verb::post, "/collections/main/_search",
      R"({"max_parallel":1,"request_id":"schedule"})");
  EXPECT_EQ(429, response.result_int()) << response.body();
  EXPECT_NE(std::string::npos, response.body().find("schedule"));
  EXPECT_EQ(200, httpRequest(port(), http::verb::get, "/health").result_int());
  server->shutdown();
}

TEST_F(HttpFailureTest, urlCommitCompletionFailureFinishesStream) {
  Signal::listen("httpUrlCommitCompletion", [](void*, void*, void*) -> void* { throw std::bad_alloc(); });
  // The batch acknowledgment already started the chunked response. A failed
  // EOF commit must truncate it rather than report a false successful EOF.
  try {
    auto response = httpRequest(port(), http::verb::post, "/collections/main/_update?commit=true",
        "{\"id\":\"a\"}\n", "application/x-ndjson");
    EXPECT_EQ(429, response.result_int()) << response.body();
  } catch (const beast::system_error& error) {
    EXPECT_TRUE(error.code() == http::error::partial_message || error.code() == http::error::end_of_stream) << error.what();
  }
  EXPECT_EQ(200, httpRequest(port(), http::verb::get, "/health").result_int());
  server->shutdown();
}


TEST_F(HttpFailureTest, finalizationFailureKeepsPausedEmitterAliveUntilCompletion) {
  helper.indexAll(std::array{flatdoc("id", "a"), flatdoc("id", "b"), flatdoc("id", "c")}, UpdateMessage::COMMIT);
  HttpServer small(*luxirNode, 1, 0, 1);
  small.start();
  auto pauses = streamPauseCount.load();
  Signal::listen("searchFinalize", [](void*, void*, void*) -> void* { throw std::bad_alloc(); });
  auto response = httpRequest(small.getPort(), http::verb::post, "/collections/main/_search",
      R"({"query":{"all":true},"limit":-1,"batch_size":1,"fields":["id"]})");
  EXPECT_EQ(200, response.result_int()) << response.body();
  EXPECT_GT(streamPauseCount.load(), pauses);
  EXPECT_NE(std::string::npos, response.body().find("failed to complete search response"));
  small.shutdown();
}

} // namespace luxir::test
