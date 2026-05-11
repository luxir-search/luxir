#pragma once

// The macros remove lower priority logging levels at compile time.
// Runtime debug levels also need to be set... i.e. spdlog::set_level(spdlog::level::debug);
#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_DEBUG

#include "spdlog/spdlog.h"

// Haven't settled on what type of logger to use... but need something better in the meantime just for debugging.

// source code location doesn't work unless you use the spdlog macros
// latest c++ standards have support for these, so hopefully someday?
namespace solux {
  template<typename... Args>
  inline void info(Args &&... args) // no-go... we need file/line for easy dev.
  {
    spdlog::info(std::forward<Args>(args)...);
  }
} // end namespace solux


#define LOG_INFO SPDLOG_INFO
#define LOG_DEBUG SPDLOG_DEBUG
#define LOG_TRACE SPDLOG_TRACE
#define LOG_WARN SPDLOG_WARN
#define LOG_ERROR SPDLOG_ERROR

namespace solux {

// RAII guard that temporarily raises the global spdlog level threshold so
// lower-severity messages are suppressed, restoring the previous level on
// destruction.  Useful around code paths that are expected to emit a log
// message (e.g. tests that intentionally trigger an error) to keep output
// clean enough that real issues stand out.
//
// Default threshold is `err`, which suppresses warn and below - the common
// case for "I know this block will warn / log debug, hush it".
//
//   {
//     LogLevelGuard quiet;  // suppress warn and below
//     req->execute();
//   }
class LogLevelGuard {
  spdlog::level::level_enum prev;
public:
  explicit LogLevelGuard(spdlog::level::level_enum level = spdlog::level::err) : prev(spdlog::get_level()) {
    spdlog::set_level(level);
  }
  ~LogLevelGuard() {
    spdlog::set_level(prev);
  }

  LogLevelGuard(const LogLevelGuard&) = delete;
  LogLevelGuard& operator=(const LogLevelGuard&) = delete;
};

} // namespace solux
