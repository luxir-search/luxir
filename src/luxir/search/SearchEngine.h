// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "SearchRequest.h"
#include "luxir/server/LuxirNode.h"

namespace luxir {

class LuxirNode;
class RootOp;
struct SearchConfig;

/// Each LuxirNode owns a SearchEngine, the main entry point for search requests.
//
class SearchEngine {
  LuxirNode& node;

  void submitBody(SearchRequest& req);
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
