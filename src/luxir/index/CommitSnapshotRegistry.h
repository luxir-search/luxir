// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>
#include <boost/unordered/unordered_flat_set.hpp>
#include <functional>
#include <map>
#include <stop_token>
#include <unordered_map>
#include "luxir/search/ReaderManager.h"

namespace luxir {

struct Manifest;

class SnapshotExpiredError : public ApiError {
public:
  SnapshotExpiredError() : ApiError(ErrorKind::NOT_FOUND, "snapshot_expired", "snapshot reservation expired") {}
};

// Per-collection snapshot owner. Publishers serialize publish(), then retire
// names no longer owned by the current snapshot or their mutable work.
class CommitSnapshotRegistry {
public:
  using Clock = std::chrono::steady_clock;
  using Now = std::function<Clock::time_point()>;
  struct Policy {
    std::chrono::milliseconds idleTimeout{60'000};
    uint64_t retainedBytes = 1024ULL * 1024 * 1024;
  };
  struct Stats {
    uint64_t pins = 0;
    uint64_t retainedBytes = 0;
    uint64_t idleDrops = 0;
    uint64_t budgetDrops = 0;
  };
private:
  struct Reservation {
    std::shared_ptr<const CommitSnapshot> snapshot;
    boost::unordered_flat_set<std::string_view> fileNames;
    Clock::time_point created;
    Clock::time_point lastRead;
    std::stop_source cancellation;
  };
  struct FileRef {
    uint64_t size;
    uint64_t pins = 0;
    bool retired = false;
  };
  std::atomic<std::shared_ptr<const CommitSnapshot>> current;
  std::mutex mutex;
  std::mutex retirementMutex; // joins directory access before close returns
  Policy policy;
  Stats counters;
  Now now;
  bool closed = false;
  std::map<CommitId, Reservation> reservations;
  std::unordered_map<std::string, FileRef> files;
public:
  Directory& dir;
  ReaderManager readers;
  // Set once by the collection before admitting requests.
  std::function<void(const CommitSnapshot&)> onPublish;
private:
  void releaseLocked(const CommitId& id, std::vector<std::string>& retired);
  void expireLocked(std::vector<std::string>& retired);
  void enforceBudgetLocked(std::vector<std::string>& retired);
  void unlink(std::span<const std::string> names) noexcept;
  void unlinkFiles(std::span<const std::string> names) noexcept;
  void retireUnreferenced(std::span<const Directory::FileInfo> listing);
public:
  explicit CommitSnapshotRegistry(Directory& dir, FilterCacheConfig config = {}, Now now = Clock::now)
      : now(std::move(now)), dir(dir), readers(dir, current, config) {}
  ~CommitSnapshotRegistry() { close(); }
  void close() noexcept;
  // Stop reservations and join retirement while admitted searches retain readers.
  void detach() noexcept;
  void publish(std::shared_ptr<const CommitSnapshot> snapshot, std::shared_ptr<IndexReader> opened = {});
  std::shared_ptr<const CommitSnapshot> snapshot() const { return current.load(); }
  void openLocalSnapshot();
  void sweepOrphans(const Manifest& manifest);
  void sweepOrphans();
  static std::vector<std::string> obsoleteFiles(const CommitSnapshot& previous,
      const CommitSnapshot& next, boost::unordered_flat_set<std::string> retained = {});
  // Reservations outlive requests and are shared by all clients of a commit.
  // Stop callbacks run under the reservation mutex: only schedule cancellation;
  // never re-enter the registry or perform I/O from a callback.
  std::shared_ptr<const CommitSnapshot> acquire(std::stop_token* cancellation = nullptr);
  std::shared_ptr<InputFile> openFile(const CommitId& id, std::string_view name, std::stop_token* cancellation = nullptr);
  bool touch(const CommitId& id, uint64_t bytes);
  bool evictOldest();
  void setPolicy(Policy value);
  Stats stats();
  void expire();
  void retire(std::span<const std::string> names);
  void retirePrefix(std::string_view prefix);
  // Quiescent tests only; retains the owner's identity and joins all cleanup.
  void testReset();
};

}
