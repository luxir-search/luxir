// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <exception>
#include <functional>
#include <optional>

#include "SearchRequest.h"
#include "luxir/server/LuxirNode.h"
#include "luxir/util/thread.h"

namespace luxir {

class LuxirNode;
class RootOp;
struct SearchConfig;

/// Each LuxirNode owns a SearchEngine, the main entry point for search requests.
//
class SearchEngine {
  LuxirNode& node;

  // Runs the request: reads ahead of parse, parse and plan, reads ahead of
  // execution, then execution. Reads ahead happen when the reader's files do
  // not all hand out free views (DeclaredReads). With mayDefer, a round that
  // does not complete inline resumes the request on an arena worker once it
  // ends, and no thread waits for it. ownTg is the fan-out scope run() made on
  // its stack, which a deferred request leaves behind.
  void submitBody(SearchRequest& req, bool mayDefer, FanoutScope* ownTg);
  void planBody(SearchRequest& req, bool mayDefer, FanoutScope* ownTg);
  void executeBody(SearchRequest& req, RootOp* root);
  // Records a failure on the request after draining its fan-out scope.
  void failBody(SearchRequest& req, std::exception& e, ErrorKind fallback);
  // Fails the request with `failure` after a round of reads, then finishBody().
  void abandon(SearchRequest& req, std::exception_ptr failure);
  // Releases segment state, fills the profile and warnings, and sends the final response.
  void finishBody(SearchRequest& req);
  // Goes on with the request after its rounds; with a failure, fails it.
  using Resume = std::function<void(std::exception_ptr failure)>;
  // Issues the request's declared rounds until it has nothing more to read
  // ahead at this stage. False when they all ended inline (or blocking,
  // without mayDefer): the caller goes on. True when the request was handed
  // over: `resume` runs once the rounds end (on an arena worker, or here with
  // a failure), and the caller must not touch the request again.
  bool readAhead(SearchRequest& req, bool mayDefer, FanoutScope* ownTg, Resume resume);
  // What fails a request after a round: a cancel or the deadline. Any other
  // failure is no failed search: the reads acquire on demand.
  static std::exception_ptr readFailure(const SearchRequest& req, const std::exception_ptr& error);
  static std::optional<FanoutPolicy> fanoutPolicy(int32_t maxParallel);
  // submit(), but with mayDefer the request may finish on an arena worker
  // after this returns (dispatch's lanes, which complete through replies).
  void run(SearchRequest& req, int32_t maxParallel, bool mayDefer);
  // Runs run() on an arena worker, isolating its waits.
  void enqueueSubmit(SearchRequest& req, int32_t maxParallel);
  RootOp* prepare(SearchRequest& req);

  /// get needed resources such as the index reader and schema
  void getResources(SearchRequest& req);

public:
  SearchEngine(LuxirNode& node): node(node) {
  }

  const SearchConfig& searchConfig() const;
  LuxirNode& getNode() const { return node; }
  Metrics& metrics() const;

  // Prepare through the ordinary parser/planner, without running calculators.
  // As a search in `tenant` would resolve it.
  std::vector<std::string> explain(const ReqProto& proto, std::string tenant);

  // Whether dispatch() may execute the query on the calling (transport)
  // thread.  An HTTP io shard may: it owns its connections, and the inline
  // lane saves a scheduler hop.  A gRPC completion-queue thread may not: gRPC
  // offers a connection's new calls to its preferred queue first, so a query
  // run there would serialize the concurrent calls multiplexed on that
  // connection.
  enum class Receiver { MAY_EXECUTE, OFFLOAD };

  // Transport entry point: route the request by max_parallel, then submit().
  // With MAY_EXECUTE, 0 starts inline and adaptively uses idle cores, -1
  // publishes every sibling, -2 stays serial inline, and 1 enqueues serial
  // work on the arena. OFFLOAD enqueues every value on the arena.
  void dispatch(SearchRequest& req, int32_t maxParallel, Receiver receiver);

  // Synchronous execution on the calling thread (dispatch() routes here).
  // 0 = adaptive, -1 = every sibling, 1 / -2 = serial.
  void submit(SearchRequest& req, int32_t maxParallel = 0);
  // bool converts to int silently and would flip meaning (false -> 0);
  // force old call sites to say what they mean.
  void submit(SearchRequest& req, bool) = delete;


  SearchEngine(const SearchEngine&) = delete;
  SearchEngine& operator=(const SearchEngine&) = delete;
};

} // namespace luxir
