// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>
#include <optional>
#include <memory>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include "LuxirNode.h"

namespace luxir {

class HttpSessionRegistry;  // tracks live sessions for graceful shutdown (defined in the .cpp)
class HttpIoShard;

// A hand-written async HTTP/JSON front end (Boost.Beast) beside the gRPC server.
// Endpoints: /collections/{c}/_search, /_update, /_schema, /_stats, plus
// node-wide GET /_stats and GET /health.
// Search responses stream as NDJSON (application/x-ndjson, chunked); each engine
// SearchResponse becomes one line.
class HttpServer {
public:
  // port == 0 binds 127.0.0.1 on an OS-assigned port (tests); otherwise binds
  // 0.0.0.0 on the given port. threads is the number of connection I/O shards;
  // threads <= 0 means auto (hw_concurrency, minimum 1). The dedicated accept
  // thread is additional.
  // streamBufferBytes <= 0 means "use server.stream_buffer_bytes from the node
  // config" (per-connection response buffering cap; see ServerConfig).
  // shardIdlePeriod controls how long an unused non-floor shard stays alive;
  // shard 0 remains running after its first connection assignment.
  HttpServer(LuxirNode& node, int threads, int port, int64_t streamBufferBytes = -1,
             std::chrono::milliseconds shardIdlePeriod = std::chrono::seconds(60));
  ~HttpServer();

  // Bind and begin accepting. Shard threads spawn lazily as connections are
  // assigned. Shard 0 stays warm after first use; other shards exit after an
  // idle linger and respawn on demand. Non-blocking; getPort() is valid after
  // this returns. Throws on bind failure.
  void start();

  // Stop accepting, drain in-flight work, and join workers.  Idempotent.
  // Drains rather than stopping shard contexts (see the comment in the impl).
  void shutdown();

  int getPort() const { return port_; }
  int getRunningShardThreads() const {
    return runningShardThreads.load(std::memory_order_relaxed);
  }
  LuxirNode& getLuxirNode() { return node; }

  HttpServer(const HttpServer&) = delete;
  HttpServer& operator=(const HttpServer&) = delete;

private:
  LuxirNode& node;
  int nthreads;
  int requestedPort;
  int64_t streamBufferBytes_;
  std::chrono::milliseconds shardIdlePeriod;
  int port_ = 0;
  bool started = false;
  std::atomic<bool> shutdownRequested{false};
  std::atomic<int> runningShardThreads{0};

  // Accepting is isolated from request execution so an inline search cannot
  // delay new connections. The outstanding async_accept keeps this context
  // alive until shutdown closes the acceptor.
  boost::asio::io_context acceptIoc{1};
  std::optional<boost::asio::ip::tcp::acceptor> acceptor;
  std::thread acceptThread;
  // Sessions and off-io work pins co-own their shard so executor teardown is
  // safe on any thread after HttpServer has joined the shard runner.
  std::vector<std::shared_ptr<HttpIoShard>> shards;
  std::shared_ptr<HttpSessionRegistry> registry;

  void doAccept();
  void armShardIdle(std::size_t idx);
  void spawnShard(std::size_t idx);
  void shardExited(std::size_t idx);
};

} // namespace luxir
