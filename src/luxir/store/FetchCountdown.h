// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <oneapi/tbb/task_arena.h>

#include <atomic>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <utility>
#include <vector>

#include "OutputStream.h"
#include "RangeFetch.h"
#include "luxir/util/log.h"

namespace luxir {

/// The reads of one request, file by file, for a fetch ahead of them.
struct FileReads {
  std::shared_ptr<InputFile> file;
  std::vector<ByteRange> ranges;
};
using ReadSet = std::vector<FileReads>;

/// Joins the fetches of one request and resumes the request when the last one
/// finishes, so that no thread waits in between.
///
/// Call fetch() for each file, then arm() once, all from one thread. When
/// every fetch finished before arm() (memory-backed and mapped files finish
/// inline), arm() returns true and the continuation never runs: the caller
/// goes on on its own thread, so a request over local files keeps its thread
/// and pays no hop. Otherwise arm() returns false, and the last fetch to
/// finish enqueues the continuation on `arena`, with the first error a fetch
/// reported (null when all succeeded). If that enqueue fails it runs on the
/// fetch thread instead, so the request is never stranded. The continuation
/// must not throw: a throw is logged and dropped.
///
/// The countdown holds every file it fetches until the continuation returns.
class FetchCountdown : public std::enable_shared_from_this<FetchCountdown> {
  struct Private {};

  oneapi::tbb::task_arena& arena;
  std::function<void(std::exception_ptr)> then;
  std::atomic<size_t> pending{1};  // the fetches in flight, plus one until arm()
  std::mutex mutex;
  std::exception_ptr error;
  std::vector<std::shared_ptr<InputFile>> files;

  void finish(std::exception_ptr failure) {
    if (failure) {
      std::lock_guard lock(mutex);
      if (!error) error = std::move(failure);
    }
    if (pending.fetch_sub(1, std::memory_order_acq_rel) != 1) return;
    auto self = shared_from_this();
    try {
      arena.enqueue([self] { self->runLogged(); });
    } catch (...) {
      runLogged();
    }
  }

  void runLogged() {
    auto held = std::move(files);
    try {
      then(failure());
    } catch (const std::exception& e) {
      LOG_ERROR("fetch continuation failed: {}", e.what());
    } catch (...) {
      LOG_ERROR("fetch continuation failed");
    }
  }

public:
  FetchCountdown(Private, oneapi::tbb::task_arena& arena, std::function<void(std::exception_ptr)> then)
      : arena(arena), then(std::move(then)) {}

  static std::shared_ptr<FetchCountdown> create(oneapi::tbb::task_arena& arena,
                                                std::function<void(std::exception_ptr)> then) {
    return std::make_shared<FetchCountdown>(Private{}, arena, std::move(then));
  }

  /// Starts one fetch. A fetchRanges that throws instead of calling done
  /// counts as that fetch failing.
  void fetch(std::shared_ptr<InputFile> file, std::span<const ByteRange> ranges, ReadDeadline deadline) {
    pending.fetch_add(1, std::memory_order_relaxed);
    files.push_back(file);
    auto once = std::make_shared<std::atomic<bool>>(false);
    auto self = shared_from_this();
    try {
      file->fetchRanges(ranges, deadline, [self, once](std::exception_ptr failure) {
        if (!once->exchange(true)) self->finish(std::move(failure));
      });
    } catch (...) {
      if (!once->exchange(true)) finish(std::current_exception());
    }
  }

  void fetch(const ReadSet& reads, ReadDeadline deadline) {
    for (const auto& read : reads) fetch(read.file, read.ranges, deadline);
  }

  /// Ends the fetches: true when all of them already finished, so the caller
  /// goes on itself and the continuation never runs.
  bool arm() {
    if (pending.fetch_sub(1, std::memory_order_acq_rel) != 1) return false;
    files.clear();
    return true;
  }

  /// The first error a fetch reported; after arm() returned true, or in the
  /// continuation.
  std::exception_ptr failure() {
    std::lock_guard lock(mutex);
    return error;
  }
};

}  // namespace luxir
