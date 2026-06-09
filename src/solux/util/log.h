#pragma once

// The macros remove lower priority logging levels at compile time.
// Runtime debug levels also need to be set... i.e. spdlog::set_level(spdlog::level::debug);
#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_DEBUG

#include "spdlog/spdlog.h"
#include "spdlog/sinks/sink.h"

#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

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

namespace log_detail {

// A sink that drops any message whose payload contains one of `drop`'s
// substrings and forwards everything else to the wrapped sinks unchanged.
class FilterSink : public spdlog::sinks::sink {
  std::vector<spdlog::sink_ptr> inner;
  std::vector<std::string> drop;
  std::atomic<size_t> dropped{0};
public:
  FilterSink(std::vector<spdlog::sink_ptr> inner, std::vector<std::string> drop)
    : inner(std::move(inner)), drop(std::move(drop)) {}

  size_t droppedCount() const { return dropped.load(std::memory_order_relaxed); }

  void log(const spdlog::details::log_msg& msg) override {
    std::string_view payload(msg.payload.data(), msg.payload.size());
    for (const auto& d : drop) {
      if (payload.find(d) != std::string_view::npos) {
        dropped.fetch_add(1, std::memory_order_relaxed);
        return;
      }
    }
    for (const auto& s : inner) {
      if (s->should_log(msg.level)) s->log(msg);
    }
  }
  void flush() override { for (const auto& s : inner) s->flush(); }
  void set_pattern(const std::string& pattern) override {
    for (const auto& s : inner) s->set_pattern(pattern);
  }
  void set_formatter(std::unique_ptr<spdlog::formatter> f) override {
    for (size_t i = 0; i < inner.size(); i++) {
      inner[i]->set_formatter(i + 1 == inner.size() ? std::move(f) : f->clone());
    }
  }
};

} // namespace log_detail

// RAII guard that suppresses *only* the expected log lines naming a known
// substring, letting every other message through.  Unlike LogLevelGuard (a
// global threshold that hides all messages below a level), this is precise: an
// unexpected error still surfaces even while the guard is installed.  That also
// makes it safe to wrap an entire multithreaded test, where the expected
// message is emitted from background threads at unpredictable times and there
// is no tight window to wrap.
//
// Install/restore mutate the logger's sink list, so they must not race with a
// concurrent log call: construct the guard before launching threads and destroy
// it after joining them.
//
//   ExpectLog quiet("injected merge failure (hammer)");
//   ... run the whole hammer ...
//   EXPECT_GT(quiet.suppressed(), 0u);  // optional: the failures really fired
class ExpectLog {
  std::shared_ptr<spdlog::logger> logger;
  std::vector<spdlog::sink_ptr> saved;
  std::shared_ptr<log_detail::FilterSink> filter;

  void install(std::vector<std::string> substrings) {
    logger = spdlog::default_logger();
    saved = logger->sinks();
    filter = std::make_shared<log_detail::FilterSink>(saved, std::move(substrings));
    logger->sinks() = {filter};
  }
public:
  explicit ExpectLog(std::string substring) { install({std::move(substring)}); }
  ExpectLog(std::initializer_list<std::string> substrings) { install(substrings); }
  ~ExpectLog() { logger->sinks() = saved; }

  // Number of messages dropped so far - lets a test assert the expected log
  // actually fired (catches the message text drifting out from under the filter).
  size_t suppressed() const { return filter->droppedCount(); }

  ExpectLog(const ExpectLog&) = delete;
  ExpectLog& operator=(const ExpectLog&) = delete;
};

} // namespace solux
