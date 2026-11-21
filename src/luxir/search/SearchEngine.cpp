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

namespace luxir {

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
    // The one batched point: planning is done and every op's root mask is
    // known, so the segments the search needs materialize now, together:
    // those collection needs (some op may match there) and those whole-index
    // preparation reads (a kNN global top k sees every segment's domain).
    // Planning itself opens only what statistics need (a segment's base for
    // term dictionary seeks), never live docs or overlays; a pruned segment
    // nothing prepares over gets nothing more.
    SegmentMask segments = root->neededSegments();
    segments |= root->preparationSegments();
    req.reader->materialize(segments);
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
      if (req.tg->getPolicy() == FanoutPolicy::ADAPTIVE && req.tg->published()) {
        fanoutParallelSearches.fetch_add(1, std::memory_order_relaxed);
      }
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
  req.bodyDone();
}

void SearchEngine::enqueueSubmit(SearchRequest& req, int32_t maxParallel) {
  try {
    // Isolation also covers waits outside the request's fan-out scope.
    node.getTaskArena().enqueue([this, &req, maxParallel] {
      oneapi::tbb::this_task_arena::isolate([&] { submit(req, maxParallel); });
    });
  } catch (const std::exception& e) {
    // Nothing was queued: complete the request here so its transport still
    // sees exactly one final reply.
    req.setError(classifyException(e, ErrorKind::INTERNAL));
    req.bodyDone();
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
      waits.awaitCommit(std::move(collection), std::move(floor),
          waits.deadlineAfter(req.proto.min_commit_timeout_ms.value_or(30000)), req.waitCancellation.get_token(),
          [this, &req, maxParallel](CommitWaits::CommitResult result) {
            if (!result.error) {
              req.floorCollection = result.collection;
              enqueueSubmit(req, maxParallel);
              return;
            }
            // CommitWaits swallows a throw from here, which would strand the
            // request: if the enqueue fails, fail the request on this thread.
            auto fail = [&req, error = *result.error] { req.setError(error); req.bodyDone(); };
            try {
              node.getTaskArena().enqueue(fail);  // copies; fail stays valid if this throws
            } catch (...) {
              fail();
            }
          });
      return;

    } catch (const std::exception& e) {
      req.setError(classifyException(e, ErrorKind::INVALID_REQUEST));
      req.bodyDone();
      return;
    }
  }
  bool runOnReceiver = receiver == Receiver::MAY_EXECUTE && maxParallel != 1;
  if (!runOnReceiver) {
    enqueueSubmit(req, maxParallel);
  } else {
    submit(req, maxParallel);
  }
}

void SearchEngine::submit(SearchRequest& req, int32_t maxParallel) {
  // Ideas: we could keep track of executing requests here, and provide ways to list / cancel them?
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
