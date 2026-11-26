// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once
#include "luxir/util/Metrics.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include "IndexReader.h"
#include "FilterCache.h"
#include "luxir/index/CommitSnapshot.h"

namespace luxir {

// A collection's search view, independent of who publishes its snapshots.
// A writer or follower serializes publish() after making a snapshot durable.
// Recreating a collection uses a new manager and cache for its new incarnation.
class ReaderManager {
  Directory& dir;
  // Guards publication of indexReader and `opening`. A reader open reads
  // storage and may take a store round trip or more, so it never holds this.
  std::mutex indexReaderMutex;
  std::condition_variable openDone;  // signalled when a getReader reopen ends
  bool opening = false;              // a getReader reopen is in progress
  uint64_t publications = 0;         // readers published, under indexReaderMutex
  std::atomic<std::shared_ptr<const CommitSnapshot>> published;
  // These read-only query hints must not share the snapshot or reader lock bit.
  alignas(64) std::atomic<bool> closed = false;
  std::atomic<uint64_t> publishedSchemaGen = 0;
  std::atomic<uint64_t> lastAdvertisedCommitTime = 0;
  // Steady-clock microseconds of the last search, stored at most once per
  // SEARCH_MARK_US so the search path rarely writes this line. Starts at
  // construction: a new manager counts as searched.
  alignas(64) std::atomic<uint64_t> lastSearchUs;
  FilterCacheConfig originalFilterCacheConfig;
  static constexpr uint64_t SEARCH_MARK_US = 1'000'000;
  static uint64_t steadyUs() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
  }
public:
  // The node's service counters; set by the collection that owns this manager.
  std::shared_ptr<Metrics> metrics = Metrics::detached();
  // A publisher opens the reader of a new snapshot only when a search ran
  // this recently; otherwise the first search after it reopens. Tests lower it.
  uint64_t idleAfterUs = 60'000'000;
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

  struct RowSpaceStats {
    std::string path;
    int32_t maxDoc = 0;
    uint64_t endsBytes = 0;
    uint64_t ownersBytes = 0;
  };

  struct SegmentStats {
    uint64_t segId = 0;
    uint64_t creatorEpoch = 0;
    uint64_t liveGen = 0;
    uint64_t minUpdateVersion = 0;
    uint64_t maxUpdateVersion = 0;
    uint64_t firstCommitTime = 0;
    uint64_t schemaGen = 0;
    uint64_t bytes = 0;
    std::vector<AuxStats> overlays;
    std::vector<RowSpaceStats> spaces;
    int32_t maxDoc = 0;
    int32_t liveDocs = 0;
    bool committed = false;

    void readSpaces(const SegmentCore& core);
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
    if (isClosed()) throwClosed();
  }
  void publishReader(std::shared_ptr<IndexReader> reader);

public:
  Stats stats(bool includeSegments, bool includeFiles = true);
  void cacheStats(CacheStats& out) const;
  explicit ReaderManager(Directory& dir, FilterCacheConfig config = {});
  // Searches take the published reader without a lock when it satisfies their
  // freshness and schema. Otherwise one thread reopens, outside
  // indexReaderMutex, and the others that need the new snapshot wait for it
  // (READER_WAIT), until it or a publish() provides it, the manager closes, or
  // `deadline` passes (callers pass the request's IoContext::waitDeadline(), a
  // margin before the transport deadline; none for a min_commit request, which
  // needs the new view). At the deadline a waiter searches the published
  // reader, counted as a cancelled READER_WAIT, but fails UNAVAILABLE
  // reader_opening when none is published or the published one predates a
  // schema change. A caller that is itself the opener waits for the open it
  // performs: the open does not see any one request's deadline, since every
  // waiter shares its result. A caller already past its deadline with a
  // reader and no open in progress takes the reader without opening.
  std::shared_ptr<IndexReader> getReader(uint64_t freshness_us = 0,
                                         std::chrono::steady_clock::time_point deadline
                                         = std::chrono::steady_clock::time_point::max());
  std::shared_ptr<const CommitSnapshot> snapshot() const { return published.load(); }
  std::shared_ptr<Schema> getSchema() const { return snapshot()->schema; }
  // Whether a search ran within idleAfterUs, so a writer publication opens
  // its reader instead of leaving it to the next search.
  bool searchedRecently() const {
    auto now = steadyUs();
    auto last = lastSearchUs.load(std::memory_order_relaxed);
    return last > now || now - last < idleAfterUs;  // another thread may be ahead
  }
  // Opens a reader for a snapshot before it is published, on the publisher's
  // thread (writer commits and schema updates, follower installs). Takes no
  // lock: it only reuses the current reader's segments, and publish() swaps.
  std::shared_ptr<IndexReader> prepare(const CommitSnapshot& snapshot) { return prepare(snapshot, dir); }
  std::shared_ptr<IndexReader> prepare(const CommitSnapshot& snapshot, Directory& source);
  // Publishes a durable snapshot. With a reader from prepare() searches switch
  // to it before the snapshot is advertised, so none of them reopens; without
  // one (an idle collection, a writer reopening an existing index, a failed
  // prepare), the first search that needs it reopens.
  void publish(std::shared_ptr<const CommitSnapshot> snapshot, std::shared_ptr<IndexReader> opened = {});
  std::string resolvedSchema();
  void close() noexcept {
    {
      std::lock_guard lock(indexReaderMutex);  // a waiter then sees it before it sleeps
      closed.store(true, std::memory_order_release);
    }
    openDone.notify_all();
  }
  bool isClosed() const { return closed.load(std::memory_order_acquire); }
  // Tests only: the next search opens its reader with this cache.
  void testSetFilterCache(std::shared_ptr<FilterCache> cache);
  // Quiescent tests only: clear the physical namespace and cache together.
  void testReset();
};

}
