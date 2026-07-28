#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <memory_resource>
#include <mutex>
#include <oneapi/tbb/task_group.h>

#include "solux/api/solux_types.hpp"
#include "solux/api/build.h"
#include "IndexReader.h"
#include "FilterCache.h"
#include "solux/schema/Schema.h"
#include "solux/util/Clock.h"
#include "solux/util/DateTime.h"
#include "solux/util/proto.h"

namespace solux {

class SearchEngine;
class SearchResponse;

// Cumulative count of emitter pauses caused by reply() flow control.
// Process-wide; read by tests and (eventually) server stats.
inline std::atomic<int64_t> streamPauseCount{0};

// --- Request/response proto model (concrete solux::api, non-owning) ---
// The request proto is parsed once (by the gRPC handler) into a NON-OWNING concrete
// struct that views the kept-alive request bytes (+ a per-call arena for nested messages).
// The handler owns those bytes; SearchRequest only borrows `proto`.
//
// The response proto is ALSO a non-owning concrete struct: the ops build it via
// build-by-backing (see solux/api/build.h) into the SearchResponse's own monotonic arena
// (`mr`), then it is serialized (solux::api::encode / write_json) and dropped. Every
// solux::api type is trivially destructible, so the response arena is released wholesale
// with zero per-object destructors. google::protobuf::Arena remains the per-request
// OBJECT allocator (ops, wrappers) - unrelated to proto message storage.
using ReqProto = solux::api::SearchRequest;
using RespProto = solux::api::SearchResponse;

// Short alias for the response build-by-backing helpers (solux/api/build.h).
namespace build = solux::api::build;

// Runtime backing for request profiling. Each calculator owns one run with a
// fixed slot per segment; the response wire objects are built only after all
// segment work has drained.
struct ExecutionProfilePieceState {
  solux::api::ExecutionProfilePiece wire;
  // Human-readable execution notes, one clause per entry; profiling is off
  // the hot path by definition, so ops just push composed strings here and
  // fillExecutionProfile builds the wire span over them.
  std::vector<std::string> details;
  bool complete = false;
};

struct ExecutionProfileRun {
  std::vector<ExecutionProfilePieceState> pieces;

  explicit ExecutionProfileRun(std::size_t numPieces) : pieces(numPieces) {}
};

struct ExecutionProfileOpState {
  std::string_view name;
  std::mutex mutex;
  std::vector<std::unique_ptr<ExecutionProfileRun>> runs;

  explicit ExecutionProfileOpState(std::string_view name) : name(name) {}
};

// The top-level request for the SearchEngine.
class SearchRequest {
  friend class SearchEngine;

public:
  SearchEngine& engine;
  const ReqProto& proto;            // borrowed non-owning view over the request bytes
  google::protobuf::Arena& arena;   // engine object allocator (NOT proto storage)
  std::shared_ptr<IndexReader> reader;
  // One Use per distinct filter key for the whole request, even with the shared
  // cache disabled: duplicate filters in separate operation trees share the
  // same borrowed or owned raw value and request-composed sets (admission also
  // counts once when a cache backend is active). shared_ptr because a standalone
  // Query::Context (tests, non-request embedders) creates and owns its own
  // registry, while request-backed Contexts all reference this one.
  std::shared_ptr<FilterCache::UseRegistry> filterUses;
  // Pins the collection schema snapshot for the request.  Schema instances are
  // immutable after publication, so query objects may keep FieldType references
  // derived from this schema for the request lifetime.
  std::shared_ptr<Schema> schema;
  // One clock snapshot for every query tree/op in this request. Parsers are
  // short-lived and numerous, so NOW belongs here rather than in a parser.
  const int64_t dateMathNowEpochMillis;
  // Resolved eagerly from proto.time_zone. Invalid IANA/fixed specifications
  // retain an error for submitBody to surface as a whole-request failure.
  std::optional<TimeZone> timeZone;
  std::string timeZoneError;
  MemPool requestPool;
  oneapi::tbb::task_group* tg = nullptr; // optional top-level task group for this request.
  // Resolved parallelism for this submission (see SearchEngine::submit).
  int32_t maxParallel = 0;
  SearchResponse* lastResponse = nullptr;
  std::mutex mutex;
  // Declared degradations accumulated during parse/build (see ParseContext);
  // copied onto the FINAL response's SearchResponse.warnings.  Message views
  // point into requestPool, which outlives response serialization.
  std::vector<api::Warning> warnings;
  // Empty, with no backing allocation, unless an instrumented op observes
  // proto.profile=true during parsing.
  std::vector<std::unique_ptr<ExecutionProfileOpState>> executionProfileOps;

  // Transport default for DocList field placement when an op leaves
  // document_format at DEFAULT: gRPC serves COLUMNS; the HTTP/JSON layer sets
  // ROWS (doc-oriented JSON consumers; row rendering becomes a passthrough).
  solux::api::DocFormat docFormatDefault = solux::api::DocFormat::COLUMNS;

  // TEST-ONLY: wrap each top-docs root query in ForcePrepareQuery so tests
  // can assert prepared execution matches normal execution end-to-end.  This
  // is the engine seam that replaced the force_prepare wire arm (debug
  // machinery does not belong on the public API); nothing wire-facing sets it.
  bool testForcePrepare = false;

  SearchRequest(SearchEngine& engine, const ReqProto& proto, google::protobuf::Arena& arena)
    : engine(engine), proto(proto), arena(arena),
      dateMathNowEpochMillis(currentEpochMillis()),
      timeZone(resolveTimeZone(proto.time_zone)) {
    if (!timeZone) timeZoneError = timeZoneResolutionError(proto.time_zone);
  }

  virtual ~SearchRequest() = default;

  ExecutionProfileOpState* addExecutionProfileOp(std::string_view name) {
    if (!proto.profile) return nullptr;
    auto state = std::make_unique<ExecutionProfileOpState>(name);
    auto* result = state.get();
    executionProfileOps.push_back(std::move(state));
    return result;
  }

  ExecutionProfileRun* addExecutionProfileRun(ExecutionProfileOpState* opState) {
    if (opState == nullptr) return nullptr;
    auto run = std::make_unique<ExecutionProfileRun>(reader->segments().size());
    auto* result = run.get();
    std::lock_guard<std::mutex> lock(opState->mutex);
    opState->runs.push_back(std::move(run));
    return result;
  }

  void fillExecutionProfile(SearchResponse& response);

  /// Flow-control advice returned by reply().  The response is always accepted
  /// (or dropped, for CANCEL); the status only tells a streaming producer what
  /// to do next.
  enum class ReplyStatus {
    OK,      // keep producing
    PAUSE,   // connection is over its buffer high-water mark: park via
             // resumeWhenDrained() before producing the next response
    CANCEL   // connection is gone: stop producing; further replies are dropped
  };

  /// Call this to send a response back to the client (or cause it to be buffered).  This can be called multiple times for a single request.
  /// replyComplete will be called when the response has actually been written and is no longer needed.
  virtual ReplyStatus reply(SearchResponse& response) = 0;

  /// Called by a streaming producer after reply() returned PAUSE.  The
  /// transport must invoke `resume` exactly once, on a task-arena thread, when
  /// the connection drains below its low-water mark (or immediately on error -
  /// the producer's next reply() then observes CANCEL).  The default is for
  /// transports without flow control: resume immediately.
  virtual void resumeWhenDrained(std::function<void()> resume) { resume(); }

  /// This is called when the reply has completed (e.g. it has been written to a socket and not just buffered)
  /// This may be called from a gRPC server thread (and with a mutex locked), so it should be fast.
  virtual void replyCallback(SearchResponse& response);

  /// This will be called last after all responses have been sent back to the client.
  /// This is the place to do final cleanup (such as deleting or resetting the Arena)
  /// It may delete *this*, so nothing else should be accessed after this is called.
  virtual void done() {
    rootCalc.reset();
    releaseArena(&arena);
  };

  // --- Streaming-emit completion protocol -----------------------------------
  //
  // A paused emitter outlives submitBody(): its op task returns after parking,
  // the task group drains, and submitBody reaches its final-response send while
  // batches remain unproduced.  The final response therefore goes out when BOTH
  // the request body has finished (bodyDone) AND every registered emit stream
  // has ended - whichever happens last sends it.

  // Keeps the calculator tree (collector output, getTarget chain) alive for
  // paused emitters; submitBody moves the root calculator here and done()
  // releases it before the arena.  Type-erased so this header does not depend
  // on SearchOp.h; the shared_ptr's deleter runs ~Calculator (virtual).
  std::shared_ptr<void> rootCalc;

  /// A streaming emitter is starting; the final response is held until every
  /// started stream has ended.
  void streamStarted() {
    std::lock_guard<std::mutex> lock(mutex);
    activeStreams++;
  }

  /// The emitter produced its final batch (or was cancelled).  May send the
  /// final response, which may delete *this* - callers must not touch the
  /// request afterwards.
  void streamEnded() { maybeSendFinal(true); }

  /// Called by submitBody in place of directly replying with lastResponse.
  /// May send the final response, which may delete *this* - callers must not
  /// touch the request afterwards.
  void bodyDone() { maybeSendFinal(false); }

private:
  // guarded by mutex:
  int activeStreams = 0;
  bool finalReady = false;
  bool finalSent = false;

  void maybeSendFinal(bool endingStream) {
    bool send;
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (endingStream) {
        assert(activeStreams > 0);
        activeStreams--;
      } else {
        finalReady = true;
      }
      send = finalReady && activeStreams == 0 && !finalSent;
      if (send) finalSent = true;
    }
    if (send) {
      reply(*lastResponse);  // may delete *this*; nothing after this call
    }
  }
};

// A response object that can be used to send back results to the client.
// One or more responses can be sent back for a single request.
//
// `proto` is a NON-OWNING concrete SearchResponse: ops assemble into it via the getTarget()
// bubble-to-root mechanism (see SearchOp.h Calculator), under req.mutex, with all message
// storage backed by this object's `mr` (build-by-backing, see solux/api/build.h).  For
// incremental emission an op makes a FRESH SearchResponse, getTarget()s into it, fills its
// slot, the handler encode's it and respondRaw(more=true), then it is dropped; the
// accumulating/final response is untouched and goes out with more=false.
class SearchResponse {
public:
  SearchRequest& req;
  google::protobuf::Arena& arena; // allocator for this wrapper object AND response data
  // pmr view over `arena` for this response's NON-OWNING message data (Vals, columns,
  // span backing). Thread-safe (parallel column loaders allocate concurrently); released
  // wholesale with the arena. All solux::api types are trivially destructible.
  ArenaResource mr;
  RespProto proto;                // non-owning concrete response, backed by `mr`
  bool last = true; // if true, this is the last response for the request.

  SearchResponse(SearchRequest& req, google::protobuf::Arena& arena, bool last)
    : req(req), arena(arena), mr(&arena), last(last) {
    // echo the request id: non-owning view over the (kept-alive) request bytes, no copy.
    proto.request_id = req.proto.request_id;
  }

  // Arena allocate a Response wrapper object.
  static SearchResponse* create(SearchRequest& req, bool last = true) {
    // If this is the last message, just use the same arena as the request since they will
    // have the same lifetimes.
    auto* arena = last ? &req.arena : createArena();
    return solux::arenaCreate<SearchResponse>(*arena, req, *arena, last);
  }

  // named columns, part of DocList or part of a FacetResult.
  using ColumnsType = decltype(std::declval<solux::api::DocList>().columns);
};


inline void SearchRequest::replyCallback(SearchResponse& response) {
  bool last = response.last;
  // if this response used a different Arena, release it.
  if (&response.arena != &arena) {
    releaseArena(&response.arena);
  }
  if (last) {
    done();
  }
};

inline void SearchRequest::fillExecutionProfile(SearchResponse& response) {
  if (!proto.profile) return;

  auto& profile = response.proto.profile.emplace();
  auto* outOps = build::allocArray(profile.ops, executionProfileOps.size(), response.mr);
  for (std::size_t opIndex = 0; opIndex < executionProfileOps.size(); opIndex++) {
    auto& state = *executionProfileOps[opIndex];
    auto& outOp = outOps[opIndex];
    outOp.name = state.name;

    std::lock_guard<std::mutex> lock(state.mutex);
    std::size_t numPieces = 0;
    for (const auto& run : state.runs) {
      for (const auto& piece : run->pieces) numPieces += piece.complete;
    }
    auto* outPieces = build::allocArray(outOp.pieces, numPieces, response.mr);
    std::size_t pieceIndex = 0;
    for (const auto& run : state.runs) {
      for (const auto& piece : run->pieces) {
        if (!piece.complete) continue;
        outPieces[pieceIndex] = piece.wire;
        if (!piece.details.empty()) {
          // Views into request-owned strings: the piece state outlives
          // response serialization (same lifetime rule as warnings).
          auto* views = build::allocArray(outPieces[pieceIndex].details,
                                          piece.details.size(), response.mr);
          for (std::size_t d = 0; d < piece.details.size(); d++) {
            views[d] = piece.details[d];
          }
        }
        pieceIndex++;
      }
    }
  }
}

}
