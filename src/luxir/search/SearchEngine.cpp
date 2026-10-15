// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <limits>
#include "luxir/api/padded_input.h"

#include "SearchEngine.h"
#include "luxir/server/CommitWaits.h"
#include "ProtobufSearchParser.h"
#include "luxir/search/ops/RootOp.h"

namespace luxir {

const SearchConfig& SearchEngine::searchConfig() const {
  return node.getConfig().search;
}

RootOp* SearchEngine::prepare(SearchRequest& req) {
  if (!req.timeZone) throw RequestError(req.timeZoneError);
  if (req.maxParallel > 1 || req.maxParallel < -1) {
    throw RequestError(
      "max_parallel must be 0 (serial on the receiving thread), 1 (serial "
      "on the shared executor), or -1 (unlimited parallelism); values "
      "above 1 are reserved for a parallelism budget and not implemented");
  }
  getResources(req);
  req.lastResponse = SearchResponse::create(req, true);

  ProtobufSearchParser parser(req);
  return parser.parse();
}

std::vector<std::string> SearchEngine::explain(const ReqProto& proto, std::string tenant) {
  class ExplainRequest final : public SearchRequest {
  public:
    using SearchRequest::SearchRequest;
    ReplyStatus reply(SearchResponse&) override { return ReplyStatus::OK; }
  };
  // String parsers splice their expansions into the request. Give preparation
  // a disposable deep copy so its pool can die without changing caller views.
  std::pmr::monotonic_buffer_resource source;
  std::vector<std::byte> encoded;
  ReqProto copy;
  if (!api::encode(proto, encoded)
      || !api::decode(copy, api::copyToPaddedInput(encoded, source), source)) {
    throw ApiError(ErrorKind::INTERNAL, "internal", "failed to copy explain request");
  }
  std::vector<std::string> notes;
  google::protobuf::Arena arena;
  auto* req = arenaCreate<ExplainRequest>(arena, *this, copy, arena);
  req->maxParallel = proto.max_parallel;
  req->tenant = std::move(tenant);
  req->resolvedFields = &notes;
  prepare(*req);
  return notes;
}

// Declared degradations ride on the final response (streaming responses with
// more=true do not carry them).
static void attachWarnings(SearchRequest& req) {
  if (req.warnings.empty()) return;
  auto* warnings = luxir::api::build::allocArray(req.lastResponse->proto.warnings,
                                                 req.warnings.size(), req.lastResponse->mr);
  for (size_t i = 0; i < req.warnings.size(); i++) {
    warnings[i] = req.warnings[i];
  }
}

void SearchEngine::submitBody(SearchRequest& req) {
  // Window loops reach the budget through the thread; task_group_run carries
  // it into the tasks this body spawns.
  ExecutionBudget::Scope budgetScope(&req.budget);
  // Until the calculators start, an unclassified failure is the request's
  // fault: parsers and planners reject authored input with bare exceptions.
  // Once execution is under way it is the engine's.  Throw sites that know
  // better say so with an ApiError, which classifies itself in either phase.
  ErrorKind fallback = ErrorKind::INVALID_REQUEST;
  RootOp* root = nullptr;
  try {
    root = prepare(req);
    root->init();
    std::unique_ptr<RootOp::Calc> calc(root->createCalculator(nullptr, -1));
    // The request owns the calculator tree: a flow-controlled emitter can
    // outlive this function, and its callbacks reach into collector output and
    // the getTarget chain.  Released in done().
    auto* rootCalc = calc.get();
    req.rootCalc = std::move(calc);
    fallback = ErrorKind::INTERNAL;
    rootCalc->start(req.tg);

    if (req.tg) {
      LOG_TRACE("SearchRequest: waiting for task group to finish for req={}", (void*)&req);
      req.tg->wait();
      LOG_TRACE("SearchRequest: DONE! will call reply next. req={}", (void*)&req);
    }
  } catch (std::exception& e) {
    // Drain any in-flight tasks so they aren't writing into the response while
    // we tear down.  wait() rethrows the first exception, so swallow it here.
    if (req.tg) {
      // TODO: notify other tasks about the error?
      try { req.tg->wait(); } catch (...) {}
    }
    ErrorInfo info = classifyException(e, fallback);
    if (info.kind == ErrorKind::INTERNAL) {
      LOG_ERROR("Search request failed: {}", info.message);
    } else {
      LOG_WARN("Search request rejected ({}): {}", info.code, info.message);
    }
    req.setError(info);
  }

  if (root != nullptr) {
    for (auto& segment : req.reader->segments()) root->releaseSegmentState(segment);
  }

  // Profile slots are written only by segment tasks. Materialize the shared
  // wire view after those tasks have drained (including the error path above).
  req.fillExecutionProfile(*req.lastResponse);
  reportBudget(req);

  attachWarnings(req);

  // Send back the final response - or, if a flow-controlled emitter is still
  // paused with batches to produce, hand off: the last stream to end sends it.
  // Do not access req after this point as it may be asynchronously deleted.
  req.bodyDone();
}

void SearchEngine::reportBudget(SearchRequest& req) {
  // Cancellation means nobody is left to read partial results: fail.  A
  // timeout keeps what was collected and says so.
  if (req.budget.cancelled.load(std::memory_order_relaxed)) {
    req.setError({ErrorKind::CANCELLED, "cancelled", "the search was cancelled"});
  } else if (req.budget.timedOut.load(std::memory_order_relaxed)) {
    req.markTimedOut();
  }
}

void SearchEngine::armBudget(SearchRequest& req) {
  if (req.budgetArmed) return;
  req.budgetArmed = true;
  auto now = ExecutionBudget::Clock::now();
  auto deadline = req.transportDeadline;
  for (uint64_t ms : {req.proto.timeout_ms, (uint64_t)std::max<int64_t>(0, searchConfig().default_timeout_ms)}) {
    // Past ~11 days a timeout means none; the cap keeps the sum from overflowing.
    if (ms == 0 || ms >= 1'000'000'000) continue;
    deadline = std::min(deadline, now + std::chrono::milliseconds(ms));
  }
  req.budget.deadline = deadline;
  req.budget.cancellation = req.waitCancellation.get_token();
}

void SearchEngine::dispatch(SearchRequest& req, int32_t maxParallel) {
  // The deadline runs from receipt: a min_commit wait and time in the
  // admission queue count against it.
  armBudget(req);
  int64_t maxConcurrent = searchConfig().max_concurrent;
  if (maxConcurrent > 0) {
    int64_t maxQueued = searchConfig().max_queued;
    {
      std::lock_guard<std::mutex> lock(admissionMutex);
      if (running < maxConcurrent) {
        running++;
        req.holdsSearchSlot = true;
      } else if ((int64_t)queued.size() < maxQueued) {
        // A finishing search hands its slot over (release()); the transport
        // thread is free meanwhile.
        req.holdsSearchSlot = true;
        queued.push_back({&req, maxParallel});
        return;
      }
    }
    if (!req.holdsSearchSlot) {
      req.setError({ErrorKind::RESOURCE_EXHAUSTED, "search_queue_full",
                    "too many concurrent searches: " + std::to_string(maxConcurrent)
                    + " running and " + std::to_string(maxQueued)
                    + " queued (search.max-concurrent, search.max-queued)"});
      req.bodyDone();
      return;
    }
  }
  begin(req, maxParallel, false);
}

void SearchEngine::begin(SearchRequest& req, int32_t maxParallel, bool onArena) {
  if (!req.proto.min_commit.empty()) {
    bool slot = req.holdsSearchSlot;
    try {
      auto floor = CommitId::parse(req.proto.min_commit);
      auto collection = LuxirNode::target(req.tenant, req.proto.collection);
      auto& waits = node.getCommitWaits();
      waits.awaitCommit(std::move(collection), std::move(floor),
          waits.deadlineAfter(req.proto.min_commit_timeout_ms.value_or(30000)), req.waitCancellation.get_token(),
          [this, &req, maxParallel, slot](CommitWaits::CommitResult result) {
            node.getTaskArena().enqueue([this, &req, maxParallel, slot, result = std::move(result)] {
              if (result.error) {
                req.setError(*result.error);
                req.bodyDone();
                if (slot) release();
              } else {
                req.floorCollection = result.collection;
                runAdmitted(req, maxParallel);
              }
            });
          });
      return;

    } catch (const std::exception& e) {
      req.setError(classifyException(e, ErrorKind::INVALID_REQUEST));
      req.bodyDone();
      if (slot) release();
      return;
    }
  }
  if (onArena) {
    // Already on an arena worker (a dequeued search): run here, as the
    // min_commit path does, whatever the lane.
    runAdmitted(req, maxParallel);
  } else if (maxParallel == 0) {
    // Default lane: the whole query runs serially right here on the receiving
    // transport thread.  No scheduler hop, and the arena's parked width never
    // wakes - the transport accepts that this thread is occupied for the
    // query's duration.
    runAdmitted(req, maxParallel);
  } else if (maxParallel == -1) {
    // Parallel lane: the request starts right here, inside the arena, so it
    // pays no handoff; arena workers steal the tasks it spawns and this thread
    // helps in its own waits.  Entry takes a reserved slot (never queues
    // behind arena backlog).  Like the default lane, this transport thread is
    // occupied for the query's (now parallel) duration.
    node.executeIsolated([&] { runAdmitted(req, maxParallel); });
  } else {
    // 1 = serial off the receiving thread: the escape hatch for a query
    // expected to be expensive, so it does not occupy this transport thread.
    // Other values are rejected in prepare(), on the arena like 1.
    node.getTaskArena().enqueue([this, &req, maxParallel] { runAdmitted(req, maxParallel); });
  }
}

void SearchEngine::runAdmitted(SearchRequest& req, int32_t maxParallel) {
  // submit() ends in bodyDone, which may delete req.
  bool slot = req.holdsSearchSlot;
  submit(req, maxParallel);
  if (slot) release();
}

void SearchEngine::release() {
  Queued next;
  {
    std::lock_guard<std::mutex> lock(admissionMutex);
    if (queued.empty()) {
      running--;
      return;
    }
    next = queued.front();
    queued.pop_front();
  }
  // The slot passes to the oldest waiter.  It starts on the arena rather than
  // here: this thread may be a transport thread, and running it inline would
  // nest one search's release inside another's without bound.
  node.getTaskArena().enqueue([this, next] { startQueued(*next.req, next.maxParallel); });
}

void SearchEngine::startQueued(SearchRequest& req, int32_t maxParallel) {
  if (req.shouldStop()) {
    // Its budget ran out while it waited: answer without running anything,
    // timed out with empty results (or cancelled), and pass the slot on.
    req.lastResponse = SearchResponse::create(req, true);
    reportBudget(req);
    attachWarnings(req);
    req.bodyDone();
    release();
    return;
  }
  begin(req, maxParallel, true);
}

void SearchEngine::submit(SearchRequest& req, int32_t maxParallel) {
  // Admission and request-level cancellation happen in dispatch(); a direct
  // submit (tests, embedders) still honors timeout_ms and waitCancellation.
  armBudget(req);
  req.maxParallel = maxParallel;
  std::optional<oneapi::tbb::task_group> stackTg;
  if (maxParallel == -1 && req.tg == nullptr) {
    req.tg = &stackTg.emplace();
  }
  submitBody(req);
}

void SearchEngine::getResources(SearchRequest& req) {
  // look up the correct index reader and the associated schema
  auto& request = req.proto;
  auto& node = req.engine.node;
  auto collection = req.floorCollection ? req.floorCollection : node.getCollection(LuxirNode::target(req.tenant, request.collection));

  // get the index reader
  // The API freshness tolerance is milliseconds; the reader clock domain is
  // microseconds (commit times).  Saturate: an absurd tolerance means "any".
  constexpr uint64_t maxUs = std::numeric_limits<uint64_t>::max();
  uint64_t freshnessUs = request.freshness_ms > maxUs / 1000
      ? maxUs : request.freshness_ms * 1000;
  req.reader = collection->getReaderManager().getReader(freshnessUs);
  if (!request.min_commit.empty()) {
    auto floor = CommitId::parse(request.min_commit);
    if (req.reader->incarnation() != floor.incarnation || req.reader->commitId() < floor.index_gen)
      req.reader = collection->getReaderManager().getReader(0);
    if (req.reader->incarnation() != floor.incarnation) throw ApiError(ErrorKind::FAILED_PRECONDITION,
        "commit_incarnation_mismatch", "min_commit belongs to a different collection incarnation");
    if (req.reader->commitId() < floor.index_gen) throw ApiError(ErrorKind::UNAVAILABLE,
        "stale_replica", "reader is older than min_commit");
  }
  req.schema = req.reader->schema();
  auto* filterCache = req.reader->filterCache();
  req.filterUses = std::make_shared<FilterCache::UseRegistry>(
      filterCache, *req.reader);
}


} // namespace luxir
