#include "RequestMemTracker.h"

#include <algorithm>
#include <cassert>
#include <limits>
#include <stdexcept>

#include <fmt/format.h>

#include "luxir/util/ApiError.h"
#include "luxir/util/luxir_util.h"

namespace luxir {

[[noreturn]] void RequestMemTracker::throwLimit(
    std::string_view breaker, std::string_view detail,
    size_t attemptedTotal) const {
  throw ResourceExhaustedError(fmt::format(
      "request memory breaker '{}' rejected {}: attempted total {} bytes "
      "exceeds the ceiling of {} bytes",
      breaker, detail, attemptedTotal, maxBytes), "request_memory_exceeded");
}

[[noreturn]] void RequestMemTracker::chargeOverflow(
    std::string_view breaker, std::string_view detail) const {
  constexpr size_t MAX = std::numeric_limits<size_t>::max();
  if (maxBytes != 0 && maxBytes < MAX) {
    throwLimit(breaker, detail, std::numeric_limits<size_t>::max());
  }
  if (maxBytes == 0) {
    throw ResourceExhaustedError(fmt::format(
        "request memory breaker '{}' rejected {}: attempted total exceeds {} "
        "bytes (size_t overflow); the ceiling is unlimited",
        breaker, detail, MAX), "request_memory_exceeded");
  }
  throw ResourceExhaustedError(fmt::format(
      "request memory breaker '{}' rejected {}: attempted total exceeds {} "
      "bytes (size_t overflow); the ceiling is {} bytes",
      breaker, detail, MAX, maxBytes), "request_memory_exceeded");
}

void RequestMemTracker::charge(size_t bytes, std::string_view breaker,
                               std::string_view detail) {
  if (bytes == 0) return;
  size_t charged = chargeUpTo(bytes, bytes, breaker, detail);
  assert(charged == bytes);
  unused(charged);
}

bool RequestMemTracker::tryCharge(size_t bytes) {
  if (bytes == 0) return true;
  size_t current = chargedBytes.load(std::memory_order_relaxed);
  for (;;) {
    if (bytes > std::numeric_limits<size_t>::max() - current) return false;
    size_t total = current + bytes;
    if (maxBytes != 0 && total > maxBytes) return false;
    if (chargedBytes.compare_exchange_weak(
            current, total, std::memory_order_relaxed)) {
      return true;
    }
  }
}

size_t RequestMemTracker::chargeUpTo(
    size_t preferredBytes, size_t minimumBytes,
    std::string_view breaker, std::string_view detail) {
  assert(preferredBytes >= minimumBytes);
  assert(minimumBytes != 0);
  size_t current = chargedBytes.load(std::memory_order_relaxed);
  for (;;) {
    bool overflow = minimumBytes > std::numeric_limits<size_t>::max() - current;
    size_t minimumTotal = overflow ? std::numeric_limits<size_t>::max()
                                   : current + minimumBytes;
    if (maxBytes != 0 && minimumTotal > maxBytes) {
      throwLimit(breaker, detail, minimumTotal);
    }
    if (overflow) chargeOverflow(breaker, detail);

    size_t granted = preferredBytes;
    if (maxBytes != 0) granted = std::min(granted, maxBytes - current);
    if (granted > std::numeric_limits<size_t>::max() - current) {
      granted = std::numeric_limits<size_t>::max() - current;
    }
    assert(granted >= minimumBytes);
    size_t total = current + granted;
    if (chargedBytes.compare_exchange_weak(
            current, total, std::memory_order_relaxed)) {
      return granted;
    }
  }
}

void RequestMemTracker::release(size_t bytes) {
  if (bytes == 0) return;
  size_t previous = chargedBytes.fetch_sub(bytes, std::memory_order_relaxed);
  assert(previous >= bytes);
  unused(previous);
}

} // namespace luxir
