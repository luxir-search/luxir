// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <poll.h>
#include <sys/socket.h>
#include <chrono>
#include <future>
#include <optional>
#include <string>
#include <thread>

#include "test/LuxirTest.h"
#include "test/HttpReq.h"
#include "luxir/server/HttpServer.h"
#include "luxir/util/Signal.h"

namespace luxir::test {

using namespace std::chrono_literals;

// server.http connection limits: client timeouts, the connection cap and the
// buffered body budget, driven over raw sockets with short limits.
class HttpLimitsTest : public LuxirTest {
protected:
  LuxirConfig config;
  std::optional<LuxirNode> node;
  std::optional<HttpServer> server;

  void start() {
    node.emplace(config);
    server.emplace(*node, 2, 0);
    server->start();
  }

  void TearDown() override {
    if (server) server->shutdown();
  }

  uint64_t events(MetricHttpEvent event) { return node->metrics->count(event); }

  // One raw client connection.
  struct Client {
    net::io_context ioc;
    beast::tcp_stream stream{ioc};
    beast::flat_buffer buffer;

    explicit Client(int port) {
      stream.connect(tcp::endpoint(net::ip::make_address("127.0.0.1"), (unsigned short)port));
    }
    void send(std::string_view bytes) {
      beast::error_code ec;
      net::write(stream.socket(), net::buffer(bytes), ec);
    }
    http::response<http::string_body> read() {
      http::response<http::string_body> res;
      http::read(stream, buffer, res);
      return res;
    }
    // Whether the server closes the connection within `wait`; data it sends
    // first is appended to `received`.
    bool closedWithin(std::chrono::milliseconds wait, std::string* received = nullptr) {
      auto deadline = std::chrono::steady_clock::now() + wait;
      int fd = stream.socket().native_handle();
      for (;;) {
        auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) return false;
        pollfd p{fd, POLLIN, 0};
        if (poll(&p, 1, (int)left.count()) <= 0) continue;
        char data[4096];
        auto n = recv(fd, data, sizeof(data), 0);
        if (n <= 0) return true;
        if (received) received->append(data, (size_t)n);
      }
    }
  };

  static std::string health() { return "GET /health HTTP/1.1\r\nHost: x\r\n\r\n"; }

  static std::string post(std::string_view target, size_t contentLength) {
    return "POST " + std::string(target) + " HTTP/1.1\r\nHost: x\r\nContent-Type: application/json\r\n"
           "Content-Length: " + std::to_string(contentLength) + "\r\n\r\n";
  }

  static std::string chunk(std::string_view data) {
    char size[32];
    snprintf(size, sizeof(size), "%zx\r\n", data.size());
    return size + std::string(data) + "\r\n";
  }

  template <class Pred>
  static bool eventually(Pred pred, std::chrono::milliseconds wait = 5s) {
    auto deadline = std::chrono::steady_clock::now() + wait;
    while (!pred()) {
      if (std::chrono::steady_clock::now() > deadline) return false;
      std::this_thread::sleep_for(5ms);
    }
    return true;
  }
};

// A served keep-alive connection is closed once idle past the idle timeout;
// its first request is not held to it.
TEST_F(HttpLimitsTest, idleKeepAliveConnectionIsClosed) {
  config.server.http.idle_timeout_ms = 200;
  start();
  Client client(server->getPort());
  client.send(health());
  EXPECT_EQ(200, client.read().result_int());
  EXPECT_TRUE(client.closedWithin(5s));
  EXPECT_EQ(1u, events(MetricHttpEvent::IDLE_TIMEOUT));
  EXPECT_EQ(0u, events(MetricHttpEvent::HEADER_TIMEOUT));
}

// The header timeout bounds a silent new connection, and a request's headers
// from their first byte however steadily they trickle in.
TEST_F(HttpLimitsTest, headerTimeoutBoundsSilenceAndTrickle) {
  config.server.http.header_timeout_ms = 300;
  start();
  {
    Client silent(server->getPort());
    EXPECT_TRUE(silent.closedWithin(5s));
  }
  Client trickle(server->getPort());
  trickle.send("GET /health HTTP/1.1\r\n");
  bool closed = false;
  std::string answer;
  for (int i = 0; i < 20 && !closed && answer.empty(); i++) {
    trickle.send("X-Pad: " + std::to_string(i) + "\r\n");
    closed = trickle.closedWithin(100ms, &answer);
  }
  EXPECT_TRUE(closed || trickle.closedWithin(5s, &answer));
  // A begun request is answered before the close.
  EXPECT_TRUE(answer.starts_with("HTTP/1.1 408")) << answer;
  EXPECT_EQ(2u, events(MetricHttpEvent::HEADER_TIMEOUT));
}

// A body is timed by its inactivity, not its total duration.
TEST_F(HttpLimitsTest, bodyTimeoutCountsInactivity) {
  config.server.http.body_timeout_ms = 300;
  start();
  std::string body = R"({"ops":{"q":{"top_docs":{"query":{"match_all":{}}}}}})";
  {
    Client slow(server->getPort());
    slow.send(post("/collections/none/_search", body.size()));
    for (char c : body) {
      slow.send(std::string_view(&c, 1));
      std::this_thread::sleep_for(20ms);  // about 1 s in all
    }
    EXPECT_NE(0, slow.read().result_int());
  }
  EXPECT_EQ(0u, events(MetricHttpEvent::BODY_TIMEOUT));
  Client stalled(server->getPort());
  stalled.send(post("/collections/none/_search", body.size()) + body.substr(0, 10));
  EXPECT_TRUE(stalled.closedWithin(5s));
  EXPECT_EQ(1u, events(MetricHttpEvent::BODY_TIMEOUT));
}

// A stream the server pauses for backpressure waits untimed: here its only
// batch indexes for several body timeouts while the client sends nothing.
TEST_F(HttpLimitsTest, ndjsonBackpressurePauseIsNotTimed) {
  config.server.http.body_timeout_ms = 200;
  config.ingest.stream_batch_docs = 1;
  config.ingest.max_inflight_batches = 1;
  start();
  std::promise<void> entered;
  std::promise<void> release;
  std::shared_future<void> released = release.get_future().share();
  Signal::listen("httpBatchIndexing", [&](void* ordinal, void*, void*) -> void* {
    if ((uintptr_t)ordinal == 0) {
      entered.set_value();
      released.wait_for(10s);
    }
    return nullptr;
  });
  Client client(server->getPort());
  client.send("POST /collections/paused/_update HTTP/1.1\r\nHost: x\r\nContent-Type: application/x-ndjson\r\n"
              "Transfer-Encoding: chunked\r\n\r\n" + chunk("{\"id\":\"a\"}\n"));
  EXPECT_EQ(std::future_status::ready, entered.get_future().wait_for(5s));
  std::this_thread::sleep_for(1s);
  release.set_value();
  client.send(chunk("{\"id\":\"b\"}\n") + chunk("{\"_end_\":{\"commit\":{}}}\n") + "0\r\n\r\n");
  auto res = client.read();
  Signal::unlisten("httpBatchIndexing");
  EXPECT_EQ(200, res.result_int()) << res.body();
  EXPECT_NE(std::string::npos, res.body().find("\"status\":\"ok\"")) << res.body();
  EXPECT_EQ(0u, events(MetricHttpEvent::BODY_TIMEOUT));
}

// Past max-connections a new connection is closed as accepted; a slot frees
// when a connection ends.
TEST_F(HttpLimitsTest, connectionCapClosesExtraConnections) {
  config.server.http.max_connections = 2;
  start();
  std::optional<Client> first(server->getPort());
  Client second(server->getPort());
  for (Client* c : {&*first, &second}) {
    c->send(health());
    EXPECT_EQ(200, c->read().result_int());
  }
  {
    Client third(server->getPort());
    EXPECT_TRUE(third.closedWithin(5s));
  }
  EXPECT_EQ(1u, events(MetricHttpEvent::CONNECTION_REJECTED));
  first.reset();
  EXPECT_TRUE(eventually([&] { return node->metrics->httpConnections.load() == 1; }));
  Client again(server->getPort());
  again.send(health());
  EXPECT_EQ(200, again.read().result_int());
}

// Buffered bodies share a node-wide budget: a request that would exceed it
// gets 503 with Retry-After before its body is read, a lone request is always
// admitted, and every reservation is returned.
TEST_F(HttpLimitsTest, bufferedBodyBudget) {
  config.server.http.max_buffered_body_bytes = 1000;
  start();
  auto& reserved = node->metrics->httpBufferedBodyBytes;
  int port = server->getPort();
  EXPECT_EQ(400, httpRequest(port, http::verb::post, "/collections/none/_search", std::string(1500, ' ')).result_int());
  {
    Client holder(port);
    holder.send(post("/collections/none/_search", 800) + "{");
    ASSERT_TRUE(eventually([&] { return reserved.load() == 800; }));
    auto refused = httpRequest(port, http::verb::post, "/collections/none/_search", std::string(300, ' '));
    EXPECT_EQ(503, refused.result_int()) << refused.body();
    EXPECT_EQ("1", refused[http::field::retry_after]);
    // A chunked body reserves as it arrives.
    Client chunked(port);
    chunked.send("POST /collections/none/_search HTTP/1.1\r\nHost: x\r\nContent-Type: application/json\r\n"
                 "Transfer-Encoding: chunked\r\n\r\n" + chunk(std::string(300, ' ')));
    EXPECT_EQ(503, chunked.read().result_int());
    EXPECT_NE(503, httpRequest(port, http::verb::post, "/collections/none/_search", std::string(200, ' ')).result_int());
    EXPECT_EQ(2u, events(MetricHttpEvent::BODY_REJECTED));
  }
  EXPECT_TRUE(eventually([&] { return reserved.load() == 0; }));
  EXPECT_NE(503, httpRequest(port, http::verb::post, "/collections/none/_search", std::string(300, ' ')).result_int());
  EXPECT_TRUE(eventually([&] { return reserved.load() == 0; }));
}

} // namespace luxir::test
