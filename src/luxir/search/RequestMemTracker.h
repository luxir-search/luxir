#pragma once

#include <atomic>
#include <cstddef>
#include <string_view>

namespace luxir {

// Request-local query-memory breaker. Charge sites name their subsystem and
// supply request-specific detail; this class owns only concurrent accounting
// and the request ceiling.
class RequestMemTracker {
  std::atomic<size_t> chargedBytes{0};
  size_t maxBytes;

  [[noreturn]] void throwLimit(std::string_view breaker,
                               std::string_view detail,
                               size_t attemptedTotal) const;

public:
  explicit RequestMemTracker(size_t maxBytes) : maxBytes(maxBytes) {}

  void charge(size_t bytes, std::string_view breaker,
              std::string_view detail);
  size_t chargeUpTo(size_t preferredBytes, size_t minimumBytes,
                    std::string_view breaker, std::string_view detail);
  [[noreturn]] void chargeOverflow(std::string_view breaker,
                                   std::string_view detail) const;
  void release(size_t bytes);

  size_t bytes() const {
    return chargedBytes.load(std::memory_order_relaxed);
  }

  size_t ceiling() const { return maxBytes; }
};

} // namespace luxir
