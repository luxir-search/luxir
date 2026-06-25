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
// Phase 0: a single POST /collections/{c}/query endpoint plus GET /health.
// Search responses stream as NDJSON (application/x-ndjson, chunked); each engine
// SearchResponse becomes one line.  See /opt/code/solux-private/http-json-api.md.
class HttpServer {
public:
  // port == 0 binds 127.0.0.1 on an OS-assigned port (tests); otherwise binds
  // 0.0.0.0 on the given port.  threads <= 0 means auto (hw_concurrency / 2).
  HttpServer(SoluxNode& node, int threads, int port);
  ~HttpServer();

  // Bind, begin accepting, and spawn worker threads.  Non-blocking; getPort() is
  // valid after this returns.  Throws on bind failure.
  void start();

  // Stop accepting, stop the io_context, and join workers.  Idempotent.
  void shutdown();

  int getPort() const { return port_; }
  SoluxNode& getSoluxNode() { return node; }

  HttpServer(const HttpServer&) = delete;
  HttpServer& operator=(const HttpServer&) = delete;

private:
  SoluxNode& node;
  int nthreads;
  int requestedPort;
  int port_ = 0;
  bool started = false;

  boost::asio::io_context ioc;
  std::optional<boost::asio::ip::tcp::acceptor> acceptor;
  std::vector<std::thread> threads;
  std::shared_ptr<HttpSessionRegistry> registry;
  // Keeps the io_context alive while idle; reset during shutdown so run() returns
  // once in-flight work has drained.
  std::optional<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> workGuard;

  void doAccept();
};

} // namespace solux
