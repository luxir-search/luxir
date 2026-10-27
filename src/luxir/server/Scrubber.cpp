// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "Scrubber.h"
#include "Collections.h"
#include "LuxirNode.h"
#include "ReplicationFollower.h"
#include "luxir/store/FileVerifier.h"
#include "luxir/util/thread.h"
#include <algorithm>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace luxir {
namespace {
struct Stopped {};
}

Scrubber::Scrubber(LuxirNode& node)
    : node(node), interval(std::chrono::seconds(node.getConfig().store.scrub_interval_s)),
      bytesPerSecond(node.getConfig().store.scrub_max_bytes_per_sec) {}

Scrubber::~Scrubber() { stop(); }

void Scrubber::start() {
  if (!reporter.joinable()) reporter = std::jthread([this](std::stop_token stop) { runReports(stop); });
  if (!enabled() || thread.joinable()) return;
  thread = std::jthread([this](std::stop_token stop) { run(stop); });
}

void Scrubber::stop() {
  if (reporter.joinable()) {
    reporter.request_stop();
    reportsWake.notify_all();
    reporter.join();
  }
  if (!thread.joinable()) return;
  thread.request_stop();
  wake.notify_all();
  thread.join();
}

void Scrubber::report(const CollectionId& id, std::weak_ptr<Collection> collection, std::vector<std::string> files,
                      std::string reason) {
  {
    std::lock_guard lock(reportsMutex);
    reports.push_back({id, std::move(collection), std::move(files), std::move(reason)});
  }
  reportsWake.notify_all();
}

void Scrubber::runReports(std::stop_token stop) {
  nameThisThread("luxir_damage");
  for (;;) {
    Report next;
    {
      std::unique_lock lock(reportsMutex);
      if (!reportsWake.wait(lock, stop, [&] { return !reports.empty(); })) return;
      next = std::move(reports.front());
      reports.pop_front();
    }
    try { handle(next); }
    catch (const std::exception& e) { LOG_WARN("Could not report damage in collection '{}': {}", next.id.label(), e.what()); }
  }
}

// Fails the collection as a pass would, if it still serves the damaged files.
void Scrubber::handle(const Report& report) {
  auto& collections = node.collections();
  auto collection = report.collection.lock();
  if (!collection || collection->isFailed() || collections.get(report.id) != collection) return;
  auto shard = collection->getShard();
  auto snapshot = shard ? shard->getSnapshots().snapshot() : nullptr;
  if (!snapshot) return;
  std::vector<FileDescriptor> served;
  std::vector<std::string> suspects;
  for (const auto& file : snapshot->files) {
    if (std::ranges::find(report.files, file.name) == report.files.end()) continue;
    served.push_back(file);
    suspects.push_back(file.name);
  }
  if (served.empty()) return;  // a newer snapshot no longer serves them
  collections.fail(report.id, collection, report.reason, suspects);
  LOG_ERROR("Collection '{}' (commit {}) is corrupt and no longer serves: {}",
            report.id.label(), snapshot->id.token(), report.reason);
  if (auto* follower = node.getFollower()) follower->repair(report.id, collection, report.reason, served);
}

void Scrubber::run(std::stop_token stop) {
  nameThisThread("luxir_scrub");
  // CPU priority only: the byte rate is what keeps a pass off the disks.
  // macOS has no per-thread nice, and its QoS classes also lower I/O priority.
#if defined(__linux__)
  (void)setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);
#endif
  while (!stop.stop_requested()) {
    {
      std::unique_lock lock(wakeMutex);
      wake.wait_for(lock, stop, interval, [] { return false; });
    }
    if (stop.stop_requested()) break;
    try {
      std::lock_guard lock(mutex);
      pass(stop, true);
    } catch (const std::exception& e) { LOG_WARN("Checksum scrub failed: {}", e.what()); }
  }
}

size_t Scrubber::scrub(std::stop_token stop) {
  // Announce first: a background pass yields at its next chunk. The lock
  // orders the announcement before the pass's predicate check, so a notify
  // cannot slip between that check and its wait.
  waiting.fetch_add(1, std::memory_order_relaxed);
  { std::lock_guard lock(wakeMutex); }
  wake.notify_all();
  std::lock_guard lock(mutex);
  waiting.fetch_sub(1, std::memory_order_relaxed);
  return pass(stop, false);
}

Scrubber::Clock::time_point Scrubber::nextDeadline(Clock::time_point due, Clock::time_point now,
                                                   size_t length, uint64_t bytesPerSecond) {
  if (!bytesPerSecond) return now;
  auto duration = std::chrono::duration_cast<Clock::duration>(
      std::chrono::duration<double>((double)length / (double)bytesPerSecond));
  return std::max(due, now) + duration;
}

// Caller holds `mutex`.
size_t Scrubber::pass(std::stop_token stop, bool preemptible) {
  auto due = Clock::now();
  auto preempted = [&] { return preemptible && waiting.load(std::memory_order_relaxed) > 0; };
  // Pace each chunk without accumulating credit; throws Stopped to end it.
  auto pace = [&](size_t length) {
    bytes.fetch_add(length, std::memory_order_relaxed);
    if (stop.stop_requested() || preempted()) throw Stopped();
    if (!bytesPerSecond) return;
    due = nextDeadline(due, Clock::now(), length, bytesPerSecond);
    std::unique_lock lock(wakeMutex);
    wake.wait_until(lock, stop, due, [&] { return preempted(); });
    if (stop.stop_requested() || preempted()) throw Stopped();
  };
  auto& collections = node.collections();
  size_t corrupted = 0;
  try {
    for (const auto& entry : collections.entries()) {
      if (!entry.error.empty()) continue; // unavailable, already failed, or being deleted
      auto shard = entry.collection->getShard();
      if (!shard) continue;
      auto& registry = shard->getSnapshots();
      auto snapshot = registry.snapshot();
      if (!snapshot) continue;
      for (const auto& file : snapshot->files) {
        std::string problem;
        try {
          auto reader = registry.openChecksum(file, true);
          if (!reader) {
            if (node.readOnly()) continue; // the live writer retired this name
            problem = "missing file: " + file.name;
          } else {
            problem = checksumMismatch(file, reader->checksum(pace));
          }
        } catch (const FileEIOError& e) {
          problem = "unreadable file: " + file.name + ": " + e.what();
        } catch (const std::exception& e) {
          // A newer publication retired it, or the collection closed.
          auto* api = dynamic_cast<const ApiError*>(&e);
          if (api && api->code == "file_not_held") continue;
          errors.fetch_add(1, std::memory_order_relaxed);
          LOG_WARN("Could not scrub file '{}' of collection '{}': {}", file.name, entry.id.label(), e.what());
          continue;
        }
        files.fetch_add(1, std::memory_order_relaxed);
        if (problem.empty()) continue;
        mismatches.fetch_add(1, std::memory_order_relaxed);
        corrupted++;
        std::array<std::string, 1> suspects{file.name};
        collections.fail(entry.id, entry.collection, problem, suspects);
        LOG_ERROR("Collection '{}' (commit {}) is corrupt and no longer serves: {}",
                  entry.id.label(), snapshot->id.token(), problem);
        if (auto* follower = node.getFollower()) follower->repair(entry.id, entry.collection, problem, std::span(&file, 1));
        break;
      }
    }
  } catch (const Stopped&) { return corrupted; }
  passes.fetch_add(1, std::memory_order_relaxed);
  lastPassEnd.store((uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count(), std::memory_order_relaxed);
  return corrupted;
}

Scrubber::Stats Scrubber::stats() const {
  return {passes.load(std::memory_order_relaxed), files.load(std::memory_order_relaxed),
          bytes.load(std::memory_order_relaxed), mismatches.load(std::memory_order_relaxed), errors.load(std::memory_order_relaxed),
          lastPassEnd.load(std::memory_order_relaxed)};
}

}
