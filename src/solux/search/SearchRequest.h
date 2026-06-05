#pragma once

#include <mutex>
#include <oneapi/tbb/task_group.h>

#include "protos/solux_types.pb.h"
#include "IndexReader.h"
#include "solux/schema/Schema.h"
#include "solux/util/proto.h"

namespace solux {

class SearchEngine;
class SearchResponse;

// The top-level request for the SearchEngine.
class SearchRequest {
  friend class SearchEngine;

public:
  SearchEngine& engine;
  solux::proto::SearchRequest& proto;
  google::protobuf::Arena& arena;
  std::shared_ptr<IndexReader> reader;
  // Pins the collection schema snapshot for the request.  Schema instances are
  // immutable after publication, so query objects may keep FieldType references
  // derived from this schema for the request lifetime.
  std::shared_ptr<Schema> schema;
  MemPool requestPool;
  oneapi::tbb::task_group* tg = nullptr; // optional top-level task group for this request.
  SearchResponse* lastResponse = nullptr;
  std::mutex mutex;

  SearchRequest(SearchEngine& engine, solux::proto::SearchRequest& proto): engine(engine), proto(proto),
                                                                           arena(*proto.GetArena()) {
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
class SearchResponse {
public:
  SearchRequest& req;
  google::protobuf::Arena& arena;
  solux::proto::SearchResponse& proto;
  bool last = true; // if true, this is the last response for the request.
  // TODO: we could add a callback here to facilitate chaining of responses (i.e. for streaming results, etc)

  SearchResponse(SearchRequest& req, google::protobuf::Arena& arena, bool last)
    : req(req), arena(arena), proto(*google::protobuf::Arena::Create<solux::proto::SearchResponse>(&arena)),
      last(last) {
    // set the response id to match the request id.
    proto.set_request_id(req.proto.request_id());
  }

  // Arena allocate a Response object.
  static SearchResponse* create(SearchRequest& req, bool last = true) {
    // If this is the last message, just use the same arena as the response since they will have
    // the same lifetimes.
    auto* arena = last ? &req.arena : createArena();
    return google::protobuf::Arena::Create<SearchResponse>(arena, req, *arena, last);
  }

  // named columns, part of DocList or part of a FacetResult.
  using ColumnsType = google::protobuf::Map<std::string, solux::proto::Column>;
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
