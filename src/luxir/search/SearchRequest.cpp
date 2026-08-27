#include "SearchRequest.h"

#include "SearchEngine.h"
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

} // namespace luxir
