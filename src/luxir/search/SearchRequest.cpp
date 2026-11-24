// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "SearchRequest.h"
#include "luxir/query/QueryPruning.h"

#include <cassert>

#include "SearchEngine.h"
#include "OwnershipRelation.h"
#include "luxir/api/build.h"
#include "luxir/search/SearchOverrides.h"

namespace luxir {

const std::shared_ptr<IndexReader>& SearchRequest::pinView(std::string_view path) {
  if (path == reader->path()) return reader;
  const std::lock_guard lock(mutex);
  if (!scopedStates) scopedStates = std::make_unique<std::map<std::string, ScopedState, std::less<>>>();
  auto found = scopedStates->find(path);
  if (found != scopedStates->end()) return found->second.reader;
  reader->schema()->requirePath(path);
  auto scoped = reader->view(path);
  // A view shares the request's segment observations once the request has them.
  auto uses = filterUses ? std::make_shared<FilterCache::UseRegistry>(scoped->filterCache(), *scoped, *filterUses)
      : std::make_shared<FilterCache::UseRegistry>(scoped->filterCache(), *scoped);
  return scopedStates->emplace(std::string(path), ScopedState{std::move(scoped), std::move(uses), {}}).first->second.reader;
}

const std::shared_ptr<FilterCache::UseRegistry>& SearchRequest::uses(IndexReader& scoped) {
  if (&scoped == reader.get()) return filterUses;
  pinView(scoped.path());
  const std::lock_guard lock(mutex);
  return scopedStates->find(scoped.path())->second.uses;
}

OwnershipRelation& SearchRequest::ownershipRelation(IndexReader& many, IndexReader& one) {
  if (many.path() == one.path()) throw RequestError("domain transform requires different spaces");
  if (pinView(many.path()).get() != &many || pinView(one.path()).get() != &one) {
    throw RequestError("relation views must belong to this request");
  }
  const std::lock_guard lock(mutex);
  auto& state = scopedStates->find(&many == reader.get() ? one.path() : many.path())->second;
  auto key = std::pair{std::string(many.path()), std::string(one.path())};
  auto found = state.relations.find(key);
  if (found != state.relations.end()) return *found->second;
  auto* relation = arenaCreate<OwnershipRelation>(arena, many, one, memoryTracker);
  state.relations.emplace(std::move(key), relation);
  return *relation;
}

Query::PlanningContext* SearchRequest::createPlanningContext(IndexReader& view) {
  Query::PlanningContext::Limits limits;
  limits.multiTermBudget = &multiTermBudget;
  auto* planning = arenaCreate<Query::PlanningContext>(arena, requestPool, view, limits, &warnings,
      FilterKeyContext{.schemaGen = schema->gen_, .timeZone = proto.time_zone},
      uses(view), &memoryTracker, proto.profile ? this : nullptr);
  planning->pruning = pruning;
  return planning;
}

SearchRequest::SearchRequest(SearchEngine& engine, const ReqProto& proto,
                             google::protobuf::Arena& arena,
                             std::chrono::steady_clock::time_point startTime)
    : engine(engine), searchConfig(engine.searchConfig()),
      memoryTracker(forcedRequestMemoryMaxBytes != 0
                        ? forcedRequestMemoryMaxBytes
                        : searchConfig.request_memory_max_bytes),
      multiTermBudget(MultiTermLimits{
          .maxExpansions = searchConfig.multiterm_max_expansions,
          .maxTermsExamined = searchConfig.multiterm_max_terms_examined,
          .maxPostings = searchConfig.multiterm_max_postings,
      }),
      pruning(std::make_shared<QueryPruning>()),
      proto(proto), arena(arena), dateMathNowEpochMillis(
          forcedDateMathNowEpochMillis.value_or(currentEpochMillis())),
      startTime(startTime),
      timeZone(resolveTimeZone(proto.time_zone)) {
  if (!timeZone) timeZoneError = timeZoneResolutionError(proto.time_zone);
}

// A dispatched search that never sent its final response was abandoned.
SearchRequest::~SearchRequest() {
  settleFilterUses(false);
  if (counting && !finalSent) count(MetricOutcome::CANCELLED, std::chrono::steady_clock::now());
}

// Counts the search for the node and, once resolved, for its collection.
void SearchRequest::count(MetricOutcome outcome, std::chrono::steady_clock::time_point end) {
  auto nanos = nanosSince(submitted, end);
  engine.metrics().complete(metricActivity(MetricStage::SEARCH), outcome, nanos);
  if (collectionMetrics) collectionMetrics->search(outcome, nanos);
}

void SearchRequest::replyCallback(SearchResponse& response) {
  bool last = response.last;
  // if this response used a different Arena, release it.
  if (&response.arena != &arena) {
    releaseArena(&response.arena);
  }
  if (last) {
    done();
  }
}

void SearchRequest::warnOnce(std::string_view code, std::string_view message) {
  std::lock_guard<std::mutex> lock(mutex);
  for (const api::Warning& warning : warnings) {
    if (warning.code == code && warning.message == message) return;
  }
  warnings.push_back({build::arenaStr(requestPool, code),
                      build::arenaStr(requestPool, message)});
}

void SearchRequest::settleFilterUses(bool completed) noexcept {
  if (filterUses) filterUses->settle(completed);
  if (scopedStates) {
    for (auto& [path, state] : *scopedStates) state.uses->settle(completed);
  }
}

void SearchRequest::maybeSendFinal(bool endingStream) {
  bool send;
  {
    std::lock_guard<std::mutex> lock(mutex);
    if (endingStream) {
      assert(activeStreams > 0);
      activeStreams--;
    } else {
      finalReady = true;
    }
    send = finalReady && activeStreams == 0 && !finalSent;
    if (send) {
      finalSent = true;
      // A failed request reports no op results.  setError cleared them, but an
      // emitter still running at that point may have re-created its target
      // since; this is the last point under the mutex before the send.
      if (lastResponse->proto.error.has_value()) lastResponse->proto.ops = {};
    }
  }
  if (send) {
    settleFilterUses(!lastResponse->proto.error.has_value() && !waitCancellation.stop_requested());
    // The only clock read at the end of a request: no other thread touches
    // lastResponse once finalSent is decided.
    auto end = std::chrono::steady_clock::now();
    lastResponse->proto.took_us = microsSince(startTime, end);
    if (!warnings.empty()) engine.metrics().add(MetricCounter::SEARCH_WARNINGS);
    if (counting) count(lastResponse->proto.error ? MetricOutcome::ERROR : MetricOutcome::OK, end);
    reply(*lastResponse);  // may delete *this*; nothing after this call
  }
}

void SearchRequest::setError(const ErrorInfo& info) {
  std::lock_guard<std::mutex> lock(mutex);
  if (lastResponse == nullptr) lastResponse = SearchResponse::create(*this, true);
  if (lastResponse->proto.error.has_value()) return;  // the first failure wins
  if (info.kind == ErrorKind::RESOURCE_EXHAUSTED)
    engine.metrics().add(MetricCounter::SEARCH_RESOURCE_EXHAUSTED);
  lastResponse->proto.error = api::build::arenaError(lastResponse->mr, info);
  lastResponse->proto.ops = {};
}

} // namespace luxir
