#include "HttpServer.h"

#include <cassert>
#include <array>
#include <cstddef>
#include <deque>
#include <functional>
#include <limits>
#include <list>
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

#include "solux/util/log.h"
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

struct HttpSearchRequestState {
  std::pmr::monotonic_buffer_resource resource;
  HttpSearchReqProto proto;  // non-owning; backed by `resource`
};

struct HttpUpdateState {
  std::pmr::monotonic_buffer_resource resource;
  HttpUpdateReqProto proto;  // non-owning; backed by `resource`
};

struct HttpStreamCommitControl {
  std::uint64_t commitWithinUs = 0;
  std::vector<std::string> buildAuxIndexes;
  bool waitForMerges = false;
  bool present = false;
};

struct HttpStreamControl {
  std::optional<bool> allowDups;
  std::optional<std::string> requestId;
  HttpStreamCommitControl commit;
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
  std::size_t sourceBytes = 0;
  std::size_t docCount = 0;
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
  std::size_t firstDocIndex = 0;
  bool failed = false;
};

struct HttpStreamGroup {
  std::string requestId;
  std::vector<std::string> ids;
  std::vector<HttpStreamAccumError> errors;
  std::uint64_t lastUpdateVersion = 0;
  std::size_t firstDocIndex = 0;
  std::size_t docCount = 0;
  std::size_t docsIndexed = 0;
  std::size_t totalErrors = 0;
  bool allowDups = false;
  bool explicitRequestId = false;
  bool touched = false;
};

struct HttpStreamUpdateState {
  static constexpr std::size_t kBatchTargetBytes = 1024 * 1024;
  static constexpr std::size_t kBatchMaxDocs = 10000;
#ifdef NDEBUG
  static constexpr std::size_t kStreamReadBufBytes = 1024 * 1024;
#else
  static constexpr std::size_t kStreamReadBufBytes = 64 * 1024;
#endif
  static constexpr std::size_t kMaxRetainedErrors = 100;
  static constexpr std::size_t kMaxRetainedIds = 100;

  NdjsonFramer framer;
  std::vector<char> readBuf;
  std::string collectionName;
  std::shared_ptr<Collection> collection;
  std::shared_ptr<IndexWriter> indexWriter;
  std::unique_ptr<HttpStreamBatchState> batch;
  std::optional<HttpStreamControl> pendingControl;
  std::shared_ptr<net::executor_work_guard<net::any_io_executor>> workGuard;
  HttpStreamGroup group;
  std::size_t docsSeen = 0;
  std::size_t docsIndexedSoFar = 0;
  bool emittedGroup = false;
  bool bodyDone = false;
  bool tailFinished = false;
  bool updateInFlight = false;
  bool failed = false;

  HttpStreamUpdateState()
    : readBuf(kStreamReadBufBytes),
      batch(std::make_unique<HttpStreamBatchState>(kBatchMaxDocs)) {}
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
// thread producers reach it through enqueueLine.
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

  // Callable from any thread.  Queues a rendered NDJSON line (an owned string)
  // on the strand.  No completion callback: reply() already freed the arena the
  // line was rendered from, so the write depends on nothing but the string.
  void enqueueLine(std::string line, bool last) {
    net::dispatch(stream_.get_executor(),
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
  std::optional<http::request_parser<http::buffer_body>> parser_;
  std::array<char, 64 * 1024> bodyBuf_{};
  std::string bufferedBody_;
  std::shared_ptr<HttpStreamUpdateState> streamUpdate_;

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

  static constexpr std::uint64_t kBufferedBodyLimit = 8 * 1024 * 1024;

  void doRead() {
    parser_.emplace();
    parser_->body_limit(kBufferedBodyLimit);  // 8 MiB request-body cap
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
    if (auto q = target.find('?'); q != std::string_view::npos) {
      target = target.substr(0, q);
    }

    std::string coll;
    if (parser_->get().method() == http::verb::post && parseUpdatePath(target, coll) &&
        isNdjsonContentType(std::string_view(parser_->get()[http::field::content_type]))) {
      parser_->body_limit(boost::none);
      startStreamingUpdate(std::move(coll));
      return;
    }

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
    if (name.empty() || name.find('/') != std::string_view::npos) return false;
    coll.assign(name);
    return true;
  }

  static bool parseQueryPath(std::string_view target, std::string& coll) {
    return parseCollectionPath(target, "/query", coll);
  }

  static bool parseUpdatePath(std::string_view target, std::string& coll) {
    return parseCollectionPath(target, "/update", coll);
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
      if (const std::string* explain = findParam(params, "explain")) {
        if (*explain != "request") {
          respondSimple(http::status::bad_request, "application/json",
                        renderErrorBody("unknown explain mode '" + *explain + "' (valid: request)"));
          return;
        }
        handleExplain(req.body(), coll);
      } else {
        handleQuery(req.body(), coll);
      }
    } else if (req.method() == http::verb::post && parseUpdatePath(target, coll)) {
      handleUpdate(req.body(), coll);
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

  void handleQuery(const std::string& body, const std::string& coll) {
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

    auto& engine = node_.getSearchEngine();
    auto* sreq = solux::arenaCreate<HttpSearchRequest>(
        *arena, engine, std::move(requestState), shared_from_this(), *arena);
    // Keep the io_context busy until this query finishes so shutdown drains it.
    sreq->workGuard.emplace(stream_.get_executor());

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

    auto workGuard = std::make_shared<net::executor_work_guard<net::any_io_executor>>(
        stream_.get_executor());
    node_.getTaskArena().enqueue([self = shared_from_this(), state, workGuard] {
      http::status status = http::status::ok;
      std::string out;
      try {
        std::shared_ptr<Collection> collection =
            self->node_.resolveCollection(state->proto.collection ? &*state->proto.collection : nullptr);
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
      } catch (const std::exception& e) {
        status = http::status::internal_server_error;
        out = renderErrorBody(e.what());
      }

      net::post(self->stream_.get_executor(),
          [self, workGuard, status, body = std::move(out)]() mutable {
            self->respondSimple(status, "application/json", std::move(body));
          });
    });
  }

  static std::int32_t cappedErrorIndex(std::size_t index) {
    constexpr std::size_t max = (std::size_t)std::numeric_limits<std::int32_t>::max();
    return index > max ? std::numeric_limits<std::int32_t>::max() : (std::int32_t)index;
  }

  static void fillCommitParams(std::optional<solux::api::CommitParams>& out,
                               const HttpStreamCommitControl& control,
                               std::pmr::memory_resource& resource) {
    auto& params = out.emplace();
    params.commit_within_us = control.commitWithinUs;
    params.wait_for_merges = control.waitForMerges;
    std::string_view* names =
        solux::api::build::allocArray(params.build_aux_indexes, control.buildAuxIndexes.size(), resource);
    for (std::size_t i = 0; i < control.buildAuxIndexes.size(); i++) {
      names[i] = solux::api::build::arenaStr(resource, control.buildAuxIndexes[i]);
    }
  }

  static bool parseStreamCommitField(std::string_view name, const solux::api::Val& val,
                                     HttpStreamCommitControl& commit, std::string& err) {
    if (name == "commit_within_us") {
      const auto* n = std::get_if<std::int64_t>(&val.kind);
      if (n == nullptr || *n < 0) { err = "commit_within_us must be a non-negative integer"; return false; }
      commit.commitWithinUs = (std::uint64_t)*n;
      return true;
    }
    if (name == "wait_for_merges") {
      const auto* b = std::get_if<bool>(&val.kind);
      if (b == nullptr) { err = "wait_for_merges must be a boolean"; return false; }
      commit.waitForMerges = *b;
      return true;
    }
    if (name == "build_aux_indexes") {
      const auto* arr = std::get_if<solux::api::ArrStr>(&val.kind);
      if (arr == nullptr) { err = "build_aux_indexes must be an array of strings"; return false; }
      commit.buildAuxIndexes.clear();
      commit.buildAuxIndexes.reserve(arr->v.size());
      for (std::string_view item : arr->v) commit.buildAuxIndexes.emplace_back(item);
      return true;
    }
    err = "unsupported commit control field '" + std::string(name) + "'";
    return false;
  }

  // A control record is a JSON object whose sole field is "_update_". Its payload is
  // parsed as the v1 streaming control subset: request_id, allow_dups, and commit.
  static bool extractStreamControl(const solux::api::Map& map, HttpStreamControl& control,
                                   bool& isControl, std::string& err) {
    isControl = false;
    if (map.fields.size() != 1) return true;
    const auto& entry = *map.fields.begin();
    if (entry.first != "_update_") return true;

    isControl = true;
    const solux::api::Val& wrapper = *entry.second;
    const auto* payload = std::get_if<solux::api::Map>(&wrapper.kind);
    if (payload == nullptr) {
      err = "_update_ control value must be an object";
      return false;
    }

    for (const auto& [name, valView] : payload->fields) {
      const solux::api::Val& val = *valView;
      if (name == "allow_dups") {
        const auto* b = std::get_if<bool>(&val.kind);
        if (b == nullptr) { err = "allow_dups control must be a boolean"; return false; }
        control.allowDups = *b;
      } else if (name == "request_id") {
        const auto* s = std::get_if<std::string_view>(&val.kind);
        if (s == nullptr) { err = "request_id control must be a string"; return false; }
        control.requestId = std::string(*s);
      } else if (name == "commit") {
        const auto* commitMap = std::get_if<solux::api::Map>(&val.kind);
        if (commitMap == nullptr) { err = "commit control must be an object"; return false; }
        control.commit.present = true;
        for (const auto& [commitName, commitValView] : commitMap->fields) {
          if (!parseStreamCommitField(commitName, *commitValView, control.commit, err)) return false;
        }
      } else {
        err = "unsupported _update_ control field '" + std::string(name) + "'";
        return false;
      }
    }
    return true;
  }

  void startStreamingUpdate(std::string coll) {
    auto state = std::make_shared<HttpStreamUpdateState>();
    state->collectionName = std::move(coll);
    state->workGuard = std::make_shared<net::executor_work_guard<net::any_io_executor>>(
        stream_.get_executor());

    try {
      std::pmr::monotonic_buffer_resource targetResource;
      std::optional<solux::api::Target> target;
      setCollectionTarget(target, state->collectionName, targetResource);
      state->collection = node_.resolveCollection(&*target);
      state->indexWriter = state->collection->getShard()->getIndexWriter();
    } catch (const std::exception& e) {
      parser_.reset();
      streamUpdate_ = state;
      keepAlive_ = false;
      respondSimple(http::status::internal_server_error, "application/json", renderErrorBody(e.what()));
      return;
    }

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
    http::async_read(stream_, buffer_, *parser_,
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

  static bool shouldEmitStreamGroup(const HttpStreamUpdateState& state, bool last) {
    return state.group.touched || state.group.explicitRequestId || (last && !state.emittedGroup);
  }

  static void startStreamGroup(HttpStreamUpdateState& state, std::string requestId) {
    state.group = HttpStreamGroup();
    state.group.requestId = std::move(requestId);
    state.group.firstDocIndex = state.docsSeen;
    state.group.explicitRequestId = true;
  }

  static bool renderStreamGroupResponseLine(const HttpStreamGroup& group, std::string& out) {
    std::pmr::monotonic_buffer_resource responseResource;
    solux::api::UpdateResponse resp;
    resp.request_id = solux::api::build::arenaStr(responseResource, group.requestId);
    resp.update_version = group.lastUpdateVersion;

    solux::api::build::SpanBuilder<std::string_view> ids(responseResource);
    ids.reserve(group.ids.size());
    for (const auto& id : group.ids) ids.push_back(solux::api::build::arenaStr(responseResource, id));
    resp.ids = ids.finish();

    solux::api::build::SpanBuilder<solux::api::UpdateResponse_::Error> errors(responseResource);
    errors.reserve(group.errors.size());
    for (const auto& src : group.errors) {
      auto& dst = errors.emplace_back();
      dst.id = solux::api::build::arenaStr(responseResource, src.id);
      dst.error_message = solux::api::build::arenaStr(responseResource, src.errorMessage);
      dst.index = src.index;
    }
    resp.errors = errors.finish();

    if (group.totalErrors == 0) {
      resp.status = solux::api::UpdateResponse_::Status::OK;
    } else if (group.docsIndexed > 0) {
      resp.status = solux::api::UpdateResponse_::Status::PARTIAL;
    } else {
      resp.status = solux::api::UpdateResponse_::Status::ERROR;
    }
    if (group.totalErrors > group.errors.size()) {
      std::string msg = "retained first " + std::to_string(group.errors.size()) + " of " +
          std::to_string(group.totalErrors) + " errors";
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

  bool emitCurrentStreamGroup(bool last) {
    auto state = streamUpdate_;
    assert(state != nullptr);
    if (!shouldEmitStreamGroup(*state, last)) return true;

    std::string out;
    if (!renderStreamGroupResponseLine(state->group, out)) {
      failStreamingUpdate("failed to serialize update response");
      return false;
    }
    state->emittedGroup = true;
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
      std::string_view requestId = state ? std::string_view(state->group.requestId) : std::string_view();
      std::uint64_t updateVersion = state ? state->group.lastUpdateVersion : 0;
      if (!renderStreamErrorResponseLine(requestId, updateVersion, message, out)) {
        doClose();
        return;
      }
      enqueueLine(std::move(out), true);
      return;
    }
    respondSimple(http::status::bad_request, "application/json", renderErrorBody(message));
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
    if (!extractStreamControl(map, control, isControl, err)) {
      failStreamingUpdate(err);
      return false;
    }

    if (isControl) {
      if (state->batch->docs.size() > 0) {
        state->pendingControl = std::move(control);
        submitStreamBatch(nullptr);
        return false;
      }
      return applyStreamControl(std::move(control));
    }

    state->batch->docs.push_back(map);
    state->batch->sourceBytes += record.size();
    if (state->batch->sourceBytes >= HttpStreamUpdateState::kBatchTargetBytes ||
        state->batch->docs.size() >= HttpStreamUpdateState::kBatchMaxDocs) {
      submitStreamBatch(nullptr);
      return false;
    }
    return true;
  }

  bool applyStreamControl(HttpStreamControl control) {
    auto state = streamUpdate_;
    assert(state != nullptr);
    if (control.requestId) {
      // A request_id control is a group boundary; sibling fields apply to the new group.
      if (!emitCurrentStreamGroup(false)) return false;
      startStreamGroup(*state, std::move(*control.requestId));
    }
    if (control.allowDups) {
      state->group.allowDups = *control.allowDups;
      state->group.touched = true;
    }
    if (control.commit.present) {
      state->group.touched = true;
      submitStreamBatch(&control.commit);
      return false;
    }
    return true;
  }

  void submitStreamBatch(const HttpStreamCommitControl* commitControl) {
    auto state = streamUpdate_;
    assert(state != nullptr);
    assert(!state->updateInFlight);
    assert(state->batch != nullptr);

    state->batch->docCount = state->batch->docs.size();
    state->batch->firstDocIndex = state->docsSeen;
    state->docsSeen += state->batch->docCount;
    state->group.touched = true;
    state->group.docCount += state->batch->docCount;
    state->batch->proto.docs = state->batch->docs.finish();
    state->batch->proto.allow_dups = state->group.allowDups;
    setCollectionTarget(state->batch->proto.collection, state->collectionName, state->batch->resource);
    if (commitControl != nullptr) {
      fillCommitParams(state->batch->proto.commit, *commitControl, state->batch->resource);
    }

    std::shared_ptr<HttpStreamBatchState> batch(std::move(state->batch));
    state->batch = std::make_unique<HttpStreamBatchState>(HttpStreamUpdateState::kBatchMaxDocs);
    state->updateInFlight = true;

    auto iw = state->indexWriter;
    auto workGuard = state->workGuard;
    node_.getTaskArena().enqueue(
        [self = shared_from_this(), state, batch, iw, workGuard] {
          HttpStreamBatchResult result;
          result.docCount = batch->docCount;
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
              [self, state, batch, workGuard, result = std::move(result)]() mutable {
                self->onStreamBatchDone(state, batch, std::move(result));
              });
        });
  }

  void onStreamBatchDone(const std::shared_ptr<HttpStreamUpdateState>& state,
                         const std::shared_ptr<HttpStreamBatchState>& batch,
                         HttpStreamBatchResult result) {
    unused(batch);
    unused(result.status);
    if (streamUpdate_ != state || state->failed) return;
    state->updateInFlight = false;
    if (result.failed) {
      failStreamingUpdate("NDJSON update batch failed: " + result.errorMessage);
      return;
    }

    foldStreamBatchResult(result);

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
    auto& group = state->group;
    group.lastUpdateVersion = result.updateVersion;

    std::size_t failedDocs = result.errors.size();
    if (failedDocs > result.docCount) failedDocs = result.docCount;
    std::size_t indexedDocs = result.docCount - failedDocs;
    group.docsIndexed += indexedDocs;
    state->docsIndexedSoFar += indexedDocs;

    for (const auto& id : result.ids) {
      if (group.ids.size() < HttpStreamUpdateState::kMaxRetainedIds) group.ids.push_back(id);
    }

    for (const auto& err : result.errors) {
      group.totalErrors++;
      if (group.errors.size() >= HttpStreamUpdateState::kMaxRetainedErrors) continue;
      std::size_t globalIndex = result.firstDocIndex;
      if (err.index >= 0) globalIndex += (std::size_t)err.index;
      std::size_t groupIndex = globalIndex >= group.firstDocIndex ? globalIndex - group.firstDocIndex : 0;
      group.errors.push_back({err.id, err.errorMessage, cappedErrorIndex(groupIndex)});
    }

    if (!result.errorMessage.empty() && result.status == solux::api::UpdateResponse_::Status::ERROR) {
      group.totalErrors++;
      if (group.errors.size() < HttpStreamUpdateState::kMaxRetainedErrors) {
        std::size_t groupIndex =
            result.firstDocIndex >= group.firstDocIndex ? result.firstDocIndex - group.firstDocIndex : 0;
        group.errors.push_back({"", result.errorMessage, cappedErrorIndex(groupIndex)});
      }
    }
  }

  void drainStreamRecords() {
    auto state = streamUpdate_;
    if (!state || state->failed || state->updateInFlight) return;

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

      if (state->batch && state->batch->docs.size() > 0) {
        submitStreamBatch(nullptr);
        return;
      }
      finishStreamingUpdate();
      return;
    }

    doStreamBodyRead();
  }

  void finishStreamingUpdate() {
    auto state = streamUpdate_;
    if (!state || state->failed) return;
    parser_.reset();

    emitCurrentStreamGroup(true);
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
