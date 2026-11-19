// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>
#include "luxir/store/CollectionId.h"

namespace luxir {

class InputFile;
class IndexReader;
class LuxirNode;

// Whether a load balancer should send this node traffic. Liveness is
// GET /health; readiness is GET /_ready and the gRPC health service
// kService. A node is ready when every collection it should serve is
// available (a collection being deleted or served by another node does not
// count), a follower has read its source's catalog and each advertised
// collection has caught up with the source once and trails it by at most
// server.ready.max-follower-lag-ms, and, when
// server.ready.min-resident-fraction is set, each collection's current files
// are at least that resident in the page cache (or its storage's own cache,
// through InputFile::residentBytes).
//
// One background thread evaluates every server.ready.refresh-ms, off the task
// arenas, and publishes the result; the transports serve the latest without
// waiting. The same thread runs warmups: queued by POST _warm, or
// automatically, once per reader, for a collection below the resident
// fraction. A warmup loads files smallest first (InputFile::load) in chunks,
// re-evaluating between chunks when an evaluation is due.
class Readiness {
public:
  static constexpr std::string_view kService = "luxir.ready";
  // Pages mincore checks per file at most; larger files are sampled.
  static constexpr size_t kSamplePages = 1024;
  static constexpr size_t kLoadChunk = 4 << 20;

  struct Warmup {
    uint64_t budget = 0;
    uint64_t loaded = 0;
    bool running = false;
    bool automatic = false;
    uint64_t started = 0;   // Unix milliseconds
    uint64_t finished = 0;  // Unix milliseconds; zero while queued or running
    std::string error;
  };
  struct CollectionState {
    CollectionId id;
    bool ready = true;
    std::string reason;
    std::optional<uint64_t> followerLagMs;
    uint64_t fileBytes = 0;
    uint64_t residentBytes = 0;
    uint64_t unknownBytes = 0;
    std::optional<double> filterCacheHitRatio;
    std::optional<Warmup> warmup;
    std::optional<double> residentFraction() const;
  };
  struct Report {
    bool ready = false;
    std::string reason;
    uint64_t checked = 0;  // Unix milliseconds
    std::vector<CollectionState> collections;
  };
  struct Residency {
    uint64_t fileBytes = 0;
    uint64_t residentBytes = 0;
    uint64_t unknownBytes = 0;
  };
  static Residency residency(std::span<InputFile* const> files);
  // The distinct files `reader` holds open: segment files and overlays.
  static std::vector<InputFile*> files(IndexReader& reader);

private:
  struct Job {
    CollectionId id;
    uint64_t budget;
    bool automatic;
  };
  // Per collection name, across evaluations; guarded by `evaluation`.
  struct Memory {
    uint64_t hits = 0, misses = 0;  // filter-cache counters at the last evaluation
    bool caughtUp = false;          // a follower copy matched its source since start
    std::string warmedIncarnation;  // the reader an automatic warmup last covered
    uint64_t warmedCoreGen = 0;
  };
  using Clock = std::chrono::steady_clock;

  LuxirNode& node;
  const int64_t maxLagMs;
  const double minResident;
  const Clock::duration interval;
  std::mutex evaluation;  // serializes evaluations and listener calls
  std::map<CollectionId, Memory> memory;
  std::optional<bool> announced;
  std::map<uint64_t, std::function<void(bool)>> listeners;
  uint64_t nextListener = 0;
  std::mutex mutex;  // jobs and warmups; never held while reading files
  std::condition_variable_any wake;
  std::deque<Job> jobs;
  std::map<CollectionId, Warmup> warmups;
  std::atomic<Clock::rep> due{0};  // when the next evaluation is due
  std::atomic<std::shared_ptr<const Report>> latest;
  std::jthread thread;

  void run(std::stop_token stop);
  void runJob(const Job& job, std::stop_token stop);
  bool queue(Job job);  // false when a warmup of the collection is already queued or running

public:
  explicit Readiness(LuxirNode& node);
  ~Readiness();
  void start();
  void stop();
  // The latest evaluation; null before the first.
  std::shared_ptr<const Report> report() const { return latest.load(); }
  // Evaluates now on the calling thread and publishes the result.
  std::shared_ptr<const Report> refresh();
  // Queues a warmup of the collection's current files, up to `budgetBytes`.
  // Throws if the collection does not exist or is unavailable. False when a
  // warmup of it is already queued or running.
  bool warm(const CollectionId& id, uint64_t budgetBytes);
  // `listener` hears the ready flag now (once evaluated) and on every change,
  // on the evaluating thread.
  uint64_t subscribe(std::function<void(bool)> listener);
  void unsubscribe(uint64_t id);
};

}
