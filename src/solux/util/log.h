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
