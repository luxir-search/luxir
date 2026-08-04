
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
#include <absl/strings/str_cat.h>
#include <grpcpp/health_check_service_interface.h>
#include <grpcpp/ext/proto_server_reflection_plugin.h>
#include <grpcpp/support/byte_buffer.h>
#include <hpp_proto/grpc/serialization.hpp>
#include <oneapi/tbb/task_group.h>

#include "GRPCServer.h"
#include "SoluxNode.h"
#include "solux/api/padded_input.h"
#include "solux/api/solux.hpp"
#include "solux/util/random.h"
#include "solux/util/solux_util.h"
#include "solux/util/TaggedPtr.h"
#include "solux/util/thread.h"
#include "solux/util/proto.h"
#include "ProtoUpdateMessage.h"
#include "Stats.h"
#include "solux/schema/Schema.h"
#include "solux_descriptors.h"


namespace solux {
// TODO - use a different logger for RPC stuff some point
// redefine DEBUG to TRACE level which shouldn't currently be logged!
#define GRPC_DEBUG LOG_TRACE
// #define GRPC_DEBUG LOG_DEBUG

GRPCServer::GRPCServer(SoluxNode& node, int nthreads, int port, int64_t streamBufferBytes)
  : soluxNode(node), startLatch(1), startLatchThreads(nthreads), nthreads(nthreads), requestedPort(port),
    // Clamp to >= 1: a non-positive high-water mark (misconfiguration) would
    // pause every reply while the drain check (queuedBytes <= low) never fires.
    streamBufferBytes_(std::max<int64_t>(1,
        streamBufferBytes > 0 ? streamBufferBytes
                              : node.getConfig().server.stream_buffer_bytes)) {
}

// NOTE: as of gRPC 1.39 there is a new C++ async callback API: https://github.com/grpc/grpc/pull/25728 in addition to an EventEngine
// interface that may help with integration with external event loops.

void solux::GRPCServer::run() {
  pthread_setname_np(pthread_self(), "solux_grpc_main");

  // Use requestedPort (default 0 for dynamic allocation, or a specific port
  // like 50051).  Dynamic test ports bind localhost; configured ports keep the
  // existing all-interfaces behavior.
  std::string bindHost = requestedPort == 0 ? "127.0.0.1" : "0.0.0.0";
  std::string server_address = bindHost + ":" + std::to_string(requestedPort);

  registerSoluxDescriptors();
  grpc::EnableDefaultHealthCheckService(true);
  grpc::reflection::InitProtoReflectionServerBuilderPlugin();
  grpc::ServerBuilder builder;
  // Listen on the given address without any authentication mechanism.
  // The actual port will be stored in serverPort
  builder.AddListeningPort(server_address, grpc::InsecureServerCredentials(), &serverPort);

  // One generic service handles every method as raw bytes; dispatch is by RPC
  // path inside GenericCallData.  registerSoluxDescriptors() makes describe and
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
};

static const MethodEntry* lookupMethod(const std::string& method);

using SearchReqProto = solux::api::SearchRequest;
using UpdateReqProto = solux::api::UpdateRequest;
using UpdateRespProto = solux::api::UpdateResponse;
using SchemaReqProto = solux::api::SchemaRequest;
using SchemaRespProto = solux::api::SchemaResponse;
using CreateCollectionReqProto = solux::api::CreateCollectionRequest;
using CreateCollectionRespProto = solux::api::CreateCollectionResponse;
using DeleteCollectionReqProto = solux::api::DeleteCollectionRequest;
using DeleteCollectionRespProto = solux::api::DeleteCollectionResponse;
using StatsReqProto = solux::api::StatsRequest;
using StatsRespProto = solux::api::StatsResponse;
using HelloReqProto = solux::api::HelloRequest;
using HelloRespProto = solux::api::HelloReply;

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
  wire.reserve(size + solux::api::PADDED_PROTO_INPUT_BYTES);
  for (const auto& slice : slices) {
    const auto* data = (const std::byte*)slice.begin();
    wire.insert(wire.end(), data, data + slice.size());
  }
  wire.resize(size + solux::api::PADDED_PROTO_INPUT_BYTES);
  return grpc::Status::OK;
}

template <typename Message>
static bool parseRequest(grpc::ByteBuffer& buf, HppRequestState<Message>& state, std::string_view method) {
  auto grpcStatus = dumpByteBuffer(buf, state.wire);
  if (!grpcStatus.ok()) {
    LOG_ERROR("{}: failed to read request ByteBuffer: {}", method, grpcStatus.error_message());
    return false;
  }

  const size_t payloadSize = state.wire.size() - solux::api::PADDED_PROTO_INPUT_BYTES;
  std::span<const std::byte> payload(state.wire.data(), payloadSize);
  if (!solux::api::decode(state.proto, payload, state.resource)) {
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
  if (!solux::api::encode(msg, *v)) {
    LOG_ERROR("gRPC: failed to serialize response");
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
  // Finish is only ever initiated on this call's cq thread (handlers may run
  // this from task-arena workers, e.g. async SayHelloStreaming).
  void decrementOutstanding(int32_t finishCount = 1) {
    const std::lock_guard<std::mutex> lock(mutex);
    responsesExpected -= finishCount;
    kickFinish();
  }

  // Callable from any thread (see decrementOutstanding).
  void finishWithError(grpc::Status status, int32_t finishCount = 1) {
    std::vector<std::function<void()>> waiters;
    {
      const std::lock_guard<std::mutex> lock(mutex);
      responsesExpected -= finishCount;
      readsDone = true;
      dropPending();
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
      server.getSoluxNode().getTaskArena().enqueue(resume);  // copies; resume stays valid if this throws
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


// Helper to resolve a Collection from a Target proto
static std::shared_ptr<Collection> resolveCollection(GRPCServer& server, const solux::api::Target* target) {
  return server.getSoluxNode().resolveCollection(target);
}

static std::shared_ptr<Collection> resolveCollection(GRPCServer& server,
                                                     const std::optional<solux::api::Target>& target) {
  return resolveCollection(server, target.has_value() ? &*target : nullptr);
}

template <typename Request>
static std::shared_ptr<Collection> resolveUpdateCollection(GRPCServer& server, const Request& request) {
  return server.getSoluxNode().resolveOrCreateCollection(request.collection.has_value() ? &*request.collection : nullptr);
}

static std::shared_ptr<Collection> resolveSetSchemaCollection(GRPCServer& server,
                                                              const std::optional<solux::api::Target>& target) {
  return server.getSoluxNode().resolveOrCreateCollection(target.has_value() ? &*target : nullptr);
}

static void finishWithException(GenericCallData& call, const std::exception& e) {
  grpc::StatusCode code = grpc::StatusCode::INTERNAL;
  if (dynamic_cast<const InvalidCollectionNameError*>(&e) != nullptr) {
    auto method = call.genericCtx.method();
    code = method == "/solux.Admin/CreateCollection" ||
                   method == "/solux.Admin/DeleteCollection"
        ? grpc::StatusCode::INVALID_ARGUMENT
        : grpc::StatusCode::NOT_FOUND;
  } else if (dynamic_cast<const CollectionExistsError*>(&e) != nullptr) {
    code = grpc::StatusCode::ALREADY_EXISTS;
  } else if (dynamic_cast<const CollectionResolutionError*>(&e) != nullptr) {
    code = grpc::StatusCode::NOT_FOUND;
  } else if (dynamic_cast<const SchemaError*>(&e) != nullptr) {
    code = grpc::StatusCode::INVALID_ARGUMENT;
  } else if (dynamic_cast<const IndexWriterClosedError*>(&e) != nullptr) {
    code = grpc::StatusCode::FAILED_PRECONDITION;
  }
  call.finishWithError(grpc::Status(code, e.what()));
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
      grpc::ByteBuffer buf = serializeToByteBuffer(response.proto);
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
    call.decrementOutstanding();  // balance the responsesExpected++ done before handle()
    return;
  }
  if (requestState->proto.response_format == solux::api::ResponseFormat::DOCS) {
    releaseArena(arena);
    // Doc-line framing is an HTTP/NDJSON concept; gRPC responses are already
    // framed DocList messages.
    call.finishWithError(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                      "response_format=docs applies to the HTTP NDJSON layer only"));
    return;
  }
  auto& engine = call.server.getSoluxNode().getSearchEngine();
  auto& req = *solux::arenaCreate<GRPCSearchRequest>(*arena, engine, requestState->proto, *arena);
  req.requestState = std::move(requestState);
  req.parent = &call;
  // Run the (synchronous) submit off this completion-queue thread (via
  // dispatch()): the cq thread must keep processing WRITE completions for
  // response flow control to advance, and a long query must not head-of-line
  // block the other calls served by this cq.  max_parallel=-1 opts out and runs
  // the query inline right here (a scheduling-overhead baseline that accepts
  // both costs).  The call outlives the request because its
  // outstanding-response count stays positive until the final reply, and
  // requestActive keeps pipelined requests on this stream ordered.
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
  bool success = iw->submitUpdate(&updateMessage);
  if (!success) throw IndexWriterClosedError("index writer is closed");
  updateMessage.blocker.wait();
  updateMessage.finishResponse();
  return serializeToByteBuffer(response);
}

//   rpc Update(UpdateRequest) returns (UpdateResponse)  [unary]
static void handleUpdate(GenericCallData& call, grpc::ByteBuffer& readBuf) {
  HppRequestState<UpdateReqProto> request;
  if (!parseRequest(readBuf, request, "Update")) {
    call.decrementOutstanding();
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
    call.decrementOutstanding();
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
      grpc::ByteBuffer buf = serializeToByteBuffer(*response);
      parent->respondRaw(std::move(buf), 1);       // may delete parent on another thread
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
        throw IndexWriterClosedError("index writer is closed");
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
    call.decrementOutstanding();
    return;
  }

  { const std::lock_guard<std::mutex> lock(call.mutex); call.requestActive = true; }
  try {
    call.server.getSoluxNode().getTaskArena().enqueue([request, &call] {
      try {
        call.server.getSoluxNode().createCollection(
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
    call.decrementOutstanding();
    return;
  }

  { const std::lock_guard<std::mutex> lock(call.mutex); call.requestActive = true; }
  try {
    call.server.getSoluxNode().getTaskArena().enqueue([request, &call] {
      try {
        call.server.getSoluxNode().deleteCollection(request->proto.name);
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
    call.decrementOutstanding();
    return;
  }
  if (!request.proto.schema.has_value()) {
    LOG_ERROR("SetSchema: missing schema");
    call.decrementOutstanding();
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
    call.decrementOutstanding();
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
    call.decrementOutstanding();
    return;
  }

  // Per-segment output is unbounded. Keep both collection sampling and
  // serialization off the completion-queue thread; responsesExpected keeps
  // the call alive and requestActive preserves per-call request ordering.
  { const std::lock_guard<std::mutex> lock(call.mutex); call.requestActive = true; }
  try {
    call.server.getSoluxNode().getTaskArena().enqueue([request, &call] {
      try {
        StatsRespProto response;
        std::pmr::monotonic_buffer_resource respArena;
        gatherStats(call.server.getSoluxNode(), request->proto, response, respArena);
        call.respondRaw(serializeToByteBuffer(response), 1);
      } catch (const std::exception& e) {
        finishWithException(call, e);
      }
    });
  } catch (const std::exception& e) {
    finishWithException(call, e);
  }
}

//   rpc SayHello(HelloRequest) returns (HelloReply)  [unary] - demo
static void handleSayHello(GenericCallData& call, grpc::ByteBuffer& readBuf) {
  HppRequestState<HelloReqProto> request;
  if (!parseRequest(readBuf, request, "SayHello")) {
    call.decrementOutstanding();
    return;
  }
  HelloRespProto response;
  // response.message is a non-owning string_view; keep the backing string alive until
  // after serialize (StrCat returns a temporary that would otherwise dangle).
  std::string message = absl::StrCat("Hello ", request.proto.name);
  response.message = message;
  call.respondRaw(serializeToByteBuffer(response), 1);
}

//   rpc SayHello2(HelloRequest) returns (HelloReply)  [unary] - demo
static void handleSayHello2(GenericCallData& call, grpc::ByteBuffer& readBuf) {
  HppRequestState<HelloReqProto> request;
  if (!parseRequest(readBuf, request, "SayHello2")) {
    call.decrementOutstanding();
    return;
  }
  HelloRespProto response;
  std::string message = absl::StrCat("Hello2 ", request.proto.name);
  response.message = message;
  call.respondRaw(serializeToByteBuffer(response), 1);
}

//   rpc SayHelloStreaming(stream HelloRequest) returns (stream HelloReply) - demo
// Each request produces response_count replies (>=1), optionally async, optionally
// sleeping between min/max us.  Response bytes are owned, so we copy the few fields we
// need and decouple from the request entirely.
static void handleSayHelloStreaming(GenericCallData& call, grpc::ByteBuffer& readBuf) {
  HppRequestState<HelloReqProto> request;
  if (!parseRequest(readBuf, request, "SayHelloStreaming")) {
    call.decrementOutstanding();
    return;
  }
  std::string name = std::string(request.proto.name);
  int count = std::max(1, request.proto.response_count);
  int minSleepUs = request.proto.min_sleep_us;
  int maxSleepUs = request.proto.max_sleep_us;
  bool async = request.proto.async;

  auto produce = [&call, name, count, minSleepUs, maxSleepUs]() {
    for (int i = 0; i < count; i++) {
      if (maxSleepUs > 0) {
        auto now = std::chrono::high_resolution_clock::now();
        solux::Rng rng(now.time_since_epoch().count());
        auto sleepUs = rng.rint(minSleepUs, maxSleepUs);
        std::this_thread::sleep_for(std::chrono::microseconds(sleepUs));
      }
      HelloRespProto reply;
      std::string message = absl::StrCat("Hello ", name);  // keep alive past serialize (non-owning view)
      reply.message = message;
      reply.response_number = i + 1;
      call.respondRaw(serializeToByteBuffer(reply), 0);  // intermediate; balance with decrementOutstanding below
    }
    call.decrementOutstanding(1);  // this request is now fully answered
  };

  if (async) {
    call.server.getSoluxNode().getTaskArena().enqueue(produce);
  } else {
    produce();
  }
}


// ---- method routing ------------------------------------------------------

static const MethodEntry* lookupMethod(const std::string& method) {
  static const std::unordered_map<std::string, MethodEntry> table = {
    {"/solux.Searcher/Search",          {handleSearch}},
    {"/solux.Indexer/Update",           {handleUpdate}},
    {"/solux.Indexer/UpdateStream",     {handleUpdateStream}},
    {"/solux.Admin/SetSchema",          {handleSetSchema}},
    {"/solux.Admin/GetSchema",          {handleGetSchema}},
    {"/solux.Admin/CreateCollection",   {handleCreateCollection}},
    {"/solux.Admin/DeleteCollection",   {handleDeleteCollection}},
    {"/solux.Admin/Stats",              {handleStats}},
    {"/solux.Greeter/SayHello",         {handleSayHello}},
    {"/solux.Greeter/SayHello2",        {handleSayHello2}},
    {"/solux.Greeter/SayHelloStreaming",{handleSayHelloStreaming}},
  };
  auto it = table.find(method);
  return it == table.end() ? nullptr : &it->second;
}


void GRPCServer::runThread(ThreadInfo& threadInfo) {
  // linux-only: give threads a nice name for debugging.
  std::string tname = "solux_grpc_" + std::to_string(threadInfo.threadno);
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
void solux::GRPCServer::shutdown() {
  LOG_INFO("Shutting down solux grpc server.");
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

} //  namespace solux
