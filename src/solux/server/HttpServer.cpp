#include "HttpServer.h"

#include <cassert>
#include <array>
#include <algorithm>
#include <cstddef>
#include <deque>
#include <functional>
#include <limits>
#include <list>
#include <map>
#include <memory_resource>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/none.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/dispatch.hpp>

#include <glaze/json/prettify.hpp>

#include "solux/util/log.h"
#include "solux/schema/Schema.h"
#include "solux/search/SearchEngine.h"
#include "solux/search/SearchRequest.h"
#include "JsonRequest.h"
#include "solux/api/build.h"
#include "JsonResponse.h"
#include "NdjsonFramer.h"
#include "ProtoUpdateMessage.h"
#include "solux/util/thread.h"

namespace solux {

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;

class HttpSession;

using HttpSearchReqProto = solux::api::SearchRequest;
using HttpUpdateReqProto = solux::api::UpdateRequest;

// Pins the io_context on behalf of off-io-thread work (engine / task-arena
// tasks).  The guard keeps run() from returning until the work completes (the
// graceful-drain contract of HttpServer::shutdown()); the shared_ptr keeps the
// context itself alive so the guard's executor copy - a non-owning view of the
// context - can be destroyed on any thread, in any lambda-capture order, even
// after shutdown() has joined the io threads.  `ioc` is declared first so it
// outlives the guard's strand teardown.  Off-io holders must use this (via
// HttpSession::makeIoPin), never a raw executor_work_guard.
struct IoPin {
  std::shared_ptr<net::io_context> ioc;
  net::executor_work_guard<net::any_io_executor> guard;
  IoPin(std::shared_ptr<net::io_context> ioc, net::any_io_executor ex)
    : ioc(std::move(ioc)), guard(std::move(ex)) {}
};

struct HttpSearchRequestState {
  std::pmr::monotonic_buffer_resource resource;
  HttpSearchReqProto proto;  // non-owning; backed by `resource`
};

// Response shape for /_query.  Selected by the request-level proto field
// SearchRequest.response_format, or the ?format= URL param as an alias (for
// URL-capable clients; the param cannot express an explicit envelope, so
// either source selecting DOCS wins).  The engine produces identical DocList
// batches either way - only the NDJSON line framing differs - and gRPC
// (framed messages) rejects DOCS outright.
enum class HttpQueryFormat {
  ENVELOPE,  // default: one NDJSON envelope line per batch ({"docs":[...],...})
  DOCS,      // bare document lines, optional _header_ meta records
};

struct HttpUpdateState {
  std::pmr::monotonic_buffer_resource resource;
  HttpUpdateReqProto proto;  // non-owning; backed by `resource`
};

struct HttpStreamWriterTarget {
  std::shared_ptr<Collection> collection;
  std::shared_ptr<IndexWriter> indexWriter;
};

struct HttpStreamControlRequest {
  std::pmr::monotonic_buffer_resource resource;
  HttpUpdateReqProto proto;  // non-owning; backed by `resource`
  std::size_t sourceBytes = 0;
};

enum class HttpStreamControlKind {
  Open,
  Close,
  Noop,  // recognized meta record with no ingest effect (e.g. _header_)
};

struct HttpStreamControl {
  HttpStreamControlKind kind = HttpStreamControlKind::Close;
  std::shared_ptr<HttpStreamControlRequest> request;
};

struct HttpStreamAccumError {
  std::string id;
  std::string errorMessage;
  std::int32_t index = 0;
};

struct HttpStreamBatchState {
  std::pmr::monotonic_buffer_resource resource;
  HttpUpdateReqProto proto;  // non-owning; backed by `resource`
  solux::api::build::SpanBuilder<solux::api::Map> docs;
  std::shared_ptr<HttpStreamControlRequest> requestOwner;
  std::string collectionName;
  std::size_t sourceBytes = 0;
  std::size_t docCount = 0;
  std::size_t deleteCount = 0;
  std::size_t firstDocIndex = 0;

  explicit HttpStreamBatchState(std::size_t reserveDocs) : docs(resource) {
    docs.reserve(reserveDocs);
  }
};

struct HttpStreamBatchResult {
  std::vector<std::string> ids;
  std::vector<HttpStreamAccumError> errors;
  std::string errorMessage;
  solux::api::UpdateResponse_::Status status = solux::api::UpdateResponse_::Status::OK;
  std::uint64_t updateVersion = 0;
  std::size_t docCount = 0;
  std::size_t deleteCount = 0;
  std::size_t firstDocIndex = 0;
  bool failed = false;
};

struct HttpStreamInterval {
  std::vector<std::string> ids;
  std::vector<HttpStreamAccumError> errors;
  std::uint64_t lastUpdateVersion = 0;
  std::size_t firstDocIndex = 0;
  std::size_t docCount = 0;
  std::size_t docsIndexed = 0;
  std::size_t totalErrors = 0;
  bool submitted = false;
  bool anySuccess = false;
};

struct HttpStreamGroup {
  std::shared_ptr<HttpStreamControlRequest> request;
  std::string collectionName;
  std::string requestId;
  std::optional<std::string> closeRequestId;
  bool open = false;

  bool allowDups() const { return request && request->proto.allow_dups; }
  bool allOrNone() const { return request && request->proto.all_or_none; }
  bool returnIds() const { return request && request->proto.return_ids; }
};

struct HttpStreamUpdateState {
#ifdef NDEBUG
  static constexpr std::size_t kStreamReadBufBytes = 1024 * 1024;
#else
  static constexpr std::size_t kStreamReadBufBytes = 64 * 1024;
#endif
  static constexpr std::size_t kMaxRetainedErrors = 100;
  static constexpr std::size_t kMaxRetainedIds = 100;

  // Auto-cut a batch when it reaches either bound (from IngestConfig).
  std::size_t batchTargetBytes;
  std::size_t batchMaxDocs;
  std::size_t maxRequestBody;

  NdjsonFramer framer;
  std::vector<char> readBuf;
  std::string defaultCollectionName;
  HttpStreamGroup group;
  std::map<std::string, HttpStreamWriterTarget> writerCache;
  std::unique_ptr<HttpStreamBatchState> batch;
  std::optional<HttpStreamControl> pendingControl;
  std::shared_ptr<IoPin> ioPin;
  HttpStreamInterval interval;
  std::size_t docsSeen = 0;
  std::size_t docsIndexedSoFar = 0;
  bool urlCommit = false;
  bool emitAfterBatch = false;
  bool resetGroupAfterBatch = false;
  bool emittedLine = false;
  bool bodyDone = false;
  bool tailFinished = false;
  bool updateInFlight = false;
  bool urlCommitInFlight = false;
  bool failed = false;

  HttpStreamUpdateState(std::size_t batchTargetBytes, std::size_t batchMaxDocs,
                        std::size_t maxRecordBytes, std::size_t maxRequestBody)
    : batchTargetBytes(batchTargetBytes),
      batchMaxDocs(batchMaxDocs),
      maxRequestBody(maxRequestBody),
      framer(maxRecordBytes),
      readBuf(kStreamReadBufBytes),
      batch(std::make_unique<HttpStreamBatchState>(batchMaxDocs)) {}
};

// One SearchRequest per HTTP query.  Engine workers call reply() from a task
// arena thread; it renders one NDJSON line and hands it to the session strand.
// Holds a shared_ptr to the session so the connection outlives in-flight work,
// and an IoPin so shutdown drains this request and the io_context survives the
// request's teardown here on the task-arena thread.
class HttpSearchRequest : public SearchRequest {
public:
  std::unique_ptr<HttpSearchRequestState> requestState;
  std::shared_ptr<HttpSession> session;
  std::shared_ptr<IoPin> ioPin;
  HttpQueryFormat format = HttpQueryFormat::ENVELOPE;
  // DOCS-format framing state.  Document bodies render unlocked (pure per
  // batch); for multi-op requests - one emitter per op, concurrent replies -
  // replyDocs serializes just the framing decisions and queue posts under a
  // short req.mutex section so runs hit the wire whole.  Single-op requests
  // touch this from one producer at a time (sequenced by the completion
  // protocol) and take no lock.  reply() is never called with req.mutex held
  // (getTarget takes and releases it during batch assembly; maybeSendFinal
  // sends outside its critical section), so the lock cannot recurse.
  DocLinesState docsState;
  // True once output has been handed to the session (enqueued, not necessarily
  // flushed) - the boundary past which an error must abort the chunked stream
  // rather than answer with a plain HTTP error response.
  bool docsOutputCommitted = false;

  HttpSearchRequest(SearchEngine& engine, std::unique_ptr<HttpSearchRequestState> requestState,
                    std::shared_ptr<HttpSession> s, google::protobuf::Arena& arena)
    : SearchRequest(engine, requestState->proto, arena),
      requestState(std::move(requestState)),
      session(std::move(s)) {
    docFormatDefault = solux::api::DocFormat::ROWS;
  }

  // All defined after HttpSession.
  ReplyStatus reply(SearchResponse& response) override;
  ReplyStatus replyDocs(SearchResponse& response);
  void resumeWhenDrained(std::function<void()> resume) override;
  // done() is inherited: releaseArena(&arena) frees this request (and its
  // IoPin).  reply() calls it eagerly once the final line is enqueued - the line
  // owns its bytes, so cleanup does not wait for the write to complete.
};

// Per-connection state.  All socket access and write-queue mutation happen on the
// connection's strand (asio analog of the gRPC BiStreamingRequest mutex); cross-
// thread producers reach it through enqueueLine.
class HttpSession : public std::enable_shared_from_this<HttpSession> {
public:
  HttpSession(std::shared_ptr<net::io_context> ioc, tcp::socket&& sock, SoluxNode& node,
              std::shared_ptr<HttpSessionRegistry> registry, int64_t streamBufferBytes)
    : ioc_(std::move(ioc)), stream_(std::move(sock)), node_(node), registry_(std::move(registry)),
      highWater_(streamBufferBytes), lowWater_(streamBufferBytes / 2) {}

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

  // Callable from any thread.  Queues a rendered NDJSON line (an owned string)
  // on the strand.  No completion callback: reply() already freed the arena the
  // line was rendered from, so the write depends on nothing but the string.
  // Returns the connection's buffered bytes including this line - the producer's
  // flow-control signal (compare against highWater()).  Accounted here, before
  // the dispatch, so the count never lags the producer.
  int64_t enqueueLine(std::string line, bool last) {
    int64_t bytes = (int64_t)line.size();
    int64_t queued = queuedBytes_.fetch_add(bytes, std::memory_order_relaxed) + bytes;
    try {
      // The handler swallows every failure of its own (marking the connection
      // failed), so an exception reaching the catch below can only mean the
      // handler never ran - which keeps the rollback there exact.  Without
      // this, dispatch running the handler INLINE (caller already on the
      // strand) could propagate a post-queue failure into that catch and
      // double-subtract bytes a completion path subtracts again.
      net::dispatch(stream_.get_executor(),
          [self = shared_from_this(), line = std::move(line), last]() mutable {
            int64_t bytes = (int64_t)line.size();  // before the move below
            if (self->errored_) {  // connection already failed; drop the line
              self->queuedBytes_.fetch_sub(bytes, std::memory_order_relaxed);
              return;
            }
            try {
              self->pendingQ_.push_back(Pending{std::move(line), last});
            } catch (...) {
              // Allocation failed; the line was never queued.
              self->queuedBytes_.fetch_sub(bytes, std::memory_order_relaxed);
              self->failWrites(net::error::make_error_code(net::error::no_memory));
              return;
            }
            try {
              self->driveWrites();
            } catch (...) {
              self->failWrites(net::error::make_error_code(net::error::no_memory));
            }
          });
    } catch (...) {
      // The handler was never queued: roll the accounting back or the phantom
      // bytes would wedge flow control at the high-water mark forever.
      queuedBytes_.fetch_sub(bytes, std::memory_order_relaxed);
      throw;
    }
    return queued;
  }

  // Callable from any thread.  Parks a paused producer's resume callback; it is
  // handed to the task arena once buffered bytes drop below the low-water mark,
  // or immediately if the connection has failed (the resumed producer's next
  // reply() then observes CANCEL).  Registration runs on the strand, serialized
  // with write completions, so a wakeup cannot be lost.
  void whenDrained(std::function<void()> resume) {
    net::dispatch(stream_.get_executor(),
        [self = shared_from_this(), resume = std::move(resume)]() mutable {
          self->drainWaiters_.push_back(std::move(resume));
          self->maybeFireDrainWaiters();
        });
  }

  // True once the connection has failed; producers should stop rendering.
  bool aborted() const { return aborted_.load(std::memory_order_relaxed); }

  int64_t highWater() const { return highWater_; }

  // Callable from any thread.  Aborts a mid-stream chunked response WITHOUT
  // the terminating 0-chunk: the client sees truncation (premature EOF)
  // instead of a complete-looking result.  This is the docs format's
  // mid-stream error signal - it has no envelope to carry an error in-band.
  void abortStream() {
    net::post(stream_.get_executor(), [self = shared_from_this()] {
      if (!self->errored_) {
        self->failWrites(net::error::make_error_code(net::error::operation_aborted));
      }
    });
  }

  // Callable from any thread.  One-shot error response for a request that has
  // not streamed anything yet (docs format, failure before the first flush).
  // Dropped if the connection has failed or a streaming response already
  // started (the abortStream contract covers that case).
  void respondErrorFromEngine(http::status status, std::string body) {
    net::post(stream_.get_executor(),
        [self = shared_from_this(), status, body = std::move(body)]() mutable {
          if (self->errored_ || self->headerSent_) return;
          self->respondSimple(status, "application/json", std::move(body));
        });
  }

  // Every off-io-thread holder of this session's executor must pin the context
  // through this helper, never via a raw executor_work_guard: IoPin's member
  // order guarantees the io_context outlives the guard's teardown wherever the
  // last reference drops.
  std::shared_ptr<IoPin> makeIoPin() {
    return std::make_shared<IoPin>(ioc_, stream_.get_executor());
  }

private:
  struct Pending { std::string line; bool last; };

  // Co-owns the io_context: the last session ref can drop on a task-arena thread
  // after shutdown() has joined, and ~stream_ releases its strand through the
  // context.  Declared first so it is destroyed last.
  std::shared_ptr<net::io_context> ioc_;
  beast::tcp_stream stream_;
  SoluxNode& node_;
  std::shared_ptr<HttpSessionRegistry> registry_;
  std::function<void()> deregister_;  // removes this session from the registry
  beast::flat_buffer buffer_;
  std::optional<http::request_parser<http::buffer_body>> parser_;
  std::array<char, 64 * 1024> bodyBuf_{};
  std::string bufferedBody_;
  std::shared_ptr<HttpStreamUpdateState> streamUpdate_;

  // Carried from the request for the (later, async) streaming response.
  unsigned httpVersion_ = 11;
  bool keepAlive_ = false;

  // Response flow control.  queuedBytes_ counts rendered bytes accepted from
  // producers but not yet written to the socket (pendingQ_ + inflightLine_);
  // it is the only cross-thread piece - everything else is strand-only.
  std::atomic<int64_t> queuedBytes_{0};
  std::atomic<bool> aborted_{false};  // producer-visible mirror of errored_
  int64_t highWater_;
  int64_t lowWater_;
  std::vector<std::function<void()>> drainWaiters_;  // parked producer resumes

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
    // The buffered request-body cap is applied after header routing. Streaming
    // NDJSON must be able to carry an unbounded Content-Length; atomic groups are
    // capped explicitly by their group accumulator instead.
    parser_->body_limit(boost::none);
    buffer_.clear();
    bufferedBody_.clear();
    streamUpdate_.reset();
    http::async_read_header(stream_, buffer_, *parser_,
        beast::bind_front_handler(&HttpSession::onRead, shared_from_this()));
  }

  void onRead(beast::error_code ec, std::size_t) {
    if (ec == http::error::end_of_stream) { doClose(); return; }
    if (ec) { LOG_TRACE("http read: {}", ec.message()); doClose(); return; }

    httpVersion_ = parser_->get().version();
    keepAlive_ = parser_->get().keep_alive();

    std::string_view target(parser_->get().target());
    std::string_view query;
    if (auto q = target.find('?'); q != std::string_view::npos) {
      query = target.substr(q + 1);
      target = target.substr(0, q);
    }

    std::string coll;
    if (parser_->get().method() == http::verb::post && parseUpdatePath(target, coll) &&
        isNdjsonContentType(std::string_view(parser_->get()[http::field::content_type]))) {
      std::vector<UrlParam> params = parseParams(query);
      bool urlCommit = false;
      if (const std::string* commit = findParam(params, "commit")) {
        if (*commit != "true") {
          respondSimple(http::status::bad_request, "application/json",
                        renderErrorBody("unknown commit mode '" + *commit + "' (valid: true)"));
          return;
        }
        urlCommit = true;
      }
      parser_->body_limit(boost::none);
      startStreamingUpdate(std::move(coll), urlCommit);
      return;
    }

    std::uint64_t bodyLimit = (std::uint64_t)node_.getConfig().ingest.max_request_body;
    if (parser_->content_length() && *parser_->content_length() > bodyLimit) {
      respondPayloadTooLarge();
      return;
    }
    parser_->body_limit(bodyLimit);
    doBufferedBodyRead();
  }

  void doBufferedBodyRead() {
    if (parser_->is_done()) {
      dispatchBufferedRequest();
      return;
    }
    parser_->get().body().data = bodyBuf_.data();
    parser_->get().body().size = bodyBuf_.size();
    http::async_read(stream_, buffer_, *parser_,
        beast::bind_front_handler(&HttpSession::onBufferedBodyRead, shared_from_this()));
  }

  void onBufferedBodyRead(beast::error_code ec, std::size_t) {
    bool needBuffer = ec == http::error::need_buffer;
    // A body that grows past the limit (e.g. chunked, no Content-Length) surfaces here.
    if (ec == http::error::body_limit) { respondPayloadTooLarge(); return; }
    if (ec && !needBuffer) { LOG_TRACE("http read: {}", ec.message()); doClose(); return; }

    std::size_t produced = bodyBuf_.size() - parser_->get().body().size;
    bufferedBody_.append(bodyBuf_.data(), produced);

    if (parser_->is_done()) {
      dispatchBufferedRequest();
      return;
    }
    doBufferedBodyRead();
  }

  void dispatchBufferedRequest() {
    http::request<http::string_body> req;
    req.base() = parser_->get().base();
    req.body() = std::move(bufferedBody_);
    parser_.reset();
    route(std::move(req));
  }

  static bool parseCollectionPath(std::string_view target, std::string_view suffix, std::string& coll) {
    // /collections/{c}/{endpoint}  (route() strips any ?query-string before matching)
    constexpr std::string_view pre = "/collections/";
    if (!target.starts_with(pre) || !target.ends_with(suffix)) return false;
    auto name = target.substr(pre.size(), target.size() - pre.size() - suffix.size());
    coll.assign(name);
    return true;
  }

  static bool parseQueryPath(std::string_view target, std::string& coll) {
    return parseCollectionPath(target, "/_query", coll);
  }

  static bool parseUpdatePath(std::string_view target, std::string& coll) {
    return parseCollectionPath(target, "/_update", coll);
  }

  static bool parseSchemaPath(std::string_view target, std::string& coll) {
    return parseCollectionPath(target, "/_schema", coll);
  }

  static std::string_view trimHeaderValue(std::string_view v) {
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.remove_prefix(1);
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t')) v.remove_suffix(1);
    return v;
  }

  static char asciiLower(char c) {
    if (c >= 'A' && c <= 'Z') return (char)(c - 'A' + 'a');
    return c;
  }

  static bool asciiEqualsIgnoreCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); i++) {
      if (asciiLower(a[i]) != asciiLower(b[i])) return false;
    }
    return true;
  }

  static bool isNdjsonContentType(std::string_view contentType) {
    if (auto semi = contentType.find(';'); semi != std::string_view::npos) {
      contentType = contentType.substr(0, semi);
    }
    contentType = trimHeaderValue(contentType);
    return asciiEqualsIgnoreCase(contentType, "application/x-ndjson");
  }

  struct UrlParam {
    std::string key;
    std::string value;
  };

  // Standard form-decoding: %XX hex escapes and '+' as space. Malformed escapes
  // pass through literally (lenient - the URL is an open channel).
  static std::string urlDecode(std::string_view s) {
    auto hex = [](char c) -> int {
      if (c >= '0' && c <= '9') return c - '0';
      if (c >= 'a' && c <= 'f') return c - 'a' + 10;
      if (c >= 'A' && c <= 'F') return c - 'A' + 10;
      return -1;
    };
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
      char c = s[i];
      if (c == '+') {
        out += ' ';
      } else if (c == '%' && i + 2 < s.size() && hex(s[i + 1]) >= 0 && hex(s[i + 2]) >= 0) {
        out += (char)(hex(s[i + 1]) * 16 + hex(s[i + 2]));
        i += 2;
      } else {
        out += c;
      }
    }
    return out;
  }

  // Parse the &-separated query string into decoded key/value pairs. Unknown
  // parameters are KEPT, not rejected: the URL is an open ecosystem channel
  // (correlation ids, tracing, middleware), and the parsed params are the seam
  // for the future typed field overlay and $var substitution bindings. Keys we
  // do recognize enforce their values strictly (a bad value on a known key is
  // an author error, not middleware noise).
  static std::vector<UrlParam> parseParams(std::string_view params) {
    std::vector<UrlParam> out;
    while (!params.empty()) {
      auto amp = params.find('&');
      std::string_view kv = params.substr(0, amp);
      params = (amp == std::string_view::npos) ? std::string_view() : params.substr(amp + 1);
      if (kv.empty()) continue;
      auto eq = kv.find('=');
      std::string_view k = kv.substr(0, eq);
      std::string_view v = (eq == std::string_view::npos) ? std::string_view() : kv.substr(eq + 1);
      out.push_back({urlDecode(k), urlDecode(v)});
    }
    return out;
  }

  // Last occurrence wins, matching map semantics elsewhere.
  static const std::string* findParam(const std::vector<UrlParam>& params, std::string_view key) {
    for (auto it = params.rbegin(); it != params.rend(); ++it) {
      if (it->key == key) return &it->value;
    }
    return nullptr;
  }

  void route(http::request<http::string_body> req) {
    httpVersion_ = req.version();
    keepAlive_ = req.keep_alive();

    std::string_view target(req.target());
    std::string_view query;
    if (auto q = target.find('?'); q != std::string_view::npos) {
      query = target.substr(q + 1);
      target = target.substr(0, q);
    }
    std::vector<UrlParam> params = parseParams(query);

    std::string coll;
    if (req.method() == http::verb::get && target == "/health") {
      respondSimple(http::status::ok, "application/json", R"({"status":"ok"})");
    } else if (req.method() == http::verb::post && parseQueryPath(target, coll)) {
      auto format = HttpQueryFormat::ENVELOPE;
      if (const std::string* f = findParam(params, "format")) {
        if (*f == "docs") {
          format = HttpQueryFormat::DOCS;
        } else {
          respondSimple(http::status::bad_request, "application/json",
                        renderErrorBody("unknown format '" + *f + "' (valid: docs)"));
          return;
        }
      }
      if (const std::string* explain = findParam(params, "explain")) {
        if (*explain != "request") {
          respondSimple(http::status::bad_request, "application/json",
                        renderErrorBody("unknown explain mode '" + *explain + "' (valid: request)"));
          return;
        }
        if (format != HttpQueryFormat::ENVELOPE) {
          respondSimple(http::status::bad_request, "application/json",
                        renderErrorBody("format=docs cannot be combined with explain"));
          return;
        }
        handleExplain(req.body(), coll);
      } else {
        handleQuery(req.body(), coll, format);
      }
    } else if (req.method() == http::verb::post && parseUpdatePath(target, coll)) {
      handleUpdate(req.body(), coll);
    } else if (parseSchemaPath(target, coll)) {
      // Writes are POST; the operation is the visible, typeable ?mode= param,
      // never an invisible HTTP verb.  mode=set (the default, and curl's
      // zero-flag path) sets each named definition exactly; the destructive
      // mode=replace_all must be typed.  PUT/PATCH are reserved.
      if (req.method() == http::verb::get) {
        handleSchemaGet(coll);
      } else if (req.method() == http::verb::post) {
        auto mode = solux::api::SchemaRequest_::Mode::SET;
        if (const std::string* m = findParam(params, "mode")) {
          if (*m == "replace_all") {
            mode = solux::api::SchemaRequest_::Mode::REPLACE_ALL;
          } else if (*m != "set") {
            respondSimple(http::status::bad_request, "application/json",
                          renderErrorBody("unknown mode '" + *m + "' (valid: set, replace_all)"));
            return;
          }
        }
        handleSchemaSet(req.body(), coll, mode);
      } else {
        respondMethodNotAllowed(
            "GET, POST",
            "method not allowed; schema writes are POST (mode=set adds or replaces the "
            "named definitions, mode=replace_all replaces the whole schema)");
      }
    } else {
      respondSimple(http::status::not_found, "application/json",
                    renderErrorBody("not found"));
    }
  }

  static void setCollectionTarget(std::optional<solux::api::Target>& collection,
                                  const std::string& coll,
                                  std::pmr::memory_resource& resource) {
    // Collection target: one-element name span, arena-backed (coll is transient).
    auto& tgt = collection.emplace();
    std::string_view* nm = solux::api::build::allocArray(tgt.name, 1, resource);
    nm[0] = solux::api::build::arenaStr(resource, coll);
  }

  // Fill state.proto with the EFFECTIVE request: the parsed body (either dialect
  // form) with the path-derived collection applied (overwrites a body-supplied one).
  // Throws with a client-facing message on malformed input.
  static void parseEffectiveRequest(const std::string& body, const std::string& coll,
                                    HttpSearchRequestState& state) {
    // Build the NON-OWNING request directly into the request state's arena.
    parseQueryRequest(body, state.proto, state.resource);
    setCollectionTarget(state.proto.collection, coll, state.resource);
  }

  // ?explain=request: parse exactly as a query would be, then return the canonical
  // JSON of the effective request INSTEAD of executing it. Sugar expands, shorthand
  // lowers, and the output is itself a valid request body (posting it back runs the
  // identical query). Parse + serialize only - no engine work, so it runs inline.
  void handleExplain(const std::string& body, const std::string& coll) {
    HttpSearchRequestState state;
    std::string out;
    try {
      parseEffectiveRequest(body, coll, state);
      // Mirrors the URL-param check in route(): the docs format can also be
      // selected in the body, and it composes with explain no better.
      if (state.proto.response_format == solux::api::ResponseFormat::DOCS) {
        respondSimple(http::status::bad_request, "application/json",
                      renderErrorBody("format=docs cannot be combined with explain"));
        return;
      }
      if (!solux::api::write_json(state.proto, out)) {
        throw std::runtime_error("failed to serialize request");
      }
    } catch (const std::exception& e) {
      respondSimple(http::status::bad_request, "application/json",
                    renderErrorBody(e.what()));
      return;
    }
    respondSimple(http::status::ok, "application/json", out);
  }

  // The docs format is pure document lines: every op must produce a DocList,
  // with nothing that would need an envelope to carry.  Multi-op requests are
  // fine - their runs are attributed by op-named _header_ markers.  Returns
  // an error message, or nullptr when the request qualifies.
  static const char* validateDocsFormat(const HttpSearchReqProto& proto) {
    if (proto.ops.size() == 0) {
      return "format=docs requires at least one op";
    }
    std::vector<std::string_view> seen;
    for (const auto& [name, opView] : proto.ops) {
      // The raw ops view preserves duplicate names (last wins at execution);
      // docs framing attributes runs BY name, so duplicates would make the
      // marker/multi-op decisions diverge from what actually executes.
      if (std::find(seen.begin(), seen.end(), name) != seen.end()) {
        return "format=docs does not support duplicate op names";
      }
      seen.push_back(name);
      const solux::api::SearchOp& op = *opView;
      if (const auto* td = std::get_if<solux::api::TopDocs>(&op.kind)) {
        if (!td->ops.empty()) return "format=docs does not support nested ops";
        if (td->document_format == solux::api::DocFormat::COLUMNS) {
          return "format=docs emits row documents; document_format COLUMNS conflicts";
        }
      } else if (const auto* f = std::get_if<solux::api::Fusion>(&op.kind)) {
        if (!f->ops.empty()) return "format=docs does not support nested ops";
        // Per-source ops are ignored by fusion execution, but silently discarding
        // authored work behind a format flag would be worse than rejecting it.
        for (const auto& [srcName, src] : f->sources) {
          if (!src.ops.empty()) return "format=docs does not support nested ops";
        }
        if (f->document_format == solux::api::DocFormat::COLUMNS) {
          return "format=docs emits row documents; document_format COLUMNS conflicts";
        }
      } else {
        return "format=docs requires top_docs or fusion ops";
      }
    }
    return nullptr;
  }

  void handleQuery(const std::string& body, const std::string& coll, HttpQueryFormat format) {
    auto* arena = createArena();
    auto requestState = std::make_unique<HttpSearchRequestState>();
    try {
      parseEffectiveRequest(body, coll, *requestState);
    } catch (const std::exception& e) {
      releaseArena(arena);
      respondSimple(http::status::bad_request, "application/json",
                    renderErrorBody(e.what()));
      return;
    }
    // The URL param is an alias for the request-level proto field (for clients
    // that cannot set query params).  The param cannot express an explicit
    // envelope, so either source selecting DOCS wins - no conflict exists.
    if (requestState->proto.response_format == solux::api::ResponseFormat::DOCS) {
      format = HttpQueryFormat::DOCS;
    }
    bool docsMultiOp = false;
    if (format == HttpQueryFormat::DOCS) {
      if (const char* err = validateDocsFormat(requestState->proto)) {
        releaseArena(arena);
        respondSimple(http::status::bad_request, "application/json", renderErrorBody(err));
        return;
      }
      docsMultiOp = requestState->proto.ops.size() > 1;
    }

    auto& engine = node_.getSearchEngine();
    auto* sreq = solux::arenaCreate<HttpSearchRequest>(
        *arena, engine, std::move(requestState), shared_from_this(), *arena);
    sreq->format = format;
    sreq->docsState.multiOp = docsMultiOp;
    // Pin the io_context: shutdown drains until this query finishes, and the
    // context survives the request's teardown on the task-arena thread.
    sreq->ioPin = makeIoPin();

    // Dispatch the synchronous engine.submit() off the strand so the io thread
    // is not blocked for the query's duration.  reply() posts results back here.
    node_.getTaskArena().enqueue([sreq, &engine] { engine.submit(*sreq, true); });
  }

  void handleUpdate(const std::string& body, const std::string& coll) {
    auto state = std::make_shared<HttpUpdateState>();
    try {
      std::string err;
      if (!solux::api::read_json(state->proto, body, state->resource, &err)) {
        throw std::runtime_error(err.empty() ? "malformed update request" : err);
      }
      setCollectionTarget(state->proto.collection, coll, state->resource);
    } catch (const std::exception& e) {
      respondSimple(http::status::bad_request, "application/json",
                    renderErrorBody(e.what()));
      return;
    }

    auto ioPin = makeIoPin();
    node_.getTaskArena().enqueue([self = shared_from_this(), state, ioPin] {
      http::status status = http::status::ok;
      std::string out;
      try {
        std::shared_ptr<Collection> collection =
            self->node_.resolveOrCreateCollection(state->proto.collection ? &*state->proto.collection : nullptr);
        auto shard = collection->getShard();
        auto iw = shard->getIndexWriter();

        class BlockingUpdateMessage : public ProtoUpdateMessage {
        public:
          Blocker blocker;
          explicit BlockingUpdateMessage(const HttpUpdateReqProto* req) : ProtoUpdateMessage(req) {}
          void done(IndexWriter& iw) override {
            unused(iw);
            blocker.notify();
          }
        };

        BlockingUpdateMessage msg(&state->proto);
        bool success = iw->submitUpdate(&msg);
        assert(success);
        unused(success);
        msg.blocker.wait();
        auto* resp = msg.finishResponse();
        if (!solux::api::write_json(*resp, out)) {
          throw std::runtime_error("failed to serialize update response");
        }
      } catch (const CollectionResolutionError& e) {
        status = http::status::bad_request;
        out = renderErrorBody(e.what());
      } catch (const std::exception& e) {
        status = http::status::internal_server_error;
        out = renderErrorBody(e.what());
      }

      net::post(self->stream_.get_executor(),
          [self, ioPin, status, body = std::move(out)]() mutable {
            self->respondSimple(status, "application/json", std::move(body));
          });
    });
  }

  // ---- /_schema -------------------------------------------------------------
  // The schema JSON is a for-humans surface (it gets pasted into forums and
  // docs), so responses are pretty-printed.  GET output is a valid write body:
  // both directions speak the authored source form (Schema::toProto), and
  // POSTing a GET body back is a no-op under either mode.

  static std::string renderSchemaBody(Schema& schema) {
    std::pmr::monotonic_buffer_resource arena;
    solux::api::SchemaDef def;
    schema.toProto(&def, arena);
    std::string compact;
    if (!solux::api::write_json(def, compact)) {
      throw std::runtime_error("failed to serialize schema");
    }
    std::string pretty;
    glz::prettify_json(compact, pretty);
    pretty += '\n';
    return pretty;
  }

  void handleSchemaGet(const std::string& coll) {
    // Read-only: never creates the collection, and the schema is an atomic
    // load + pure render, so this runs inline on the io thread.
    http::status status = http::status::ok;
    std::string out;
    try {
      std::pmr::monotonic_buffer_resource targetResource;
      std::optional<solux::api::Target> target;
      setCollectionTarget(target, coll, targetResource);
      auto collection = node_.resolveCollection(&*target);
      auto schema = collection->getSchema();
      out = renderSchemaBody(*schema);
    } catch (const CollectionResolutionError& e) {
      status = http::status::not_found;
      out = renderErrorBody(e.what());
    } catch (const std::exception& e) {
      status = http::status::internal_server_error;
      out = renderErrorBody(e.what());
    }
    respondSimple(status, "application/json", std::move(out));
  }

  void handleSchemaSet(const std::string& body, const std::string& coll,
                       solux::api::SchemaRequest_::Mode mode) {
    struct SchemaSetState {
      std::pmr::monotonic_buffer_resource resource;
      solux::api::SchemaDef def;  // non-owning; backed by `resource`
    };
    auto state = std::make_shared<SchemaSetState>();
    try {
      std::string err;
      if (!solux::api::read_json(state->def, body, state->resource, &err)) {
        throw std::runtime_error(err.empty() ? "malformed schema" : err);
      }
    } catch (const std::exception& e) {
      respondSimple(http::status::bad_request, "application/json",
                    renderErrorBody(e.what()));
      return;
    }

    // updateSchema persists (fsync) under the collection's schema lock, so run
    // it off the io thread like handleUpdate.
    auto ioPin = makeIoPin();
    node_.getTaskArena().enqueue([self = shared_from_this(), state, ioPin, mode, coll] {
      http::status status = http::status::ok;
      std::string out;
      try {
        std::pmr::monotonic_buffer_resource targetResource;
        std::optional<solux::api::Target> target;
        setCollectionTarget(target, coll, targetResource);
        auto collection = self->node_.resolveOrCreateCollection(&*target);
        auto newSchema = collection->updateSchema(state->def, mode);
        out = renderSchemaBody(*newSchema);
      } catch (const SchemaError& e) {
        status = http::status::bad_request;
        out = renderErrorBody(e.what());
      } catch (const CollectionResolutionError& e) {
        status = http::status::bad_request;
        out = renderErrorBody(e.what());
      } catch (const std::exception& e) {
        status = http::status::internal_server_error;
        out = renderErrorBody(e.what());
      }

      net::post(self->stream_.get_executor(),
          [self, ioPin, status, body = std::move(out)]() mutable {
            self->respondSimple(status, "application/json", std::move(body));
          });
    });
  }

  void respondMethodNotAllowed(std::string_view allow, std::string_view message) {
    auto resp = std::make_shared<http::response<http::string_body>>(
        http::status::method_not_allowed, httpVersion_);
    resp->set(http::field::server, "solux");
    resp->set(http::field::content_type, "application/json");
    resp->set(http::field::allow, allow);
    resp->keep_alive(keepAlive_);
    resp->body() = renderErrorBody(message);
    resp->prepare_payload();
    http::async_write(stream_, *resp,
        [self = shared_from_this(), resp](beast::error_code ec, std::size_t) {
          if (ec) { self->doClose(); return; }
          if (resp->keep_alive()) self->doRead();
          else self->doClose();
        });
  }

  static std::int32_t cappedErrorIndex(std::size_t index) {
    constexpr std::size_t max = (std::size_t)std::numeric_limits<std::int32_t>::max();
    return index > max ? std::numeric_limits<std::int32_t>::max() : (std::int32_t)index;
  }

  static void copyCommitParams(std::optional<solux::api::CommitParams>& out,
                               const solux::api::CommitParams& src,
                               std::pmr::memory_resource& resource) {
    auto& params = out.emplace();
    params.commit_within_us = src.commit_within_us;
    params.wait_for_merges = src.wait_for_merges;
    params.max_segments = src.max_segments;
    std::string_view* names =
        solux::api::build::allocArray(params.build_aux_indexes, src.build_aux_indexes.size(), resource);
    for (std::size_t i = 0; i < src.build_aux_indexes.size(); i++) {
      names[i] = solux::api::build::arenaStr(resource, src.build_aux_indexes[i]);
    }
  }

  static void copyCollectionTarget(std::optional<solux::api::Target>& out,
                                   const solux::api::Target& src,
                                   std::pmr::memory_resource& resource) {
    auto& target = out.emplace();
    std::string_view* names = solux::api::build::allocArray(target.name, src.name.size(), resource);
    for (std::size_t i = 0; i < src.name.size(); i++) {
      names[i] = solux::api::build::arenaStr(resource, src.name[i]);
    }
  }

  static bool validateEndControlPayload(const solux::api::Map& payload, std::string& err) {
    for (const auto& [name, val] : payload.fields) {
      unused(val);
      if (name == "request_id" || name == "commit") continue;
      if (name == "collection" || name == "allow_dups" || name == "all_or_none" ||
          name == "return_ids" || name == "docs" || name == "delete_ids" ||
          name == "columns" || name == "stream_id") {
        err = "_end_ control cannot carry submit-time field '" + std::string(name) + "'";
        return false;
      }
      err = "unsupported _end_ control field '" + std::string(name) + "'";
      return false;
    }
    return true;
  }

  static bool validateUpdateControlPayload(const solux::api::Map& payload, std::string& err) {
    for (const auto& [name, val] : payload.fields) {
      unused(val);
      if (name == "collection" || name == "allow_dups" || name == "all_or_none" ||
          name == "return_ids" || name == "request_id" || name == "commit" ||
          name == "docs" || name == "delete_ids") {
        continue;
      }
      err = "unsupported _update_ control field '" + std::string(name) + "'";
      return false;
    }
    return true;
  }

  static bool decodeStreamControlPayload(std::string_view keyword, const solux::api::Map& payload,
                                         std::size_t recordBytes,
                                         HttpStreamControl& control, std::string& err) {
    std::string json;
    solux::api::Val wrapper;
    wrapper.kind = payload;
    if (!solux::api::write_json(wrapper, json)) {
      err = "failed to serialize " + std::string(keyword) + " control payload";
      return false;
    }

    auto request = std::make_shared<HttpStreamControlRequest>();
    request->sourceBytes = recordBytes;
    if (!solux::api::read_json(request->proto, json, request->resource, &err)) {
      if (err.empty()) err = "malformed " + std::string(keyword) + " control payload";
      return false;
    }
    control.request = std::move(request);
    return true;
  }

  // A control record is `{}` or a JSON object whose sole field is "_update_" or
  // "_end_".  Sole-underscore-field records with OBJECT payloads are reserved
  // as the meta/control namespace: "_header_" (emitted by format=docs query
  // responses, so exported output pipes straight back into /update) is a
  // recognized no-op, and any other object-valued _name_ is an error rather
  // than silently indexed as a document (a control-record typo must not become
  // data).  Scalar-valued sole-underscore fields stay documents - {"_version_":42}
  // is a legitimate one-field export, and every real control carries an object.
  // The control payload is decoded as a full UpdateRequest through the canonical JSON
  // codec so the streaming path accepts the same fields as buffered /update.
  static bool extractStreamControl(const solux::api::Map& map, HttpStreamControl& control,
                                   bool& isControl, std::size_t recordBytes, std::string& err) {
    isControl = false;
    if (map.fields.empty()) {
      isControl = true;
      control.kind = HttpStreamControlKind::Close;
      control.request = std::make_shared<HttpStreamControlRequest>();
      control.request->sourceBytes = recordBytes;
      return true;
    }
    if (map.fields.size() != 1) return true;
    const auto& entry = *map.fields.begin();
    if (entry.first == "_header_") {
      isControl = true;
      control.kind = HttpStreamControlKind::Noop;
      if (std::get_if<solux::api::Map>(&(*entry.second).kind) == nullptr) {
        err = "_header_ control value must be an object";
        return false;
      }
      return true;
    }
    if (entry.first != "_update_" && entry.first != "_end_") {
      if (entry.first.size() >= 2 && entry.first.front() == '_' && entry.first.back() == '_' &&
          std::get_if<solux::api::Map>(&(*entry.second).kind) != nullptr) {
        err = "unknown control record '" + std::string(entry.first) + "'";
        return false;
      }
      return true;
    }

    isControl = true;
    control.kind = entry.first == "_update_" ? HttpStreamControlKind::Open
                                             : HttpStreamControlKind::Close;
    const solux::api::Val& wrapper = *entry.second;
    const auto* payload = std::get_if<solux::api::Map>(&wrapper.kind);
    if (payload == nullptr) {
      err = std::string(entry.first) + " control value must be an object";
      return false;
    }

    if (control.kind == HttpStreamControlKind::Close &&
        !validateEndControlPayload(*payload, err)) {
      return false;
    }
    if (control.kind == HttpStreamControlKind::Open &&
        !validateUpdateControlPayload(*payload, err)) {
      return false;
    }
    return decodeStreamControlPayload(entry.first, *payload, recordBytes, control, err);
  }

  void startStreamingUpdate(std::string coll, bool urlCommit) {
    const auto& ingest = node_.getConfig().ingest;
    auto state = std::make_shared<HttpStreamUpdateState>(
        (std::size_t)ingest.stream_batch_size,
        (std::size_t)ingest.stream_batch_docs,
        (std::size_t)ingest.maxRecordBytes(),
        (std::size_t)ingest.max_request_body);
    state->defaultCollectionName = std::move(coll);
    state->group.collectionName = state->defaultCollectionName;
    state->urlCommit = urlCommit;
    state->ioPin = makeIoPin();

    streamUpdate_ = std::move(state);
    doStreamBodyRead();
  }

  void doStreamBodyRead() {
    auto state = streamUpdate_;
    if (!state || state->failed || state->updateInFlight) return;
    if (parser_->is_done()) {
      state->bodyDone = true;
      drainStreamRecords();
      return;
    }
    parser_->get().body().data = state->readBuf.data();
    parser_->get().body().size = state->readBuf.size();
    // async_read_SOME, not async_read: it delivers whatever bytes are available now
    // (one read), so a partial arrival is processed immediately. This is load-bearing
    // for checkpoints - a client that sends a small batch + a `{}` marker and then
    // blocks waiting for its ack must have those bytes framed+indexed+responded now.
    // async_read would instead wait until readBuf fills or the body ends, so the `{}`
    // ack would never arrive before EOF and the client would deadlock. Do not "optimize"
    // this to async_read.
    http::async_read_some(stream_, buffer_, *parser_,
        beast::bind_front_handler(&HttpSession::onStreamBodyRead, shared_from_this()));
  }

  void onStreamBodyRead(beast::error_code ec, std::size_t) {
    auto state = streamUpdate_;
    if (!state || state->failed) return;

    bool needBuffer = ec == http::error::need_buffer;
    if (ec && !needBuffer) {
      failStreamingUpdate("failed to read NDJSON request body: " + ec.message());
      return;
    }

    std::size_t produced = state->readBuf.size() - parser_->get().body().size;
    if (produced > 0) {
      state->framer.feed(std::string_view(state->readBuf.data(), produced));
      if (state->framer.error()) {
        failStreamingUpdate(state->framer.message());
        return;
      }
    }

    if (parser_->is_done()) state->bodyDone = true;
    drainStreamRecords();
  }

  static bool streamIntervalHasActivity(const HttpStreamUpdateState& state) {
    return state.interval.submitted || state.interval.docCount > 0;
  }

  static void resetStreamInterval(HttpStreamUpdateState& state) {
    state.interval = HttpStreamInterval();
    state.interval.firstDocIndex = state.docsSeen;
  }

  static std::string streamTargetKey(const std::optional<solux::api::Target>& target,
                                     const std::string& defaultCollectionName) {
    if (!target || target->name.empty()) return defaultCollectionName;
    return std::string(target->name.back());
  }

  static std::string_view currentStreamResponseRequestId(const HttpStreamUpdateState& state) {
    if (state.group.closeRequestId) return *state.group.closeRequestId;
    return state.group.requestId;
  }

  static void resetCurrentStreamGroup(HttpStreamUpdateState& state) {
    state.group = {};
    state.group.collectionName = state.defaultCollectionName;
  }

  static bool renderStreamIntervalResponseLine(const HttpStreamUpdateState& state,
                                               std::string_view requestId,
                                               std::string& out) {
    const auto& interval = state.interval;
    std::pmr::monotonic_buffer_resource responseResource;
    solux::api::UpdateResponse resp;
    resp.request_id = solux::api::build::arenaStr(responseResource, requestId);
    resp.update_version = interval.lastUpdateVersion;

    solux::api::build::SpanBuilder<std::string_view> ids(responseResource);
    ids.reserve(interval.ids.size());
    for (const auto& id : interval.ids) ids.push_back(solux::api::build::arenaStr(responseResource, id));
    resp.ids = ids.finish();

    solux::api::build::SpanBuilder<solux::api::UpdateResponse_::Error> errors(responseResource);
    errors.reserve(interval.errors.size());
    for (const auto& src : interval.errors) {
      auto& dst = errors.emplace_back();
      dst.id = solux::api::build::arenaStr(responseResource, src.id);
      dst.error_message = solux::api::build::arenaStr(responseResource, src.errorMessage);
      dst.index = src.index;
    }
    resp.errors = errors.finish();

    if (interval.totalErrors == 0) {
      resp.status = solux::api::UpdateResponse_::Status::OK;
    } else if (interval.anySuccess) {
      resp.status = solux::api::UpdateResponse_::Status::PARTIAL;
    } else {
      resp.status = solux::api::UpdateResponse_::Status::ERROR;
    }
    if (interval.totalErrors > interval.errors.size()) {
      std::string msg = "retained first " + std::to_string(interval.errors.size()) + " of " +
          std::to_string(interval.totalErrors) + " errors";
      resp.error_message = solux::api::build::arenaStr(responseResource, msg);
    }

    out.clear();
    if (!solux::api::write_json(resp, out)) return false;
    out += '\n';
    return true;
  }

  static bool renderStreamErrorResponseLine(std::string_view requestId, std::uint64_t updateVersion,
                                            std::string_view message, std::string& out) {
    std::pmr::monotonic_buffer_resource responseResource;
    solux::api::UpdateResponse resp;
    resp.request_id = solux::api::build::arenaStr(responseResource, requestId);
    resp.update_version = updateVersion;
    resp.status = solux::api::UpdateResponse_::Status::ERROR;
    resp.error_message = solux::api::build::arenaStr(responseResource, message);

    out.clear();
    if (!solux::api::write_json(resp, out)) return false;
    out += '\n';
    return true;
  }

  bool emitCurrentStreamInterval(bool last, bool force) {
    auto state = streamUpdate_;
    assert(state != nullptr);
    if (!force && !streamIntervalHasActivity(*state)) return true;

    std::string out;
    if (!renderStreamIntervalResponseLine(*state, currentStreamResponseRequestId(*state), out)) {
      failStreamingUpdate("failed to serialize update response");
      return false;
    }
    state->emittedLine = true;
    state->group.closeRequestId.reset();
    resetStreamInterval(*state);
    enqueueLine(std::move(out), last);
    return true;
  }

  void failStreamingUpdate(std::string message) {
    auto state = streamUpdate_;
    if (state && state->failed) return;
    std::size_t docsIndexed = state ? state->docsIndexedSoFar : 0;
    if (state) state->failed = true;
    parser_.reset();
    keepAlive_ = false;
    message += " (docs_indexed_so_far=" + std::to_string(docsIndexed) + ")";
    if (headerSent_) {
      std::string out;
      std::string_view requestId = state ? currentStreamResponseRequestId(*state) : std::string_view();
      std::uint64_t updateVersion = state ? state->interval.lastUpdateVersion : 0;
      if (!renderStreamErrorResponseLine(requestId, updateVersion, message, out)) {
        doClose();
        return;
      }
      enqueueLine(std::move(out), true);
      return;
    }
    respondSimple(http::status::bad_request, "application/json", renderErrorBody(message));
  }

  static bool streamRequestHasInlineOps(const HttpUpdateReqProto& request) {
    return !request.docs.empty() || !request.delete_ids.empty();
  }

  void startImplicitStreamGroup() {
    auto state = streamUpdate_;
    assert(state != nullptr);
    if (state->group.open) return;
    resetCurrentStreamGroup(*state);
    state->group.open = true;
  }

  HttpStreamWriterTarget* streamWriterTarget(const std::string& collectionName, std::string& err) {
    auto state = streamUpdate_;
    assert(state != nullptr);
    auto [it, inserted] = state->writerCache.try_emplace(collectionName);
    if (!inserted) return &it->second;

    try {
      std::pmr::monotonic_buffer_resource targetResource;
      std::optional<solux::api::Target> target;
      setCollectionTarget(target, collectionName, targetResource);
      it->second.collection = node_.resolveOrCreateCollection(&*target);
      it->second.indexWriter = it->second.collection->getShard()->getIndexWriter();
    } catch (const std::exception& e) {
      err = e.what();
      state->writerCache.erase(it);
      return nullptr;
    } catch (...) {
      err = "unknown non-standard exception";
      state->writerCache.erase(it);
      return nullptr;
    }
    return &it->second;
  }

  void prepareStreamBatchCollection(HttpStreamBatchState& batch) {
    auto state = streamUpdate_;
    assert(state != nullptr);
    batch.collectionName = state->group.collectionName;
    if (state->group.request && state->group.request->proto.collection) {
      copyCollectionTarget(batch.proto.collection, *state->group.request->proto.collection, batch.resource);
    } else {
      setCollectionTarget(batch.proto.collection, state->group.collectionName, batch.resource);
    }
  }

  void submitPreparedStreamBatch() {
    auto state = streamUpdate_;
    assert(state != nullptr);
    assert(!state->updateInFlight);
    assert(state->batch != nullptr);

    std::string err;
    HttpStreamWriterTarget* target = streamWriterTarget(state->batch->collectionName, err);
    if (target == nullptr) {
      failStreamingUpdate("failed to resolve collection '" + state->batch->collectionName + "': " + err);
      return;
    }

    std::shared_ptr<HttpStreamBatchState> batch(std::move(state->batch));
    state->batch = std::make_unique<HttpStreamBatchState>(state->batchMaxDocs);
    state->updateInFlight = true;

    auto iw = target->indexWriter;
    auto ioPin = state->ioPin;
    node_.getTaskArena().enqueue(
        [self = shared_from_this(), state, batch, iw, ioPin] {
          HttpStreamBatchResult result;
          result.docCount = batch->docCount;
          result.deleteCount = batch->deleteCount;
          result.firstDocIndex = batch->firstDocIndex;
          try {
            class BlockingUpdateMessage : public ProtoUpdateMessage {
            public:
              Blocker blocker;
              explicit BlockingUpdateMessage(const HttpUpdateReqProto* req) : ProtoUpdateMessage(req) {}
              void done(IndexWriter& iw) override {
                unused(iw);
                blocker.notify();
              }
            };

            BlockingUpdateMessage msg(&batch->proto);
            bool success = iw->submitUpdate(&msg);
            assert(success);
            unused(success);
            msg.blocker.wait();
            auto* resp = msg.finishResponse();
            result.status = resp->status;
            result.updateVersion = resp->update_version;
            result.errorMessage = std::string(resp->error_message);
            result.ids.reserve(resp->ids.size());
            for (std::string_view id : resp->ids) result.ids.emplace_back(id);
            result.errors.reserve(resp->errors.size());
            for (const auto& e : resp->errors) {
              result.errors.push_back({std::string(e.id), std::string(e.error_message), e.index});
            }
          } catch (const std::exception& e) {
            result.failed = true;
            result.errorMessage = e.what();
          } catch (...) {
            result.failed = true;
            result.errorMessage = "unknown non-standard exception";
          }

          net::post(self->stream_.get_executor(),
              [self, state, batch, ioPin, result = std::move(result)]() mutable {
                self->onStreamBatchDone(state, batch, std::move(result));
              });
        });
  }

  void accountStreamSubmission(HttpStreamBatchState& batch, std::size_t docCount,
                               std::size_t deleteCount) {
    auto state = streamUpdate_;
    assert(state != nullptr);
    batch.docCount = docCount;
    batch.deleteCount = deleteCount;
    batch.firstDocIndex = state->docsSeen;
    state->docsSeen += batch.docCount;
    state->interval.docCount += batch.docCount;
    state->interval.submitted = true;
  }

  void submitStreamBatch(const solux::api::CommitParams* commitParams) {
    auto state = streamUpdate_;
    assert(state != nullptr);
    assert(state->batch != nullptr);

    accountStreamSubmission(*state->batch, state->batch->docs.size(), 0);
    state->batch->proto.docs = state->batch->docs.finish();
    state->batch->proto.allow_dups = state->group.allowDups();
    state->batch->proto.all_or_none = state->group.allOrNone();
    state->batch->proto.request_id =
        solux::api::build::arenaStr(state->batch->resource, state->group.requestId);
    // Only fetch ids while the interval can still retain more (capped at
    // kMaxRetainedIds); past the cap the engine's ids are discarded in the fold, so
    // skip the work on later slices of a large group.  Strict read/batch alternation
    // means the prior slice's fold already ran, so interval.ids is current here.
    state->batch->proto.return_ids =
        state->group.returnIds() && state->interval.ids.size() < HttpStreamUpdateState::kMaxRetainedIds;
    state->batch->requestOwner = state->group.request;
    prepareStreamBatchCollection(*state->batch);
    if (commitParams != nullptr) {
      copyCommitParams(state->batch->proto.commit, *commitParams, state->batch->resource);
    }

    submitPreparedStreamBatch();
  }

  bool submitInlineStreamRequest(const std::shared_ptr<HttpStreamControlRequest>& request) {
    auto state = streamUpdate_;
    assert(state != nullptr);
    assert(state->batch != nullptr);
    if (request->sourceBytes > state->maxRequestBody) {
      failStreamingUpdate("_update_ inline request exceeds ingest.max-request-body");
      return false;
    }
    if (state->batch->docs.size() != 0) {
      failStreamingUpdate("internal error: inline _update_ encountered a non-empty stream batch");
      return false;
    }

    state->batch->requestOwner = request;
    state->batch->proto = request->proto;
    state->batch->collectionName = state->group.collectionName;
    accountStreamSubmission(*state->batch, request->proto.docs.size(), request->proto.delete_ids.size());
    state->batch->proto.allow_dups = state->group.allowDups();
    state->batch->proto.all_or_none = state->group.allOrNone();
    state->batch->proto.return_ids =
        state->group.returnIds() && state->interval.ids.size() < HttpStreamUpdateState::kMaxRetainedIds;
    if (!state->batch->proto.collection) {
      setCollectionTarget(state->batch->proto.collection, state->group.collectionName, state->batch->resource);
    }
    state->emitAfterBatch = true;
    state->resetGroupAfterBatch = true;
    submitPreparedStreamBatch();
    return false;
  }

  bool closeCurrentStreamGroup(const std::shared_ptr<HttpStreamControlRequest>& closeRequest,
                               bool forceEmit) {
    auto state = streamUpdate_;
    assert(state != nullptr);
    assert(state->batch != nullptr);

    if (closeRequest && !closeRequest->proto.request_id.empty()) {
      state->group.closeRequestId = std::string(closeRequest->proto.request_id);
    }

    const solux::api::CommitParams* commitParams = nullptr;
    if (closeRequest && closeRequest->proto.commit) {
      commitParams = &*closeRequest->proto.commit;
    } else if (state->group.request && state->group.request->proto.commit) {
      commitParams = &*state->group.request->proto.commit;
    }

    if (state->batch->docs.size() > 0 || commitParams != nullptr) {
      state->emitAfterBatch = true;
      state->resetGroupAfterBatch = true;
      submitStreamBatch(commitParams);
      return false;
    }

    if ((forceEmit || streamIntervalHasActivity(*state)) &&
        !emitCurrentStreamInterval(false, true)) {
      return false;
    }
    resetCurrentStreamGroup(*state);
    return true;
  }

  bool openStreamGroup(HttpStreamControl control) {
    auto state = streamUpdate_;
    assert(state != nullptr);
    assert(control.request != nullptr);
    state->group = {};
    state->group.request = std::move(control.request);
    const auto& request = state->group.request->proto;
    state->group.collectionName = streamTargetKey(request.collection, state->defaultCollectionName);
    state->group.requestId = std::string(request.request_id);
    state->group.open = true;

    if (streamRequestHasInlineOps(request)) {
      return submitInlineStreamRequest(state->group.request);
    }
    return true;
  }

  bool applyStreamControl(HttpStreamControl control) {
    auto state = streamUpdate_;
    assert(state != nullptr);
    if (control.kind == HttpStreamControlKind::Open) {
      if (state->group.open || streamIntervalHasActivity(*state) || state->batch->docs.size() > 0) {
        if (!closeCurrentStreamGroup(nullptr, state->group.open)) {
          state->pendingControl = std::move(control);
          return false;
        }
      }
      return openStreamGroup(std::move(control));
    }

    return closeCurrentStreamGroup(control.request, true);
  }

  bool processStreamRecord(std::string_view record) {
    auto state = streamUpdate_;
    assert(state != nullptr);
    assert(state->batch != nullptr);

    solux::api::Map map;
    std::string err;
    if (!solux::api::read_json(map, record, state->batch->resource, &err)) {
      failStreamingUpdate(err.empty() ? "malformed NDJSON record" : err);
      return false;
    }

    HttpStreamControl control;
    bool isControl = false;
    if (!extractStreamControl(map, control, isControl, record.size(), err)) {
      failStreamingUpdate(err);
      return false;
    }

    if (isControl && control.kind != HttpStreamControlKind::Noop) {
      return applyStreamControl(std::move(control));
    }

    if (isControl) {
      // Noop meta record (e.g. _header_).  It was decoded into the batch
      // arena, so it must count toward rotation or a stream of meta records
      // would grow the arena without bound.  With no docs buffered an empty
      // batch carries no state (proto/collection are populated at submission),
      // so past the target it is simply replaced - the same rotation
      // submitStreamBatch performs, minus the submit.  With docs pending, fall
      // through to the shared accounting and let the normal thresholds rotate.
      if (state->batch->docs.size() == 0) {
        state->batch->sourceBytes += record.size();
        if (state->batch->sourceBytes >= state->batchTargetBytes) {
          state->batch = std::make_unique<HttpStreamBatchState>(state->batchMaxDocs);
        }
        return true;
      }
    } else {
      startImplicitStreamGroup();
      state->batch->docs.push_back(map);
    }
    state->batch->sourceBytes += record.size();
    if (state->group.allOrNone()) {
      if (state->batch->sourceBytes > state->maxRequestBody) {
        failStreamingUpdate("all_or_none NDJSON group exceeds ingest.max-request-body");
        return false;
      }
      return true;
    }
    if (state->batch->sourceBytes >= state->batchTargetBytes ||
        state->batch->docs.size() >= state->batchMaxDocs) {
      submitStreamBatch(nullptr);
      return false;
    }
    return true;
  }

  void onStreamBatchDone(const std::shared_ptr<HttpStreamUpdateState>& state,
                         const std::shared_ptr<HttpStreamBatchState>& batch,
                         HttpStreamBatchResult result) {
    unused(batch);
    if (streamUpdate_ != state || state->failed) return;
    state->updateInFlight = false;
    if (result.failed) {
      failStreamingUpdate("NDJSON update batch failed: " + result.errorMessage);
      return;
    }

    foldStreamBatchResult(result);

    if (state->emitAfterBatch) {
      state->emitAfterBatch = false;
      if (!emitCurrentStreamInterval(false, true)) return;
    }
    if (state->resetGroupAfterBatch) {
      state->resetGroupAfterBatch = false;
      resetCurrentStreamGroup(*state);
    }

    if (state->pendingControl) {
      HttpStreamControl control = std::move(*state->pendingControl);
      state->pendingControl.reset();
      if (!applyStreamControl(std::move(control))) return;
    }
    drainStreamRecords();
  }

  void foldStreamBatchResult(const HttpStreamBatchResult& result) {
    auto state = streamUpdate_;
    assert(state != nullptr);
    auto& interval = state->interval;
    interval.lastUpdateVersion = result.updateVersion;

    std::size_t indexedDocs = 0;
    if (result.status != solux::api::UpdateResponse_::Status::ERROR) {
      std::size_t failedDocs = result.errors.size();
      if (failedDocs > result.docCount) failedDocs = result.docCount;
      indexedDocs = result.docCount - failedDocs;
    }
    interval.docsIndexed += indexedDocs;
    state->docsIndexedSoFar += indexedDocs;
    if (result.status != solux::api::UpdateResponse_::Status::ERROR &&
        (indexedDocs > 0 || result.deleteCount > 0)) {
      interval.anySuccess = true;
    }

    for (const auto& id : result.ids) {
      if (interval.ids.size() < HttpStreamUpdateState::kMaxRetainedIds) interval.ids.push_back(id);
    }

    for (const auto& err : result.errors) {
      interval.totalErrors++;
      if (interval.errors.size() >= HttpStreamUpdateState::kMaxRetainedErrors) continue;
      std::size_t globalIndex = result.firstDocIndex;
      if (err.index >= 0) globalIndex += (std::size_t)err.index;
      std::size_t intervalIndex =
          globalIndex >= interval.firstDocIndex ? globalIndex - interval.firstDocIndex : 0;
      interval.errors.push_back({err.id, err.errorMessage, cappedErrorIndex(intervalIndex)});
    }

    if (!result.errorMessage.empty() && result.status == solux::api::UpdateResponse_::Status::ERROR) {
      interval.totalErrors++;
      if (interval.errors.size() < HttpStreamUpdateState::kMaxRetainedErrors) {
        std::size_t intervalIndex =
            result.firstDocIndex >= interval.firstDocIndex ? result.firstDocIndex - interval.firstDocIndex : 0;
        interval.errors.push_back({"", result.errorMessage, cappedErrorIndex(intervalIndex)});
      }
    }
  }

  void drainStreamRecords() {
    auto state = streamUpdate_;
    if (!state || state->failed || state->updateInFlight || state->urlCommitInFlight) return;

    std::string_view record;
    while (state->framer.next(record)) {
      if (!processStreamRecord(record)) return;
      if (state->failed || state->updateInFlight) return;
    }

    if (state->bodyDone) {
      if (!state->tailFinished) {
        state->tailFinished = true;
        if (state->framer.finish(record)) {
          if (!processStreamRecord(record)) return;
          if (state->failed || state->updateInFlight) return;
        } else if (state->framer.error()) {
          failStreamingUpdate(state->framer.message());
          return;
        }
      }

      bool forceEmit = state->group.open || !state->emittedLine;
      if (!closeCurrentStreamGroup(nullptr, forceEmit)) return;
      finishStreamingUpdate();
      return;
    }

    doStreamBodyRead();
  }

  void finishStreamingUpdate() {
    auto state = streamUpdate_;
    if (!state || state->failed) return;
    parser_.reset();

    if (state->urlCommit) {
      std::string err;
      if (streamWriterTarget(state->defaultCollectionName, err) == nullptr) {
        failStreamingUpdate("failed to resolve collection '" + state->defaultCollectionName + "': " + err);
        return;
      }
      submitUrlCommits();
      return;
    }

    if (streamIntervalHasActivity(*state) || !state->emittedLine) {
      emitCurrentStreamInterval(true, true);
      return;
    }
    enqueueLine("", true);
  }

  void submitUrlCommits() {
    auto state = streamUpdate_;
    assert(state != nullptr);
    assert(!state->urlCommitInFlight);

    std::vector<std::pair<std::string, std::shared_ptr<IndexWriter>>> writers;
    writers.reserve(state->writerCache.size());
    for (const auto& [name, target] : state->writerCache) {
      writers.push_back({name, target.indexWriter});
    }
    state->urlCommit = false;
    state->urlCommitInFlight = true;

    auto ioPin = state->ioPin;
    node_.getTaskArena().enqueue(
        [self = shared_from_this(), state, writers = std::move(writers), ioPin] {
          std::string err;
          try {
            for (const auto& [name, writer] : writers) {
              unused(name);
              writer->commit();
            }
          } catch (const std::exception& e) {
            err = e.what();
          } catch (...) {
            err = "unknown non-standard exception";
          }

          net::post(self->stream_.get_executor(),
              [self, state, ioPin, err = std::move(err)]() mutable {
                if (self->streamUpdate_ != state || state->failed) return;
                state->urlCommitInFlight = false;
                if (!err.empty()) {
                  self->failStreamingUpdate("NDJSON EOF commit failed: " + err);
                  return;
                }
                self->finishStreamingUpdate();
              });
        });
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
      // An empty final line means "close the stream, no more data" (streaming ingest
      // ends this way when the last thing before EOF was already a response line, e.g.
      // a trailing commit/`{}` - see finishStreamingUpdate). Skip make_chunk here: a
      // chunk of an empty buffer encodes as `0\r\n\r\n`, which is itself the chunked
      // terminator, so writing it and then make_chunk_last would double-terminate.
      if (p.last && inflightLine_.empty()) {
        chunkLastSent_ = true;
        writeOutstanding_ = true;
        net::async_write(stream_, http::make_chunk_last(),
            beast::bind_front_handler(&HttpSession::onChunkLastWritten, shared_from_this()));
        return;
      }
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
    queuedBytes_.fetch_sub((int64_t)inflightLine_.size(), std::memory_order_relaxed);
    inflightLine_.clear();
    if (ec) { failWrites(ec); return; }
    driveWrites();
    maybeFireDrainWaiters();
  }

  void onChunkLastWritten(beast::error_code ec, std::size_t) {
    writeOutstanding_ = false;
    if (ec) { failWrites(ec); return; }
    finishResponse();
  }

  void failWrites(beast::error_code ec) {
    LOG_TRACE("http write: {}", ec.message());
    errored_ = true;
    aborted_.store(true, std::memory_order_relaxed);
    // Pending entries are just owned strings (their arenas were freed in reply()),
    // so dropping them frees everything.
    int64_t dropped = 0;
    for (const auto& p : pendingQ_) dropped += (int64_t)p.line.size();
    pendingQ_.clear();
    queuedBytes_.fetch_sub(dropped, std::memory_order_relaxed);
    // Wake paused producers so their requests can finish (and be freed); their
    // next reply() observes CANCEL via aborted_.
    maybeFireDrainWaiters();
    doClose();
  }

  // Strand only.  Hands parked producer resumes to the task arena once the
  // queue has drained below low-water (or unconditionally after an error).
  // Must not throw: it runs inside io handlers (ioc->run() has no catch), and
  // a lost waiter strands its request forever - so an enqueue allocation
  // failure falls back to running the resume inline.
  void maybeFireDrainWaiters() {
    if (drainWaiters_.empty()) return;
    if (!errored_ && queuedBytes_.load(std::memory_order_relaxed) > lowWater_) return;
    auto waiters = std::move(drainWaiters_);
    drainWaiters_.clear();
    for (auto& w : waiters) {
      try {
        node_.getTaskArena().enqueue(w);  // copies; w stays valid if this throws
      } catch (...) {
        if (w) w();
      }
    }
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

  // A 413 for a request body past ingest.max-request-body.  The body was not fully
  // consumed, so the connection cannot be reused - respond, then close.
  void respondPayloadTooLarge() {
    keepAlive_ = false;
    if (parser_.has_value()) {
      unsigned v = parser_->get().version();
      if (v != 0) httpVersion_ = v;
    }
    respondSimple(http::status::payload_too_large, "application/json",
                  renderErrorBody("request body exceeds ingest.max-request-body"));
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

SearchRequest::ReplyStatus HttpSearchRequest::reply(SearchResponse& response) {
  if (format == HttpQueryFormat::DOCS) return replyDocs(response);
  // The line is rendered into an owned string, so once it is enqueued the proto
  // (and its arena) are dead weight - free them eagerly here instead of deferring
  // to a post-write callback.  Cleanup is unconditional after the try, so a
  // throwing render/post still releases the arena and lets shutdown drain.
  bool last = response.last;
  auto status = ReplyStatus::OK;
  try {
    response.proto.more = !last;
    if (session->aborted()) {
      status = ReplyStatus::CANCEL;  // connection failed; skip the render
    } else {
      int64_t queued = session->enqueueLine(renderSearchResponseLine(response.proto), last);
      if (queued > session->highWater()) status = ReplyStatus::PAUSE;
    }
  } catch (...) {
    // fall through to cleanup
  }
  if (last) {
    done();  // releases the request arena (this), its work guard, and session ref
  } else if (&response.arena != &arena) {
    releaseArena(&response.arena);  // this batch's own arena
  }
  return status;
}

// format=docs: every line is a document; an optional _header_ meta record
// (found and/or warnings) leads the first reply.  Errors have no in-band form
// here: before any output is committed to the session the request gets a
// plain HTTP error response; after that the chunked stream is aborted without
// its terminator so the client sees truncation rather than a
// complete-looking result.
SearchRequest::ReplyStatus HttpSearchRequest::replyDocs(SearchResponse& response) {
  bool last = response.last;
  auto status = ReplyStatus::OK;
  try {
    if (session->aborted()) {
      status = ReplyStatus::CANCEL;
    } else if (!response.proto.error.empty()) {
      if (docsOutputCommitted) {
        session->abortStream();
      } else {
        // TODO: distinguish request errors from server errors (submitBody has
        // the same gap); everything surfaces as 400 for now.
        session->respondErrorFromEngine(http::status::bad_request,
                                        renderErrorBody(response.proto.error));
      }
    } else {
      // Document bodies are a pure function of the batch: render them OUTSIDE
      // any lock (this is the expensive part).  Only run framing depends on
      // cross-emitter interleaving.
      auto runs = renderDocRuns(response.proto);

      // Multi-op requests have one emitter per op replying concurrently: the
      // framing decisions and the queue posts must agree on order, so both
      // happen under a SHORT critical section (marker render + posts; the
      // strand does the actual writes).  Single-op requests have a single
      // producer whose replies are already sequenced by the completion
      // protocol - no lock, and the normal path is unchanged.
      std::optional<std::lock_guard<std::mutex>> lock;
      if (docsState.multiOp) lock.emplace(mutex);

      std::vector<std::string> outLines;
      for (auto& run : runs) {
        std::string marker;
        if (!frameDocRun(run, docsState, warnings, marker)) continue;
        if (!marker.empty()) outLines.push_back(std::move(marker));
        if (!run.body.empty()) outLines.push_back(std::move(run.body));
      }
      if (outLines.empty()) {
        if (last) session->enqueueLine("", true);  // close the stream
      } else {
        int64_t queued = 0;
        for (size_t i = 0; i < outLines.size(); i++) {
          queued = session->enqueueLine(std::move(outLines[i]), last && i + 1 == outLines.size());
        }
        // Only after enqueueLine accepts the bytes: a throwing dispatch rolls
        // the queue back, and the error path must then still be free to answer
        // with a plain HTTP error rather than abort a stream that never began.
        docsOutputCommitted = true;
        if (queued > session->highWater()) status = ReplyStatus::PAUSE;
      }
    }
  } catch (...) {
    // fall through to cleanup
  }
  if (last) {
    done();  // releases the request arena (this), its work guard, and session ref
  } else if (&response.arena != &arena) {
    releaseArena(&response.arena);  // this batch's own arena
  }
  return status;
}

void HttpSearchRequest::resumeWhenDrained(std::function<void()> resume) {
  session->whenDrained(std::move(resume));
}

HttpServer::HttpServer(SoluxNode& node, int threads, int port, int64_t streamBufferBytes)
  : node(node), nthreads(threads), requestedPort(port),
    // Clamp to >= 1: a non-positive high-water mark (misconfiguration) would
    // pause every reply while the drain check (queuedBytes <= low) never fires.
    streamBufferBytes_(std::max<int64_t>(1,
        streamBufferBytes > 0 ? streamBufferBytes
                              : node.getConfig().server.stream_buffer_bytes)),
    ioc(std::make_shared<net::io_context>()) {}

HttpServer::~HttpServer() { shutdown(); }

void HttpServer::start() {
  if (started) return;
  int n = nthreads > 0 ? nthreads
                       : (int)std::max(1u, std::thread::hardware_concurrency() / 2);

  std::string host = requestedPort == 0 ? "127.0.0.1" : "0.0.0.0";
  tcp::endpoint ep(net::ip::make_address(host), (unsigned short)requestedPort);

  registry = std::make_shared<HttpSessionRegistry>();
  workGuard.emplace(ioc->get_executor());

  // The acceptor runs on its own strand so its operations (async_accept in the
  // accept loop, and close() during shutdown) are serialized on one executor.
  acceptor.emplace(net::make_strand(*ioc));
  acceptor->open(ep.protocol());
  acceptor->set_option(net::socket_base::reuse_address(true));
  acceptor->bind(ep);
  acceptor->listen(net::socket_base::max_listen_connections);
  port_ = acceptor->local_endpoint().port();

  doAccept();

  threads.reserve(n);
  for (int i = 0; i < n; i++) threads.emplace_back([this] { ioc->run(); });
  started = true;
  LOG_INFO("HTTP server listening on {}:{}", host, port_);
}

void HttpServer::doAccept() {
  acceptor->async_accept(net::make_strand(*ioc),
      [this](beast::error_code ec, tcp::socket sock) {
        if (ec == net::error::operation_aborted) return;  // shutting down
        if (!ec) {
          // Small request/response exchanges on a keep-alive connection stall
          // ~40ms per round trip under Nagle + delayed ACK; disable Nagle like
          // every HTTP server does.  Best-effort: an ec here is not fatal.
          beast::error_code nde;
          sock.set_option(tcp::no_delay(true), nde);
          std::make_shared<HttpSession>(ioc, std::move(sock), node, registry,
                                        streamBufferBytes_)->run();
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
  //    This must stay a drain - never ioc->stop().  stop() would leave uninvoked
  //    handlers owned by the context; those handlers hold session refs and each
  //    session co-owns the context, so stopping would create a retention cycle
  //    instead of a clean teardown.
  workGuard.reset();
  for (auto& t : threads) if (t.joinable()) t.join();
  threads.clear();
  started = false;
}

} // namespace solux
