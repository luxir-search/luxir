// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include <limits>
#include "luxir/api/padded_input.h"

#include "SearchEngine.h"
#include "luxir/server/ReplicationCatalog.h"
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

std::vector<std::string> SearchEngine::explain(const ReqProto& proto) {
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
  req->resolvedFields = &notes;
  prepare(*req);
  return notes;
}

void SearchEngine::submitBody(SearchRequest& req) {
  // Until the calculators start, an unclassified failure is the request's
  // fault: parsers and planners reject authored input with bare exceptions.
  // Once execution is under way it is the engine's.  Throw sites that know
  // better say so with an ApiError, which classifies itself in either phase.
  ErrorKind fallback = ErrorKind::INVALID_REQUEST;
  try {
    auto* root = prepare(req);
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
  if (!req.proto.min_commit.empty()) {
    try {
      auto floor = CommitId::parse(req.proto.min_commit);
      auto error = std::make_shared<std::optional<ErrorInfo>>();
      using Event = ReplicationCatalog::Event;
      auto ready = [this, &req, floor, error](Event event) {
        if (event == Event::CANCELLED || (event == Event::REMOVED && !node.following())) {
          *error = ErrorInfo{ErrorKind::UNAVAILABLE, "stale_replica", "search floor wait cancelled or collection deleted"};
          return true;
        }
        try {
          auto collection = node.resolveCollection(req.proto.collection);
          auto snapshot = collection->getShard()->getSnapshots().snapshot();
          if (snapshot && snapshot->id.incarnation != floor.incarnation) {
            if (!node.following() || event == Event::DEADLINE) {
              *error = ErrorInfo{ErrorKind::FAILED_PRECONDITION, "commit_incarnation_mismatch", "min_commit belongs to a different collection incarnation"};
              return true;
            }
          } else if (snapshot && snapshot->id.index_gen >= floor.index_gen) {
            req.floorCollection = std::move(collection);
            return true;
          }
        } catch (const CollectionNotFoundError&) {}
        catch (const std::exception& e) { *error = classifyException(e, ErrorKind::INTERNAL); return true; }
        if (event != Event::DEADLINE) return false;
        *error = ErrorInfo{ErrorKind::UNAVAILABLE, "stale_replica", "min_commit was not available before min_commit_timeout_ms"};
        return true;
      };
      auto collection = req.proto.collection.empty() ? std::string(LuxirNode::kDefaultCollectionName) : std::string(req.proto.collection);
      bool immediate = node.getReplication().await(std::move(collection), std::move(ready),
          [this, &req, error, maxParallel] {
            node.getTaskArena().enqueue([this, &req, error, maxParallel] {
              if (*error) { req.setError(**error); req.bodyDone(); }
              else submit(req, maxParallel);
            });
          }, req.proto.min_commit_timeout_ms.value_or(30000), req.waitCancellation.get_token());
      if (!immediate) return;
      if (*error) { req.setError(**error); req.bodyDone(); return; }

    } catch (const std::exception& e) {
      req.setError(classifyException(e, ErrorKind::INVALID_REQUEST));
      req.bodyDone();
      return;
    }
  }
  if (maxParallel == 0) {
    // Default lane: the whole query runs serially right here on the receiving
    // transport thread.  No scheduler hop, and the arena's parked width never
    // wakes - the transport accepts that this thread is occupied for the
    // query's duration.
    submit(req, maxParallel);
  } else {
    // Every explicit non-zero value is an arena contract: 1 = serial off the
    // receiving thread, -1 = unlimited intra-request parallelism (>1 reserved
    // for a bounded width budget).  Query work stays inside the TBB-governed
    // thread set either way.
    node.getTaskArena().enqueue([this, &req, maxParallel] { submit(req, maxParallel); });
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
  auto collection = req.floorCollection ? req.floorCollection : node.resolveCollection(request.collection);

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
