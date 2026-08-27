#include "SearchRequest.h"

#include <cstring>

#include "SearchEngine.h"

namespace luxir {

SearchRequest::SearchRequest(SearchEngine& engine, const ReqProto& proto,
                             google::protobuf::Arena& arena)
    : engine(engine), searchConfig(engine.searchConfig()), proto(proto),
      arena(arena), dateMathNowEpochMillis(currentEpochMillis()),
      timeZone(resolveTimeZone(proto.time_zone)) {
  if (!timeZone) timeZoneError = timeZoneResolutionError(proto.time_zone);
}

void SearchRequest::warnOnce(std::string_view code, std::string_view message) {
  std::lock_guard<std::mutex> lock(mutex);
  for (const api::Warning& warning : warnings) {
    if (warning.code == code && warning.message == message) return;
  }
  auto copy = [&](std::string_view text) {
    char* target = requestPool.alloc(text.size());
    std::memcpy(target, text.data(), text.size());
    return std::string_view(target, text.size());
  };
  warnings.push_back({copy(code), copy(message)});
}

} // namespace luxir
