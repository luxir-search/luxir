// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "SearchRequest.h"

#include <cassert>

#include "SearchEngine.h"
#include "luxir/api/build.h"
#include "luxir/search/SearchOverrides.h"

namespace luxir {

SearchRequest::SearchRequest(SearchEngine& engine, const ReqProto& proto,
                             google::protobuf::Arena& arena)
    : engine(engine), searchConfig(engine.searchConfig()),
      memoryTracker(forcedRequestMemoryMaxBytes != 0
                        ? forcedRequestMemoryMaxBytes
                        : searchConfig.request_memory_max_bytes),
      proto(proto), arena(arena), dateMathNowEpochMillis(
          forcedDateMathNowEpochMillis.value_or(currentEpochMillis())),
      timeZone(resolveTimeZone(proto.time_zone)) {
  if (!timeZone) timeZoneError = timeZoneResolutionError(proto.time_zone);
}

// A dispatched search that never sent its final response was abandoned.
SearchRequest::~SearchRequest() {
  if (counting && !finalSent) count(MetricOutcome::CANCELLED);
}

// Counts the search for the node and, once resolved, for its collection.
void SearchRequest::count(MetricOutcome outcome) {
  auto nanos = nanosSince(submitted);
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
    if (!warnings.empty()) engine.metrics().add(MetricCounter::SEARCH_WARNINGS);
    if (counting) count(lastResponse->proto.error ? MetricOutcome::ERROR : MetricOutcome::OK);
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
