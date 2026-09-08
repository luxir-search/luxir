// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "SearchRequest.h"
#include "luxir/server/LuxirNode.h"

namespace luxir {

class LuxirNode;
struct SearchConfig;

/// The SearchEngine is a singleton owned by the LuxirNode object and is responsible for
/// executing search requests.  It is the main entry point for the search subsystem.
//
class SearchEngine {
  LuxirNode& node;

  void submitBody(SearchRequest& req);

  /// get needed resources such as the index reader and schema
  void getResources(SearchRequest& req);

public:
  SearchEngine(LuxirNode& node): node(node) {
  }

  const SearchConfig& searchConfig() const;

  // Transport entry point: route the request by max_parallel, then submit().
  // 0 (the default) executes inline on the calling thread (no cross-thread
  // hop at all - the caller eats the query's latency); every non-zero value
  // enqueues on the shared TBB task arena, where 1 runs without a task_group
  // (serial) and -1 parallelizes.
  void dispatch(SearchRequest& req, int32_t maxParallel);

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
