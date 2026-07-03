#pragma once

#include <memory_resource>
#include <mutex>
#include <oneapi/tbb/task_group.h>

#include "solux/api/solux_types.hpp"
#include "solux/api/build.h"
#include "IndexReader.h"
#include "solux/schema/Schema.h"
#include "solux/util/proto.h"

namespace solux {

class SearchEngine;
class SearchResponse;

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

// The top-level request for the SearchEngine.
class SearchRequest {
  friend class SearchEngine;

public:
  SearchEngine& engine;
  const ReqProto& proto;            // borrowed non-owning view over the request bytes
  google::protobuf::Arena& arena;   // engine object allocator (NOT proto storage)
  std::shared_ptr<IndexReader> reader;
  // Pins the collection schema snapshot for the request.  Schema instances are
  // immutable after publication, so query objects may keep FieldType references
  // derived from this schema for the request lifetime.
  std::shared_ptr<Schema> schema;
  MemPool requestPool;
  oneapi::tbb::task_group* tg = nullptr; // optional top-level task group for this request.
  SearchResponse* lastResponse = nullptr;
  std::mutex mutex;
  // Declared degradations accumulated during parse/build (see ParseContext);
  // copied onto the FINAL response's SearchResponse.warnings.  Message views
  // point into requestPool, which outlives response serialization.
  std::vector<api::Warning> warnings;

  SearchRequest(SearchEngine& engine, const ReqProto& proto, google::protobuf::Arena& arena)
    : engine(engine), proto(proto), arena(arena) {
  }

  virtual ~SearchRequest() = default;

  /// Call this to send a response back to the client (or cause it to be buffered).  This can be called multiple times for a single request.
  /// replyComplete will be called when the response has actually been written and is no longer needed.
  /// Returns the number of buffered responses (0 if the response was written immediately).
  virtual int reply(SearchResponse& response) = 0;

  /// This is called when the reply has completed (e.g. it has been written to a socket and not just buffered)
  /// This may be called from a gRPC server thread (and with a mutex locked), so it should be fast.
  virtual void replyCallback(SearchResponse& response);

  /// This will be called last after all responses have been sent back to the client.
  /// This is the place to do final cleanup (such as deleting or resetting the Arena)
  /// It may delete *this*, so nothing else should be accessed after this is called.
  virtual void done() {
    releaseArena(&arena);
  };
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

}
