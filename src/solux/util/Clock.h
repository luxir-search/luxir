#pragma once

#include <chrono>
#include <cstdint>

namespace solux {

inline int64_t currentEpochMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
}

} // namespace solux
