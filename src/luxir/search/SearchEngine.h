// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <deque>
#include <mutex>

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
  struct Queued {
    SearchRequest* req = nullptr;
    int32_t maxParallel = 0;
  };

  LuxirNode& node;
  // Admission (search.max_concurrent / search.max_queued), under
  // admissionMutex: searches holding a slot, and those waiting for one.
  std::mutex admissionMutex;
  int64_t running = 0;
  std::deque<Queued> queued;

  void submitBody(SearchRequest& req);
  RootOp* prepare(SearchRequest& req);

  // Resolve the request's deadline (timeout_ms, search.default_timeout_ms,
  // transportDeadline) and cancellation token, once.
  void armBudget(SearchRequest& req);
  // After admission: the min_commit wait, then the max_parallel lane (or, on
  // an arena worker already, run in place).
  void begin(SearchRequest& req, int32_t maxParallel, bool onArena);
  // submit(), then give up the request's slot if it holds one.
  void runAdmitted(SearchRequest& req, int32_t maxParallel);
  // Hand a finished search's slot to the oldest queued one, or free it.
  void release();
  void startQueued(SearchRequest& req, int32_t maxParallel);
  // Flag a timeout on the final response, or fail a cancelled request.
  void reportBudget(SearchRequest& req);

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

  // Transport entry point: arm the deadline, admit the request (run, queue
  // FIFO, or reject with RESOURCE_EXHAUSTED once search.max_concurrent run
  // and search.max_queued wait), then route it by max_parallel to submit().
  // A queued request starts on the task arena when a slot frees; one whose
  // deadline passed meanwhile answers timed out with empty results.
  // 0 (the default) executes serially inline on the calling thread (no
  // cross-thread hop at all - the caller eats the query's latency); -1 also
  // starts on the calling thread, entering the shared TBB task arena so arena
  // workers parallelize it; 1 enqueues on the arena and runs serially there,
  // off the calling thread.
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
