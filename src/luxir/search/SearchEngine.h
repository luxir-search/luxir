// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "SearchRequest.h"
#include "luxir/server/LuxirNode.h"

namespace luxir {

class LuxirNode;
class RootOp;
struct SearchConfig;

/// The SearchEngine is a singleton owned by the LuxirNode object and is responsible for
/// executing search requests.  It is the main entry point for the search subsystem.
//
class SearchEngine {
  LuxirNode& node;

  void submitBody(SearchRequest& req);
  // Runs submit() on an arena worker; -1's waits stay within the request.
  void enqueueSubmit(SearchRequest& req, int32_t maxParallel);
  RootOp* prepare(SearchRequest& req);

  /// get needed resources such as the index reader and schema
  void getResources(SearchRequest& req);

public:
  SearchEngine(LuxirNode& node): node(node) {
  }

  const SearchConfig& searchConfig() const;
  Metrics& metrics() const;

  // Prepare through the ordinary parser/planner, without running calculators.
  // As a search in `tenant` would resolve it.
  std::vector<std::string> explain(const ReqProto& proto, std::string tenant);

  // Collection-level pruning: whether the collection's manifest summaries
  // leave any chance that a query-bearing op of `proto` matches. Pins the
  // snapshot it evaluates (its commit token is returned) and opens no
  // segment. With `includeSegments`, also lists the segments no op can match.
  struct CanMatchResult {
    struct Op {
      std::string name;
      bool mayMatch = true;
      std::string reason;
    };
    bool mayMatch = true;
    bool summaryComplete = false;
    std::string commit;
    size_t segments = 0;
    std::vector<Op> ops;
    std::vector<uint64_t> prunedSegments;
  };
  CanMatchResult canMatch(const ReqProto& proto, std::string tenant, bool includeSegments);

  // Whether dispatch() may execute the query on the calling (transport)
  // thread.  An HTTP io shard may: it owns its connections, and the inline
  // lane saves a scheduler hop.  A gRPC completion-queue thread may not: gRPC
  // offers a connection's new calls to its preferred queue first, so a query
  // run there would serialize the concurrent calls multiplexed on that
  // connection.
  enum class Receiver { MAY_EXECUTE, OFFLOAD };

  // Transport entry point: route the request by max_parallel, then submit().
  // With Receiver::MAY_EXECUTE, 0 (the default) executes serially inline on
  // the calling thread (no cross-thread hop at all - the caller eats the
  // query's latency); -1 also starts on the calling thread, entering the
  // shared TBB task arena so arena workers parallelize it; 1 enqueues on the
  // arena and runs serially there, off the calling thread.  With
  // Receiver::OFFLOAD every request is enqueued on the arena: 0 and 1 run
  // serially on a worker, -1 starts on a worker and fans out from there.
  void dispatch(SearchRequest& req, int32_t maxParallel, Receiver receiver);

  // Synchronous execution on the calling thread (dispatch() routes here).
  // maxParallel: 0 / 1 = single-threaded, -1 = unlimited intra-request
  // parallelism (task_group), >1 reserved (rejected as a request error).
  void submit(SearchRequest& req, int32_t maxParallel = 0);
  // bool converts to int silently and would flip meaning (false -> 0);
  // force old call sites to say what they mean.
  void submit(SearchRequest& req, bool) = delete;


  SearchEngine(const SearchEngine&) = delete;
  SearchEngine& operator=(const SearchEngine&) = delete;
};

} // namespace luxir
