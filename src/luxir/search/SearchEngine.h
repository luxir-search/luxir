// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "SearchAdmission.h"
#include "SearchRequest.h"
#include "luxir/server/LuxirNode.h"

namespace luxir {

class LuxirNode;
class RootOp;
struct SearchConfig;

/// Each LuxirNode owns a SearchEngine, the main entry point for search requests.
//
class SearchEngine {
public:
  // Whether dispatch() may execute the query on the calling (transport)
  // thread.  An HTTP io shard may: it owns its connections, and the inline
  // lane saves a scheduler hop.  A gRPC completion-queue thread may not: gRPC
  // offers a connection's new calls to its preferred queue first, so a query
  // run there would serialize the concurrent calls multiplexed on that
  // connection.
  enum class Receiver { MAY_EXECUTE, OFFLOAD };

private:
  LuxirNode& node;
  // Node-wide admission of dispatched searches (search.max_running and
  // friends): see dispatch().
  SearchAdmission admission;

  void submitBody(SearchRequest& req);
  // Takes an admission slot, then runs the request inline (MAY_EXECUTE, a
  // free slot, nothing queued) or on the arena; queues it for a slot, or
  // refuses it.  The caller must not touch req afterwards.
  void admitThenRun(SearchRequest& req, int32_t maxParallel, Receiver receiver, bool remote);
  // Runs submit() on an arena worker, isolating its waits.  Never throws: a
  // failed enqueue completes the request on this thread.
  void enqueueRun(SearchRequest& req, int32_t maxParallel) noexcept;
  // Completes a request that never ran, on this thread.  The transports'
  // reply() is safe from any thread, so a refusal never waits behind the
  // saturated arena it was refused from.
  static void failHere(SearchRequest& req, const ErrorInfo& error, MetricOutcome outcome) noexcept;
  // Whether the request's collection may fetch bytes over a network.
  bool readsRemotely(const SearchRequest& req);
  static bool readsRemotely(const Collection& collection) noexcept;
  RootOp* prepare(SearchRequest& req);

  /// get needed resources such as the index reader and schema
  void getResources(SearchRequest& req);

public:
  explicit SearchEngine(LuxirNode& node);

  const SearchConfig& searchConfig() const;
  LuxirNode& getNode() const { return node; }
  Metrics& metrics() const;
  SearchAdmission& getAdmission() { return admission; }
  // The admission limits a node with this config and worker count resolves.
  static SearchAdmission::Limits admissionLimits(const SearchConfig& config, int workers);

  // Wraps a paused emitter's resume so it re-admits through the admission
  // priority lane: it runs where the transport placed it when a slot is free,
  // otherwise on an arena worker once one is.  Never refused.
  std::function<void()> admitResume(std::function<void()> resume);

  // Prepare through the ordinary parser/planner, without running calculators.
  // As a search in `tenant` would resolve it.
  std::vector<std::string> explain(const ReqProto& proto, std::string tenant);


  // Transport entry point: admit the request, route it by max_parallel, then
  // submit().  With MAY_EXECUTE, 0 starts inline and adaptively uses idle
  // cores, -1 publishes every sibling, -2 stays serial inline, and 1 enqueues
  // serial work on the arena.  OFFLOAD enqueues every value on the arena.
  //
  // Admission comes first, in every lane (SearchAdmission): a search runs
  // inline only with a slot that is free right now while nothing queues;
  // otherwise it queues and runs on the arena once started (offloaded), so a
  // busy node never parks a shard on a long search.  A full queue or a wait
  // past search.max_queue_ms refuses it with RESOURCE_EXHAUSTED.  A min_commit
  // request is admitted after its commit wait, never holding a slot through
  // it.  A search of a collection on remote storage leaves the receiving
  // thread in every lane (its reads can wait on the network, stalling every
  // connection of an HTTP shard) and counts against search.max_running_remote.
  // max_parallel=-1 fans out adaptively, like 0, while the node is crowded
  // (searches queue, or others run): it would otherwise take every worker.
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
