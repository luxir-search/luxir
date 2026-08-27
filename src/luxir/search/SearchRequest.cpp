#include "SearchRequest.h"

#include <cstring>
#include <limits>

#include <fmt/format.h>

#include "SearchEngine.h"
#include "luxir/search/SearchOverrides.h"

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

void SearchRequest::chargeFacetAggregateState(
    size_t bytes, std::string_view facetName, std::string_view metricName) {
  size_t limit = forcedFacetAggregateStateByteBudget != 0
      ? forcedFacetAggregateStateByteBudget
      : searchConfig.facet_aggregate_state_max_bytes;
  size_t current = facetAggregateStateBytes.load(std::memory_order_relaxed);
  for (;;) {
    bool overflow = bytes > std::numeric_limits<size_t>::max() - current;
    size_t estimate = overflow ? std::numeric_limits<size_t>::max()
                               : current + bytes;
    if (limit != 0 && estimate > limit) {
      throw std::runtime_error(fmt::format(
          "facet '{}' metric '{}': aggregate state estimate {} bytes exceeds "
          "the request limit of {} bytes",
          facetName, metricName, estimate, limit));
    }
    if (overflow) {
      throw std::runtime_error(fmt::format(
          "facet '{}' metric '{}': aggregate state byte estimate overflow",
          facetName, metricName));
    }
    if (facetAggregateStateBytes.compare_exchange_weak(
            current, estimate, std::memory_order_relaxed)) {
      return;
    }
  }
}

void SearchRequest::releaseFacetAggregateState(size_t bytes) {
  size_t previous = facetAggregateStateBytes.fetch_sub(
      bytes, std::memory_order_relaxed);
  assert(previous >= bytes);
}

} // namespace luxir
