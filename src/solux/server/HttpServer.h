#pragma once

#include <thread>
#include <vector>
#include <optional>
#include <memory>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include "SoluxNode.h"

namespace solux {

class HttpSessionRegistry;  // tracks live sessions for graceful shutdown (defined in the .cpp)

// A hand-written async HTTP/JSON front end (Boost.Beast) beside the gRPC server.
// Endpoints: /collections/{c}/_query, /_update, /_schema, plus GET /health.
// Search responses stream as NDJSON (application/x-ndjson, chunked); each engine
// SearchResponse becomes one line.
class HttpServer {
public:
  // port == 0 binds 127.0.0.1 on an OS-assigned port (tests); otherwise binds
  // 0.0.0.0 on the given port.  threads <= 0 means auto (hw_concurrency / 2).
  // streamBufferBytes <= 0 means "use server.stream_buffer_bytes from the node
  // config" (per-connection response buffering cap; see ServerConfig).
  HttpServer(SoluxNode& node, int threads, int port, int64_t streamBufferBytes = -1);
  ~HttpServer();

  // Bind, begin accepting, and spawn worker threads.  Non-blocking; getPort() is
  // valid after this returns.  Throws on bind failure.
  void start();

  // Stop accepting, drain in-flight work, and join workers.  Idempotent.
  // Drains rather than stopping the io_context (see the comment in the impl).
  void shutdown();

  int getPort() const { return port_; }
  SoluxNode& getSoluxNode() { return node; }

  HttpServer(const HttpServer&) = delete;
  HttpServer& operator=(const HttpServer&) = delete;

private:
  SoluxNode& node;
  int nthreads;
  int requestedPort;
  int64_t streamBufferBytes_;
  int port_ = 0;
  bool started = false;

  // Shared: sessions and off-io work pins co-own the context so their teardown
  // (strand release through the context's allocator) is safe from any thread,
  // even after shutdown() has joined the io threads.  The context may therefore
  // briefly outlive this object.  See IoPin in HttpServer.cpp.
  std::shared_ptr<boost::asio::io_context> ioc;
  std::optional<boost::asio::ip::tcp::acceptor> acceptor;
  std::vector<std::thread> threads;
  std::shared_ptr<HttpSessionRegistry> registry;
  // Keeps run() from returning while idle; reset during shutdown so run() returns
  // once in-flight work has drained.
  std::optional<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> workGuard;

  void doAccept();
};

} // namespace solux
