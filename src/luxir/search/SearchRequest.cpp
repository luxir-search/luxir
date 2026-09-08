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
      proto(proto), arena(arena), dateMathNowEpochMillis(currentEpochMillis()),
      timeZone(resolveTimeZone(proto.time_zone)) {
  if (!timeZone) timeZoneError = timeZoneResolutionError(proto.time_zone);
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
    reply(*lastResponse);  // may delete *this*; nothing after this call
  }
}

void SearchRequest::setError(const ErrorInfo& info) {
  std::lock_guard<std::mutex> lock(mutex);
  if (lastResponse == nullptr) lastResponse = SearchResponse::create(*this, true);
  if (lastResponse->proto.error.has_value()) return;  // the first failure wins
  lastResponse->proto.error = api::build::arenaError(lastResponse->mr, info);
  lastResponse->proto.ops = {};
}

} // namespace luxir
