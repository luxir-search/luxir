// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <ctime>
#include <format>
#include <limits>
#include <optional>

#include "luxir/api/padded_input.h"

#include "SearchEngine.h"
#include "luxir/server/CommitWaits.h"
#include "ProtobufSearchParser.h"
#include "luxir/search/ops/RootOp.h"
#include "luxir/reader/SkipStats.h"

namespace luxir {

namespace {

uint64_t threadCpuNanos() {
  timespec now{};
  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now) != 0) return 0;
  return (uint64_t)now.tv_sec * 1'000'000'000ull + (uint64_t)now.tv_nsec;
}

constexpr std::string_view laneName(Lane lane) {
  switch (lane) {
    case Lane::SERIAL: return "serial";
    case Lane::PARALLEL: return "parallel";
    case Lane::PROBE: return "probe";
  }
  return "";
}

constexpr std::string_view laneReasonName(LaneReason reason) {
  switch (reason) {
    case LaneReason::BELOW_SAVING: return "below_saving";
    case LaneReason::NO_SPARE_WORKERS: return "no_spare_workers";
    case LaneReason::SAVING: return "saving";
    case LaneReason::UNKNOWN_WORK: return "unknown_work";
    case LaneReason::POLICY: return "policy";
  }
  return "";
}

}  // namespace

SearchEngine::SearchEngine(LuxirNode& node)
  : node(node),
    lanePolicy(LanePolicyRegistry::global().make(node.getConfig().search.lane_policy, node)) {}

SearchEngine::~SearchEngine() = default;

int SearchEngine::spareWorkers() const {
  return node.getWorkers() - std::max(0, getActiveSearches() - 1);
}

bool SearchEngine::shouldEscalate(SearchRequest& req) {
  if (std::chrono::steady_clock::now() < req.laneProbeDeadline || spareWorkers() < 2) return false;
  if (!req.laneEscalated) {
    req.laneEscalated = true;
    metrics().add(MetricCounter::SEARCH_LANE_ESCALATED);
  }
  return true;
}

bool SearchRequest::laneShouldEscalate() { return engine.shouldEscalate(*this); }

void SearchRequest::runInArena(const std::function<void()>& f) {
  engine.getNode().executeIsolated(f);
}

std::optional<LaneFacts> SearchEngine::chooseLane(SearchRequest& req, RootOp& root, LaneDecision& decision) {
  // SkipStats counters are single-threaded; instrumented runs stay serial.
  if (req.maxParallel != 0 || req.tg != nullptr || SkipStats::enabled
      || req.reader->segments().size() <= 1 || !lanePolicy->active()) {
    return std::nullopt;
  }
  WorkQuote quote(req.reader->segments().size());
  root.quoteWork(quote);
  LaneFacts facts{.tenant = req.tenant, .collection = req.proto.collection,
                  .workers = node.getWorkers(), .spareWorkers = spareWorkers()};
  for (size_t i = 0; i < quote.required.size(); i++) {
    facts.requiredUnits += quote.required[i];
    facts.prunableUnits += quote.prunable[i];
    facts.largestRequiredUnits = std::max(facts.largestRequiredUnits, quote.required[i]);
    facts.largestPrunableUnits = std::max(facts.largestPrunableUnits, quote.prunable[i]);
    facts.viableSegments += quote.required[i] > 0 || quote.prunable[i] > 0;
  }
  decision = lanePolicy->decide(facts);
  switch (decision.lane) {
    case Lane::PARALLEL:
      metrics().add(MetricCounter::SEARCH_LANE_PARALLEL);
      break;
    case Lane::PROBE:
      req.laneProbing = true;
      req.laneProbeDeadline = std::chrono::steady_clock::now()
          + std::chrono::microseconds(decision.probeBudgetUs);
      metrics().add(MetricCounter::SEARCH_LANE_PROBE);
      break;
    case Lane::SERIAL:
      metrics().add(decision.reason == LaneReason::NO_SPARE_WORKERS
          ? MetricCounter::SEARCH_LANE_SERIAL_BUSY : MetricCounter::SEARCH_LANE_SERIAL);
      break;
  }
  if (auto* run = req.addExecutionProfileRun(req.addExecutionProfileOp("_lane"))) {
    auto& piece = run->pieces.at(0);
    piece.wire.kind = "lane";
    piece.details = {
        std::format("lane={} reason={}", laneName(decision.lane), laneReasonName(decision.reason)),
        std::format("required_units={:.0f} largest_segment_units={:.0f} prunable_units={:.0f}",
                    facts.requiredUnits, facts.largestRequiredUnits, facts.prunableUnits),
        std::format("viable_segments={} workers={} spare_workers={} ns_per_unit={:.2f}",
                    facts.viableSegments, facts.workers, facts.spareWorkers, lanePolicy->nsPerUnit())};
    piece.complete = true;
  }
  LOG_DEBUG("search lane {} reason {} required {:.0f} prunable {:.0f} spare {}", laneName(decision.lane),
            laneReasonName(decision.reason), facts.requiredUnits, facts.prunableUnits, facts.spareWorkers);
  if (!decision.observe) return std::nullopt;
  return facts;
}

const SearchConfig& SearchEngine::searchConfig() const {
  return node.getConfig().search;
}

RootOp* SearchEngine::prepare(SearchRequest& req) {
  if (!req.timeZone) throw RequestError(req.timeZoneError);
  if (req.maxParallel > 1 || req.maxParallel < -1) {
    throw RequestError(
      "max_parallel must be 0 (engine default, serial today), 1 (serial "
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

Metrics& SearchEngine::metrics() const { return *node.metrics; }

void SearchEngine::submitBody(SearchRequest& req) {
  MetricScope execution(*node.metrics, metricActivity(MetricStage::SEARCH_EXECUTION));
  // Until the calculators start, an unclassified failure is the request's
  // fault: parsers and planners reject authored input with bare exceptions.
  // Once execution is under way it is the engine's.  Throw sites that know
  // better say so with an ApiError, which classifies itself in either phase.
  ErrorKind fallback = ErrorKind::INVALID_REQUEST;
  RootOp* root = nullptr;
  // The lane policy may fan a max_parallel 0 request out across segments once
  // planning has priced it; other requests stay serial and pay no handoff.
  std::optional<oneapi::tbb::task_group> autoTg;
  LaneDecision laneDecision;
  std::optional<LaneFacts> observedFacts;
  std::chrono::steady_clock::time_point observeWall;
  uint64_t observeCpu = 0;
  activeSearches.fetch_add(1, std::memory_order_relaxed);
  try {
    root = prepare(req);
    root->init();
    observedFacts = chooseLane(req, *root, laneDecision);
    if (laneDecision.lane == Lane::PARALLEL) {
      req.tg = &autoTg.emplace();
      req.autoParallel = true;
    }
    if (observedFacts) {
      observeWall = std::chrono::steady_clock::now();
      observeCpu = threadCpuNanos();
    }
    std::unique_ptr<RootOp::Calc> calc(root->createCalculator(nullptr, -1));
    // The request owns the calculator tree: a flow-controlled emitter can
    // outlive this function, and its callbacks reach into collector output and
    // the getTarget chain.  Released in done().
    auto* rootCalc = calc.get();
    req.rootCalc = std::move(calc);
    fallback = ErrorKind::INTERNAL;
    if (autoTg.has_value()) {
      // The receiving thread may be outside the arena; tasks must spawn in it.
      node.executeIsolated([&] {
        rootCalc->start(req.tg);
        req.tg->wait();
      });
    } else {
      rootCalc->start(req.tg);
    }

    if (req.tg && !autoTg.has_value()) {
      LOG_TRACE("SearchRequest: waiting for task group to finish for req={}", (void*)&req);
      req.tg->wait();
      LOG_TRACE("SearchRequest: DONE! will call reply next. req={}", (void*)&req);
    }
    if (observedFacts) {
      lanePolicy->observe(*observedFacts, laneDecision, {
          (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - observeWall).count(),
          threadCpuNanos() - observeCpu, req.laneEscalated});
    }
  } catch (std::exception& e) {
    // Cancel and drain in-flight tasks before tearing down their response.
    // Swallow any parked task exception so the original error is preserved.
    if (req.tg) {
      req.tg->cancel();
      if (autoTg.has_value()) {
        node.executeIsolated([&] {
          try { req.tg->wait(); } catch (...) {}
        });
      } else {
        try { req.tg->wait(); } catch (...) {}
      }
    }
    ErrorInfo info = classifyException(e, fallback);
    if (info.kind == ErrorKind::INTERNAL) {
      LOG_ERROR("Search request failed: {}", info.message);
    } else {
      LOG_WARN("Search request rejected ({}): {}", info.code, info.message);
    }
    req.setError(info);
  }
  activeSearches.fetch_sub(1, std::memory_order_relaxed);

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
  // autoTg dies with this frame; later emitter resumptions must not see it.
  if (autoTg.has_value()) req.tg = nullptr;
  execution.finish();
  req.bodyDone();
}

void SearchEngine::enqueueSubmit(SearchRequest& req, int32_t maxParallel) {
  try {
    // Isolated: a -1 request waiting on its task group helps only with its own
    // tasks, never stealing another request's.
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
  // 0 and -1 start on the receiving thread when its transport allows it.
  bool runOnReceiver = receiver == Receiver::MAY_EXECUTE && (maxParallel == 0 || maxParallel == -1);
  if (!runOnReceiver) {
    // Off the receiving thread: always for an offloading transport, and for
    // 1 (serial on the arena, the escape hatch for a query expected to be
    // expensive).  Other values are rejected in prepare(), on the arena like 1.
    enqueueSubmit(req, maxParallel);
  } else if (maxParallel == 0) {
    // Default lane: the whole query runs serially right here on the receiving
    // transport thread.  No scheduler hop, and the arena's parked width never
    // wakes - the transport accepts that this thread is occupied for the
    // query's duration.
    submit(req, maxParallel);
  } else {
    // Parallel lane: the request starts right here, inside the arena, so it
    // pays no handoff; arena workers steal the tasks it spawns and this thread
    // helps in its own waits.  Entry takes a reserved slot (never queues
    // behind arena backlog).  Like the default lane, this transport thread is
    // occupied for the query's (now parallel) duration.
    node.executeIsolated([&] { submit(req, maxParallel); });
  }
}

void SearchEngine::submit(SearchRequest& req, int32_t maxParallel) {
  // Ideas: we could keep track of executing requests here, and provide ways to list / cancel them?
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
  req.schema = req.reader->schema();
  auto* filterCache = req.reader->filterCache();
  req.filterUses = std::make_shared<FilterCache::UseRegistry>(
      filterCache, *req.reader);
}


} // namespace luxir
