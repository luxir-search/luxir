
#include <string>
#include <algorithm>
#include <cstddef>
#include <deque>
#include <memory_resource>
#include <mutex>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>
#include <grpcpp/alarm.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/generic/async_generic_service.h>
#include <grpcpp/health_check_service_interface.h>
#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/support/byte_buffer.h>
#include <hpp_proto/grpc/serialization.hpp>
#include <oneapi/tbb/task_group.h>

#include "GRPCServer.h"
#include "LuxirNode.h"
#include "luxir/api/padded_input.h"
#include "luxir/util/luxir_util.h"
#include "luxir/util/TaggedPtr.h"
#include "luxir/util/thread.h"
#include "luxir/util/proto.h"
#include "luxir/util/ApiError.h"
#include "RpcStatus.h"
#include "ProtoUpdateMessage.h"
#include "Stats.h"
#include "luxir/schema/Schema.h"
#include "luxir_descriptors.h"


namespace luxir {
// TODO - use a different logger for RPC stuff some point
// redefine DEBUG to TRACE level which shouldn't currently be logged!
#define GRPC_DEBUG LOG_TRACE
// #define GRPC_DEBUG LOG_DEBUG

GRPCServer::GRPCServer(LuxirNode& node, int nthreads, int port, int64_t streamBufferBytes)
  : luxirNode(node), startLatch(1), startLatchThreads(nthreads), nthreads(nthreads), requestedPort(port),
    // Clamp to >= 1: a non-positive high-water mark (misconfiguration) would
    // pause every reply while the drain check (queuedBytes <= low) never fires.
    streamBufferBytes_(std::max<int64_t>(1,
        streamBufferBytes > 0 ? streamBufferBytes
                              : node.getConfig().server.stream_buffer_bytes)) {
}

// NOTE: as of gRPC 1.39 there is a new C++ async callback API: https://github.com/grpc/grpc/pull/25728 in addition to an EventEngine
// interface that may help with integration with external event loops.

void luxir::GRPCServer::run() {
  pthread_setname_np(pthread_self(), "luxir_grpc_main");

  // Use requestedPort (default 0 for dynamic allocation, or a specific port
  // like 50051).  Dynamic test ports bind localhost; configured ports keep the
  // existing all-interfaces behavior.
  std::string bindHost = requestedPort == 0 ? "127.0.0.1" : "0.0.0.0";
  std::string server_address = bindHost + ":" + std::to_string(requestedPort);

  registerLuxirDescriptors();
  grpc::EnableDefaultHealthCheckService(true);
  grpc::reflection::InitProtoReflectionServerBuilderPlugin();
  grpc::ServerBuilder builder;
  // Listen on the given address without any authentication mechanism.
  // The actual port will be stored in serverPort
  builder.AddListeningPort(server_address, grpc::InsecureServerCredentials(), &serverPort);

  // One generic service handles every method as raw bytes; dispatch is by RPC
  // path inside GenericCallData.  registerLuxirDescriptors() makes describe and
  // FileContainingSymbol work through the generated descriptor pool.  TODO:
  // add a descriptor-backed reflection service for ListServices, since the
  // stock plugin lists registered typed services and generic service leaves it
  // empty.
  builder.RegisterAsyncGenericService(&genericService);

  threadInfos.reserve(nthreads);

  // Give each thread a completion queue, but don't let them use it
  // before the server starts (implemented with startLatch)
  for (int i=0; i<nthreads; i++) {
    threadInfos.emplace_back();
    threadInfos.back().threadno = i;
    threadInfos.back().cq = builder.AddCompletionQueue(); // each thread gets it's own completion queue
  }

  this->server = builder.BuildAndStart();
  if (!server) {
    LOG_ERROR("GRPCServer failed to listen on {}", server_address);
    startLatch.count_down();
    startLatchThreads.count_down(nthreads);
    return;
  }
  LOG_INFO("GRPCServer listening on {}:{}", bindHost, serverPort);

  threads.reserve(nthreads);
  for (int i=0; i<nthreads; i++) {
    threads.emplace_back([this,i]{ this->runThread(threadInfos[i]); });
  }

  // inform everyone that the server is up and running
  startLatch.count_down();
  // At this point the completion queues have all been created, but runThread() may not have continued (hence we have
  // not requested to the completion queue to handle certain messages.) If a client request comes in, it will still
  // wait to be handled (and then will be handled correctly.)  This was tested manually by adding a long sleep
  // in runThread() before requesting calls.

  // Wait for the server to shutdown. Note that some other thread must be
  // responsible for shutting down the server for this call to ever return.
  server->Wait();
  GRPC_DEBUG("server->Wait() returned!");
}


class CallData {
public:
  // The tagged pointer is to enable adding extra correlation information outside of the internal state machine
  // of each CallData.  Useful for handling things like AsyncNotifyWhenDone so you can tell what operation
  // happened on this CallData.
  using TaggedPtrType = TaggedPtr<CallData>;

  void* make_tag(uint32_t tag=0) {
    return TaggedPtrType(this, tag).packedBitsAsVoid();
  }

  CallData(GRPCServer& server, GRPCServer::ThreadInfo& threadInfo) : server(server), threadInfo(threadInfo) {
  }

  virtual ~CallData() = default;

  // The GRPC "tag" is a tagged pointer to this.  The pointer is unwrapped and proceed is called,
  // also passing the tag on "this".
  virtual void proceed(bool ok, uint32_t tag) = 0;

  GRPCServer& server;
  GRPCServer::ThreadInfo& threadInfo;
};


class GenericCallData;

// A routed method: how it parses the request bytes, runs the engine, and produces
// response bytes.  handle() is invoked once per inbound message (once for unary,
// repeatedly for client streams); it must arrange for call.respondRaw(...) to be
// called for each response and account for the responsesExpected++ the read loop
// did before calling it (respondRaw's finishCount, or decrementOutstanding()).
struct MethodEntry {
  void (*handle)(GenericCallData& call, grpc::ByteBuffer& readBuf);
  // Whether the method can write.  A read-only node refuses these at dispatch,
  // before any request bytes are read.  Storage refuses them too (see
  // ReadOnlyDirectory), but that backstop is for paths holding a Directory
  // directly - it is not a decent error for a caller.
  bool mutating = false;
};

static const MethodEntry* lookupMethod(const std::string& method);

using SearchReqProto = luxir::api::SearchRequest;
using UpdateReqProto = luxir::api::UpdateRequest;
using UpdateRespProto = luxir::api::UpdateResponse;
using SchemaReqProto = luxir::api::SchemaRequest;
using SchemaRespProto = luxir::api::SchemaResponse;
using CreateCollectionReqProto = luxir::api::CreateCollectionRequest;
using CreateCollectionRespProto = luxir::api::CreateCollectionResponse;
using DeleteCollectionReqProto = luxir::api::DeleteCollectionRequest;
using DeleteCollectionRespProto = luxir::api::DeleteCollectionResponse;
using StatsReqProto = luxir::api::StatsRequest;
using StatsRespProto = luxir::api::StatsResponse;
using CacheControlReqProto = luxir::api::CacheControlRequest;
using CacheControlRespProto = luxir::api::CacheControlResponse;

template <typename Message>
struct HppRequestState {
  std::vector<std::byte> wire;
  std::pmr::monotonic_buffer_resource resource;
  Message proto;
};

static grpc::Status dumpByteBuffer(grpc::ByteBuffer& buf, std::vector<std::byte>& wire) {
  std::vector<grpc::Slice> slices;
  auto status = buf.Dump(&slices);
  if (!status.ok()) {
    return status;
  }
  wire.clear();
  size_t size = 0;
  for (const auto& slice : slices) {
    size += slice.size();
  }
  wire.reserve(size + luxir::api::PADDED_PROTO_INPUT_BYTES);
  for (const auto& slice : slices) {
    const auto* data = (const std::byte*)slice.begin();
    wire.insert(wire.end(), data, data + slice.size());
  }
  wire.resize(size + luxir::api::PADDED_PROTO_INPUT_BYTES);
  return grpc::Status::OK;
}

static grpc::StatusCode grpcStatusCode(ErrorKind kind) {
  switch (kind) {
    case ErrorKind::INVALID_REQUEST: return grpc::StatusCode::INVALID_ARGUMENT;
    case ErrorKind::NOT_FOUND: return grpc::StatusCode::NOT_FOUND;
    case ErrorKind::ALREADY_EXISTS: return grpc::StatusCode::ALREADY_EXISTS;
    case ErrorKind::FAILED_PRECONDITION: return grpc::StatusCode::FAILED_PRECONDITION;
    case ErrorKind::RESOURCE_EXHAUSTED: return grpc::StatusCode::RESOURCE_EXHAUSTED;
    case ErrorKind::UNAVAILABLE: return grpc::StatusCode::UNAVAILABLE;
    case ErrorKind::INTERNAL:
    case ErrorKind::UNKNOWN: break;
  }
  return grpc::StatusCode::INTERNAL;
}

// The transport status for a classified failure: the kind's gRPC code, the
// message, and the luxir.Error packed into google.rpc.Status details so
// a generated client can read the stable code and kind.
static grpc::Status grpcStatus(const ErrorInfo& info) {
  grpc::StatusCode code = grpcStatusCode(info.kind);
  luxir::api::Error wire;
  wire.kind = (luxir::api::Error::Kind)info.kind;
  wire.code = info.code;
  wire.message = info.message;
  return grpc::Status(code, info.message, encodeRpcStatusDetails((int32_t)code, info.message, wire));
}

template <typename Message>
static bool parseRequest(grpc::ByteBuffer& buf, HppRequestState<Message>& state, std::string_view method) {
  auto grpcStatus = dumpByteBuffer(buf, state.wire);
  if (!grpcStatus.ok()) {
    LOG_ERROR("{}: failed to read request ByteBuffer: {}", method, grpcStatus.error_message());
    return false;
  }

  const size_t payloadSize = state.wire.size() - luxir::api::PADDED_PROTO_INPUT_BYTES;
  std::span<const std::byte> payload(state.wire.data(), payloadSize);
  if (!luxir::api::decode(state.proto, payload, state.resource)) {
    LOG_ERROR("{}: failed to parse request", method);
    return false;
  }
  return true;
}

// Serialize a concrete message into an OWNED ByteBuffer (decoupled from any arena).
// The encoded bytes are handed to gRPC without a copy: the Slice takes ownership of
// the heap-allocated vector and frees it when the transport drops its last ref.
template <typename Message>
static grpc::ByteBuffer serializeToByteBuffer(const Message& msg) {
  auto v = std::make_unique<std::vector<std::byte>>();
  if (!luxir::api::encode(msg, *v)) {
    throw ApiError(ErrorKind::INTERNAL, "internal", "failed to serialize the response");
  }
  if (v->empty()) {
    grpc::Slice slice;
    return grpc::ByteBuffer(&slice, 1);
  }
  grpc::Slice slice(v->data(), v->size(),
                    [](void* p) { delete (std::vector<std::byte>*)p; }, v.get());
  v.release();
  return grpc::ByteBuffer(&slice, 1);
}


// Generic raw call: one state machine for every method, routed by RPC path.
//
// Generalizes the Track-1 RawSearcherSearchStreamingCall onto grpc::AsyncGenericService.
// Every RPC - unary or streaming - is handled as a raw ByteBuffer stream: outgoing
// bytes are OWNED here (writeBuffer + pending), fully
// decoupled from any request/response arena, so handlers do eager cleanup with no
// post-write callback. A unary RPC is just a stream with one read + one write.
class GenericCallData : public CallData {
public:
  grpc::AsyncGenericService& genericService;
  grpc::GenericServerContext genericCtx;
  grpc::GenericServerAsyncReaderWriter readerWriter;  // ServerAsyncReaderWriter<ByteBuffer,ByteBuffer>
  grpc::ByteBuffer readBuf;  // incoming raw request bytes

  const MethodEntry* methodEntry = nullptr;  // resolved on CONNECT from the RPC path

  const int64_t highWater;  // response flow control: pause producers above this
  const int64_t lowWater;   // ... and resume them below this

  std::mutex mutex;
  // protected by mutex:
  std::deque<grpc::ByteBuffer> pending;  // buffered outgoing responses (owned bytes)
  grpc::ByteBuffer writeBuffer;          // bytes of the in-flight write; kept alive until WRITE completes
  int64_t queuedBytes = 0;               // bytes in pending + writeBuffer
  std::vector<std::function<void()>> drainWaiters;  // parked producer resumes
  bool errored = false;
  bool readsDone = false;
  bool writeOutstanding = false;
  bool finishSent = false;
  // Serializes async request execution per call: while a dispatched request is
  // active the next READ is not re-armed, so pipelined requests on one stream
  // execute (and respond) in order.  rearmPending marks a READ whose re-arm
  // was deferred to the active request's completion.
  bool requestActive = false;
  bool rearmPending = false;
  int32_t responsesExpected = 0;
  grpc::Status finishStatus = grpc::Status::OK;
  // Engaged when a non-cq thread needs maybeSendFinish() to run: the KICK tag
  // hands evaluation to this call's cq thread, the only thread allowed to
  // initiate Finish (a Finish initiated elsewhere could race the FINISH
  // completion deleting this object).  While engaged, Finish is held off so
  // the in-flight alarm tag can never dangle.
  std::optional<grpc::Alarm> finishKick;

  enum CallTags { READ = 1, WRITE = 2, FINISH = 3, CONNECT = 4, KICK = 5 };

  GenericCallData(GRPCServer& server, grpc::AsyncGenericService& genericService, GRPCServer::ThreadInfo& threadInfo)
      : CallData(server, threadInfo), genericService(genericService), readerWriter(&genericCtx),
        highWater(server.streamBufferBytes()), lowWater(server.streamBufferBytes() / 2) {
    genericService.RequestCall(&genericCtx, &readerWriter, threadInfo.cq.get(), threadInfo.cq.get(), make_tag(CONNECT));
  }

  void createNew() {
    new GenericCallData(server, genericService, threadInfo);
  }

  // mutex held
  void doWrite(grpc::ByteBuffer&& buf) {
    assert(!writeOutstanding);
    writeOutstanding = true;
    writeBuffer = std::move(buf);  // retain bytes for the duration of the async write
    readerWriter.Write(writeBuffer, make_tag(WRITE));
  }

  // mutex held; must only run on this call's cq thread (see finishKick).
  void maybeSendFinish() {
    if (!finishSent && !finishKick && readsDone && pending.empty() && !writeOutstanding &&
        responsesExpected <= 0) {
      finishSent = true;
      readerWriter.Finish(finishStatus, make_tag(FINISH));
    }
  }

  // mutex held; callable from any thread.  Schedules maybeSendFinish() on this
  // call's cq thread via an immediately-expiring alarm.
  void kickFinish() {
    if (finishSent || finishKick) return;
    finishKick.emplace();
    finishKick->Set(threadInfo.cq.get(), gpr_now(GPR_CLOCK_MONOTONIC), make_tag(KICK));
  }

  /// Enqueue an owned response (one outstanding write at a time).
  /// Returns the call's buffered (not-yet-written) response bytes - the
  /// producer's flow-control signal (compare against highWater) - or -1 if the
  /// call has errored and the response was dropped.
  /// NOTE: with finishCount>0 this may eventually delete "this" on another thread,
  /// so make it the last use of "this" in the caller.
  int64_t respondRaw(grpc::ByteBuffer&& buf, int32_t finishCount = 1) {
    const std::lock_guard<std::mutex> lock(mutex);
    responsesExpected -= finishCount;
    if (finishCount > 0) requestCompleted();
    if (errored) {
      // Client is gone: drop the bytes.  The finish accounting above must still
      // take effect, but Finish may only be initiated from the cq thread - kick
      // it over there.
      kickFinish();
      return -1;
    }
    queuedBytes += (int64_t)buf.Length();
    if (writeOutstanding) {
      pending.emplace_back(std::move(buf));
    } else {
      doWrite(std::move(buf));
    }
    return queuedBytes;
  }

  // mutex held.  The active async request sent its final response: allow the
  // next pipelined request to be read, re-arming the READ if proceed() already
  // deferred one to us.
  void requestCompleted() {
    requestActive = false;
    if (rearmPending && !readsDone && !finishSent) {
      rearmPending = false;
      readRequest();
    }
  }

  // Callable from any thread.  Parks a paused producer's resume callback; it is
  // handed to the task arena once buffered bytes drop below the low-water mark,
  // or immediately if the call has already drained or errored (the resumed
  // producer's next respondRaw then observes the error).
  void whenDrained(std::function<void()> resume) {
    {
      const std::lock_guard<std::mutex> lock(mutex);
      if (!errored && queuedBytes > lowWater) {
        drainWaiters.push_back(std::move(resume));
        return;
      }
    }
    enqueueResume(std::move(resume));
  }

  // Callable from any thread: finish evaluation goes through kickFinish so
  // Finish is only ever initiated on this call's cq thread.
  void decrementOutstanding(int32_t finishCount = 1) {
    const std::lock_guard<std::mutex> lock(mutex);
    responsesExpected -= finishCount;
    kickFinish();
  }

  // Callable from any thread (see decrementOutstanding).
  // Ends the call with `status` once every response already accepted has
  // been written: no further requests are read, but nothing accepted is
  // dropped, so a client of a multiplexed UpdateStream still sees the
  // acknowledgements that preceded the failure, then the terminal status.
  void finishWithError(grpc::Status status, int32_t finishCount = 1) {
    std::vector<std::function<void()>> waiters;
    {
      const std::lock_guard<std::mutex> lock(mutex);
      responsesExpected -= finishCount;
      readsDone = true;
      finishStatus = status;
      kickFinish();
      waiters = std::move(drainWaiters);
    }
    for (auto& w : waiters) enqueueResume(std::move(w));
  }

  void writeFinished(bool ok) {
    std::vector<std::function<void()>> waiters;
    {
      const std::lock_guard<std::mutex> lock(mutex);
      if (!ok) { errored = true; readsDone = true; }
      assert(writeOutstanding);
      writeOutstanding = false;
      queuedBytes -= (int64_t)writeBuffer.Length();
      writeBuffer.Clear();  // release the bytes we just wrote
      if (errored) {
        dropPending();  // don't chain doomed writes into a dead stream
      }
      if (!pending.empty()) {
        grpc::ByteBuffer next = std::move(pending.front());
        pending.pop_front();
        doWrite(std::move(next));
      } else {
        maybeSendFinish();
      }
      if (errored || queuedBytes <= lowWater) waiters = std::move(drainWaiters);
    }
    // Outside the lock: wake paused producers (below low-water, or so an
    // errored call's requests can finish and be freed).
    for (auto& w : waiters) enqueueResume(std::move(w));
  }

  // mutex held
  void dropPending() {
    for (const auto& p : pending) queuedBytes -= (int64_t)p.Length();
    pending.clear();
  }

  // Must not throw: callers run inside cq event handlers (runThread has no
  // catch), and a lost resume strands its request forever - so an enqueue
  // allocation failure falls back to running the resume inline.
  void enqueueResume(std::function<void()> resume) {
    try {
      server.getLuxirNode().getTaskArena().enqueue(resume);  // copies; resume stays valid if this throws
    } catch (...) {
      if (resume) resume();
    }
  }

  void readRequest() {
    readBuf.Clear();
    readerWriter.Read(&readBuf, make_tag(READ));
  }

  virtual void proceed(bool ok, uint32_t tag) override {
    switch (tag) {
      case CONNECT:
        if (!ok) { delete this; break; }
        // re-arm a fresh acceptor before doing any work
        createNew();
        methodEntry = lookupMethod(genericCtx.method());
        if (methodEntry == nullptr) {
          LOG_ERROR("gRPC: no handler for method '{}'", genericCtx.method());
          readsDone = true;
          finishSent = true;
          readerWriter.Finish(grpc::Status(grpc::StatusCode::UNIMPLEMENTED, "unknown method"), make_tag(FINISH));
          break;
        }
        if (methodEntry->mutating && server.getLuxirNode().readOnly()) {
          readsDone = true;
          finishSent = true;
          readerWriter.Finish(
              grpcStatus({ErrorKind::FAILED_PRECONDITION, "read_only",
                          "node is read-only (--read-only): " + genericCtx.method() +
                              " is not allowed"}),
              make_tag(FINISH));
          break;
        }
        readRequest();
        break;
      case READ:
        if (!ok) {
          readsDone = true;
          { const std::lock_guard<std::mutex> lock(mutex); maybeSendFinish(); }
          break;
        }
        // respondRaw() can be called (possibly from another thread) before handle()
        // returns, so account for the expected response before dispatching.
        { const std::lock_guard<std::mutex> lock(mutex); responsesExpected++; }
        methodEntry->handle(*this, readBuf);
        {
          const std::lock_guard<std::mutex> lock(mutex);
          if (readsDone || finishSent) break;
          if (requestActive) {
            // Async request still running (or paused on flow control): defer
            // the next READ to its completion so pipelined requests on this
            // stream execute, and respond, in order.
            rearmPending = true;
            break;
          }
        }
        readRequest();
        break;
      case WRITE:
        writeFinished(ok);
        break;
      case KICK: {
        // A non-cq thread requested a finish evaluation (ok is irrelevant -
        // even a cancelled alarm must clear finishKick so Finish can proceed).
        const std::lock_guard<std::mutex> lock(mutex);
        finishKick.reset();
        maybeSendFinish();
        break;
      }
      case FINISH:
        delete this;
        break;
      default:
        LOG_ERROR("Unknown tag {} on generic call {}", tag, (void*)this);
        break;
    }
  }
};


static std::shared_ptr<Collection> resolveCollection(GRPCServer& server,
                                                     std::string_view collection) {
  return server.getLuxirNode().resolveCollection(collection);
}

template <typename Request>
static std::shared_ptr<Collection> resolveUpdateCollection(GRPCServer& server, const Request& request) {
  return server.getLuxirNode().resolveOrCreateCollection(request.collection);
}

static std::shared_ptr<Collection> resolveSetSchemaCollection(GRPCServer& server,
                                                              std::string_view collection) {
  return server.getLuxirNode().resolveOrCreateCollection(collection);
}

static void finishWithError(GenericCallData& call, const ErrorInfo& info) {
  call.finishWithError(grpcStatus(info));
}

// Unary handlers run after request parsing, so an unclassified exception is
// the engine's; the request-class failures they meet (collection resolution,
// schema validation, read-only storage) are typed.
static void finishWithException(GenericCallData& call, const std::exception& e,
                                ErrorKind fallback = ErrorKind::INTERNAL) {
  finishWithError(call, classifyException(e, fallback));
}


// ---- per-method handlers -------------------------------------------------

//   rpc Search(stream SearchRequest) returns (stream SearchResponse)
static void handleSearch(GenericCallData& call, grpc::ByteBuffer& readBuf) {
  // SearchRequest subclass whose reply() serializes to ByteBuffer and drops eagerly.
  class GRPCSearchRequest : public SearchRequest {
  public:
    GenericCallData* parent = nullptr;
    // Owns the padded request bytes and parse resource that `proto` (and the
    // op tree) borrow views of: submit() runs async on the task arena and a
    // flow-controlled emitter can outlive it, so the views must live until
    // done() releases this request.
    std::unique_ptr<HppRequestState<SearchReqProto>> requestState;

    GRPCSearchRequest(SearchEngine& engine, const SearchReqProto& proto, google::protobuf::Arena& arena)
      : SearchRequest(engine, proto, arena) {}

    ReplyStatus reply(SearchResponse& response) override {
      // Serialize eagerly into an OWNED ByteBuffer, then drop arenas (no post-write callback).
      response.proto.more = !response.last;
      grpc::ByteBuffer buf;
      try {
        buf = serializeToByteBuffer(response.proto);
      } catch (const std::exception& e) {
        // The response cannot be delivered: the call ends with the failure
        // instead of a silently empty message.
        parent->finishWithError(grpcStatus(classifyException(e, ErrorKind::INTERNAL)),
                                response.last ? 1 : 0);
        response.req.replyCallback(response);
        return ReplyStatus::CANCEL;
      }
      // Only the request's final response decrements the call's outstanding
      // count: an intermediate batch must not, or a transiently drained queue
      // after the client's WritesDone would Finish the call mid-stream.
      // Snapshot highWater first: once the final response is handed to
      // respondRaw, the call can Finish and delete parent at any moment.
      const int64_t highWater = parent->highWater;
      int64_t buffered = parent->respondRaw(std::move(buf), response.last ? 1 : 0);
      auto status = ReplyStatus::OK;
      if (buffered < 0) status = ReplyStatus::CANCEL;
      else if (buffered > highWater) status = ReplyStatus::PAUSE;
      // replyCallback() may delete "this" on the last response, so it must be the
      // last use of "this"/"response".
      response.req.replyCallback(response);
      return status;
    }

    void resumeWhenDrained(std::function<void()> resume) override {
      parent->whenDrained(std::move(resume));
    }
  };

  auto* arena = createArena();
  auto requestState = std::make_unique<HppRequestState<SearchReqProto>>();
  if (!parseRequest(readBuf, *requestState, "Search")) {
    releaseArena(arena);
    finishWithError(call, ErrorInfo::of(ErrorKind::INVALID_REQUEST, "Search: malformed request"));
    return;
  }
  if (requestState->proto.response_format == luxir::api::ResponseFormat::DOCS) {
    releaseArena(arena);
    // Doc-line framing is an HTTP/NDJSON concept; gRPC responses are already
    // framed DocList messages.
    finishWithError(call, ErrorInfo::of(ErrorKind::INVALID_REQUEST,
                                        "response_format=docs applies to the HTTP NDJSON layer only"));
    return;
  }
  auto& engine = call.server.getLuxirNode().getSearchEngine();
  auto& req = *luxir::arenaCreate<GRPCSearchRequest>(*arena, engine, requestState->proto, *arena);
  req.requestState = std::move(requestState);
  req.parent = &call;
  // Route by max_parallel (via dispatch()): the default (0) runs the whole
  // query serially right here on this completion-queue thread - the cheapest
  // path, accepting that this cq's other calls wait for the query's duration
  // (a paused streaming producer parks and returns, so flow control still
  // advances).  Non-zero values run on the task arena instead, keeping this
  // cq thread free to process completions.  The call outlives the request
  // because its outstanding-response count stays positive until the final
  // reply, and requestActive keeps pipelined requests on this stream ordered.
  { const std::lock_guard<std::mutex> lock(call.mutex); call.requestActive = true; }
  engine.dispatch(req, req.proto.max_parallel);
}


// Shared blocking update used by the unary Update handler. Returns the serialized
// response: the response is NON-OWNING and its spans are backed by updateMessage's arena,
// so it must be serialized here (while updateMessage is alive), not by the caller.
static grpc::ByteBuffer doBlockingUpdate(GRPCServer& server, const UpdateReqProto& request) {
  std::shared_ptr<Collection> collection = resolveUpdateCollection(server, request);

  auto shard = collection->getShard();
  auto iw = shard->getIndexWriter();

  class BlockingUpdateMessage : public ProtoUpdateMessage {
  public:
    Blocker blocker;
    BlockingUpdateMessage(const UpdateReqProto* req, UpdateRespProto* response)
        : ProtoUpdateMessage(req, response) {}
    virtual void done(IndexWriter& iw) override {
      unused(iw);
      blocker.notify();
    }
  };

  UpdateRespProto response;
  BlockingUpdateMessage updateMessage(&request, &response);
  // A closed writer still admits the message; it comes back errored in the response.
  if (!iw->submitUpdate(&updateMessage)) throw std::runtime_error("update was not admitted");
  updateMessage.blocker.wait();
  updateMessage.finishResponse();
  return serializeToByteBuffer(response);
}

//   rpc Update(UpdateRequest) returns (UpdateResponse)  [unary]
static void handleUpdate(GenericCallData& call, grpc::ByteBuffer& readBuf) {
  HppRequestState<UpdateReqProto> request;
  if (!parseRequest(readBuf, request, "Update")) {
    finishWithError(call, ErrorInfo::of(ErrorKind::INVALID_REQUEST, "Update: malformed request"));
    return;
  }
  try {
    grpc::ByteBuffer buf = doBlockingUpdate(call.server, request.proto);
    call.respondRaw(std::move(buf), 1);
  } catch (const std::exception& e) {
    finishWithException(call, e);
  }
}

//   rpc UpdateStream(stream UpdateRequest) returns (stream UpdateResponse)
static void handleUpdateStream(GenericCallData& call, grpc::ByteBuffer& readBuf) {
  auto request = std::make_unique<HppRequestState<UpdateReqProto>>();
  if (!parseRequest(readBuf, *request, "UpdateStream")) {
    finishWithError(call, ErrorInfo::of(ErrorKind::INVALID_REQUEST, "UpdateStream: malformed request"));
    return;
  }

  // The update is async; its done() serializes the response into owned bytes, hands
  // them to the call, then deletes this message, freeing the borrowed request bytes
  // and response state.
  class Update : public ProtoUpdateMessage {
  public:
    std::unique_ptr<HppRequestState<UpdateReqProto>> request;
    GenericCallData* parent;
    Update(std::unique_ptr<HppRequestState<UpdateReqProto>> requestState, GenericCallData* parent)
      : ProtoUpdateMessage(&requestState->proto), request(std::move(requestState)), parent(parent) {}
    virtual void done(IndexWriter& iw) override {
      unused(iw);
      auto* response = finishResponse();
      try {
        grpc::ByteBuffer buf = serializeToByteBuffer(*response);
        parent->respondRaw(std::move(buf), 1);     // may delete parent on another thread
      } catch (const std::exception& e) {
        parent->finishWithError(grpcStatus(classifyException(e, ErrorKind::INTERNAL)));
      }
      delete this;                                 // frees request bytes, parse resource, and response
    }
  };

  try {
    auto collection = resolveUpdateCollection(call.server, request->proto);
    auto shard = collection->getShard();
    auto iw = shard->getIndexWriter();

    Update* updateMessage = new Update(std::move(request), &call);
    try {
      if (!iw->submitUpdate(updateMessage)) {
        throw std::runtime_error("update was not admitted");
      }
    } catch (...) {
      delete updateMessage;
      throw;
    }
  } catch (const std::exception& e) {
    finishWithException(call, e);
  }
}

//   rpc CreateCollection(CreateCollectionRequest) returns (CreateCollectionResponse)  [unary]
static void handleCreateCollection(GenericCallData& call, grpc::ByteBuffer& readBuf) {
  auto request = std::make_shared<HppRequestState<CreateCollectionReqProto>>();
  if (!parseRequest(readBuf, *request, "CreateCollection")) {
    finishWithError(call, ErrorInfo::of(ErrorKind::INVALID_REQUEST, "CreateCollection: malformed request"));
    return;
  }

  { const std::lock_guard<std::mutex> lock(call.mutex); call.requestActive = true; }
  try {
    call.server.getLuxirNode().getTaskArena().enqueue([request, &call] {
      try {
        call.server.getLuxirNode().createCollection(
            nullptr, request->proto.name,
            request->proto.schema ? &*request->proto.schema : nullptr);
        CreateCollectionRespProto response;
        response.name = request->proto.name;
        call.respondRaw(serializeToByteBuffer(response), 1);
      } catch (const std::exception& e) {
        finishWithException(call, e);
      }
    });
  } catch (const std::exception& e) {
    finishWithException(call, e);
  }
}

//   rpc DeleteCollection(DeleteCollectionRequest) returns (DeleteCollectionResponse)  [unary]
static void handleDeleteCollection(GenericCallData& call, grpc::ByteBuffer& readBuf) {
  auto request = std::make_shared<HppRequestState<DeleteCollectionReqProto>>();
  if (!parseRequest(readBuf, *request, "DeleteCollection")) {
    finishWithError(call, ErrorInfo::of(ErrorKind::INVALID_REQUEST, "DeleteCollection: malformed request"));
    return;
  }

  { const std::lock_guard<std::mutex> lock(call.mutex); call.requestActive = true; }
  try {
    call.server.getLuxirNode().getTaskArena().enqueue([request, &call] {
      try {
        call.server.getLuxirNode().deleteCollection(request->proto.name);
        DeleteCollectionRespProto response;
        response.name = request->proto.name;
        call.respondRaw(serializeToByteBuffer(response), 1);
      } catch (const std::exception& e) {
        finishWithException(call, e);
      }
    });
  } catch (const std::exception& e) {
    finishWithException(call, e);
  }
}

//   rpc SetSchema(SchemaRequest) returns (SchemaResponse)  [unary]
static void handleSetSchema(GenericCallData& call, grpc::ByteBuffer& readBuf) {
  HppRequestState<SchemaReqProto> request;
  if (!parseRequest(readBuf, request, "SetSchema")) {
    finishWithError(call, ErrorInfo::of(ErrorKind::INVALID_REQUEST, "SetSchema: malformed request"));
    return;
  }
  if (!request.proto.schema.has_value()) {
    finishWithError(call, ErrorInfo::of(ErrorKind::INVALID_REQUEST, "SetSchema: schema is required"));
    return;
  }
  try {
    SchemaRespProto response;
    std::pmr::monotonic_buffer_resource respArena;  // backs the non-owning response SchemaDef

    auto collection = resolveSetSchemaCollection(call.server, request.proto.collection);
    auto newSchema = collection->updateSchema(*request.proto.schema, request.proto.mode);
    newSchema->toProto(&response.schema.emplace(), respArena);

    grpc::ByteBuffer buf = serializeToByteBuffer(response);
    call.respondRaw(std::move(buf), 1);
  } catch (const std::exception& e) {
    finishWithException(call, e);
  }
}

//   rpc GetSchema(SchemaRequest) returns (SchemaResponse)  [unary]
static void handleGetSchema(GenericCallData& call, grpc::ByteBuffer& readBuf) {
  HppRequestState<SchemaReqProto> request;
  if (!parseRequest(readBuf, request, "GetSchema")) {
    finishWithError(call, ErrorInfo::of(ErrorKind::INVALID_REQUEST, "GetSchema: malformed request"));
    return;
  }
  try {
    SchemaRespProto response;
    std::pmr::monotonic_buffer_resource respArena;  // backs the non-owning response SchemaDef

    auto collection = resolveCollection(call.server, request.proto.collection);
    auto schema = collection->getSchema();
    schema->toProto(&response.schema.emplace(), respArena);

    grpc::ByteBuffer buf = serializeToByteBuffer(response);
    call.respondRaw(std::move(buf), 1);
  } catch (const std::exception& e) {
    finishWithException(call, e);
  }
}

//   rpc Stats(StatsRequest) returns (StatsResponse)  [unary]
static void handleStats(GenericCallData& call, grpc::ByteBuffer& readBuf) {
  auto request = std::make_shared<HppRequestState<StatsReqProto>>();
  if (!parseRequest(readBuf, *request, "Stats")) {
    finishWithError(call, ErrorInfo::of(ErrorKind::INVALID_REQUEST, "Stats: malformed request"));
    return;
  }

  // Per-segment output is unbounded. Keep both collection sampling and
  // serialization off the completion-queue thread; responsesExpected keeps
  // the call alive and requestActive preserves per-call request ordering.
  { const std::lock_guard<std::mutex> lock(call.mutex); call.requestActive = true; }
  try {
    call.server.getLuxirNode().getTaskArena().enqueue([request, &call] {
      try {
        StatsRespProto response;
        std::pmr::monotonic_buffer_resource respArena;
        gatherStats(call.server.getLuxirNode(), request->proto, response, respArena);
        call.respondRaw(serializeToByteBuffer(response), 1);
      } catch (const std::exception& e) {
        finishWithException(call, e);
      }
    });
  } catch (const std::exception& e) {
    finishWithException(call, e);
  }
}

//   rpc CacheControl(CacheControlRequest) returns (CacheControlResponse)  [unary]
static void handleCacheControl(GenericCallData& call, grpc::ByteBuffer& readBuf) {
  auto request = std::make_shared<HppRequestState<CacheControlReqProto>>();
  if (!parseRequest(readBuf, *request, "CacheControl")) {
    finishWithError(call, ErrorInfo::of(ErrorKind::INVALID_REQUEST, "CacheControl: malformed request"));
    return;
  }

  // A dump is unbounded like per-segment stats, and a flush walks the entry
  // map; keep both off the completion-queue thread.
  { const std::lock_guard<std::mutex> lock(call.mutex); call.requestActive = true; }
  try {
    call.server.getLuxirNode().getTaskArena().enqueue([request, &call] {
      try {
        CacheControlRespProto response;
        std::pmr::monotonic_buffer_resource respArena;
        gatherCacheControl(call.server.getLuxirNode(), request->proto, response,
                           respArena);
        call.respondRaw(serializeToByteBuffer(response), 1);
      } catch (const std::exception& e) {
        finishWithException(call, e);
      }
    });
  } catch (const std::exception& e) {
    finishWithException(call, e);
  }
}

// ---- method routing ------------------------------------------------------

static const MethodEntry* lookupMethod(const std::string& method) {
  static const std::unordered_map<std::string, MethodEntry> table = {
    {"/luxir.Searcher/Search",          {handleSearch}},
    {"/luxir.Indexer/Update",           {handleUpdate, true}},
    {"/luxir.Indexer/UpdateStream",     {handleUpdateStream, true}},
    {"/luxir.Admin/SetSchema",          {handleSetSchema, true}},
    {"/luxir.Admin/GetSchema",          {handleGetSchema}},
    {"/luxir.Admin/CreateCollection",   {handleCreateCollection, true}},
    {"/luxir.Admin/DeleteCollection",   {handleDeleteCollection, true}},
    {"/luxir.Admin/Stats",              {handleStats}},
    {"/luxir.Admin/CacheControl",       {handleCacheControl}},
  };
  auto it = table.find(method);
  return it == table.end() ? nullptr : &it->second;
}


void GRPCServer::runThread(ThreadInfo& threadInfo) {
  // linux-only: give threads a nice name for debugging.
  std::string tname = "luxir_grpc_" + std::to_string(threadInfo.threadno);
  pthread_setname_np(pthread_self(), tname.c_str());

  // wait for the server to start before trying to use it.
  startLatch.wait();

  // Used to test that a client request will still be handled correctly if it comes in before we've registered the calls
  // std::this_thread::sleep_for (std::chrono::seconds(10));

  // Pre-arm several generic acceptors per CQ (each re-arms on accept, so the
  // accept backlog stays at this depth).  Matches the previous depth of 8
  // per-method acceptors.
  constexpr int kGenericAcceptors = 8;
  for (int i = 0; i < kGenericAcceptors; i++) {
    new GenericCallData(*this, genericService, threadInfo);
  }

  /*
   * This startLatchThreads was added because shutting down the server very quickly would generate this failed assertion:
   * E0723 22:24:54.308864118 3078 server_cc.cc:216]  assertion failed: grpc_server_request_registered_call( server_->server(), registered_method, &call_, &context_->deadline_, context_->client_metadata_.arr(), payload, call_cq_->cq(), notification_cq->cq(), this) == GRPC_CALL_OK
   * It only happened in release mode (not debug mode) presumably because of timing.  Now GRPCServer::waitForStart()
   * waits for the calls to be registered into the completion queue before signalling that we are ready.
   * Best guess is that it was these registrations that were failing after the completion queue was shut down.
   */
  startLatchThreads.count_down(); // signal that we registered calls

  while (true) {
    void* tag;  // uniquely identifies a request.
    bool ok;
    bool gotEvent = threadInfo.cq->Next(&tag, &ok);

    if (!gotEvent) {
      // shutting down... completion queue should be empty at this point.
      GRPC_DEBUG("completionQueue->Next() returned false.");
      break;
    }

    CallData::TaggedPtrType taggedPtr = CallData::TaggedPtrType::fromTaggedPtrBits(tag);
    CallData* callData = taggedPtr.ptr();
    GRPC_DEBUG("PRECALL: this={} tag={} ok={}", (void*)callData, taggedPtr.tag(), ok);
    callData->proceed(ok, taggedPtr.tag());
    assert(MemPool::sanityCheck());
  }

  GRPC_DEBUG("GRPCServer thread shutting down.");
}


bool GRPCServer::waitForStart() {
  startLatchThreads.wait();
  return server != nullptr;
}

/*** NOTE: after upgrading from grpc1.41 (and dependencies) to grpc1.44 via vcpkg,
 * valgrind reports a definite memory leak (along with a possible memory leak in abseil):
    Leak_DefinitelyLost
    ares_library_init.c
    10 bytes in 1 blocks are definitely lost in loss record 12 of 76
    0x4848899 malloc
    0x11C77D1 default_malloc ares_library_init.c:48
    0x11CD532 ares_strdup ares_strdup.c:33
    0x11D3C1D ares__readaddrinfo ares__readaddrinfo.c:62
    0x11D0FB2 file_lookup ares_getaddrinfo.c:477
    0x11D10B2 next_lookup ares_getaddrinfo.c:513
    0x11D1663 ares_getaddrinfo ares_getaddrinfo.c:698
    0x11C2BCA ares_gethostbyname ares_gethostbyname.c:113
    0xF94E55 grpc_dns_lookup_ares_continue_after_check_localhost_and_ip_literals_locked grpc_ares_wrapper.cc:875
    0xF959FA grpc_dns_lookup_ares_impl grpc_ares_wrapper.cc:1069
    0xF8C8DF grpc_core::AresClientChannelDNSResolver::StartResolvingLocked dns_resolver_ares.cc:454
    0xF8C710 grpc_core::AresClientChannelDNSResolver::MaybeStartResolvingLocked dns_resolver_ares.cc:443

    Another grpc example (written by someone else) also showed a leak after upgrading.
 */
void luxir::GRPCServer::shutdown() {
  LOG_INFO("Shutting down luxir grpc server.");
  if (!server) {
    return;
  }

  // server should be shut down before completion queues
  server->Shutdown();

  GRPC_DEBUG("server->Shutdown() returned.");

  // calling Shutdown on the completion queue will cause cq->Next() to return false
  // rather than block.
  for (auto& threadInfo : threadInfos) {
    GRPC_DEBUG("Calling completion queue Shutdown for cq={}", (void*)threadInfo.cq.get());
    threadInfo.cq->Shutdown();
  }

  GRPC_DEBUG("joining threads");

  for (auto& thread : threads) {
    thread.join();
  }

  GRPC_DEBUG("shutdown complete");
}

} //  namespace luxir
