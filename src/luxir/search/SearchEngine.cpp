// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <limits>
#include <optional>

#include "luxir/api/padded_input.h"

#include "SearchEngine.h"
#include "luxir/server/CommitWaits.h"
#include "ProtobufSearchParser.h"
#include "luxir/search/ops/RootOp.h"
#include "luxir/reader/SkipStats.h"
#include "luxir/store/Directory.h"

namespace luxir {

namespace {

// The final response of a search admission refused.
ErrorInfo refusalError(SearchAdmission::Refusal why) {
  switch (why) {
    case SearchAdmission::Refusal::QUEUE_FULL:
      return {ErrorKind::RESOURCE_EXHAUSTED, "search_queue_full",
              "too many searches are waiting for a slot (search.max-queued); retry later"};
    case SearchAdmission::Refusal::QUEUE_TIMEOUT:
      return {ErrorKind::RESOURCE_EXHAUSTED, "search_queue_timeout",
              "search waited longer than search.max-queue-ms for a slot; retry later"};
    case SearchAdmission::Refusal::CANCELLED:
      return {ErrorKind::UNAVAILABLE, "cancelled", "search cancelled while waiting for a slot"};
    case SearchAdmission::Refusal::SHUTDOWN:
      break;
  }
  return {ErrorKind::UNAVAILABLE, "shutting_down", "search refused: the node is shutting down"};
}

}  // namespace

SearchAdmission::Limits SearchEngine::admissionLimits(const SearchConfig& config, int workers) {
  workers = std::max(1, workers);
  SearchAdmission::Limits limits;
  limits.maxRunning = (uint32_t)(config.max_running > 0 ? config.max_running : workers);
  limits.maxRunningPerCollection = (uint32_t)(config.max_running_per_collection > 0
      ? config.max_running_per_collection : std::max(1, (int)limits.maxRunning - 1));
  limits.maxRunningRemote = (uint32_t)(config.max_running_remote > 0
      ? config.max_running_remote : std::max(1, (int)limits.maxRunning / 2));
  limits.maxQueued = (uint32_t)(config.max_queued >= 0 ? config.max_queued : 8 * workers);
  limits.maxQueueWait = std::chrono::milliseconds(std::max<int64_t>(0, config.max_queue_ms));
  return limits;
}

SearchEngine::SearchEngine(LuxirNode& node)
    : node(node), admission(admissionLimits(node.getConfig().search, node.getWorkers())) {}

const SearchConfig& SearchEngine::searchConfig() const {
  return node.getConfig().search;
}

RootOp* SearchEngine::prepare(SearchRequest& req) {
  if (!req.timeZone) throw RequestError(req.timeZoneError);
  if (req.maxParallel > 1 || req.maxParallel < -2) {
    throw RequestError(
      "max_parallel must be 0 (adaptive), 1 (serial on an arena worker), "
      "-1 (publish every sibling), or -2 (serial inline)");
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

Metrics& SearchEngine::metrics() const { return *node.metrics; }

void SearchEngine::submitBody(SearchRequest& req) {
  MetricScope execution(*node.metrics, metricActivity(MetricStage::SEARCH_EXECUTION));
  LoadScope load(LoadKind::REQUEST);
  std::optional<FanoutCreditScope> credit;
  if (req.tg) credit.emplace(execution.startNs());
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
    auto wait = [&] {
      bool published = req.tg->published();
      uint64_t start = published ? TaskClock{}() : 0;
      auto finish = scope_guard([&] {
        if (published) {
          fanoutShareWaitNsSum.fetch_add(TaskClock{}() - start, std::memory_order_relaxed);
          fanoutShareWaitCount.fetch_add(1, std::memory_order_relaxed);
        }
      });
      req.tg->join();
    };
    rootCalc->start(req.tg);

    if (req.tg) {
      LOG_TRACE("SearchRequest: waiting for task group to finish for req={}", (void*)&req);
      wait();
      LOG_TRACE("SearchRequest: DONE! will call reply next. req={}", (void*)&req);
    }
  } catch (std::exception& e) {
    // Cancel and drain in-flight tasks before tearing down their response.
    // Swallow any parked task exception so the original error is preserved.
    if (req.tg) {
      req.tg->cancel();
      try { req.tg->join(); } catch (...) {}
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

  // Declared degradations ride on the final response (streaming responses
  // with more=true do not carry them).
  if (!req.warnings.empty()) {
    auto* warnings = luxir::api::build::allocArray(req.lastResponse->proto.warnings,
                                                   req.warnings.size(), req.lastResponse->mr);
    for (size_t i = 0; i < req.warnings.size(); i++) {
      warnings[i] = req.warnings[i];
    }
  }

  // Send back the final response - or, if a flow-controlled emitter is still
  // paused with batches to produce, hand off: the last stream to end sends it.
  // Do not access req after this point as it may be asynchronously deleted.
  // The request scope is stack-owned; emitters retain only configuration.
  req.tg = nullptr;
  execution.finish();
  // The admission slot goes to the next search now, before the final
  // response: the body's work is done, and a paused stream's resumed bursts
  // re-admit through the priority lane (admitResume).
  req.admissionSlot.reset();
  req.bodyDone();
}

void SearchEngine::failHere(SearchRequest& req, const ErrorInfo& error, MetricOutcome outcome) noexcept {
  req.errorOutcome = outcome;
  try {
    req.setError(error);
  } catch (...) {
    LOG_ERROR("Search request could not record its failure: {}", error.message);
  }
  // Without a response to carry it there is nothing to send; setError creates
  // it, so this holds only if that allocation failed too.
  if (req.lastResponse) req.bodyDone();
}

void SearchEngine::enqueueRun(SearchRequest& req, int32_t maxParallel) noexcept {
  req.offloaded = true;
  try {
    // Isolation also covers waits outside the request's fan-out scope.
    node.getTaskArena().enqueue([this, &req, maxParallel] {
      oneapi::tbb::this_task_arena::isolate([&] { submit(req, maxParallel); });
    });
  } catch (...) {
    // Nothing was queued: complete the request here so its transport still
    // sees exactly one final reply, and free its slot first.
    req.admissionSlot.reset();
    failHere(req, currentExceptionInfo(ErrorKind::INTERNAL), MetricOutcome::ERROR);
  }
}

bool SearchEngine::readsRemotely(const Collection& collection) noexcept {
  try {
    return collection.getShard()->getDirectory()->capabilities() & Directory::REMOTE;
  } catch (const std::exception&) {
    return false;  // getResources reports the unavailable collection
  }
}

bool SearchEngine::readsRemotely(const SearchRequest& req) {
  try {
    return readsRemotely(*node.getCollection(LuxirNode::target(req.tenant, req.proto.collection)));
  } catch (const std::exception&) {
    return false;  // getResources reports the failed lookup, as for any collection
  }
}

void SearchEngine::admitThenRun(SearchRequest& req, int32_t maxParallel, Receiver receiver, bool remote) {
  // Inline only where the transport allows it, for a lane that wants it, and
  // for a collection whose reads cannot wait on the network.
  bool inlineLane = receiver == Receiver::MAY_EXECUTE && maxParallel != 1 && !remote;
  SearchAdmission::Slot slot;
  try {
    auto id = LuxirNode::target(req.tenant, req.proto.collection);
    auto collection = id.tenant + "/" + id.name;  // _stats splits it back
    slot = admission.tryAcquire(collection, remote);
    if (!slot) {
      // A queued search holds no thread and always runs on the arena: the
      // thread that frees a slot only enqueues it.  Refusals and cancellations
      // answer from the thread that decides them, never through the arena the
      // search was refused from.
      admission.admit(collection, remote, req.admissionStop(),
          [this, &req, maxParallel](SearchAdmission::Slot granted) {
            req.admissionSlot = std::move(granted);
            enqueueRun(req, maxParallel);
          },
          [&req](SearchAdmission::Refusal why) {
            failHere(req, refusalError(why),
                     why == SearchAdmission::Refusal::CANCELLED ? MetricOutcome::CANCELLED : MetricOutcome::REJECTED);
          });
      return;
    }
  } catch (...) {
    // admit() throws only before it takes the search.
    failHere(req, currentExceptionInfo(ErrorKind::INTERNAL), MetricOutcome::ERROR);
    return;
  }
  req.admissionSlot = std::move(slot);
  if (inlineLane) {
    submit(req, maxParallel);
  } else {
    enqueueRun(req, maxParallel);
  }
}

void SearchEngine::dispatch(SearchRequest& req, int32_t maxParallel, Receiver receiver) {
  req.counting = true;
  req.submitted = std::chrono::steady_clock::now();
  metrics().start(metricActivity(MetricStage::SEARCH));
  if (!req.proto.min_commit.empty()) {
    try {
      auto floor = CommitId::parse(req.proto.min_commit);
      auto collection = LuxirNode::target(req.tenant, req.proto.collection);
      auto& waits = node.getCommitWaits();
      // Admitted after the wait, so no slot is held through it, and run on the
      // arena: the continuation may fire on a commit or timer thread.
      waits.awaitCommit(std::move(collection), std::move(floor),
          waits.deadlineAfter(req.proto.min_commit_timeout_ms.value_or(30000)), req.waitCancellation.get_token(),
          [this, &req, maxParallel](CommitWaits::CommitResult result) {
            if (!result.error) {
              req.floorCollection = result.collection;
              admitThenRun(req, maxParallel, Receiver::OFFLOAD, readsRemotely(*result.collection));
              return;
            }
            // CommitWaits swallows a throw from here, which would strand the
            // request: answer on this thread.
            failHere(req, *result.error, MetricOutcome::ERROR);
          });
      return;

    } catch (const std::exception& e) {
      req.setError(classifyException(e, ErrorKind::INVALID_REQUEST));
      req.bodyDone();
      return;
    }
  }
  admitThenRun(req, maxParallel, receiver, readsRemotely(req));
}

std::function<void()> SearchEngine::admitResume(std::function<void()> resume) {
  return [this, resume = std::move(resume)] {
    try {
      admission.resume([this, resume](SearchAdmission::Slot slot) {
        if (!slot.waited()) {
          resume();  // here, where the transport placed it; the slot frees after the burst
          return;
        }
        // Started by the thread that freed a slot, which only hands work off.
        auto run = [held = std::make_shared<SearchAdmission::Slot>(std::move(slot)), resume] {
          oneapi::tbb::this_task_arena::isolate(resume);
          held->reset();
        };
        try {
          node.getTaskArena().enqueue(run);  // copies; run stays valid if this throws
        } catch (...) {
          run();
        }
      });
    } catch (...) {
      // Nothing was queued (the lane allocates first): never strand a stream.
      resume();
    }
  };
}

void SearchEngine::submit(SearchRequest& req, int32_t maxParallel) {
  // Ideas: we could keep track of executing requests here, and provide ways to list / cancel them?
  // -1 is not load aware (task_group_fanout publishes every sibling): an
  // admitted -1 search fans out adaptively, like 0, while searches queue or
  // others run, so it does not take every worker from them.
  if (maxParallel == -1 && req.admissionSlot && admission.crowded((uint32_t)node.getWorkers())) {
    maxParallel = 0;
    admission.countDowngrade();
  }
  req.maxParallel = maxParallel;
  std::optional<FanoutScope> stackTg;
  // SkipStats counters are single-threaded; adaptive instrumented runs stay serial.
  if (req.tg == nullptr && (maxParallel == -1 || (maxParallel == 0 && !SkipStats::enabled))) {
    req.tg = &stackTg.emplace(&node.getTaskArena(),
        maxParallel == -1 ? FanoutPolicy::EVERY : FanoutPolicy::ADAPTIVE);
  }
  submitBody(req);
}

void SearchEngine::getResources(SearchRequest& req) {
  if (!req.reader) {
    // look up the correct index reader and the associated schema
    auto& request = req.proto;
    auto& node = req.engine.node;
    auto collection = req.floorCollection ? req.floorCollection : node.getCollection(LuxirNode::target(req.tenant, request.collection));
    req.collectionMetrics = collection->sharedMetrics();

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
  }
  req.schema = req.reader->schema();
  auto* filterCache = req.reader->filterCache();
  req.filterUses = std::make_shared<FilterCache::UseRegistry>(
      filterCache, *req.reader);
}


} // namespace luxir
