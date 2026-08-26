#include <limits>

#include "SearchEngine.h"
#include "ProtobufSearchParser.h"
#include "luxir/search/ops/RootOp.h"

namespace luxir {

const SearchConfig& SearchEngine::searchConfig() const {
  return node.getConfig().search;
}

void SearchEngine::submitBody(SearchRequest& req) {
  try {
    if (!req.timeZone) throw std::runtime_error(req.timeZoneError);
    if (req.maxParallel > 1 || req.maxParallel < -1) {
      throw std::runtime_error(
          "max_parallel must be -1 (run on the submitting thread), 0 (auto), or 1 "
          "(single-threaded); values above 1 are not implemented");
    }
    getResources(req);
    // LOG_DEBUG("submitBody: IndexReader commitTime={}", req.reader->commitTime());
    req.lastResponse = SearchResponse::create(req, true);

    ProtobufSearchParser parser(req);
    auto* root = parser.parse();
    root->init();
    std::unique_ptr<RootOp::Calc> calc(root->createCalculator(nullptr, -1));
    // The request owns the calculator tree: a flow-controlled emitter can
    // outlive this function, and its callbacks reach into collector output and
    // the getTarget chain.  Released in done().
    auto* rootCalc = calc.get();
    req.rootCalc = std::move(calc);
    rootCalc->start(req.tg);

    if (req.tg) {
      LOG_TRACE("SearchRequest: waiting for task group to finish for req={}", (void*)&req);
      req.tg->wait();
      LOG_TRACE("SearchRequest: DONE! will call reply next. req={}", (void*)&req);
    }
  } catch (std::exception& e) {
    // TODO: distinguish between request errors and server errors.

    // Drain any in-flight tasks so they aren't writing into the response while
    // we tear down.  wait() rethrows the first exception, so swallow it here.
    if (req.tg) {
      // TODO: notify other tasks about the error?
      try { req.tg->wait(); } catch (...) {}
    }
    LOG_WARN("Search request failed: {}", e.what());
    if (req.lastResponse == nullptr) {
      req.lastResponse = SearchResponse::create(req, true);
    }
    // proto.error is a non-owning string_view; e.what() points into the exception object,
    // which is destroyed when this catch block exits. Copy it into the response arena.
    // Under req.mutex: a paused emitter resumed by the transport may be
    // assembling its final batch into lastResponse (getTarget) concurrently.
    std::lock_guard<std::mutex> lock(req.mutex);
    req.lastResponse->proto.error = luxir::api::build::arenaStr(req.lastResponse->mr, e.what());
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
  req.bodyDone();
}

void SearchEngine::dispatch(SearchRequest& req, int32_t maxParallel) {
  if (maxParallel == -1) {
    submit(req, maxParallel);
  } else {
    // 0 and 1 both run on the shared arena; 1 just skips the task_group, so
    // "don't parallelize me" never moves the request outside the TBB-governed
    // thread set (a second executor would contend with arena work the market
    // cannot see).  -1 is the no-scheduler lane when that isolation matters.
    node.getTaskArena().enqueue([this, &req, maxParallel] { submit(req, maxParallel); });
  }
}

void SearchEngine::submit(SearchRequest& req, int32_t maxParallel) {
  // Ideas: we could keep track of executing requests here, and provide ways to list / cancel them?
  req.maxParallel = maxParallel;
  std::optional<oneapi::tbb::task_group> stackTg;
  if (maxParallel == 0 && req.tg == nullptr) {
    req.tg = &stackTg.emplace();
  }
  submitBody(req);
}

void SearchEngine::getResources(SearchRequest& req) {
  // look up the correct index reader and the associated schema
  auto& request = req.proto;
  auto& node = req.engine.node;
  auto collection = node.resolveCollection(request.collection ? &*request.collection : nullptr);

  // get the index reader
  req.schema = collection->getSchema();
  // The API freshness tolerance is milliseconds; the reader clock domain is
  // microseconds (commit times).  Saturate: an absurd tolerance means "any".
  constexpr uint64_t maxUs = std::numeric_limits<uint64_t>::max();
  uint64_t freshnessUs = request.freshness_ms > maxUs / 1000
      ? maxUs : request.freshness_ms * 1000;
  req.reader = collection->getShard()->getIndexWriter()->getIndexReader(freshnessUs);
  auto* filterCache = req.reader->filterCache();
  req.filterUses = std::make_shared<FilterCache::UseRegistry>(
      filterCache, *req.reader);
}


} // namespace luxir
