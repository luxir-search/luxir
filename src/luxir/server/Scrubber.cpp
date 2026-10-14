// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "Scrubber.h"
#include "Collections.h"
#include "luxir/store/FileVerifier.h"
#include "luxir/util/thread.h"
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace luxir {
namespace {
struct Stopped {};
}

Scrubber::Scrubber(Collections& collections, const StoreConfig& config)
    : collections(collections), interval(std::chrono::seconds(config.scrub_interval_s)),
      bytesPerSecond(config.scrub_max_bytes_per_sec) {}

Scrubber::~Scrubber() { stop(); }

void Scrubber::start() {
  if (!enabled() || thread.joinable()) return;
  thread = std::jthread([this](std::stop_token stop) { run(stop); });
}

void Scrubber::stop() {
  if (!thread.joinable()) return;
  thread.request_stop();
  wake.notify_all();
  thread.join();
}

void Scrubber::run(std::stop_token stop) {
  nameThisThread("luxir_scrub");
  // Best effort: yield the CPU to request and indexing threads.
  (void)setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);
  while (!stop.stop_requested()) {
    {
      std::unique_lock lock(wakeMutex);
      wake.wait_for(lock, stop, interval, [] { return false; });
    }
    if (stop.stop_requested()) break;
    try { scrub(stop); }
    catch (const std::exception& e) { LOG_WARN("Checksum scrub failed: {}", e.what()); }
  }
}

size_t Scrubber::scrub(std::stop_token stop) {
  std::lock_guard pass(mutex);
  using Clock = std::chrono::steady_clock;
  auto start = Clock::now();
  uint64_t read = 0;
  // Sleeps while the pass is ahead of the byte rate; throws Stopped to end it.
  auto pace = [&](size_t length) {
    read += length;
    bytes.fetch_add(length, std::memory_order_relaxed);
    if (stop.stop_requested()) throw Stopped();
    if (!bytesPerSecond) return;
    auto due = start + std::chrono::microseconds((uint64_t)((double)read * 1e6 / (double)bytesPerSecond));
    if (due <= Clock::now()) return;
    std::unique_lock lock(wakeMutex);
    wake.wait_until(lock, stop, due, [] { return false; });
    if (stop.stop_requested()) throw Stopped();
  };
  size_t corrupted = 0;
  try {
    for (const auto& entry : collections.entries()) {
      if (!entry.error.empty()) continue; // unavailable, already corrupt, or being deleted
      auto shard = entry.collection->getShard();
      if (!shard) continue;
      auto& registry = shard->getSnapshots();
      auto snapshot = registry.snapshot();
      if (!snapshot) continue;
      for (const auto& file : snapshot->files) {
        std::shared_ptr<InputFile> input;
        std::string problem;
        bool held = true;
        try { input = registry.openFile(file); }
        catch (const ApiError& e) {
          // Not held: retired by a newer publication, or the collection closed.
          held = e.kind != ErrorKind::NOT_FOUND;
          if (held) problem = "checksum mismatch: " + file.name + ": unreadable: " + e.what();
        }
        catch (const std::exception& e) { problem = "checksum mismatch: " + file.name + ": unreadable: " + e.what(); }
        if (!held) continue;
        if (input) problem = checksumMismatch(file, hashFile(*input, pace));
        files.fetch_add(1, std::memory_order_relaxed);
        if (problem.empty()) continue;
        mismatches.fetch_add(1, std::memory_order_relaxed);
        corrupted++;
        LOG_ERROR("Collection '{}' (commit {}) is corrupt and no longer serves: {}",
                  entry.id.label(), snapshot->id.token(), problem);
        entry.collection->markCorrupt(problem);
        // Republishes the collection's availability, e.g. to the replication catalog.
        if (registry.onChange) registry.onChange();
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
          bytes.load(std::memory_order_relaxed), mismatches.load(std::memory_order_relaxed),
          lastPassEnd.load(std::memory_order_relaxed)};
}

}
