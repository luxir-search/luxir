#include "HttpServer.h"

#include <cassert>
#include <cstddef>
#include <deque>
#include <functional>
#include <list>
#include <memory_resource>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/dispatch.hpp>

#include "solux/util/log.h"
#include "solux/search/SearchEngine.h"
#include "solux/search/SearchRequest.h"
#include "JsonRequest.h"
#include "solux/api/build.h"
#include "JsonResponse.h"

namespace solux {

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;

class HttpSession;

using HttpSearchReqProto = solux::api::SearchRequest;

struct HttpSearchRequestState {
  std::pmr::monotonic_buffer_resource resource;
  HttpSearchReqProto proto;  // non-owning; backed by `resource`
};

// One SearchRequest per HTTP query.  Engine workers call reply() from a task
// arena thread; it renders one NDJSON line and hands it to the session strand.
// Holds a shared_ptr to the session so the connection outlives in-flight work,
// and a work guard so the io_context does not finish draining until this request
// completes (used by graceful shutdown).
class HttpSearchRequest : public SearchRequest {
public:
  std::unique_ptr<HttpSearchRequestState> requestState;
  std::shared_ptr<HttpSession> session;
  std::optional<net::executor_work_guard<net::any_io_executor>> workGuard;

  HttpSearchRequest(SearchEngine& engine, std::unique_ptr<HttpSearchRequestState> requestState,
                    std::shared_ptr<HttpSession> s, google::protobuf::Arena& arena)
    : SearchRequest(engine, requestState->proto, arena),
      requestState(std::move(requestState)),
      session(std::move(s)) {}

  int reply(SearchResponse& response) override;  // defined after HttpSession
  // done() is inherited: releaseArena(&arena) frees this request (and its work
  // guard).  reply() calls it eagerly once the final line is enqueued - the line
  // owns its bytes, so cleanup does not wait for the write to complete.
};

// Per-connection state.  All socket access and write-queue mutation happen on the
// connection's strand (asio analog of the gRPC BiStreamingRequest mutex); cross-
// thread producers reach it via net::post (enqueueLine).
class HttpSession : public std::enable_shared_from_this<HttpSession> {
public:
  HttpSession(tcp::socket&& sock, SoluxNode& node, std::shared_ptr<HttpSessionRegistry> registry)
    : stream_(std::move(sock)), node_(node), registry_(std::move(registry)) {}

  ~HttpSession() { if (deregister_) deregister_(); }

  void run();  // defined after HttpSessionRegistry (touches the registry)

  // Posts a socket close onto this session's strand, unblocking any outstanding
  // read/write so the connection drains during shutdown.
  void closeFromServer() {
    net::post(stream_.get_executor(), [self = shared_from_this()] {
      beast::error_code ec;
      self->stream_.socket().shutdown(tcp::socket::shutdown_both, ec);
      self->stream_.socket().close(ec);
    });
  }

  // Callable from any thread.  Posts a rendered NDJSON line (an owned string)
  // onto the strand.  No completion callback: reply() already freed the arena the
  // line was rendered from, so the write depends on nothing but the string.
  void enqueueLine(std::string line, bool last) {
    net::post(stream_.get_executor(),
        [self = shared_from_this(), line = std::move(line), last]() mutable {
          if (self->errored_) return;  // connection already failed; drop the line
          self->pendingQ_.push_back(Pending{std::move(line), last});
          self->driveWrites();
        });
  }

private:
  struct Pending { std::string line; bool last; };

  beast::tcp_stream stream_;
  SoluxNode& node_;
  std::shared_ptr<HttpSessionRegistry> registry_;
  std::function<void()> deregister_;  // removes this session from the registry
  beast::flat_buffer buffer_;
  std::optional<http::request_parser<http::string_body>> parser_;

  // Carried from the request for the (later, async) streaming response.
  unsigned httpVersion_ = 11;
  bool keepAlive_ = false;

  // Streaming write state - strand only.
  std::deque<Pending> pendingQ_;
  std::string inflightLine_;  // owns the chunk buffer during its async_write
  std::optional<http::response<http::empty_body>> res_;
  std::optional<http::response_serializer<http::empty_body>> sr_;
  bool writeOutstanding_ = false;
  bool headerSent_ = false;
  bool lastSeen_ = false;
  bool chunkLastSent_ = false;
  bool errored_ = false;

  void doRead() {
    parser_.emplace();
    parser_->body_limit(8 * 1024 * 1024);  // 8 MiB request-body cap
    buffer_.clear();
    http::async_read(stream_, buffer_, *parser_,
        beast::bind_front_handler(&HttpSession::onRead, shared_from_this()));
  }

  void onRead(beast::error_code ec, std::size_t) {
    if (ec == http::error::end_of_stream) { doClose(); return; }
    if (ec) { LOG_TRACE("http read: {}", ec.message()); doClose(); return; }
    route(parser_->release());
  }

  static bool parseQueryPath(std::string_view target, std::string& coll) {
    // /collections/{c}/query  (ignore any ?query-string)
    auto q = target.find('?');
    if (q != std::string_view::npos) target = target.substr(0, q);
    constexpr std::string_view pre = "/collections/";
    constexpr std::string_view suf = "/query";
    if (!target.starts_with(pre) || !target.ends_with(suf)) return false;
    auto name = target.substr(pre.size(), target.size() - pre.size() - suf.size());
    if (name.empty() || name.find('/') != std::string_view::npos) return false;
    coll.assign(name);
    return true;
  }

  void route(http::request<http::string_body> req) {
    httpVersion_ = req.version();
    keepAlive_ = req.keep_alive();

    std::string coll;
    if (req.method() == http::verb::get && req.target() == "/health") {
      respondSimple(http::status::ok, "application/json", R"({"status":"ok"})");
    } else if (req.method() == http::verb::post && parseQueryPath(req.target(), coll)) {
      handleQuery(req.body(), coll);
    } else {
      respondSimple(http::status::not_found, "application/json",
                    renderErrorBody("not found"));
    }
  }

  void handleQuery(const std::string& body, const std::string& coll) {
    auto* arena = createArena();
    auto requestState = std::make_unique<HttpSearchRequestState>();
    try {
      // Build the NON-OWNING request directly into the request state's arena.
      parseQueryRequest(body, requestState->proto, requestState->resource);
      // Collection target: one-element name span, arena-backed (coll is transient).
      auto& tgt = requestState->proto.collection.emplace();
      std::string_view* nm = solux::api::build::allocArray(tgt.name, 1, requestState->resource);
      nm[0] = solux::api::build::arenaStr(requestState->resource, coll);
    } catch (const std::exception& e) {
      releaseArena(arena);
      respondSimple(http::status::bad_request, "application/json",
                    renderErrorBody(e.what()));
      return;
    }

    auto& engine = node_.getSearchEngine();
    auto* sreq = solux::arenaCreate<HttpSearchRequest>(
        *arena, engine, std::move(requestState), shared_from_this(), *arena);
    // Keep the io_context busy until this query finishes so shutdown drains it.
    sreq->workGuard.emplace(stream_.get_executor());

    // Dispatch the synchronous engine.submit() off the strand so the io thread
    // is not blocked for the query's duration.  reply() posts results back here.
    node_.getTaskArena().enqueue([sreq, &engine] { engine.submit(*sreq, true); });
  }

  // --- streaming (chunked NDJSON) write pump --------------------------------

  void startStreamingHeader() {
    res_.emplace();
    res_->result(http::status::ok);
    res_->version(httpVersion_);
    res_->set(http::field::server, "solux");
    res_->set(http::field::content_type, "application/x-ndjson");
    res_->chunked(true);
    res_->keep_alive(keepAlive_);
    sr_.emplace(*res_);
  }

  void driveWrites() {
    if (writeOutstanding_ || errored_) return;
    if (!headerSent_) {
      if (pendingQ_.empty()) return;  // wait for the first line
      headerSent_ = true;
      startStreamingHeader();
      writeOutstanding_ = true;
      http::async_write_header(stream_, *sr_,
          beast::bind_front_handler(&HttpSession::onHeaderWritten, shared_from_this()));
      return;
    }
    if (!pendingQ_.empty()) {
      Pending p = std::move(pendingQ_.front());
      pendingQ_.pop_front();
      inflightLine_ = std::move(p.line);
      if (p.last) lastSeen_ = true;
      writeOutstanding_ = true;
      net::async_write(stream_, http::make_chunk(net::buffer(inflightLine_)),
          beast::bind_front_handler(&HttpSession::onChunkWritten, shared_from_this()));
      return;
    }
    if (lastSeen_ && !chunkLastSent_) {
      chunkLastSent_ = true;
      writeOutstanding_ = true;
      net::async_write(stream_, http::make_chunk_last(),
          beast::bind_front_handler(&HttpSession::onChunkLastWritten, shared_from_this()));
    }
  }

  void onHeaderWritten(beast::error_code ec, std::size_t) {
    writeOutstanding_ = false;
    if (ec) { failWrites(ec); return; }
    driveWrites();
  }

  void onChunkWritten(beast::error_code ec, std::size_t) {
    writeOutstanding_ = false;
    inflightLine_.clear();
    if (ec) { failWrites(ec); return; }
    driveWrites();
  }

  void onChunkLastWritten(beast::error_code ec, std::size_t) {
    writeOutstanding_ = false;
    if (ec) { failWrites(ec); return; }
    finishResponse();
  }

  void failWrites(beast::error_code ec) {
    LOG_TRACE("http write: {}", ec.message());
    errored_ = true;
    // Pending entries are just owned strings (their arenas were freed in reply()),
    // so dropping them frees everything.
    pendingQ_.clear();
    doClose();
  }

  void finishResponse() {
    if (keepAlive_) {
      // Reset streaming state and serve the next request on this connection.
      res_.reset();
      sr_.reset();
      headerSent_ = lastSeen_ = chunkLastSent_ = false;
      doRead();
    } else {
      doClose();
    }
  }

  // --- one-shot (non-streaming) responses: health, 404, 400 -----------------

  void respondSimple(http::status status, std::string_view contentType, std::string body) {
    auto resp = std::make_shared<http::response<http::string_body>>(status, httpVersion_);
    resp->set(http::field::server, "solux");
    resp->set(http::field::content_type, contentType);
    resp->keep_alive(keepAlive_);
    resp->body() = std::move(body);
    resp->prepare_payload();
    http::async_write(stream_, *resp,
        [self = shared_from_this(), resp](beast::error_code ec, std::size_t) {
          if (ec) { self->doClose(); return; }
          if (resp->keep_alive()) self->doRead();
          else self->doClose();
        });
  }

  void doClose() {
    beast::error_code ec;
    stream_.socket().shutdown(tcp::socket::shutdown_send, ec);
  }
};

// Tracks live sessions so shutdown can close idle keep-alive connections.  Weak
// references: the registry never extends a session's lifetime.
class HttpSessionRegistry {
public:
  std::mutex mtx;
  bool shuttingDown = false;
  std::list<std::weak_ptr<HttpSession>> sessions;
};

void HttpSession::run() {
  {
    std::lock_guard<std::mutex> lk(registry_->mtx);
    if (registry_->shuttingDown) {
      // Accepted during shutdown: close immediately, do not start reading.
      beast::error_code ec;
      stream_.socket().shutdown(tcp::socket::shutdown_both, ec);
      stream_.socket().close(ec);
      return;
    }
    registry_->sessions.push_front(weak_from_this());
    auto it = registry_->sessions.begin();
    deregister_ = [reg = registry_, it] {
      std::lock_guard<std::mutex> lk(reg->mtx);
      reg->sessions.erase(it);
    };
  }
  net::dispatch(stream_.get_executor(),
      beast::bind_front_handler(&HttpSession::doRead, shared_from_this()));
}

int HttpSearchRequest::reply(SearchResponse& response) {
  // The line is rendered into an owned string, so once it is enqueued the proto
  // (and its arena) are dead weight - free them eagerly here instead of deferring
  // to a post-write callback.  Cleanup is unconditional after the try, so a
  // throwing render/post still releases the arena and lets shutdown drain.
  bool last = response.last;
  try {
    response.proto.more = !last;
    session->enqueueLine(renderSearchResponseLine(response.proto), last);
  } catch (...) {
    // fall through to cleanup
  }
  if (last) {
    done();  // releases the request arena (this), its work guard, and session ref
  } else if (&response.arena != &arena) {
    releaseArena(&response.arena);  // this batch's own arena
  }
  return 0;
}

HttpServer::HttpServer(SoluxNode& node, int threads, int port)
  : node(node), nthreads(threads), requestedPort(port) {}

HttpServer::~HttpServer() { shutdown(); }

void HttpServer::start() {
  if (started) return;
  int n = nthreads > 0 ? nthreads
                       : (int)std::max(1u, std::thread::hardware_concurrency() / 2);

  std::string host = requestedPort == 0 ? "127.0.0.1" : "0.0.0.0";
  tcp::endpoint ep(net::ip::make_address(host), (unsigned short)requestedPort);

  registry = std::make_shared<HttpSessionRegistry>();
  workGuard.emplace(ioc.get_executor());

  // The acceptor runs on its own strand so its operations (async_accept in the
  // accept loop, and close() during shutdown) are serialized on one executor.
  acceptor.emplace(net::make_strand(ioc));
  acceptor->open(ep.protocol());
  acceptor->set_option(net::socket_base::reuse_address(true));
  acceptor->bind(ep);
  acceptor->listen(net::socket_base::max_listen_connections);
  port_ = acceptor->local_endpoint().port();

  doAccept();

  threads.reserve(n);
  for (int i = 0; i < n; i++) threads.emplace_back([this] { ioc.run(); });
  started = true;
  LOG_INFO("HTTP server listening on {}:{}", host, port_);
}

void HttpServer::doAccept() {
  acceptor->async_accept(net::make_strand(ioc),
      [this](beast::error_code ec, tcp::socket sock) {
        if (ec == net::error::operation_aborted) return;  // shutting down
        if (!ec) {
          std::make_shared<HttpSession>(std::move(sock), node, registry)->run();
        }
        if (acceptor && acceptor->is_open()) doAccept();
      });
}

void HttpServer::shutdown() {
  if (!started) return;
  // Must be driven from a non-io thread: we join the io threads below, so a
  // self-call would deadlock.  Current callers (main / test thread, destructor)
  // satisfy this; assert to catch a future io-thread caller.
  for (auto& t : threads) {
    assert(t.get_id() != std::this_thread::get_id()
           && "HttpServer::shutdown() must not be called from an io thread");
  }

  // 1) Stop accepting.  Posted onto the acceptor's strand so it does not race the
  //    accept loop; the drain below keeps the io_context alive until it runs.
  if (acceptor) {
    net::post(acceptor->get_executor(), [this] {
      beast::error_code ec;
      if (acceptor) acceptor->close(ec);
    });
  }

  // 2) Mark shutting down and snapshot live sessions (keeping them alive while we
  //    close them).  A connection accepted in the close window self-closes in
  //    run() because shuttingDown is already set here.
  std::vector<std::shared_ptr<HttpSession>> live;
  {
    std::lock_guard<std::mutex> lk(registry->mtx);
    registry->shuttingDown = true;
    for (auto& w : registry->sessions) if (auto s = w.lock()) live.push_back(std::move(s));
  }

  // 3) Close each live connection, then drop our refs.  Closing unblocks idle
  //    reads; in-flight queries keep the io_context alive via their work guards
  //    until they finish (and release their response arenas).
  for (auto& s : live) s->closeFromServer();
  live.clear();

  // 4) Release the keep-alive guard and let run() return once work drains.
  workGuard.reset();
  for (auto& t : threads) if (t.joinable()) t.join();
  threads.clear();
  started = false;
}

} // namespace solux
