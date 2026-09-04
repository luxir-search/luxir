#include "HttpServer.h"

#include <cassert>
#include <array>
#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
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
#include <atomic>
#include <thread>
#include <utility>
#include <vector>

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/none.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/steady_timer.hpp>

// prettify_json is a pure text transform (no api-type serialization), so this TU needs
// glaze but not the Luxir JSON dialect. prettify.hpp is not self-contained: read_iterators
// comes from core/read.hpp.
#include <glaze/core/read.hpp>
#include <glaze/json/prettify.hpp>

#include "luxir/util/log.h"
#include "luxir/schema/Schema.h"
#include "luxir/search/SearchEngine.h"
#include "luxir/search/SearchRequest.h"
#include "JsonRequest.h"
#include "luxir/api/build.h"
#include "JsonResponse.h"
#include "NdjsonFramer.h"
#include "ProtoUpdateMessage.h"
#include "Stats.h"
#include "luxir/util/thread.h"
#include "luxir/util/ApiError.h"

namespace luxir {

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;

class HttpSession;

class HttpIoShard {
public:
  net::io_context ioc{1};
  net::steady_timer idleTimer{ioc};
  // Shard 0 is the warm floor after its first assignment. The accept thread
  // creates this guard before spawning that thread; shutdown alone resets it.
  std::optional<net::executor_work_guard<net::io_context::executor_type>> floorGuard;
  std::thread thread;
  std::atomic<std::uint64_t> activityEpoch{0};
  // Live connections assigned here. Incremented by the accept thread at
  // assignment, decremented by session teardown (any thread); drives
  // least-connections assignment in doAccept.
  std::atomic<int64_t> liveConnections{0};
};

using HttpSearchReqProto = luxir::api::SearchRequest;
using HttpUpdateReqProto = luxir::api::UpdateRequest;

// Pins one shard on behalf of off-io-thread work (engine / task-arena tasks).
// The guard keeps run() from returning until the work completes; the shared_ptr
// keeps the context alive so its non-owning executor can be destroyed on any
// thread after shutdown has joined the runner. `shard` is declared first so it
// outlives guard teardown. Off-io holders must use HttpSession::makeShardPin().
struct ShardPin {
  std::shared_ptr<HttpIoShard> shard;
  net::executor_work_guard<net::io_context::executor_type> guard;
  explicit ShardPin(std::shared_ptr<HttpIoShard> shard)
    : shard(std::move(shard)), guard(this->shard->ioc.get_executor()) {}
};

// The HTTP status for a classified failure: the error's kind decides, except
// for two conditions with an HTTP idiom of their own.
static http::status httpStatusFor(const ErrorInfo& info) {
  if (info.code == "method_not_allowed") return http::status::method_not_allowed;
  if (info.code == "request_too_large") return http::status::payload_too_large;
  switch (info.kind) {
    case ErrorKind::INVALID_REQUEST: return http::status::bad_request;
    case ErrorKind::NOT_FOUND: return http::status::not_found;
    case ErrorKind::ALREADY_EXISTS: return http::status::conflict;
    case ErrorKind::FAILED_PRECONDITION: return http::status::forbidden;
    case ErrorKind::RESOURCE_EXHAUSTED: return http::status::too_many_requests;
    case ErrorKind::UNAVAILABLE: return http::status::service_unavailable;
    case ErrorKind::INTERNAL:
    case ErrorKind::UNKNOWN: break;
  }
  return http::status::internal_server_error;
}

struct HttpSearchRequestState {
  std::pmr::monotonic_buffer_resource resource;
  HttpSearchReqProto proto;  // non-owning; backed by `resource`
};

// Response shape for /_search.  Selected by the request-level proto field
// SearchRequest.response_format, or the ?format= URL param as an alias (for
// URL-capable clients; the param cannot express an explicit envelope, so
// either source selecting DOCS wins).  The engine produces identical DocList
// batches either way - only the NDJSON line framing differs - and gRPC
// (framed messages) rejects DOCS outright.
enum class HttpSearchFormat {
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
  ErrorInfo error;
  std::int32_t index = 0;
};

struct HttpStreamBatchState {
  std::pmr::monotonic_buffer_resource resource;
  HttpUpdateReqProto proto;  // non-owning; backed by `resource`
  luxir::api::build::SpanBuilder<luxir::api::Map> docs;
  std::shared_ptr<HttpStreamControlRequest> requestOwner;
  std::string collectionName;
  std::size_t sourceBytes = 0;
  std::size_t docCount = 0;
  std::size_t deleteCount = 0;
  std::size_t firstDocIndex = 0;
  // The engine receives a raw UpdateMessage pointer.  The in-flight map owns
  // this batch, and the batch owns the message until the shard folds its
  // completion.
  std::shared_ptr<ProtoUpdateMessage> message;

  explicit HttpStreamBatchState(std::size_t reserveDocs) : docs(resource) {
    docs.reserve(reserveDocs);
  }
};

struct HttpStreamBatchResult {
  std::vector<std::string> ids;
  std::vector<HttpStreamAccumError> errors;
  // The batch's request-level failure (status ERROR), or the completion's own
  // failure when `failed`.
  std::optional<ErrorInfo> error;
  luxir::api::UpdateResponse_::Status status = luxir::api::UpdateResponse_::Status::OK;
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
  bool dropUnmapped() const { return request && request->proto.drop_unmapped; }
  // View into the opening _update_'s arena; batches keep it alive via requestOwner.
  luxir::api::map_view<std::string_view, std::string_view> fieldMap() const {
    return request ? request->proto.field_map
                   : luxir::api::map_view<std::string_view, std::string_view>{};
  }
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
  bool batchReady = false;
  std::map<std::uint64_t, std::shared_ptr<HttpStreamBatchState>> inFlightBatches;
  std::map<std::uint64_t, HttpStreamBatchResult> completedBatchResults;
  std::optional<HttpStreamControl> pendingControl;
  std::optional<ErrorInfo> inputFailurePending;
  std::shared_ptr<ShardPin> shardPin;
  HttpStreamInterval interval;
  std::size_t docsSeen = 0;
  std::size_t docsIndexedSoFar = 0;
  std::size_t inFlight = 0;
  std::size_t maxInFlight;
  std::uint64_t nextSubmitOrdinal = 0;
  std::uint64_t nextFoldOrdinal = 0;
  // Stream-level field-map default from ?field_map=/?drop_unmapped= URL params; a
  // group that sets either knob overrides the pair for its docs.
  std::vector<std::pair<std::string, std::string>> urlFieldMap;
  bool urlDropUnmapped = false;
  bool urlCommit = false;
  bool emitAfterBatch = false;
  bool resetGroupAfterBatch = false;
  bool emittedLine = false;
  bool bodyDone = false;
  bool tailFinished = false;
  bool readInFlight = false;
  bool barrierPending = false;
  bool eofPending = false;
  bool urlCommitInFlight = false;
  bool failed = false;

  HttpStreamUpdateState(std::size_t batchTargetBytes, std::size_t batchMaxDocs,
                        std::size_t maxRecordBytes, std::size_t maxRequestBody,
                        std::size_t maxInFlight)
    : batchTargetBytes(batchTargetBytes),
      batchMaxDocs(batchMaxDocs),
      maxRequestBody(maxRequestBody),
      framer(maxRecordBytes),
      readBuf(kStreamReadBufBytes),
      batch(std::make_unique<HttpStreamBatchState>(batchMaxDocs)),
      maxInFlight(maxInFlight) {
    assert(maxInFlight > 0);
  }

  bool canAdmit() const { return inFlight < maxInFlight && !barrierPending; }
};

// One SearchRequest per HTTP query.  Engine workers call reply() from a task
// arena thread; it renders one NDJSON line and hands it to the session shard.
// Holds a shared_ptr to the session so the connection outlives in-flight work,
// and a ShardPin so shutdown drains this request and its shard survives the
// request's teardown here on the task-arena thread.
class HttpSearchRequest : public SearchRequest {
public:
  std::unique_ptr<HttpSearchRequestState> requestState;
  std::shared_ptr<HttpSession> session;
  std::shared_ptr<ShardPin> shardPin;
  HttpSearchFormat format = HttpSearchFormat::ENVELOPE;
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
  bool outputCommitted = false;

  HttpSearchRequest(SearchEngine& engine, std::unique_ptr<HttpSearchRequestState> requestState,
                    std::shared_ptr<HttpSession> s, google::protobuf::Arena& arena)
    : SearchRequest(engine, requestState->proto, arena),
      requestState(std::move(requestState)),
      session(std::move(s)) {
    docFormatDefault = luxir::api::DocFormat::ROWS;
  }

  // All defined after HttpSession.
  ReplyStatus reply(SearchResponse& response) override;
  ReplyStatus replyDocs(SearchResponse& response);
  // A response could not be rendered or queued: the client must not wait for
  // a final line that never comes.  Before any output, answer with an internal
  // error; after it, abort the stream so the truncation is visible.
  void failDelivery(const SearchResponse& response) noexcept;
  void resumeWhenDrained(std::function<void()> resume) override;
  // done() is inherited: releaseArena(&arena) frees this request (and its
  // ShardPin). reply() calls it eagerly once the final line is enqueued - the line
  // owns its bytes, so cleanup does not wait for the write to complete.
};

// Per-connection state. All socket access and mutable session state live on the
// connection's single-threaded shard; cross-thread producers enter through the
// executor-facing helpers below.
class HttpSession : public std::enable_shared_from_this<HttpSession> {
public:
  HttpSession(std::shared_ptr<HttpIoShard> shard, tcp::socket&& sock, LuxirNode& node,
              std::shared_ptr<HttpSessionRegistry> registry, int64_t streamBufferBytes)
    : shard_(std::move(shard)), stream_(std::move(sock)), node_(node), registry_(std::move(registry)),
      highWater_(streamBufferBytes), lowWater_(streamBufferBytes / 2) {}

  ~HttpSession() {
    if (deregister_) deregister_();
    // Publish activity before publishing the transition to zero. The idle
    // timer's acquire load of liveConnections then observes this epoch bump.
    shard_->activityEpoch.fetch_add(1, std::memory_order_release);
    shard_->liveConnections.fetch_sub(1, std::memory_order_release);
  }

  void run();  // defined after HttpSessionRegistry (touches the registry)

  // Posts a socket close onto this session's shard, unblocking any outstanding
  // read/write so the connection drains during shutdown.
  void closeFromServer() {
    net::post(stream_.get_executor(), [self = shared_from_this()] {
      beast::error_code ec;
      self->stream_.socket().shutdown(tcp::socket::shutdown_both, ec);
      self->stream_.socket().close(ec);
    });
  }

  // Callable from any thread.  Queues a rendered NDJSON line (an owned string)
  // on the shard. No completion callback: reply() already freed the arena the
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
      // shard) could propagate a post-queue failure into that catch and
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

  // Callable from any thread.  Parks a paused producer's resume callback; it
  // is fired once buffered bytes drop below the low-water mark, or
  // immediately if the connection has failed (the resumed producer's next
  // reply() then observes CANCEL). Registration runs on the shard, serialized
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

  // Every off-io-thread holder of this session must pin its shard through this
  // helper, never via a raw executor_work_guard.
  std::shared_ptr<ShardPin> makeShardPin() {
    return std::make_shared<ShardPin>(shard_);
  }

  LuxirNode& node() { return node_; }

private:
  struct Pending { std::string line; bool last; };

  // The last session ref can drop on a task-arena thread after shutdown has
  // joined. Declared first so the shard is destroyed after stream_.
  std::shared_ptr<HttpIoShard> shard_;
  beast::tcp_stream stream_;
  LuxirNode& node_;
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
  // it is the only cross-thread piece - everything else is shard-only.
  std::atomic<int64_t> queuedBytes_{0};
  std::atomic<bool> aborted_{false};  // producer-visible mirror of errored_
  int64_t highWater_;
  int64_t lowWater_;
  std::vector<std::function<void()>> drainWaiters_;  // parked producer resumes

  // Streaming write state - shard only.
  std::deque<Pending> pendingQ_;
  std::string inflightLine_;  // owns the chunk buffer during its async_write
  std::optional<http::response<http::empty_body>> res_;
  std::optional<http::response_serializer<http::empty_body>> sr_;
  bool writeOutstanding_ = false;
  std::string requestId_;  // the current request's id (URL param, then the parsed body)
  bool headerSent_ = false;
  bool lastSeen_ = false;
  bool chunkLastSent_ = false;
  bool errored_ = false;
  bool terminalStreamingFailure_ = false;

  // Engine completion runs on the update graph.  Extract every non-owning
  // response field there, then hand an owning result to the session shard.
  // The in-flight batch entry owns this message; the callback keeps it alive
  // while that entry is erased, including the case where the shard runs
  // before done() has returned on the engine thread.
  class StreamingUpdateMessage final
    : public ProtoUpdateMessage,
      public std::enable_shared_from_this<StreamingUpdateMessage> {
    std::shared_ptr<HttpSession> session_;
    std::shared_ptr<HttpStreamUpdateState> state_;
    std::weak_ptr<HttpStreamBatchState> batch_;
    std::uint64_t ordinal_;
    HttpStreamBatchResult result_;

  public:
    StreamingUpdateMessage(const HttpUpdateReqProto* req,
                           std::shared_ptr<HttpSession> session,
                           std::shared_ptr<HttpStreamUpdateState> state,
                           std::uint64_t ordinal,
                           const std::shared_ptr<HttpStreamBatchState>& batch)
      : ProtoUpdateMessage(req),
        session_(std::move(session)),
        state_(std::move(state)),
        batch_(batch),
        ordinal_(ordinal) {
      result_.docCount = batch->docCount;
      result_.deleteCount = batch->deleteCount;
      result_.firstDocIndex = batch->firstDocIndex;
    }

    void done(IndexWriter& iw) noexcept override {
      try {
        unused(iw);
        try {
          auto* resp = finishResponse();
          result_.status = resp->status;
          result_.updateVersion = resp->update_version;
          if (resp->error) result_.error = luxir::api::build::errorInfo(*resp->error);
          result_.ids.reserve(resp->ids.size());
          for (std::string_view id : resp->ids) result_.ids.emplace_back(id);
          result_.errors.reserve(resp->errors.size());
          for (const auto& e : resp->errors) {
            result_.errors.push_back({std::string(e.id),
                                      e.error ? luxir::api::build::errorInfo(*e.error)
                                              : ErrorInfo::of(ErrorKind::INTERNAL,
                                                              "document error without detail"),
                                      e.index});
          }
        } catch (...) {
          result_.failed = true;
          result_.error = currentExceptionInfo(ErrorKind::INTERNAL);
        }

        auto keepAlive = shared_from_this();
        auto batchGuard = batch_.lock();
        assert(batchGuard != nullptr);
        // post still allocates handler storage.  If that allocation fails, the
        // outer catch protects the shared update graph, but this connection
        // loses its completion and remains pinned until bounded-drain recovery
        // is added.
        net::post(session_->stream_.get_executor(),
            [session = session_, state = state_, ordinal = ordinal_,
             keepAlive = std::move(keepAlive), batchGuard,
             result = std::move(result_)]() mutable {
              session->onStreamBatchDone(state, ordinal, std::move(result));
              unused(keepAlive);
              unused(batchGuard);
            });
      } catch (const std::exception& e) {
        try {
          LOG_ERROR("StreamingUpdateMessage::done completion handoff failed: msg={} "
                    "ordinal={} exception={}", (void*)this, ordinal_, e.what());
        } catch (...) {
        }
      } catch (...) {
        try {
          LOG_ERROR("StreamingUpdateMessage::done completion handoff failed: msg={} "
                    "ordinal={} unknown exception", (void*)this, ordinal_);
        } catch (...) {
        }
      }
    }
  };

  void doRead() {
    parser_.emplace();
    // The buffered request-body cap is applied after header routing. Streaming
    // NDJSON must be able to carry an unbounded Content-Length; atomic groups are
    // capped explicitly by their group accumulator instead.
    parser_->body_limit(boost::none);
    buffer_.clear();
    bufferedBody_.clear();
    streamUpdate_.reset();
    terminalStreamingFailure_ = false;
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

    // A URL request_id is known before the body arrives, so it identifies even
    // a request whose body never parses or is refused unread as too large.  A
    // parsed body's request_id replaces it.
    std::vector<UrlParam> params = parseParams(query);
    requestId_.clear();
    if (const std::string* id = findParam(params, "request_id")) requestId_ = *id;

    std::string coll;
    if (parser_->get().method() == http::verb::post && parseUpdatePath(target, coll) &&
        isNdjsonContentType(std::string_view(parser_->get()[http::field::content_type]))) {
      bool urlCommit = false;
      if (const std::string* commit = findParam(params, "commit")) {
        if (*commit != "true") {
          respondError(ErrorInfo::of(ErrorKind::INVALID_REQUEST,
                                     "unknown commit mode '" + *commit + "' (valid: true)"));
          return;
        }
        urlCommit = true;
      }
      std::vector<std::pair<std::string, std::string>> urlFieldMap;
      bool urlDropUnmapped = false;
      std::string paramErr;
      if (!parseFieldMapParams(params, urlFieldMap, paramErr) ||
          !parseDropUnmappedParam(params, urlDropUnmapped, paramErr)) {
        respondError(ErrorInfo::of(ErrorKind::INVALID_REQUEST, paramErr));
        return;
      }
      // Refuse before lifting the body limit: otherwise a read-only node reads an
      // unbounded NDJSON stream only to reject it.  The body is unread, so like
      // respondPayloadTooLarge() this ends the connection rather than reusing it.
      if (node_.readOnly()) {
        keepAlive_ = false;
        respondError(readOnlyError(target));
        return;
      }
      parser_->body_limit(boost::none);
      startStreamingUpdate(std::move(coll), urlCommit, std::move(urlFieldMap), urlDropUnmapped);
      return;
    }

    std::uint64_t bodyLimit = (std::uint64_t)node_.getConfig().ingest.max_request_body;
    if (parser_->content_length() && *parser_->content_length() > bodyLimit) {
      // The body will not be read, so classify from the headers alone: an
      // unknown path, a wrong method, or a read-only refusal outranks the size
      // of a body nobody would have consumed.  Each answer closes the
      // connection, as respondPayloadTooLarge() does.
      RouteMatch match = matchRoute(target);
      keepAlive_ = false;
      if (match.route == Route::NONE) {
        respondError(ErrorInfo::of(ErrorKind::NOT_FOUND, "no such route: " + std::string(target)));
      } else if (!methodAllowed(match.allow, parser_->get().method())) {
        respondMethodNotAllowed(match, parser_->get().method(), target);
      } else if (node_.readOnly() && isMutatingRequest(parser_->get().method(), target)) {
        respondError(readOnlyError(target));
      } else {
        respondPayloadTooLarge();
      }
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

  // The write surface, named once.  A read-only node refuses these at dispatch;
  // storage refuses them again (see ReadOnlyDirectory), but only this gate can
  // produce a decent error, and only it stops the work before it starts.
  static bool isMutatingRequest(http::verb method, std::string_view target) {
    if (target == "/collections/_create" || target == "/collections/_delete") return true;
    if (method != http::verb::post) return false;
    std::string coll;
    return parseUpdatePath(target, coll) || parseSchemaPath(target, coll);
  }

  static ErrorInfo readOnlyError(std::string_view target) {
    return {ErrorKind::FAILED_PRECONDITION, "read_only",
            "node is read-only (--read-only): " + std::string(target) + " is not allowed"};
  }

  static bool parseCollectionPath(std::string_view target, std::string_view suffix, std::string& coll) {
    // /collections/{c}/{endpoint}  (route() strips any ?query-string before matching)
    constexpr std::string_view pre = "/collections/";
    if (!target.starts_with(pre) || !target.ends_with(suffix)) return false;
    auto name = target.substr(pre.size(), target.size() - pre.size() - suffix.size());
    coll.assign(name);
    return true;
  }

  static bool parseSearchPath(std::string_view target, std::string& coll) {
    return parseCollectionPath(target, "/_search", coll);
  }

  static bool parseUpdatePath(std::string_view target, std::string& coll) {
    return parseCollectionPath(target, "/_update", coll);
  }

  static bool parseSchemaPath(std::string_view target, std::string& coll) {
    return parseCollectionPath(target, "/_schema", coll);
  }

  static bool parseStatsPath(std::string_view target, std::string& coll) {
    return parseCollectionPath(target, "/_stats", coll);
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

  struct SearchUrlOverlay {
    std::string json;
    bool hasTopDocs = false;
    bool empty = true;
  };

  static void appendJsonStringLiteral(std::string& out, std::string_view value) {
    static constexpr char hex[] = "0123456789abcdef";
    out += '"';
    for (char c : value) {
      switch (c) {
        case '"': out += R"(\")"; break;
        case '\\': out += R"(\\)"; break;
        case '\b': out += R"(\b)"; break;
        case '\f': out += R"(\f)"; break;
        case '\n': out += R"(\n)"; break;
        case '\r': out += R"(\r)"; break;
        case '\t': out += R"(\t)"; break;
        default: {
          auto u = (unsigned char)c;
          if (u < 0x20) {
            out += R"(\u00)";
            out += hex[u >> 4];
            out += hex[u & 0x0f];
          } else {
            out += c;
          }
        }
      }
    }
    out += '"';
  }

  struct OverlayJsonBuilder {
    std::string json{"{"};
    bool first = true;

    void key(std::string_view name) {
      if (!first) json += ',';
      first = false;
      appendJsonStringLiteral(json, name);
      json += ':';
    }

    void string(std::string_view name, std::string_view value) {
      key(name);
      appendJsonStringLiteral(json, value);
    }

    void raw(std::string_view name, std::string_view value) {
      key(name);
      json += value;
    }

    std::string finish() {
      json += '}';
      return std::move(json);
    }
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

  template <typename T>
  static bool appendIntegerParam(const std::vector<UrlParam>& params, std::string_view name,
                                 std::string_view expected, bool topDocs,
                                 OverlayJsonBuilder& json, SearchUrlOverlay& overlay,
                                 std::string& err) {
    const std::string* text = findParam(params, name);
    if (text == nullptr) return true;
    T value{};
    auto [end, ec] = std::from_chars(text->data(), text->data() + text->size(), value);
    if (ec != std::errc{} || end != text->data() + text->size()) {
      err = "invalid URL parameter '" + std::string(name) + "': expected " +
            std::string(expected) + ", got '" + *text + "'";
      return false;
    }
    json.raw(name, std::to_string(value));
    overlay.hasTopDocs |= topDocs;
    return true;
  }

  static bool appendBoolParam(const std::vector<UrlParam>& params, std::string_view name,
                              bool topDocs, OverlayJsonBuilder& json,
                              SearchUrlOverlay& overlay, std::string& err) {
    const std::string* text = findParam(params, name);
    if (text == nullptr) return true;
    if (*text != "true" && *text != "false") {
      err = "invalid URL parameter '" + std::string(name) +
            "': expected true or false, got '" + *text + "'";
      return false;
    }
    json.raw(name, *text);
    overlay.hasTopDocs |= topDocs;
    return true;
  }

  static void appendStringParam(const std::vector<UrlParam>& params, std::string_view name,
                                bool topDocs, OverlayJsonBuilder& json,
                                SearchUrlOverlay& overlay) {
    if (const std::string* text = findParam(params, name)) {
      json.string(name, *text);
      overlay.hasTopDocs |= topDocs;
    }
  }

  static bool asciiWhitespace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
  }

  static bool parseSortClause(std::string_view text, std::string_view& expr,
                              std::optional<std::string_view>& dir) {
    std::size_t end = text.size();
    while (end > 0 && asciiWhitespace(text[end - 1])) end--;
    if (end == 0) return false;

    std::size_t tokenStart = end;
    while (tokenStart > 0 && !asciiWhitespace(text[tokenStart - 1])) tokenStart--;
    std::string_view token = text.substr(tokenStart, end - tokenStart);
    if (token == "asc" || token == "desc") {
      std::size_t exprEnd = tokenStart;
      while (exprEnd > 0 && asciiWhitespace(text[exprEnd - 1])) exprEnd--;
      if (exprEnd == 0) return false;
      expr = text.substr(0, exprEnd);
      dir = token;
    } else {
      expr = text.substr(0, end);
      dir.reset();
    }
    return true;
  }

  static bool appendFieldsParam(const std::vector<UrlParam>& params,
                                OverlayJsonBuilder& json, SearchUrlOverlay& overlay,
                                std::string& err) {
    const std::string* text = findParam(params, "fields");
    if (text == nullptr) return true;
    overlay.hasTopDocs = true;
    json.key("fields");
    json.json += '[';
    if (!text->empty()) {
      std::size_t start = 0;
      bool first = true;
      for (;;) {
        std::size_t comma = text->find(',', start);
        std::string_view field(*text);
        field = field.substr(start, comma == std::string::npos ? comma : comma - start);
        if (field.empty()) {
          err = "invalid URL parameter 'fields': empty list item in '" + *text + "'";
          return false;
        }
        if (!first) json.json += ',';
        first = false;
        appendJsonStringLiteral(json.json, field);
        if (comma == std::string::npos) break;
        start = comma + 1;
      }
    }
    json.json += ']';
    return true;
  }

  static bool appendSortParams(const std::vector<UrlParam>& params,
                               OverlayJsonBuilder& json, SearchUrlOverlay& overlay,
                               std::string& err) {
    std::vector<std::string_view> values;
    bool haveEmpty = false;
    bool haveNonEmpty = false;
    for (const auto& param : params) {
      if (param.key != "sort") continue;
      values.push_back(param.value);
      haveEmpty |= param.value.empty();
      haveNonEmpty |= !param.value.empty();
    }
    if (values.empty()) return true;
    if (haveEmpty && haveNonEmpty) {
      err = "invalid URL parameter 'sort': empty value cannot be combined with other sort values";
      return false;
    }

    overlay.hasTopDocs = true;
    // The body field is plural "sorts". The singular URL name is deliberate:
    // each repeated sort parameter contributes one ordered clause.
    json.key("sorts");
    json.json += '[';
    if (haveNonEmpty) {
      bool first = true;
      for (std::string_view value : values) {
        std::string_view expr;
        std::optional<std::string_view> dir;
        if (!parseSortClause(value, expr, dir)) {
          err = "invalid URL parameter 'sort': expected a non-empty expression optionally "
                "followed by ' asc' or ' desc', got '" + std::string(value) + "'";
          return false;
        }
        if (!first) json.json += ',';
        first = false;
        json.json += R"({"field":)";
        appendJsonStringLiteral(json.json, expr);
        if (dir) {
          json.json += R"(,"dir":)";
          appendJsonStringLiteral(json.json, *dir);
        }
        json.json += '}';
      }
    }
    json.json += ']';
    return true;
  }

  static bool parseSearchUrlOverlay(const std::vector<UrlParam>& params,
                                    SearchUrlOverlay& overlay, std::string& err) {
    OverlayJsonBuilder json;
    appendStringParam(params, "request_id", false, json, overlay);
    if (!appendIntegerParam<std::uint64_t>(params, "freshness_ms",
                                          "unsigned 64-bit decimal integer", false,
                                          json, overlay, err)) return false;
    appendStringParam(params, "time_zone", false, json, overlay);
    if (!appendBoolParam(params, "profile", false, json, overlay, err)) return false;
    if (!appendIntegerParam<std::int32_t>(params, "max_parallel",
                                         "signed 32-bit decimal integer", false,
                                         json, overlay, err)) return false;

    appendStringParam(params, "query", true, json, overlay);
    if (!appendIntegerParam<std::int64_t>(params, "limit",
                                         "signed 64-bit decimal integer", true,
                                         json, overlay, err) ||
        !appendIntegerParam<std::int64_t>(params, "offset",
                                         "signed 64-bit decimal integer", true,
                                         json, overlay, err) ||
        !appendFieldsParam(params, json, overlay, err) ||
        !appendSortParams(params, json, overlay, err) ||
        !appendIntegerParam<std::int32_t>(params, "batch_size",
                                         "signed 32-bit decimal integer", true,
                                         json, overlay, err)) {
      return false;
    }
    if (const std::string* text = findParam(params, "document_format")) {
      if (*text != "default" && *text != "rows" && *text != "columns") {
        err = "invalid URL parameter 'document_format': expected default, rows, or columns, got '" +
              *text + "'";
        return false;
      }
      json.string("document_format", *text);
      overlay.hasTopDocs = true;
    }
    if (!appendBoolParam(params, "get_number", true, json, overlay, err) ||
        !appendBoolParam(params, "get_scores", true, json, overlay, err)) {
      return false;
    }

    overlay.empty = json.first;
    overlay.json = json.finish();
    return true;
  }

  // Known paths answer a wrong method with 405 and an Allow header; only an
  // unknown path is 404.  A read-only node then refuses mutations with 403.
  enum class Route {
    NONE, HEALTH, COLLECTION_LIST, COLLECTION_CREATE, COLLECTION_DELETE, SEARCH, UPDATE, STATS, SCHEMA
  };

  struct RouteMatch {
    Route route = Route::NONE;
    std::string_view allow;  // the methods the path accepts, as an Allow header value
    std::string_view hint;   // appended to a 405 message when the verb choice needs teaching
    std::string coll;
  };

  static RouteMatch matchRoute(std::string_view target) {
    RouteMatch m;
    if (target == "/health") {
      m.route = Route::HEALTH; m.allow = "GET";
    } else if (target == "/collections/_list") {
      // The canonical _list spelling also accepts POST (matching the other
      // _-verb endpoints; any body is ignored - the request has no parameters).
      m.route = Route::COLLECTION_LIST; m.allow = "GET, POST";
    } else if (target == "/collections") {
      m.route = Route::COLLECTION_LIST; m.allow = "GET";  // synonym for _list
    } else if (target == "/collections/_create") {
      m.route = Route::COLLECTION_CREATE; m.allow = "POST, PUT";
    } else if (target == "/collections/_delete") {
      m.route = Route::COLLECTION_DELETE; m.allow = "POST";
    } else if (parseSearchPath(target, m.coll)) {
      m.route = Route::SEARCH; m.allow = "GET, POST";
    } else if (parseUpdatePath(target, m.coll)) {
      m.route = Route::UPDATE; m.allow = "POST";
    } else if (target == "/_stats" || parseStatsPath(target, m.coll)) {
      m.route = Route::STATS; m.allow = "GET";
    } else if (parseSchemaPath(target, m.coll)) {
      m.route = Route::SCHEMA; m.allow = "GET, POST";
      m.hint = "schema writes are POST (mode=set adds or replaces the named definitions, "
               "mode=replace_all replaces the whole schema)";
    }
    return m;
  }

  static bool methodAllowed(std::string_view allow, http::verb method) {
    std::string_view name = http::to_string(method);
    while (!allow.empty()) {
      auto comma = allow.find(',');
      if (allow.substr(0, comma) == name) return true;
      if (comma == std::string_view::npos) break;
      allow.remove_prefix(comma + 1);
      while (!allow.empty() && allow.front() == ' ') allow.remove_prefix(1);
    }
    return false;
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

    RouteMatch match = matchRoute(target);
    if (match.route == Route::NONE) {
      respondError(ErrorInfo::of(ErrorKind::NOT_FOUND, "no such route: " + std::string(target)));
      return;
    }
    if (!methodAllowed(match.allow, req.method())) {
      respondMethodNotAllowed(match, req.method(), target);
      return;
    }
    if (node_.readOnly() && isMutatingRequest(req.method(), target)) {
      respondError(readOnlyError(target));
      return;
    }

    const std::string& coll = match.coll;
    switch (match.route) {
      case Route::HEALTH:
        respondSimple(http::status::ok, "application/json", R"({"status":"ok"})");
        break;
      case Route::COLLECTION_LIST:
        handleCollectionList();
        break;
      case Route::COLLECTION_CREATE:
        handleCollectionCreate(req.body());
        break;
      case Route::COLLECTION_DELETE:
        handleCollectionDelete(req.body());
        break;
      case Route::SEARCH: {
        if (req.method() == http::verb::get && !req.body().empty()) {
          respondError(ErrorInfo::of(ErrorKind::INVALID_REQUEST,
                                     "GET _search does not accept a request body; use POST"));
          return;
        }
        SearchUrlOverlay overlay;
        std::string overlayErr;
        if (!parseSearchUrlOverlay(params, overlay, overlayErr)) {
          respondError(ErrorInfo::of(ErrorKind::INVALID_REQUEST, overlayErr));
          return;
        }
        auto format = HttpSearchFormat::ENVELOPE;
        if (const std::string* f = findParam(params, "format")) {
          if (*f == "docs") {
            format = HttpSearchFormat::DOCS;
          } else {
            respondError(ErrorInfo::of(ErrorKind::INVALID_REQUEST,
                                       "unknown format '" + *f + "' (valid: docs)"));
            return;
          }
        }
        if (const std::string* explain = findParam(params, "explain")) {
          if (*explain != "request") {
            respondError(ErrorInfo::of(ErrorKind::INVALID_REQUEST,
                                       "unknown explain mode '" + *explain + "' (valid: request)"));
            return;
          }
          if (format != HttpSearchFormat::ENVELOPE) {
            respondError(ErrorInfo::of(ErrorKind::INVALID_REQUEST,
                                       "format=docs cannot be combined with explain"));
            return;
          }
          handleExplain(req.body(), coll, overlay);
        } else {
          handleSearch(req.body(), coll, format, overlay);
        }
        break;
      }
      case Route::UPDATE: {
        std::vector<std::pair<std::string, std::string>> urlFieldMap;
        bool urlDropUnmapped = false;
        std::string paramErr;
        if (!parseFieldMapParams(params, urlFieldMap, paramErr) ||
            !parseDropUnmappedParam(params, urlDropUnmapped, paramErr)) {
          respondError(ErrorInfo::of(ErrorKind::INVALID_REQUEST, paramErr));
          return;
        }
        handleUpdate(req.body(), coll, urlFieldMap, urlDropUnmapped);
        break;
      }
      case Route::STATS: {
        bool includeSegments = false;
        if (const std::string* value = findParam(params, "segments")) {
          if (*value == "true") {
            includeSegments = true;
          } else if (*value != "false") {
            respondError(ErrorInfo::of(ErrorKind::INVALID_REQUEST,
                                       "invalid segments value '" + *value +
                                           "' (valid: true, false)"));
            return;
          }
        }
        handleStats(target == "/_stats" ? std::nullopt : std::optional<std::string>(coll),
                    includeSegments);
        break;
      }
      case Route::SCHEMA: {
        // Writes are POST; the operation is the visible, typeable ?mode= param,
        // never an invisible HTTP verb.  mode=set (the default, and curl's
        // zero-flag path) sets each named definition exactly; the destructive
        // mode=replace_all must be typed.  PUT/PATCH are reserved.
        if (req.method() == http::verb::get) {
          handleSchemaGet(coll);
          break;
        }
        auto mode = luxir::api::SchemaRequest_::Mode::SET;
        if (const std::string* m = findParam(params, "mode")) {
          if (*m == "replace_all") {
            mode = luxir::api::SchemaRequest_::Mode::REPLACE_ALL;
          } else if (*m != "set") {
            respondError(ErrorInfo::of(ErrorKind::INVALID_REQUEST,
                                       "unknown mode '" + *m + "' (valid: set, replace_all)"));
            return;
          }
        }
        handleSchemaSet(req.body(), coll, mode);
        break;
      }
      case Route::NONE:
        break;
    }
  }

  static void setCollectionTarget(std::optional<luxir::api::Target>& collection,
                                  const std::string& coll,
                                  std::pmr::memory_resource& resource) {
    // Collection target: one-element name span, arena-backed (coll is transient).
    auto& tgt = collection.emplace();
    std::string_view* nm = luxir::api::build::allocArray(tgt.name, 1, resource);
    nm[0] = luxir::api::build::arenaStr(resource, coll);
  }

  // Fill state.proto with the EFFECTIVE request: the parsed body (either dialect
  // form) with the path-derived collection applied (overwrites a body-supplied one).
  // Throws with a client-facing message on malformed input.
  static void parseEffectiveRequest(const std::string& body, const std::string& coll,
                                    const SearchUrlOverlay& overlay,
                                    HttpSearchRequestState& state) {
    // Build the NON-OWNING request directly into the request state's arena.
    parseQueryRequest(body.empty() ? "{}" : std::string_view(body), state.proto, state.resource);
    if (overlay.hasTopDocs && !state.proto.ops.empty()) {
      const auto* q = state.proto.ops.find("q");
      if (q == nullptr || std::get_if<luxir::api::TopDocs>(&(**q).kind) == nullptr) {
        throw RequestError(
            "URL TopDocs parameters target ops.q, but the request body has no top_docs op named 'q'");
      }
    }
    if (!overlay.empty) {
      overlayQueryRequest(overlay.json, state.proto, state.resource);
    }
    setCollectionTarget(state.proto.collection, coll, state.resource);
  }

  // ?explain=request: parse exactly as a query would be, then return the canonical
  // JSON of the effective request INSTEAD of executing it. Sugar expands, shorthand
  // lowers, and the output is itself a valid request body (posting it back runs the
  // identical query). Parse + serialize only - no engine work, so it runs inline.
  void handleExplain(const std::string& body, const std::string& coll,
                     const SearchUrlOverlay& overlay) {
    HttpSearchRequestState state;
    std::string out;
    try {
      parseEffectiveRequest(body, coll, overlay, state);
      requestId_ = std::string(state.proto.request_id);
      // Mirrors the URL-param check in route(): the docs format can also be
      // selected in the body, and it composes with explain no better.
      if (state.proto.response_format == luxir::api::ResponseFormat::DOCS) {
        respondError(ErrorInfo::of(ErrorKind::INVALID_REQUEST,
                                   "format=docs cannot be combined with explain"));
        return;
      }
      if (!luxir::api::write_json(state.proto, out)) {
        throw ApiError(ErrorKind::INTERNAL, "internal", "failed to serialize request");
      }
    } catch (const std::exception& e) {
      respondException(e, ErrorKind::INVALID_REQUEST);
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
      const luxir::api::SearchOp& op = *opView;
      if (const auto* td = std::get_if<luxir::api::TopDocs>(&op.kind)) {
        if (!td->ops.empty()) return "format=docs does not support nested ops";
        if (td->document_format == luxir::api::DocFormat::COLUMNS) {
          return "format=docs emits row documents; document_format COLUMNS conflicts";
        }
      } else if (const auto* f = std::get_if<luxir::api::Fusion>(&op.kind)) {
        if (!f->ops.empty()) return "format=docs does not support nested ops";
        // Per-source ops are ignored by fusion execution, but silently discarding
        // authored work behind a format flag would be worse than rejecting it.
        for (const auto& [srcName, src] : f->sources) {
          if (!src.ops.empty()) return "format=docs does not support nested ops";
        }
        if (f->document_format == luxir::api::DocFormat::COLUMNS) {
          return "format=docs emits row documents; document_format COLUMNS conflicts";
        }
      } else {
        return "format=docs requires top_docs or fusion ops";
      }
    }
    return nullptr;
  }

  void handleSearch(const std::string& body, const std::string& coll, HttpSearchFormat format,
                    const SearchUrlOverlay& overlay) {
    auto* arena = createArena();
    auto requestState = std::make_unique<HttpSearchRequestState>();
    try {
      parseEffectiveRequest(body, coll, overlay, *requestState);
    } catch (const std::exception& e) {
      releaseArena(arena);
      respondException(e, ErrorKind::INVALID_REQUEST);
      return;
    }
    requestId_ = std::string(requestState->proto.request_id);
    // The URL param is an alias for the request-level proto field (for clients
    // that cannot set query params).  The param cannot express an explicit
    // envelope, so either source selecting DOCS wins - no conflict exists.
    if (requestState->proto.response_format == luxir::api::ResponseFormat::DOCS) {
      format = HttpSearchFormat::DOCS;
    }
    bool docsMultiOp = false;
    if (format == HttpSearchFormat::DOCS) {
      if (const char* err = validateDocsFormat(requestState->proto)) {
        releaseArena(arena);
        respondError(ErrorInfo::of(ErrorKind::INVALID_REQUEST, err));
        return;
      }
      docsMultiOp = requestState->proto.ops.size() > 1;
    }

    auto& engine = node_.getSearchEngine();
    auto* sreq = luxir::arenaCreate<HttpSearchRequest>(
        *arena, engine, std::move(requestState), shared_from_this(), *arena);
    sreq->format = format;
    sreq->docsState.multiOp = docsMultiOp;
    sreq->docsState.requestId = sreq->proto.request_id;
    // Pin the io_context: shutdown drains until this query finishes, and the
    // context survives the request's teardown on the task-arena thread.
    sreq->shardPin = makeShardPin();

    // Route by max_parallel: the default (0) deliberately runs the whole
    // query right here on the shard thread - serial, no scheduler, blocking
    // this connection's io until it completes.  Non-zero values move the
    // synchronous engine.submit() onto the task arena so this io thread is
    // not occupied for the query's duration; reply() then posts results back
    // here.
    engine.dispatch(*sreq, sreq->proto.max_parallel);
  }

  void handleUpdate(const std::string& body, const std::string& coll,
                    const std::vector<std::pair<std::string, std::string>>& urlFieldMap,
                    bool urlDropUnmapped) {
    auto state = std::make_shared<HttpUpdateState>();
    try {
      std::string err;
      if (!luxir::api::read_json(state->proto, body, state->resource, &err)) {
        throw RequestError(err.empty() ? "malformed update request" : err, "invalid_json");
      }
      setCollectionTarget(state->proto.collection, coll, state->resource);
      // Same unit rule as streaming groups: a body that sets either field-map knob
      // owns the pair; otherwise the URL-param default applies.
      if (state->proto.field_map.empty() && !state->proto.drop_unmapped &&
          (!urlFieldMap.empty() || urlDropUnmapped)) {
        copyFieldMap(state->proto, urlFieldMap, state->resource);
        state->proto.drop_unmapped = urlDropUnmapped;
      }
      requestId_ = std::string(state->proto.request_id);
    } catch (const std::exception& e) {
      respondException(e, ErrorKind::INVALID_REQUEST);
      return;
    }

    auto shardPin = makeShardPin();
    node_.getTaskArena().enqueue([self = shared_from_this(), state, shardPin] {
      std::string out;
      std::optional<ErrorInfo> failure;
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
        // A closed writer still admits the message; it comes back errored below.
        if (!iw->submitUpdate(&msg)) throw std::runtime_error("update was not admitted");
        msg.blocker.wait();
        auto* resp = msg.finishResponse();
        if (!luxir::api::write_json(*resp, out)) {
          throw std::runtime_error("failed to serialize update response");
        }
      } catch (const std::exception& e) {
        failure = classifyException(e, ErrorKind::INTERNAL);
      }

      net::post(self->stream_.get_executor(),
          [self, shardPin, failure = std::move(failure), body = std::move(out)]() mutable {
            if (failure) self->respondError(*failure);
            else self->respondSimple(http::status::ok, "application/json", std::move(body));
          });
    });
  }

  // Names only - /_stats carries the detail.  Pretty-printed like the other
  // for-humans admin reads.  A pure in-memory snapshot, so it runs inline on
  // the io thread.
  void handleCollectionList() {
    std::string out;
    std::optional<ErrorInfo> failure;
    try {
      auto entries = node_.collectionEntries();
      std::vector<std::string_view> names(entries.size());
      for (std::size_t i = 0; i < entries.size(); i++) names[i] = entries[i].name;
      luxir::api::ListCollectionsResponse response;
      response.collections = names;
      std::string compact;
      if (!luxir::api::write_json(response, compact)) {
        throw std::runtime_error("failed to serialize list collections response");
      }
      glz::prettify_json(compact, out);
      out += '\n';
    } catch (const std::exception& e) {
      failure = classifyException(e, ErrorKind::INTERNAL);
    }
    if (failure) respondError(*failure);
    else respondSimple(http::status::ok, "application/json", std::move(out));
  }

  void handleCollectionCreate(const std::string& body) {
    struct State {
      std::pmr::monotonic_buffer_resource resource;
      luxir::api::CreateCollectionRequest request;
    };
    auto state = std::make_shared<State>();
    try {
      std::string err;
      if (!luxir::api::read_json(state->request, body, state->resource, &err)) {
        throw RequestError(err.empty() ? "malformed create collection request" : err, "invalid_json");
      }
    } catch (const std::exception& e) {
      respondException(e, ErrorKind::INVALID_REQUEST);
      return;
    }

    auto shardPin = makeShardPin();
    node_.getTaskArena().enqueue([self = shared_from_this(), state, shardPin] {
      std::string out;
      std::optional<ErrorInfo> failure;
      try {
        self->node_.createCollection(
            nullptr, state->request.name,
            state->request.schema ? &*state->request.schema : nullptr);
        luxir::api::CreateCollectionResponse response;
        response.name = state->request.name;
        if (!luxir::api::write_json(response, out)) {
          throw std::runtime_error("failed to serialize create collection response");
        }
      } catch (const std::exception& e) {
        failure = classifyException(e, ErrorKind::INTERNAL);
      }

      net::post(self->stream_.get_executor(),
          [self, shardPin, failure = std::move(failure), body = std::move(out)]() mutable {
            if (failure) self->respondError(*failure);
            else self->respondSimple(http::status::ok, "application/json", std::move(body));
          });
    });
  }

  void handleCollectionDelete(const std::string& body) {
    struct State {
      std::pmr::monotonic_buffer_resource resource;
      luxir::api::DeleteCollectionRequest request;
    };
    auto state = std::make_shared<State>();
    try {
      std::string err;
      if (!luxir::api::read_json(state->request, body, state->resource, &err)) {
        throw RequestError(err.empty() ? "malformed delete collection request" : err, "invalid_json");
      }
    } catch (const std::exception& e) {
      respondException(e, ErrorKind::INVALID_REQUEST);
      return;
    }

    auto shardPin = makeShardPin();
    node_.getTaskArena().enqueue([self = shared_from_this(), state, shardPin] {
      std::string out;
      std::optional<ErrorInfo> failure;
      try {
        self->node_.deleteCollection(state->request.name);
        luxir::api::DeleteCollectionResponse response;
        response.name = state->request.name;
        if (!luxir::api::write_json(response, out)) {
          throw std::runtime_error("failed to serialize delete collection response");
        }
      } catch (const std::exception& e) {
        failure = classifyException(e, ErrorKind::INTERNAL);
      }

      net::post(self->stream_.get_executor(),
          [self, shardPin, failure = std::move(failure), body = std::move(out)]() mutable {
            if (failure) self->respondError(*failure);
            else self->respondSimple(http::status::ok, "application/json", std::move(body));
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
    luxir::api::SchemaDef def;
    schema.toProto(&def, arena);
    std::string compact;
    if (!luxir::api::write_json(def, compact)) {
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
    std::string out;
    std::optional<ErrorInfo> failure;
    try {
      std::pmr::monotonic_buffer_resource targetResource;
      std::optional<luxir::api::Target> target;
      setCollectionTarget(target, coll, targetResource);
      auto collection = node_.resolveCollection(&*target);
      auto schema = collection->getSchema();
      out = renderSchemaBody(*schema);
    } catch (const std::exception& e) {
      failure = classifyException(e, ErrorKind::INTERNAL);
    }
    if (failure) respondError(*failure);
    else respondSimple(http::status::ok, "application/json", std::move(out));
  }

  void handleStats(std::optional<std::string> coll, bool includeSegments) {
    auto shardPin = makeShardPin();
    node_.getTaskArena().enqueue(
        [self = shared_from_this(), coll = std::move(coll), includeSegments, shardPin] {
          std::string out;
          std::optional<ErrorInfo> failure;
          try {
            std::pmr::monotonic_buffer_resource resource;
            luxir::api::StatsRequest request;
            request.segments = includeSegments;
            if (coll) setCollectionTarget(request.collection, *coll, resource);

            luxir::api::StatsResponse response;
            gatherStats(self->node_, request, response, resource);

            std::string compact;
            if (!luxir::api::write_json(response, compact)) {
              throw std::runtime_error("failed to serialize stats response");
            }
            glz::prettify_json(compact, out);
            out += '\n';
          } catch (const std::exception& e) {
            failure = classifyException(e, ErrorKind::INTERNAL);
          }

          net::post(self->stream_.get_executor(),
                    [self, shardPin, failure = std::move(failure), body = std::move(out)]() mutable {
                      if (failure) self->respondError(*failure);
                      else self->respondSimple(http::status::ok, "application/json", std::move(body));
                    });
        });
  }

  void handleSchemaSet(const std::string& body, const std::string& coll,
                       luxir::api::SchemaRequest_::Mode mode) {
    struct SchemaSetState {
      std::pmr::monotonic_buffer_resource resource;
      luxir::api::SchemaDef def;  // non-owning; backed by `resource`
    };
    auto state = std::make_shared<SchemaSetState>();
    try {
      std::string err;
      if (!luxir::api::read_json(state->def, body, state->resource, &err)) {
        throw RequestError(err.empty() ? "malformed schema" : err, "invalid_json");
      }
    } catch (const std::exception& e) {
      respondException(e, ErrorKind::INVALID_REQUEST);
      return;
    }

    // updateSchema persists (fsync) under the collection's schema lock, so run
    // it off the io thread like handleUpdate.
    auto shardPin = makeShardPin();
    node_.getTaskArena().enqueue([self = shared_from_this(), state, shardPin, mode, coll] {
      std::string out;
      std::optional<ErrorInfo> failure;
      try {
        std::pmr::monotonic_buffer_resource targetResource;
        std::optional<luxir::api::Target> target;
        setCollectionTarget(target, coll, targetResource);
        auto collection = self->node_.resolveOrCreateCollection(&*target);
        auto newSchema = collection->updateSchema(state->def, mode);
        out = renderSchemaBody(*newSchema);
      } catch (const std::exception& e) {
        failure = classifyException(e, ErrorKind::INTERNAL);
      }

      net::post(self->stream_.get_executor(),
          [self, shardPin, failure = std::move(failure), body = std::move(out)]() mutable {
            if (failure) self->respondError(*failure);
            else self->respondSimple(http::status::ok, "application/json", std::move(body));
          });
    });
  }

  void respondMethodNotAllowed(const RouteMatch& match, http::verb method, std::string_view target) {
    std::string message = std::string(http::to_string(method)) + " is not allowed for " +
                          std::string(target) + "; allowed: " + std::string(match.allow);
    if (!match.hint.empty()) message += "; " + std::string(match.hint);
    ErrorInfo info{ErrorKind::INVALID_REQUEST, "method_not_allowed", std::move(message)};
    std::string_view allow = match.allow;
    auto resp = std::make_shared<http::response<http::string_body>>(
        http::status::method_not_allowed, httpVersion_);
    resp->set(http::field::server, "luxir");
    resp->set(http::field::content_type, "application/json");
    resp->set(http::field::allow, allow);
    resp->keep_alive(keepAlive_);
    resp->body() = renderErrorBody(info, requestId_);
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

  static void copyCommitParams(std::optional<luxir::api::CommitParams>& out,
                               const luxir::api::CommitParams& src,
                               std::pmr::memory_resource& resource) {
    auto& params = out.emplace();
    params.commit_within_ms = src.commit_within_ms;
    params.wait_for_merges = src.wait_for_merges;
    params.max_segments = src.max_segments;
    std::string_view* names =
        luxir::api::build::allocArray(params.build_aux_indexes, src.build_aux_indexes.size(), resource);
    for (std::size_t i = 0; i < src.build_aux_indexes.size(); i++) {
      names[i] = luxir::api::build::arenaStr(resource, src.build_aux_indexes[i]);
    }
  }

  static void copyCollectionTarget(std::optional<luxir::api::Target>& out,
                                   const luxir::api::Target& src,
                                   std::pmr::memory_resource& resource) {
    auto& target = out.emplace();
    std::string_view* names = luxir::api::build::allocArray(target.name, src.name.size(), resource);
    for (std::size_t i = 0; i < src.name.size(); i++) {
      names[i] = luxir::api::build::arenaStr(resource, src.name[i]);
    }
  }

  // Parses repeatable ?field_map=from:to,from2:to2 URL params into entries for
  // UpdateRequest.field_map. The LAST ':' splits an entry, so input keys may contain
  // ':'; an empty target ("notes:") drops the key. Input keys containing ',' need the
  // body/control-object form.
  static bool parseFieldMapParams(const std::vector<UrlParam>& params,
                                  std::vector<std::pair<std::string, std::string>>& out,
                                  std::string& err) {
    for (const auto& p : params) {
      if (p.key != "field_map") continue;
      std::string_view rest = p.value;
      while (!rest.empty()) {
        auto comma = rest.find(',');
        std::string_view entry = rest.substr(0, comma);
        rest = (comma == std::string_view::npos) ? std::string_view() : rest.substr(comma + 1);
        auto colon = entry.rfind(':');
        if (colon == std::string_view::npos || colon == 0) {
          err = "field_map entry '" + std::string(entry) +
                "' is not from:to (an empty to drops the key)";
          return false;
        }
        std::string_view to = entry.substr(colon + 1);
        if (!to.empty() && !Schema::validFieldName(to)) {
          err = "field_map target is not a valid field name: " + std::string(to);
          return false;
        }
        out.emplace_back(entry.substr(0, colon), to);
      }
    }
    return true;
  }

  static bool parseDropUnmappedParam(const std::vector<UrlParam>& params, bool& out,
                                     std::string& err) {
    if (const std::string* value = findParam(params, "drop_unmapped")) {
      if (*value == "true") {
        out = true;
      } else if (*value == "false") {
        out = false;
      } else {
        err = "invalid drop_unmapped value '" + *value + "' (valid: true, false)";
        return false;
      }
    }
    return true;
  }

  static void copyFieldMap(luxir::api::UpdateRequest& proto,
                           const std::vector<std::pair<std::string, std::string>>& entries,
                           std::pmr::memory_resource& resource) {
    using Pair = std::pair<std::string_view, std::string_view>;
    Pair* a = (Pair*)resource.allocate(sizeof(Pair) * entries.size(), alignof(Pair));
    for (std::size_t i = 0; i < entries.size(); i++) {
      a[i] = {luxir::api::build::arenaStr(resource, entries[i].first),
              luxir::api::build::arenaStr(resource, entries[i].second)};
    }
    proto.field_map = luxir::api::map_view<std::string_view, std::string_view>(
        std::span<const Pair>(a, entries.size()));
  }

  static bool validateEndControlPayload(const luxir::api::Map& payload, std::string& err) {
    for (const auto& [name, val] : payload.fields) {
      unused(val);
      if (name == "request_id" || name == "commit") continue;
      if (name == "collection" || name == "allow_dups" || name == "all_or_none" ||
          name == "return_ids" || name == "docs" || name == "delete_ids" ||
          name == "columns" || name == "stream_id" || name == "field_map" ||
          name == "drop_unmapped") {
        err = "_end_ control cannot carry submit-time field '" + std::string(name) + "'";
        return false;
      }
      err = "unsupported _end_ control field '" + std::string(name) + "'";
      return false;
    }
    return true;
  }

  static bool validateUpdateControlPayload(const luxir::api::Map& payload, std::string& err) {
    for (const auto& [name, val] : payload.fields) {
      unused(val);
      if (name == "collection" || name == "allow_dups" || name == "all_or_none" ||
          name == "return_ids" || name == "request_id" || name == "commit" ||
          name == "docs" || name == "delete_ids" || name == "field_map" ||
          name == "drop_unmapped") {
        continue;
      }
      err = "unsupported _update_ control field '" + std::string(name) + "'";
      return false;
    }
    return true;
  }

  static bool decodeStreamControlPayload(std::string_view keyword, const luxir::api::Map& payload,
                                         std::size_t recordBytes,
                                         HttpStreamControl& control, std::string& err) {
    std::string json;
    luxir::api::Val wrapper;
    wrapper.kind = payload;
    if (!luxir::api::write_json(wrapper, json)) {
      err = "failed to serialize " + std::string(keyword) + " control payload";
      return false;
    }

    auto request = std::make_shared<HttpStreamControlRequest>();
    request->sourceBytes = recordBytes;
    if (!luxir::api::read_json(request->proto, json, request->resource, &err)) {
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
  static bool extractStreamControl(const luxir::api::Map& map, HttpStreamControl& control,
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
      if (std::get_if<luxir::api::Map>(&(*entry.second).kind) == nullptr) {
        err = "_header_ control value must be an object";
        return false;
      }
      return true;
    }
    if (entry.first != "_update_" && entry.first != "_end_") {
      if (entry.first.size() >= 2 && entry.first.front() == '_' && entry.first.back() == '_' &&
          std::get_if<luxir::api::Map>(&(*entry.second).kind) != nullptr) {
        err = "unknown control record '" + std::string(entry.first) + "'";
        return false;
      }
      return true;
    }

    isControl = true;
    control.kind = entry.first == "_update_" ? HttpStreamControlKind::Open
                                             : HttpStreamControlKind::Close;
    const luxir::api::Val& wrapper = *entry.second;
    const auto* payload = std::get_if<luxir::api::Map>(&wrapper.kind);
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

  void startStreamingUpdate(std::string coll, bool urlCommit,
                            std::vector<std::pair<std::string, std::string>> urlFieldMap,
                            bool urlDropUnmapped) {
    const auto& ingest = node_.getConfig().ingest;
    std::size_t arenaConcurrency =
        (std::size_t)std::max(1, node_.getTaskArena().max_concurrency());
    std::size_t maxInFlight = ingest.max_inflight_batches == 0
        ? arenaConcurrency + 2
        : (std::size_t)ingest.max_inflight_batches;
    auto state = std::make_shared<HttpStreamUpdateState>(
        (std::size_t)ingest.stream_batch_size,
        (std::size_t)ingest.stream_batch_docs,
        (std::size_t)ingest.maxRecordBytes(),
        (std::size_t)ingest.max_request_body,
        maxInFlight);
    state->defaultCollectionName = std::move(coll);
    state->group.collectionName = state->defaultCollectionName;
    state->urlCommit = urlCommit;
    state->urlFieldMap = std::move(urlFieldMap);
    state->urlDropUnmapped = urlDropUnmapped;
    state->shardPin = makeShardPin();

    // http::async_read_some uses its dynamic buffer capacity to select a socket
    // read size (capped at 64 KiB).  Header parsing otherwise leaves flat_buffer
    // at Beast's 512-byte bootstrap allocation, so a firehose stream reaches us
    // in ~512-byte turns even when the client writes multi-MiB chunks.  Reserve
    // the composed read's cap without changing its important return-on-available-
    // bytes behavior for checkpoint/control records.
    buffer_.reserve(64 * 1024);
    streamUpdate_ = std::move(state);
    doStreamBodyRead();
  }

  void doStreamBodyRead() {
    auto state = streamUpdate_;
    if (!state || state->failed || state->readInFlight || !state->canAdmit() ||
        state->batchReady || state->urlCommitInFlight) {
      return;
    }
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
    state->readInFlight = true;
    http::async_read_some(stream_, buffer_, *parser_,
        beast::bind_front_handler(&HttpSession::onStreamBodyRead, shared_from_this()));
  }

  void onStreamBodyRead(beast::error_code ec, std::size_t) {
    auto state = streamUpdate_;
    if (!state) return;
    state->readInFlight = false;
    if (state->failed) {
      parser_.reset();
      return;
    }
    if (state->inputFailurePending) return;

    bool needBuffer = ec == http::error::need_buffer;
    if (ec && !needBuffer) {
      failStreamingUpdate(ErrorInfo::of(ErrorKind::INVALID_REQUEST,
                                        "failed to read NDJSON request body: " + ec.message()));
      return;
    }

    std::size_t produced = state->readBuf.size() - parser_->get().body().size;
    if (produced > 0) {
      state->framer.feed(std::string_view(state->readBuf.data(), produced));
      if (state->framer.error()) {
        failStreamingInput({ErrorKind::RESOURCE_EXHAUSTED, "request_too_large", state->framer.message()});
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

  static std::string streamTargetKey(const std::optional<luxir::api::Target>& target,
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

  // The response line for the current interval.  `terminal` is the failure
  // ending the stream, if any: the line then reports status ERROR and the
  // error alongside whatever the interval had folded so far.  total_errors
  // counts every failed document even past the retention cap on `errors`.
  static bool renderStreamIntervalResponseLine(const HttpStreamUpdateState& state,
                                               std::string_view requestId,
                                               const ErrorInfo* terminal,
                                               std::string& out) {
    const auto& interval = state.interval;
    std::pmr::monotonic_buffer_resource responseResource;
    luxir::api::UpdateResponse resp;
    resp.request_id = luxir::api::build::arenaStr(responseResource, requestId);
    resp.update_version = interval.lastUpdateVersion;

    luxir::api::build::SpanBuilder<std::string_view> ids(responseResource);
    ids.reserve(interval.ids.size());
    for (const auto& id : interval.ids) ids.push_back(luxir::api::build::arenaStr(responseResource, id));
    resp.ids = ids.finish();

    luxir::api::build::SpanBuilder<luxir::api::UpdateResponse_::DocError> errors(responseResource);
    errors.reserve(interval.errors.size());
    for (const auto& src : interval.errors) {
      auto& dst = errors.emplace_back();
      dst.id = luxir::api::build::arenaStr(responseResource, src.id);
      dst.index = src.index;
      dst.error = luxir::api::build::arenaError(responseResource, src.error);
    }
    resp.errors = errors.finish();
    resp.total_errors = (int64_t)interval.totalErrors;

    if (terminal != nullptr) {
      resp.status = luxir::api::UpdateResponse_::Status::ERROR;
      resp.error = luxir::api::build::arenaError(responseResource, *terminal);
    } else if (interval.totalErrors == 0) {
      resp.status = luxir::api::UpdateResponse_::Status::OK;
    } else if (interval.anySuccess) {
      resp.status = luxir::api::UpdateResponse_::Status::PARTIAL;
    } else {
      resp.status = luxir::api::UpdateResponse_::Status::ERROR;
    }

    out.clear();
    if (!luxir::api::write_json(resp, out)) return false;
    out += '\n';
    return true;
  }

  // The terminal line when no stream state survives to render an interval.
  static bool renderStreamFailureLine(std::string_view requestId, const ErrorInfo& failure,
                                      std::string& out) {
    std::pmr::monotonic_buffer_resource responseResource;
    luxir::api::UpdateResponse resp;
    resp.request_id = luxir::api::build::arenaStr(responseResource, requestId);
    resp.status = luxir::api::UpdateResponse_::Status::ERROR;
    resp.error = luxir::api::build::arenaError(responseResource, failure);

    out.clear();
    if (!luxir::api::write_json(resp, out)) return false;
    out += '\n';
    return true;
  }

  bool emitCurrentStreamInterval(bool last, bool force) {
    auto state = streamUpdate_;
    assert(state != nullptr);
    if (!force && !streamIntervalHasActivity(*state)) return true;

    std::string out;
    if (!renderStreamIntervalResponseLine(*state, currentStreamResponseRequestId(*state), nullptr, out)) {
      failStreamingUpdate(ErrorInfo::of(ErrorKind::INTERNAL, "failed to serialize update response"));
      return false;
    }
    state->emittedLine = true;
    state->group.closeRequestId.reset();
    resetStreamInterval(*state);
    enqueueLine(std::move(out), last);
    return true;
  }

  // Ends the stream with `failure`: the terminal response line when the
  // chunked response has begun, a plain HTTP error otherwise.
  void failStreamingUpdate(ErrorInfo failure) {
    auto state = streamUpdate_;
    if (state && state->failed) return;
    std::size_t docsIndexed = state ? state->docsIndexedSoFar : 0;
    if (state) {
      state->failed = true;
      state->completedBatchResults.clear();
      if (state->inFlight == 0) state->shardPin.reset();
    }
    // A pipelined batch can fail while the next body read is outstanding.
    // Beast's composed read still owns the parser in that case; its completion
    // observes failed and releases it above.
    if (!state || !state->readInFlight) parser_.reset();
    keepAlive_ = false;
    terminalStreamingFailure_ = true;
    failure.message += " (docs_indexed_so_far=" + std::to_string(docsIndexed) + ")";
    std::string_view requestId = state ? currentStreamResponseRequestId(*state) : std::string_view();
    if (headerSent_) {
      std::string out;
      bool rendered = state ? renderStreamIntervalResponseLine(*state, requestId, &failure, out)
                            : renderStreamFailureLine(requestId, failure, out);
      if (!rendered) {
        doTerminalStreamingClose();
        return;
      }
      enqueueLine(std::move(out), true);
      return;
    }
    respondError(failure, requestId);
  }

  // A failure ordered after every batch already admitted: fatal input, or a
  // batch's request-level failure while later batches are in flight.  The
  // admitted prefix drains and folds first, so the terminal line's counts and
  // evidence cover everything the stream actually did.
  void failStreamingInput(ErrorInfo failure) {
    auto state = streamUpdate_;
    if (!state || state->failed || state->inputFailurePending) return;
    if (state->inFlight == 0 && !state->barrierPending) {
      failStreamingUpdate(std::move(failure));
      return;
    }
    state->inputFailurePending = std::move(failure);
    state->barrierPending = true;
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

  HttpStreamWriterTarget* streamWriterTarget(const std::string& collectionName, ErrorInfo& err) {
    auto state = streamUpdate_;
    assert(state != nullptr);
    auto [it, inserted] = state->writerCache.try_emplace(collectionName);
    if (!inserted) return &it->second;

    try {
      std::pmr::monotonic_buffer_resource targetResource;
      std::optional<luxir::api::Target> target;
      setCollectionTarget(target, collectionName, targetResource);
      it->second.collection = node_.resolveOrCreateCollection(&*target);
      it->second.indexWriter = it->second.collection->getShard()->getIndexWriter();
    } catch (...) {
      err = currentExceptionInfo(ErrorKind::INTERNAL);
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

  bool submitPreparedStreamBatch(bool barrierSubmission = false) {
    auto state = streamUpdate_;
    assert(state != nullptr);
    assert(state->batch != nullptr);
    assert(state->batchReady);
    if (!barrierSubmission && !state->canAdmit()) return false;
    assert(state->inFlight < state->maxInFlight);

    ErrorInfo err;
    HttpStreamWriterTarget* target = streamWriterTarget(state->batch->collectionName, err);
    if (target == nullptr) {
      failStreamingUpdate(std::move(err));
      return false;
    }
    // The stream holds a cached writer, so a collection deleted mid-stream is only
    // visible here.  A batch that races the close is instead rejected inside the
    // graph and folded as a batch error; the batch after it ends the stream here.
    if (target->indexWriter->isClosed()) {
      failStreamingUpdate({ErrorKind::UNAVAILABLE, "writer_closed",
                           "NDJSON batch rejected: index writer for collection '" +
                               state->batch->collectionName + "' is closed"});
      return false;
    }

    std::shared_ptr<HttpStreamBatchState> batch(std::move(state->batch));
    state->batch = std::make_unique<HttpStreamBatchState>(state->batchMaxDocs);
    state->batchReady = false;
    std::uint64_t ordinal = state->nextSubmitOrdinal++;
    auto iw = target->indexWriter;
    auto message = std::make_shared<StreamingUpdateMessage>(
        &batch->proto, shared_from_this(), state, ordinal, batch);
    batch->message = message;
    auto [entry, inserted] = state->inFlightBatches.emplace(ordinal, batch);
    unused(entry);
    assert(inserted);
    unused(inserted);
    state->inFlight++;

    // This handler runs on the session shard. execute enters the node arena
    // only for the immediate try_put and returns without waiting for indexing.
    // Consecutive calls therefore reach startUpdateNode in stream order; enqueue
    // would not preserve the updateVersion ordering required by overwrite.
    bool success = false;
    std::optional<ErrorInfo> submitError;
    try {
      node_.getTaskArena().execute([&] { success = iw->submitUpdate(message.get()); });
    } catch (...) {
      submitError = currentExceptionInfo(ErrorKind::INTERNAL);
    }
    if (!success) {
      HttpStreamBatchResult result;
      result.docCount = batch->docCount;
      result.deleteCount = batch->deleteCount;
      result.firstDocIndex = batch->firstDocIndex;
      result.failed = true;
      if (submitError) {
        submitError->message = "update graph submission failed: " + submitError->message;
        result.error = std::move(submitError);
      } else {
        result.error = ErrorInfo::of(ErrorKind::INTERNAL, "update graph rejected the NDJSON batch");
      }
      // Dead today: startUpdateNode is a queueing function_node, so try_put
      // always accepts.  This becomes reentrant under a rejecting policy.
      onStreamBatchDone(state, ordinal, std::move(result));
      return false;
    }
    return true;
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

  // The (field_map, drop_unmapped) pair travels as a unit: a group that sets either
  // knob owns both; otherwise the stream's URL-param default applies. URL entries are
  // copied into the batch arena so the batch proto never references stream state.
  static void applyStreamBatchFieldMap(HttpStreamUpdateState& state, HttpStreamBatchState& batch) {
    if (!state.group.fieldMap().empty() || state.group.dropUnmapped()) {
      batch.proto.field_map = state.group.fieldMap();
      batch.proto.drop_unmapped = state.group.dropUnmapped();
    } else if (!state.urlFieldMap.empty() || state.urlDropUnmapped) {
      copyFieldMap(batch.proto, state.urlFieldMap, batch.resource);
      batch.proto.drop_unmapped = state.urlDropUnmapped;
    }
  }

  bool submitStreamBatch(const luxir::api::CommitParams* commitParams,
                         bool barrierSubmission = false) {
    auto state = streamUpdate_;
    assert(state != nullptr);
    assert(state->batch != nullptr);
    assert(!state->batchReady);

    accountStreamSubmission(*state->batch, state->batch->docs.size(), 0);
    state->batch->proto.docs = state->batch->docs.finish();
    state->batch->proto.allow_dups = state->group.allowDups();
    state->batch->proto.all_or_none = state->group.allOrNone();
    applyStreamBatchFieldMap(*state, *state->batch);
    state->batch->proto.request_id =
        luxir::api::build::arenaStr(state->batch->resource, state->group.requestId);
    // Once the folded prefix reaches the retention cap, later slices can skip
    // building ids.  With pipelining, already-submitted earlier slices may not
    // have folded yet, so a bounded window can still request a few excess id
    // vectors; ordinal folding below keeps the response cap deterministic.
    state->batch->proto.return_ids =
        state->group.returnIds() && state->interval.ids.size() < HttpStreamUpdateState::kMaxRetainedIds;
    state->batch->requestOwner = state->group.request;
    prepareStreamBatchCollection(*state->batch);
    if (commitParams != nullptr) {
      copyCommitParams(state->batch->proto.commit, *commitParams, state->batch->resource);
    }

    state->batchReady = true;
    return submitPreparedStreamBatch(barrierSubmission);
  }

  bool submitInlineStreamRequest(const std::shared_ptr<HttpStreamControlRequest>& request) {
    auto state = streamUpdate_;
    assert(state != nullptr);
    assert(state->batch != nullptr);
    if (request->sourceBytes > state->maxRequestBody) {
      failStreamingUpdate({ErrorKind::RESOURCE_EXHAUSTED, "request_too_large",
                           "_update_ inline request exceeds indexing.max-request-body"});
      return false;
    }
    if (state->batch->docs.size() != 0) {
      failStreamingUpdate(ErrorInfo::of(ErrorKind::INTERNAL, "internal error: inline _update_ encountered a non-empty stream batch"));
      return false;
    }

    state->batch->requestOwner = request;
    state->batch->proto = request->proto;
    state->batch->collectionName = state->group.collectionName;
    accountStreamSubmission(*state->batch, request->proto.docs.size(), request->proto.delete_ids.size());
    state->batch->proto.allow_dups = state->group.allowDups();
    state->batch->proto.all_or_none = state->group.allOrNone();
    applyStreamBatchFieldMap(*state, *state->batch);
    state->batch->proto.return_ids =
        state->group.returnIds() && state->interval.ids.size() < HttpStreamUpdateState::kMaxRetainedIds;
    if (!state->batch->proto.collection) {
      setCollectionTarget(state->batch->proto.collection, state->group.collectionName, state->batch->resource);
    }
    state->emitAfterBatch = true;
    state->resetGroupAfterBatch = true;
    state->batchReady = true;
    bool submitted = submitPreparedStreamBatch(true);
    if (!submitted && !state->failed) {
      failStreamingUpdate(ErrorInfo::of(ErrorKind::INTERNAL, "internal error: failed to submit inline _update_ barrier"));
    }
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

    const luxir::api::CommitParams* commitParams = nullptr;
    if (closeRequest && closeRequest->proto.commit) {
      commitParams = &*closeRequest->proto.commit;
    } else if (state->group.request && state->group.request->proto.commit) {
      commitParams = &*state->group.request->proto.commit;
    }

    if (state->batch->docs.size() > 0 || commitParams != nullptr) {
      state->emitAfterBatch = true;
      state->resetGroupAfterBatch = true;
      bool submitted = submitStreamBatch(commitParams, true);
      if (!submitted && !state->failed) {
        failStreamingUpdate(ErrorInfo::of(ErrorKind::INTERNAL, "internal error: failed to submit NDJSON close barrier"));
      }
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
    assert(state->barrierPending);
    assert(state->inFlight == 0);
    assert(control.kind != HttpStreamControlKind::Noop);

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

  void continueStreamBarrier() {
    auto state = streamUpdate_;
    if (!state || state->failed || !state->barrierPending || state->inFlight != 0) return;
    assert(state->completedBatchResults.empty());

    if (state->inputFailurePending) {
      ErrorInfo failure = std::move(*state->inputFailurePending);
      state->inputFailurePending.reset();
      failStreamingUpdate(std::move(failure));
      return;
    }

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

    if (state->eofPending) {
      bool forceEmit = state->group.open || !state->emittedLine;
      if (!closeCurrentStreamGroup(nullptr, forceEmit)) return;
      state->eofPending = false;
      state->barrierPending = false;
      finishStreamingUpdate();
      return;
    }

    state->barrierPending = false;
    net::post(stream_.get_executor(), [self = shared_from_this(), state] {
      if (self->streamUpdate_ == state && !state->failed) self->drainStreamRecords();
    });
  }

  bool beginStreamControlBarrier(HttpStreamControl control) {
    auto state = streamUpdate_;
    assert(state != nullptr);
    assert(!state->barrierPending);
    assert(!state->pendingControl);
    assert(control.kind != HttpStreamControlKind::Noop);
    state->barrierPending = true;
    state->pendingControl = std::move(control);
    continueStreamBarrier();
    return false;
  }

  bool processStreamRecord(std::string_view record) {
    auto state = streamUpdate_;
    assert(state != nullptr);
    assert(state->batch != nullptr);
    assert(!state->batchReady);

    luxir::api::Map map;
    std::string err;
    if (!luxir::api::read_json(map, record, state->batch->resource, &err)) {
      failStreamingInput({ErrorKind::INVALID_REQUEST, "invalid_json",
                          err.empty() ? "malformed NDJSON record" : err});
      return false;
    }

    HttpStreamControl control;
    bool isControl = false;
    if (!extractStreamControl(map, control, isControl, record.size(), err)) {
      failStreamingInput(ErrorInfo::of(ErrorKind::INVALID_REQUEST, err));
      return false;
    }

    if (isControl) {
      // Noop meta record (e.g. _header_).  It was decoded into the batch
      // arena, so it must count toward rotation or a stream of meta records
      // would grow the arena without bound.  With no docs buffered an empty
      // batch carries no state (proto/collection are populated at submission),
      // so past the target it is simply replaced - the same rotation
      // submitStreamBatch performs, minus the submit.  With docs pending the
      // bytes stay in that batch, but the no-op itself never forces submission.
      if (control.kind == HttpStreamControlKind::Noop) {
        state->batch->sourceBytes += record.size();
        if (state->group.allOrNone() && state->batch->sourceBytes > state->maxRequestBody) {
          failStreamingInput({ErrorKind::RESOURCE_EXHAUSTED, "request_too_large",
                              "all_or_none NDJSON group exceeds indexing.max-request-body"});
          return false;
        }
        if (state->batch->docs.size() == 0 &&
            state->batch->sourceBytes >= state->batchTargetBytes) {
          state->batch = std::make_unique<HttpStreamBatchState>(state->batchMaxDocs);
        }
        return true;
      }
      return beginStreamControlBarrier(std::move(control));
    } else {
      startImplicitStreamGroup();
      state->batch->docs.push_back(map);
    }
    state->batch->sourceBytes += record.size();
    if (state->group.allOrNone()) {
      if (state->batch->sourceBytes > state->maxRequestBody) {
        failStreamingInput({ErrorKind::RESOURCE_EXHAUSTED, "request_too_large",
                              "all_or_none NDJSON group exceeds indexing.max-request-body"});
        return false;
      }
      return true;
    }
    if (state->batch->sourceBytes >= state->batchTargetBytes ||
        state->batch->docs.size() >= state->batchMaxDocs) {
      if (!submitStreamBatch(nullptr)) return false;
      return state->canAdmit();
    }
    return true;
  }

  void onStreamBatchDone(const std::shared_ptr<HttpStreamUpdateState>& state,
                         std::uint64_t ordinal,
                         HttpStreamBatchResult result) {
    auto entry = state->inFlightBatches.find(ordinal);
    if (entry == state->inFlightBatches.end()) return;
    assert(state->inFlight > 0);
    state->inFlight--;
    state->inFlightBatches.erase(entry);

    if (state->failed || streamUpdate_ != state) {
      if (state->inFlight == 0) state->shardPin.reset();
      return;
    }

    auto [completed, inserted] =
        state->completedBatchResults.emplace(ordinal, std::move(result));
    unused(completed);
    assert(inserted);
    unused(inserted);
    for (;;) {
      auto next = state->completedBatchResults.find(state->nextFoldOrdinal);
      if (next == state->completedBatchResults.end()) break;
      HttpStreamBatchResult nextResult = std::move(next->second);
      state->completedBatchResults.erase(next);
      state->nextFoldOrdinal++;
      if (nextResult.failed) {
        ErrorInfo failure =
            nextResult.error.value_or(ErrorInfo::of(ErrorKind::INTERNAL, "unknown failure"));
        failure.message = "NDJSON update batch failed: " + failure.message;
        failStreamingUpdate(std::move(failure));
        return;
      }
      foldStreamBatchResult(nextResult);
      if (nextResult.error) {
        // A request-level failure (status ERROR with an error set: the commit
        // pipeline, a closed writer, a bad field_map) ends the stream.  What
        // follows would fail the same way, and the response line must report
        // it as the request's failure, never as a document's.  The batch's
        // own evidence (document errors, update version) was folded above;
        // batches already in flight drain before the terminal line.
        failStreamingInput(*nextResult.error);
        if (state->failed) return;
        // Otherwise the failure waits behind the barrier: keep folding what
        // has already completed so the terminal line covers it.
      }
    }

    if (state->barrierPending) {
      continueStreamBarrier();
      return;
    }
    if (state->batchReady && state->canAdmit()) {
      if (!submitPreparedStreamBatch()) return;
    }
    drainStreamRecords();
  }

  void foldStreamBatchResult(const HttpStreamBatchResult& result) {
    auto state = streamUpdate_;
    assert(state != nullptr);
    auto& interval = state->interval;
    interval.lastUpdateVersion = result.updateVersion;

    std::size_t indexedDocs = 0;
    if (result.status != luxir::api::UpdateResponse_::Status::ERROR) {
      std::size_t failedDocs = result.errors.size();
      if (failedDocs > result.docCount) failedDocs = result.docCount;
      indexedDocs = result.docCount - failedDocs;
    }
    interval.docsIndexed += indexedDocs;
    state->docsIndexedSoFar += indexedDocs;
    if (result.status != luxir::api::UpdateResponse_::Status::ERROR &&
        (indexedDocs > 0 || result.deleteCount > 0)) {
      interval.anySuccess = true;
    }

    // Ids on a request-level failure are not proof of anything durable.
    if (result.status != luxir::api::UpdateResponse_::Status::ERROR) {
      for (const auto& id : result.ids) {
        if (interval.ids.size() < HttpStreamUpdateState::kMaxRetainedIds) interval.ids.push_back(id);
      }
    }

    for (const auto& err : result.errors) {
      interval.totalErrors++;
      if (interval.errors.size() >= HttpStreamUpdateState::kMaxRetainedErrors) continue;
      std::size_t globalIndex = result.firstDocIndex;
      if (err.index >= 0) globalIndex += (std::size_t)err.index;
      std::size_t intervalIndex =
          globalIndex >= interval.firstDocIndex ? globalIndex - interval.firstDocIndex : 0;
      interval.errors.push_back({err.id, err.error, cappedErrorIndex(intervalIndex)});
    }
  }

  void drainStreamRecords() {
    auto state = streamUpdate_;
    if (!state || state->failed || state->barrierPending || state->urlCommitInFlight) return;

    if (state->batchReady) {
      if (!state->canAdmit() || !submitPreparedStreamBatch()) return;
    }
    if (!state->canAdmit()) return;

    std::string_view record;
    while (state->canAdmit() && state->framer.next(record)) {
      if (!processStreamRecord(record)) return;
      if (state->failed || state->barrierPending || state->batchReady) return;
    }
    if (!state->canAdmit()) return;

    if (state->bodyDone) {
      if (!state->tailFinished) {
        state->tailFinished = true;
        if (state->framer.finish(record)) {
          if (!processStreamRecord(record)) return;
          if (state->failed || state->barrierPending || state->batchReady ||
              !state->canAdmit()) {
            return;
          }
        } else if (state->framer.error()) {
          failStreamingInput({ErrorKind::RESOURCE_EXHAUSTED, "request_too_large", state->framer.message()});
          return;
        }
      }

      state->eofPending = true;
      state->barrierPending = true;
      continueStreamBarrier();
      return;
    }

    doStreamBodyRead();
  }

  void finishStreamingUpdate() {
    auto state = streamUpdate_;
    if (!state || state->failed) return;
    parser_.reset();

    if (state->urlCommit) {
      ErrorInfo err;
      if (streamWriterTarget(state->defaultCollectionName, err) == nullptr) {
        failStreamingUpdate(std::move(err));
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

    auto shardPin = state->shardPin;
    node_.getTaskArena().enqueue(
        [self = shared_from_this(), state, writers = std::move(writers), shardPin] {
          std::optional<ErrorInfo> err;
          try {
            for (const auto& [name, writer] : writers) {
              unused(name);
              writer->commit();
            }
          } catch (...) {
            err = currentExceptionInfo(ErrorKind::INTERNAL);
          }

          net::post(self->stream_.get_executor(),
              [self, state, shardPin, err = std::move(err)]() mutable {
                if (self->streamUpdate_ != state || state->failed) return;
                state->urlCommitInFlight = false;
                if (err) {
                  err->message = "NDJSON EOF commit failed: " + err->message;
                  self->failStreamingUpdate(std::move(*err));
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
    res_->set(http::field::server, "luxir");
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
    if (streamUpdate_ && !streamUpdate_->failed) {
      streamUpdate_->failed = true;
      streamUpdate_->completedBatchResults.clear();
      if (streamUpdate_->inFlight == 0) streamUpdate_->shardPin.reset();
    }
    // Pending entries are just owned strings (their arenas were freed in reply()),
    // so dropping them frees everything.
    int64_t dropped = 0;
    for (const auto& p : pendingQ_) dropped += (int64_t)p.line.size();
    pendingQ_.clear();
    queuedBytes_.fetch_sub(dropped, std::memory_order_relaxed);
    // Wake paused producers so their requests can finish (and be freed); their
    // next reply() observes CANCEL via aborted_.
    maybeFireDrainWaiters();
    if (terminalStreamingFailure_) doTerminalStreamingClose();
    else doClose();
  }

  // Shard only. Fires parked producer resumes once the queue has drained
  // below low-water (or unconditionally after an error).  Resumes are POSTED
  // to this connection's shard, not invoked: an already-drained park must not
  // recurse into produce(), and running production here keeps a default-lane
  // request on the thread that owns it (arena-lane requests re-enqueue
  // themselves - see HttpSearchRequest::resumeWhenDrained).
  // Must not throw: it runs inside io handlers (ioc->run() has no catch), and
  // a lost waiter strands its request forever - so a post allocation failure
  // falls back to running the resume inline.
  void maybeFireDrainWaiters() {
    if (drainWaiters_.empty()) return;
    if (!errored_ && queuedBytes_.load(std::memory_order_relaxed) > lowWater_) return;
    auto waiters = std::move(drainWaiters_);
    drainWaiters_.clear();
    for (auto& w : waiters) {
      try {
        net::post(stream_.get_executor(), w);  // copies; w stays valid if this throws
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
      if (terminalStreamingFailure_) doTerminalStreamingClose();
      else doClose();
    }
  }

  // --- one-shot (non-streaming) responses: health, 404, 400 -----------------

  void respondSimple(http::status status, std::string_view contentType, std::string body) {
    auto resp = std::make_shared<http::response<http::string_body>>(status, httpVersion_);
    resp->set(http::field::server, "luxir");
    resp->set(http::field::content_type, contentType);
    resp->keep_alive(keepAlive_);
    resp->body() = std::move(body);
    resp->prepare_payload();
    http::async_write(stream_, *resp,
        [self = shared_from_this(), resp](beast::error_code ec, std::size_t) {
          if (ec) {
            if (self->terminalStreamingFailure_) self->doTerminalStreamingClose();
            else self->doClose();
            return;
          }
          if (resp->keep_alive()) self->doRead();
          else if (self->terminalStreamingFailure_) self->doTerminalStreamingClose();
          else self->doClose();
        });
  }

  // Every failure answered with an HTTP error status: the status follows the
  // error's kind (httpStatusFor); the body is the same {request_id, error}
  // object an in-band error line carries.
  void respondError(const ErrorInfo& info) { respondError(info, requestId_); }
  void respondError(const ErrorInfo& info, std::string_view requestId) {
    respondSimple(httpStatusFor(info), "application/json", renderErrorBody(info, requestId));
  }
  void respondException(const std::exception& e, ErrorKind fallback) {
    respondError(classifyException(e, fallback));
  }

  // A 413 for a request body past indexing.max-request-body.  The body was not fully
  // consumed, so the connection cannot be reused - respond, then close.
  void respondPayloadTooLarge() {
    keepAlive_ = false;
    if (parser_.has_value()) {
      unsigned v = parser_->get().version();
      if (v != 0) httpVersion_ = v;
    }
    respondError({ErrorKind::RESOURCE_EXHAUSTED, "request_too_large",
                  "request body exceeds indexing.max-request-body"});
  }

  void doClose() {
    beast::error_code ec;
    stream_.socket().shutdown(tcp::socket::shutdown_send, ec);
  }

  void doTerminalStreamingClose() {
    beast::error_code ec;
    stream_.socket().shutdown(tcp::socket::shutdown_both, ec);
    stream_.socket().close(ec);
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
  net::post(stream_.get_executor(),
      beast::bind_front_handler(&HttpSession::doRead, shared_from_this()));
}

SearchRequest::ReplyStatus HttpSearchRequest::reply(SearchResponse& response) {
  if (format == HttpSearchFormat::DOCS) return replyDocs(response);
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
      outputCommitted = true;
      if (queued > session->highWater()) status = ReplyStatus::PAUSE;
    }
  } catch (...) {
    failDelivery(response);
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
    } else if (response.proto.error.has_value()) {
      if (outputCommitted) {
        session->abortStream();
      } else {
        ErrorInfo info = luxir::api::build::errorInfo(*response.proto.error);
        session->respondErrorFromEngine(httpStatusFor(info),
                                        renderErrorBody(info, response.proto.request_id));
      }
    } else {
      // Document bodies are a pure function of the batch: render them OUTSIDE
      // any lock (this is the expensive part).  Only run framing depends on
      // cross-emitter interleaving.
      auto runs = renderDocRuns(response.proto);

      // Multi-op requests have one emitter per op replying concurrently: the
      // framing decisions and the queue posts must agree on order, so both
      // happen under a SHORT critical section (marker render + posts; the
      // shard does the actual writes). Single-op requests have a single
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
        outputCommitted = true;
        if (queued > session->highWater()) status = ReplyStatus::PAUSE;
      }
    }
  } catch (...) {
    failDelivery(response);
  }
  if (last) {
    done();  // releases the request arena (this), its work guard, and session ref
  } else if (&response.arena != &arena) {
    releaseArena(&response.arena);  // this batch's own arena
  }
  return status;
}

void HttpSearchRequest::failDelivery(const SearchResponse& response) noexcept {
  try {
    if (outputCommitted) {
      session->abortStream();
      return;
    }
    ErrorInfo info = ErrorInfo::of(ErrorKind::INTERNAL, "failed to render the response");
    session->respondErrorFromEngine(httpStatusFor(info),
                                    renderErrorBody(info, response.proto.request_id));
  } catch (...) {
    try { session->abortStream(); } catch (...) {}
  }
}

void HttpSearchRequest::resumeWhenDrained(std::function<void()> resume) {
  if (maxParallel != 0) {
    // The request opted its query work off the io plane (max_parallel != 0),
    // so resumed production belongs on the arena too.  The wrapper runs on
    // the shard (see maybeFireDrainWaiters) and must not throw: fall back to
    // producing inline rather than stranding the stream.
    resume = [&node = session->node(), resume = std::move(resume)] {
      try {
        node.getTaskArena().enqueue(resume);
      } catch (...) {
        if (resume) resume();
      }
    };
  }
  session->whenDrained(std::move(resume));
}

HttpServer::HttpServer(LuxirNode& node, int threads, int port, int64_t streamBufferBytes,
                       std::chrono::milliseconds shardIdlePeriod)
  : node(node), nthreads(threads), requestedPort(port),
    // Clamp to >= 1: a non-positive high-water mark (misconfiguration) would
    // pause every reply while the drain check (queuedBytes <= low) never fires.
    streamBufferBytes_(std::max<int64_t>(1,
        streamBufferBytes > 0 ? streamBufferBytes
                              : node.getConfig().server.stream_buffer_bytes)),
    shardIdlePeriod(shardIdlePeriod) {}

HttpServer::~HttpServer() { shutdown(); }

void HttpServer::start() {
  if (started) return;
  shutdownRequested.store(false, std::memory_order_release);
  int n = nthreads > 0 ? nthreads
                       : (int)std::max(1u, std::thread::hardware_concurrency());

  std::string host = requestedPort == 0 ? "127.0.0.1" : "0.0.0.0";
  tcp::endpoint ep(net::ip::make_address(host), (unsigned short)requestedPort);

  registry = std::make_shared<HttpSessionRegistry>();
  shards.reserve(n);
  for (int i = 0; i < n; i++) {
    shards.push_back(std::make_shared<HttpIoShard>());
  }

  acceptor.emplace(acceptIoc);
  acceptor->open(ep.protocol());
  acceptor->set_option(net::socket_base::reuse_address(true));
  acceptor->bind(ep);
  acceptor->listen(net::socket_base::max_listen_connections);
  port_ = acceptor->local_endpoint().port();

  doAccept();

  // Shard threads spawn on first connection assignment. Shard 0 stays warm
  // after that first use; other shards exit after their independent idle
  // linger and can be spawned again by a later assignment.
  acceptThread = std::thread([this] {
    nameThisThread("luxir_accept");
    acceptIoc.run();
  });
  started = true;
  LOG_INFO("HTTP server listening on {}:{}", host, port_);
}

void HttpServer::armShardIdle(std::size_t idx) {
  assert(idx != 0);
  HttpIoShard* shard = shards[idx].get();
  std::uint64_t observedEpoch = shard->activityEpoch.load(std::memory_order_acquire);
  shard->idleTimer.expires_after(shardIdlePeriod);
  // The server owns every shard until shutdown has joined and drained all
  // contexts. Keep maintenance handlers non-owning so a stopped context cannot
  // retain its own shard.
  shard->idleTimer.async_wait([this, shard, idx, observedEpoch](beast::error_code ec) {
    if (shutdownRequested.load(std::memory_order_acquire)) return;
    if (ec) return;

    int64_t live = shard->liveConnections.load(std::memory_order_acquire);
    std::uint64_t currentEpoch = shard->activityEpoch.load(std::memory_order_acquire);
    if (live == 0 && currentEpoch == observedEpoch) return;
    armShardIdle(idx);
  });
}

void HttpServer::spawnShard(std::size_t idx) {
  auto shard = shards[idx];
  assert(!shard->thread.joinable());
  if (idx == 0) {
    if (!shard->floorGuard) shard->floorGuard.emplace(shard->ioc.get_executor());
  } else if (!shutdownRequested.load(std::memory_order_acquire)) {
    armShardIdle(idx);
  }
  // A close-window accept can spawn after this shard's cancel handler ran.
  // During shutdown its session and queued handlers are the only work needed;
  // arming a new linger timer here would delay join by the full idle period.

  shard->thread = std::thread([this, shard, idx] {
    runningShardThreads.fetch_add(1, std::memory_order_relaxed);
    char name[16];
    snprintf(name, sizeof(name), "luxir_http_%zu", idx);
    nameThisThread(name);
    shard->ioc.run();
    runningShardThreads.fetch_sub(1, std::memory_order_relaxed);
    net::post(acceptIoc, [this, idx] { shardExited(idx); });
  });
}

void HttpServer::shardExited(std::size_t idx) {
  auto shard = shards[idx];
  assert(shard->thread.joinable());
  // The runner has finished ioc.run() before posting this notification, but it
  // may still be returning from net::post, so join can briefly wait.
  shard->thread.join();
  shard->ioc.restart();
  if (shard->liveConnections.load(std::memory_order_acquire) > 0) spawnShard(idx);
}

void HttpServer::doAccept() {
  // Least-connections assignment, preferring an already-running shard on
  // ties. Sequential short-lived connections therefore reuse one warm
  // thread instead of round-robin spawning every shard, while concurrent
  // connections still fan out one per shard. Also balances better than
  // round-robin for long-lived connections of unequal lifetime. Counts are
  // exact enough: assignment happens only here, and a stale decrement just
  // delays reuse by one accept.
  std::size_t idx = 0;
  auto score = [this](std::size_t i) {
    return std::pair<int64_t, int>(
        shards[i]->liveConnections.load(std::memory_order_relaxed),
        shards[i]->thread.joinable() ? 0 : 1);
  };
  for (std::size_t i = 1; i < shards.size(); i++) {
    if (score(i) < score(idx)) idx = i;
  }
  auto shard = shards[idx];
  acceptor->async_accept(shard->ioc.get_executor(),
      [this, shard, idx](beast::error_code ec, tcp::socket sock) {
        if (ec == net::error::operation_aborted) return;  // shutting down
        if (!ec) {
          shard->activityEpoch.fetch_add(1, std::memory_order_release);
          shard->liveConnections.fetch_add(1, std::memory_order_release);
          // Only the accept thread spawns and reaps. joinable() therefore means
          // spawned and not yet reaped; the runner may already have returned.
          // shardExited() respawns if this assignment lands in that window.
          if (!shard->thread.joinable()) spawnShard(idx);
          // Small request/response exchanges on a keep-alive connection stall
          // ~40ms per round trip under Nagle + delayed ACK; disable Nagle like
          // every HTTP server does.  Best-effort: an ec here is not fatal.
          beast::error_code nde;
          sock.set_option(tcp::no_delay(true), nde);
          std::make_shared<HttpSession>(shard, std::move(sock), node, registry,
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
#ifndef NDEBUG
  assert(acceptThread.get_id() != std::this_thread::get_id()
         && "HttpServer::shutdown() must not be called from the accept thread");
  for (auto& shard : shards) {
    assert(shard->thread.get_id() != std::this_thread::get_id()
           && "HttpServer::shutdown() must not be called from an io thread");
  }
#endif

  // 1) Tell every idle timer to retire, regardless of whether its completion
  //    races cancellation. The cancel handlers are non-owning; virgin or
  //    already-parked contexts execute them in the final inline drain.
  shutdownRequested.store(true, std::memory_order_release);
  for (auto& shard : shards) {
    HttpIoShard* shardPtr = shard.get();
    net::post(shard->ioc, [shardPtr] {
      try {
        shardPtr->idleTimer.cancel();
      } catch (...) {
        // The shutdown flag still prevents a later completion from rearming.
        // steady_timer offers no error_code cancel overload in this Boost.
      }
    });
  }

  // 2) Close the registry gate before asking the accept loop to stop. A
  //    connection accepted in the close window then self-closes in run().
  //    Snapshot live sessions while holding the same gate, keeping them alive
  //    until their close has been posted.
  std::vector<std::shared_ptr<HttpSession>> live;
  {
    std::lock_guard<std::mutex> lk(registry->mtx);
    registry->shuttingDown = true;
    for (auto& w : registry->sessions) if (auto s = w.lock()) live.push_back(std::move(s));
  }

  // 3) Stop accepting. The accept context has one runner, so this is serialized
  //    with the accept loop without a strand.
  if (acceptor) {
    net::post(acceptor->get_executor(), [this] {
      beast::error_code ec;
      if (acceptor) acceptor->close(ec);
    });
  }

  // 4) Close each live connection, then drop our refs. Closing unblocks idle
  //    reads; in-flight queries keep the io_context alive via their work guards
  //    until they finish (and release their response arenas).
  for (auto& s : live) s->closeFromServer();
  live.clear();

  // The accept loop must be quiescent before shutdown releases shard 0's floor
  // guard or joins workers: a successful accept racing the close can still
  // assign a connection and spawn a shard.
  if (acceptThread.joinable()) acceptThread.join();
#ifndef NDEBUG
  assert(!acceptThread.joinable());
#endif

  // 5) Release shard 0's permanent floor guard, then join every runner. Other
  //    shards are held only by real io work, their idle timer, and ShardPins.
  //    Never call stop(): uninvoked handlers hold sessions which co-own their
  //    shard, creating a retention cycle instead of a clean teardown.
  if (!shards.empty()) shards[0]->floorGuard.reset();
  for (auto& shard : shards) {
    if (shard->thread.joinable()) shard->thread.join();
#ifndef NDEBUG
    assert(!shard->thread.joinable());
#endif
  }

  // 6) A runner can return just before a connection or maintenance handler is
  //    posted, leaving work queued on a stopped context. With every dedicated
  //    runner joined, drain every shard inline without filtering by its prior
  //    thread state.
  for (auto& shard : shards) {
    if (shard->ioc.stopped()) shard->ioc.restart();
    shard->ioc.run();
  }
#ifndef NDEBUG
  assert(runningShardThreads.load(std::memory_order_relaxed) == 0);
#endif

  // The acceptor and registry may only be released after every executor that
  // references them has drained. Sessions can briefly outlive this point on an
  // arena thread; they co-own the registry and shard they need for destruction.
  acceptor.reset();
  registry.reset();
  shards.clear();
  started = false;
}

} // namespace luxir
