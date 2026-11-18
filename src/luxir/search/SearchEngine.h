// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "SearchRequest.h"
#include "luxir/util/thread.h"
#include "luxir/store/FetchCountdown.h"
#include "luxir/server/LuxirNode.h"

namespace luxir {

class LuxirNode;
class RootOp;
struct SearchConfig;

/// Each LuxirNode owns a SearchEngine, the main entry point for search requests.
//
class SearchEngine {
  LuxirNode& node;

  // Prepares the request, then executes it. With mayDefer, the reads that
  // materializing its segments needs are fetched first (fetchAhead); when
  // they do not complete inline, execution resumes on an arena worker once
  // they finish, and no thread waits for them. ownTg is the fan-out scope
  // run() made on its stack, which a deferred request leaves behind.
  void submitBody(SearchRequest& req, bool mayDefer, FanoutScope* ownTg);
  // Fetches `reads` ahead of the work that needs them. False when they
  // completed inline (local files): the caller goes on. True when the request
  // was handed over: `resume` runs on an arena worker once they finish, under
  // the fan-out scope run() would have made, and the caller must not touch
  // the request again.
  bool fetchAhead(SearchRequest& req, ReadSet reads, FanoutScope* ownTg, std::function<void()> resume);
  // Materializes the segments and runs the calculators, then finishBody().
  void execute(SearchRequest& req, RootOp* root, const SegmentMask& segments, MetricScope& execution);
  // Records a failure on the request after draining its fan-out scope.
  void failBody(SearchRequest& req, std::exception& e, ErrorKind fallback);
  // Releases segment state, fills the profile and warnings, and sends the final response.
  void finishBody(SearchRequest& req, RootOp* root, MetricScope& execution);
  // submit(), but with mayDefer the request may finish on an arena worker
  // after this returns (dispatch's lanes, which complete through replies).
  void run(SearchRequest& req, int32_t maxParallel, bool mayDefer);
  // The fan-out a request with this max_parallel runs under, if any.
  static std::optional<FanoutPolicy> fanoutPolicy(int32_t maxParallel);
  // Runs submit() on an arena worker, isolating its waits.
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

  // Synchronous execution on the calling thread: the request has finished
  // when this returns. dispatch() routes through the same body, except that
  // there a request whose segments must be fetched first resumes on a worker.
  // 0 = adaptive, -1 = every sibling, 1 / -2 = serial.
  void submit(SearchRequest& req, int32_t maxParallel = 0);
  // bool converts to int silently and would flip meaning (false -> 0);
  // force old call sites to say what they mean.
  void submit(SearchRequest& req, bool) = delete;


  SearchEngine(const SearchEngine&) = delete;
  SearchEngine& operator=(const SearchEngine&) = delete;
};

} // namespace luxir
