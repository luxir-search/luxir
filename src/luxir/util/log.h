// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

// The macros remove lower priority logging levels at compile time.
// Runtime debug levels also need to be set... i.e. spdlog::set_level(spdlog::level::debug);
#define SPDLOG_ACTIVE_LEVEL SPDLOG_LEVEL_DEBUG

#include "spdlog/spdlog.h"
#include "spdlog/sinks/sink.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

// Haven't settled on what type of logger to use... but need something better in the meantime just for debugging.

// source code location doesn't work unless you use the spdlog macros
// latest c++ standards have support for these, so hopefully someday?
namespace luxir {
  template<typename... Args>
  inline void info(Args &&... args) // no-go... we need file/line for easy dev.
  {
    spdlog::info(std::forward<Args>(args)...);
  }
} // end namespace luxir


#define LOG_INFO SPDLOG_INFO
#define LOG_DEBUG SPDLOG_DEBUG
#define LOG_TRACE SPDLOG_TRACE
#define LOG_WARN SPDLOG_WARN
#define LOG_ERROR SPDLOG_ERROR

namespace luxir {

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

// One active suppression rule: a set of payload substrings to drop and a
// counter of how many messages this rule has dropped.  Owned by an ExpectLog;
// the FilterSink only holds a raw pointer to it, valid for the ExpectLog's
// lifetime (it deregisters in its destructor under the same mutex).
struct Expectation {
  std::vector<std::string> drop;
  std::atomic<size_t> dropped{0};
};

// A sink wrapping the logger's real sinks.  It drops any message whose payload
// contains a substring named by one of the currently-registered expectations
// (forwarding everything else unchanged) and bumps that expectation's counter.
//
// Installed exactly once per process (see ExpectLog) and never swapped back out,
// so the logger's sink list is mutated only at that first install - not on every
// guard scope.  That matters because background threads (e.g. merge workers) log
// concurrently: registering/deregistering an expectation only touches the
// mutex-guarded `active` list, never the logger's sink vector, so there is no
// data race between a guard's construction/destruction and a concurrent log().
class FilterSink : public spdlog::sinks::sink {
  std::vector<spdlog::sink_ptr> inner;
  std::mutex mtx;
  std::vector<Expectation*> active;  // guarded by mtx
public:
  explicit FilterSink(std::vector<spdlog::sink_ptr> inner) : inner(std::move(inner)) {}

  void add(Expectation* e) {
    std::lock_guard<std::mutex> lock(mtx);
    active.push_back(e);
  }
  void remove(Expectation* e) {
    std::lock_guard<std::mutex> lock(mtx);
    active.erase(std::remove(active.begin(), active.end(), e), active.end());
  }

  void log(const spdlog::details::log_msg& msg) override {
    std::string_view payload(msg.payload.data(), msg.payload.size());
    {
      std::lock_guard<std::mutex> lock(mtx);
      for (auto* e : active) {
        for (const auto& d : e->drop) {
          if (payload.find(d) != std::string_view::npos) {
            e->dropped.fetch_add(1, std::memory_order_relaxed);
            return;
          }
        }
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

// The process-wide FilterSink, installed on first use into the default logger.
// Only ever installed by a test constructing an ExpectLog, so the server binary
// (which never constructs one) keeps its plain sink chain untouched.  The swap
// happens once, under call_once, at a quiescent point before any test launches
// its threads, so it does not race with concurrent logging.
inline FilterSink& filterSink() {
  static std::shared_ptr<FilterSink> instance = [] {
    auto logger = spdlog::default_logger();
    auto sink = std::make_shared<FilterSink>(logger->sinks());
    logger->sinks() = {sink};
    return sink;
  }();
  return *instance;
}

} // namespace log_detail

// RAII guard that suppresses *only* the expected log lines naming a known
// substring, letting every other message through.  Unlike LogLevelGuard (a
// global threshold that hides all messages below a level), this is precise: an
// unexpected error still surfaces even while the guard is installed.  That also
// makes it safe to wrap an entire multithreaded test, where the expected
// message is emitted from background threads at unpredictable times and there
// is no tight window to wrap.
//
// Constructing the guard registers its substrings on the process-wide filter
// sink (installed lazily on first use); destroying it deregisters them.  Both
// are mutex-guarded against concurrent log() calls, so - unlike a sink-list
// swap - the guard may be created and destroyed while background threads log.
//
//   ExpectLog quiet("injected merge failure (hammer)");
//   ... run the whole hammer ...
//   EXPECT_GT(quiet.suppressed(), 0u);  // optional: the failures really fired
class ExpectLog {
  log_detail::Expectation expectation;

  void install(std::vector<std::string> substrings) {
    expectation.drop = std::move(substrings);
    log_detail::filterSink().add(&expectation);
  }
public:
  explicit ExpectLog(std::string substring) { install({std::move(substring)}); }
  ExpectLog(std::initializer_list<std::string> substrings) { install(substrings); }
  ~ExpectLog() { log_detail::filterSink().remove(&expectation); }

  // Number of messages dropped so far - lets a test assert the expected log
  // actually fired (catches the message text drifting out from under the filter).
  size_t suppressed() const { return expectation.dropped.load(std::memory_order_relaxed); }

  ExpectLog(const ExpectLog&) = delete;
  ExpectLog& operator=(const ExpectLog&) = delete;
};

} // namespace luxir
