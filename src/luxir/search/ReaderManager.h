// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <atomic>
#include <mutex>
#include "IndexReader.h"
#include "FilterCache.h"
#include "luxir/index/CommitSnapshot.h"

namespace luxir {

// A collection's search view, independent of who installs its snapshots.
// A writer or follower serializes install() after making a snapshot durable.
// Recreating a collection uses a new manager and cache for its new incarnation.
class ReaderManager {
  Directory& dir;
  std::mutex indexReaderMutex;
  std::atomic<std::shared_ptr<const CommitSnapshot>>& published;
  // These read-only query hints must not share the snapshot or reader lock bit.
  alignas(64) std::atomic<bool> closed = false;
  std::atomic<uint64_t> publishedSchemaGen = 0;
  std::atomic<uint64_t> lastAdvertisedCommitTime = 0;
  FilterCacheConfig originalFilterCacheConfig;
public:
  std::shared_ptr<FilterCache> filterCache;
  alignas(64) std::atomic<std::shared_ptr<IndexReader>> indexReader;

  struct AuxStats {
    std::string kind;
    std::string field;
    std::string name;
    uint64_t gen = 0;
    uint64_t commitTime = 0;
    uint64_t builtCoreGen = 0;
    uint64_t bytes = 0;
    std::vector<std::string> files;
  };

  struct SegmentStats {
    uint64_t segId = 0;
    uint64_t liveGen = 0;
    uint64_t minUpdateVersion = 0;
    uint64_t maxUpdateVersion = 0;
    uint64_t firstCommitTime = 0;
    uint64_t schemaGen = 0;
    uint64_t bytes = 0;
    std::vector<AuxStats> overlays;
    int32_t maxDoc = 0;
    int32_t liveDocs = 0;
    bool committed = false;
  };

  struct CacheStats {
    FilterCache::CounterValues filterCacheCounters;
    uint64_t filterCacheMaxBytes = 0;
    uint64_t filterCacheResidentBytes = 0;
    uint64_t filterCacheMetadataBytes = 0;
    bool filterCacheEnabled = false;
  };

  template<class Segment>
  struct SnapshotStats : CacheStats {
    uint64_t commitTime = 0;
    uint64_t indexGen = 0;
    uint64_t coreGen = 0;
    uint64_t updateVersion = 0;
    uint64_t schemaGen = 0;
    uint64_t segments = 0;
    uint64_t committedSegments = 0;
    uint64_t maxDocs = 0;
    uint64_t liveDocs = 0;
    uint64_t totalBytes = 0;
    std::vector<AuxStats> auxIndexes;
    std::vector<Segment> segmentStats;
  };

  using Stats = SnapshotStats<SegmentStats>;

private:
  [[noreturn]] static void throwClosed();
  void checkOpen() const {
    if (closed.load(std::memory_order_acquire)) throwClosed();
  }
  void publishReader(std::shared_ptr<IndexReader> reader);

public:
  Stats stats(bool includeSegments);
  void cacheStats(CacheStats& out) const;
  explicit ReaderManager(Directory& dir, std::atomic<std::shared_ptr<const CommitSnapshot>>& published,
                         FilterCacheConfig config = {});
  std::shared_ptr<IndexReader> getReader(uint64_t freshness_us = 0);
  std::shared_ptr<const CommitSnapshot> snapshot() const { return published.load(); }
  std::shared_ptr<Schema> getSchema() const { return snapshot()->schema; }
  // Local writers install durable metadata and reopen lazily on demand.
  void install(std::shared_ptr<const CommitSnapshot> snapshot) noexcept;
  // Installers open first, persist the local root, then install the opened view.
  std::shared_ptr<IndexReader> prepare(const CommitSnapshot& snapshot);
  void installOpened(std::shared_ptr<const CommitSnapshot> snapshot, std::shared_ptr<IndexReader> reader);
  std::string resolvedSchema();
  void close() noexcept { closed.store(true, std::memory_order_release); }
  // Quiescent tests only: clear the physical namespace and cache together.
  void testReset();
};

}
