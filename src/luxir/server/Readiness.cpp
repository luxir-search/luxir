// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "Readiness.h"
#include <algorithm>
#include <format>
#include <set>
#include "LuxirNode.h"
#include "ReplicationFollower.h"
#include "luxir/search/IndexReader.h"
#include "luxir/search/ReaderManager.h"
#include "luxir/util/log.h"
#include "luxir/util/thread.h"

namespace luxir {
namespace {
uint64_t unixMillis() {
  return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
}
}

std::optional<double> Readiness::CollectionState::residentFraction() const {
  uint64_t known = fileBytes - unknownBytes;
  if (!known) return std::nullopt;
  return std::min(1.0, (double)residentBytes / (double)known);
}

Readiness::Residency Readiness::residency(std::span<InputFile* const> files) {
  Residency out;
  for (auto* file : files) {
    uint64_t size = file->size();
    out.fileBytes += size;
    if (auto resident = file->residentBytes(kSamplePages)) out.residentBytes += std::min(size, *resident);
    else out.unknownBytes += size;
  }
  return out;
}

std::vector<InputFile*> Readiness::files(IndexReader& reader) {
  std::vector<InputFile*> out;
  std::set<const void*> seen;
  auto add = [&](InputFile& file) { if (seen.insert(&file).second) out.push_back(&file); };
  auto addAux = [&](std::span<const std::shared_ptr<AuxReader>> readers) {
    for (const auto& aux : readers) if (aux) aux->visitFiles(add);
  };
  for (const auto& segment : reader.segments()) {
    if (seen.insert(&segment.segmentCore()).second) segment.segmentCore().visitFiles(add);
    addAux(segment.auxReaders());
  }
  addAux(reader.auxReaders());
  return out;
}

Readiness::Readiness(LuxirNode& node)
    : node(node), maxLagMs(node.getConfig().server.ready.max_follower_lag_ms),
      minResident(node.getConfig().server.ready.min_resident_fraction),
      interval(std::chrono::milliseconds(std::max<int64_t>(1, node.getConfig().server.ready.refresh_ms))) {}

Readiness::~Readiness() { stop(); }

void Readiness::start() {
  if (thread.joinable()) return;
  thread = std::jthread([this](std::stop_token stop) { run(stop); });
}

void Readiness::stop() {
  if (!thread.joinable()) return;
  thread.request_stop();
  wake.notify_all();
  thread.join();
}

void Readiness::run(std::stop_token stop) {
  nameThisThread("luxir_ready");
  while (!stop.stop_requested()) {
    try { refresh(); }
    catch (const std::exception& e) { LOG_WARN("Readiness evaluation failed: {}", e.what()); }
    while (!stop.stop_requested()) {
      std::optional<Job> job;
      {
        std::unique_lock lock(mutex);
        wake.wait_until(lock, stop, Clock::time_point(Clock::duration(due.load())), [&] { return !jobs.empty(); });
        if (jobs.empty()) break;  // an evaluation is due, or stopping
        job = std::move(jobs.front());
        jobs.pop_front();
      }
      runJob(*job, stop);
      if (Clock::now().time_since_epoch().count() >= due.load()) break;
    }
  }
}

bool Readiness::queue(Job job) {
  {
    std::lock_guard lock(mutex);
    auto& warmup = warmups[job.id];
    bool queued = std::ranges::any_of(jobs, [&](const Job& j) { return j.id == job.id; });
    if (warmup.running || queued) return false;
    warmup = {};
    warmup.budget = job.budget;
    warmup.automatic = job.automatic;
    jobs.push_back(std::move(job));
  }
  wake.notify_all();
  return true;
}

bool Readiness::warm(const CollectionId& id, uint64_t budgetBytes) {
  node.getCollection(id);  // throws if absent or unavailable
  return queue({id, budgetBytes, false});
}

void Readiness::runJob(const Job& job, std::stop_token stop) {
  auto update = [&](auto&& change) {
    std::lock_guard lock(mutex);
    change(warmups[job.id]);
  };
  update([&](Warmup& w) { w.running = true; w.started = unixMillis(); });
  std::string error;
  uint64_t loaded = 0;
  try {
    auto reader = node.getCollection(job.id)->getReaderManager().getReader();
    auto list = files(*reader);  // the reader keeps them open
    std::ranges::stable_sort(list, {}, [](InputFile* file) { return file->size(); });
    uint64_t total = 0;
    for (auto* file : list) total += file->size();
    update([&](Warmup& w) { w.budget = std::min(job.budget, total); });
    for (auto* file : list) {
      size_t size = file->size();
      for (size_t at = 0; at < size && loaded < job.budget && !stop.stop_requested();) {
        size_t length = (size_t)std::min<uint64_t>({(uint64_t)kLoadChunk, (uint64_t)(size - at), job.budget - loaded});
        file->load(at, length);
        at += length;
        loaded += length;
        update([&](Warmup& w) { w.loaded = loaded; });
        if (Clock::now().time_since_epoch().count() >= due.load()) refresh();
      }
    }
  } catch (const std::exception& e) {
    error = e.what();
    LOG_WARN("Warmup of collection '{}' failed: {}", job.id.label(), error);
  }
  update([&](Warmup& w) {
    w.running = false;
    w.finished = unixMillis();
    w.error = error;
  });
}

std::shared_ptr<const Readiness::Report> Readiness::refresh() {
  std::lock_guard serial(evaluation);
  due.store((Clock::now() + interval).time_since_epoch().count());
  auto report = std::make_shared<Report>();
  report->checked = unixMillis();
  auto* follower = node.getFollower();
  std::optional<ReplicationFollower::Progress> progress;
  if (follower) progress = follower->progress();
  std::map<CollectionId, const ReplicationFollower::Progress::Row*> rows;
  if (progress) for (const auto& row : progress->collections) rows.emplace(row.id, &row);
  auto now = node.now();
  std::set<CollectionId> listed;
  std::vector<Job> automatic;

  auto followerCheck = [&](CollectionState& state, Memory& mem, const ReplicationFollower::Progress::Row& row) {
    if (row.caughtUp) mem.caughtUp = true;
    uint64_t lag = 0;
    if (row.behindSince && now > *row.behindSince)
      lag = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(now - *row.behindSince).count();
    state.followerLagMs = lag;
    if (!mem.caughtUp) {
      state.ready = false;
      state.reason = row.installed ? "catching up with the source" : "not installed";
    } else if (maxLagMs > 0 && lag > (uint64_t)maxLagMs) {
      state.ready = false;
      state.reason = std::format("{} ms behind the source (server.ready.max-follower-lag-ms={})", lag, maxLagMs);
    }
  };

  for (const auto& entry : node.collectionEntries()) {
    auto reason = entry.collection->getUnavailableReason();
    if (!reason.empty() && Collections::expectedAbsence(reason)) continue;
    listed.insert(entry.id);
    auto& state = report->collections.emplace_back();
    state.id = entry.id;
    auto& mem = memory[entry.id];
    if (!reason.empty()) {
      state.ready = false;
      state.reason = "unavailable: " + reason;
      continue;
    }
    auto& readers = entry.collection->getReaderManager();
    std::shared_ptr<IndexReader> reader;
    try {
      // Gating on residency opens the reader: serving needs it anyway.
      reader = minResident > 0 ? readers.getReader() : readers.indexReader.load();
    } catch (const std::exception& e) {
      state.ready = false;
      state.reason = std::string("cannot open a reader: ") + e.what();
      continue;
    }
    if (reader) {
      auto list = files(*reader);
      auto counted = residency(list);
      state.fileBytes = counted.fileBytes;
      state.residentBytes = counted.residentBytes;
      state.unknownBytes = counted.unknownBytes;
    }
    ReaderManager::CacheStats cache;
    readers.cacheStats(cache);
    const auto& counters = cache.filterCacheCounters;
    if (counters.hits >= mem.hits && counters.misses >= mem.misses) {  // not reset since
      uint64_t hits = counters.hits - mem.hits, lookups = hits + counters.misses - mem.misses;
      if (lookups) state.filterCacheHitRatio = (double)hits / (double)lookups;
    }
    mem.hits = counters.hits;
    mem.misses = counters.misses;
    if (auto row = rows.find(entry.id); row != rows.end()) followerCheck(state, mem, *row->second);
    if (minResident > 0 && state.ready) {
      auto fraction = state.residentFraction();
      if (fraction && *fraction < minResident) {
        state.ready = false;
        state.reason = std::format("warming: {:.1f}% of {} file bytes resident (server.ready.min-resident-fraction={})",
                                   *fraction * 100, state.fileBytes, minResident);
        if (reader && (mem.warmedIncarnation != reader->incarnation() || mem.warmedCoreGen != reader->coreGen())) {
          mem.warmedIncarnation = std::string(reader->incarnation());
          mem.warmedCoreGen = reader->coreGen();
          automatic.push_back({entry.id, UINT64_MAX, true});
        }
      }
    }
  }
  // Advertised collections without a local copy yet.
  for (const auto& [id, row] : rows) {
    if (listed.contains(id)) continue;
    listed.insert(id);
    auto& state = report->collections.emplace_back();
    state.id = id;
    followerCheck(state, memory[id], *row);
  }
  std::erase_if(memory, [&](const auto& item) { return !listed.contains(item.first); });
  std::ranges::sort(report->collections, {}, &CollectionState::id);

  for (auto& job : automatic) queue(std::move(job));
  {
    std::lock_guard lock(mutex);
    std::erase_if(warmups, [&](const auto& item) {
      return !listed.contains(item.first) && !item.second.running &&
             std::ranges::none_of(jobs, [&](const Job& j) { return j.id == item.first; });
    });
    for (auto& state : report->collections) {
      if (auto it = warmups.find(state.id); it != warmups.end()) state.warmup = it->second;
    }
  }

  report->ready = true;
  if (progress && !progress->discovered) {
    report->ready = false;
    report->reason = "follower has not read its source's catalog";
  }
  for (const auto& state : report->collections) {
    if (state.ready) continue;
    if (report->ready) report->reason = state.id.label() + ": " + state.reason;
    report->ready = false;
  }
  latest.store(report);
  if (announced != report->ready) {
    announced = report->ready;
    if (report->ready) LOG_INFO("Node is ready");
    else LOG_INFO("Node is not ready: {}", report->reason);
    for (const auto& [id, listener] : listeners) listener(report->ready);
  }
  return report;
}

uint64_t Readiness::subscribe(std::function<void(bool)> listener) {
  std::lock_guard serial(evaluation);
  if (announced) listener(*announced);
  listeners.emplace(++nextListener, std::move(listener));
  return nextListener;
}

void Readiness::unsubscribe(uint64_t id) {
  std::lock_guard serial(evaluation);
  listeners.erase(id);
}

}
