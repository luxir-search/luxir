#include "SearchRequest.h"

#include "SearchEngine.h"

namespace luxir {

SearchRequest::SearchRequest(SearchEngine& engine, const ReqProto& proto,
                             google::protobuf::Arena& arena)
    : engine(engine), searchConfig(engine.searchConfig()), proto(proto),
      arena(arena), dateMathNowEpochMillis(currentEpochMillis()),
      timeZone(resolveTimeZone(proto.time_zone)) {
  if (!timeZone) timeZoneError = timeZoneResolutionError(proto.time_zone);
}

} // namespace luxir
