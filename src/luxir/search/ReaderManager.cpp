// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "ReaderManager.h"
#include "luxir/reader/OwnershipShape.h"
#include "luxir/store/Manifest.h"
#include "luxir/util/ApiError.h"
#include "luxir/util/Signal.h"

namespace luxir {

ReaderManager::ReaderManager(Directory& dir, FilterCacheConfig config)
    : dir(dir), lastSearchUs(steadyUs()), originalFilterCacheConfig(config),
      filterCache(std::make_shared<FilterCache>(config)) {}

void ReaderManager::throwClosed() {
  throw ApiError(ErrorKind::UNAVAILABLE, "reader_closed", "reader manager is closed");
}

std::shared_ptr<IndexReader> ReaderManager::prepare(const CommitSnapshot& snapshot, Directory& source,
                                                   bool materialize) {
  MetricWork metric(*metrics, MetricStage::READER_REFRESH);
  checkOpen();
  // No lock: searches and a search-side reopen proceed while this opens. The
  // previous reader only lends its segments; publish() swaps under the lock.
  auto previous = indexReader.load(std::memory_order_acquire);
  Signal::emit("indexReaderPreparing", this);
  return std::make_shared<IndexReader>(source, snapshot, previous.get(), filterCache, metrics, materialize,
                                       onUnreadable);
}

void ReaderManager::publishReader(std::shared_ptr<IndexReader> reader) {
  auto previous = indexReader.load(std::memory_order_relaxed);
  if (!previous || reader->snapshot->core != previous->snapshot->core) filterCache->onReaderPublished(*reader);
  indexReader.store(std::move(reader), std::memory_order_release);
}

void ReaderManager::publish(std::shared_ptr<const CommitSnapshot> snapshot, std::shared_ptr<IndexReader> opened) {
  std::unique_lock lock(indexReaderMutex, std::defer_lock);
  if (opened) {
    // A durable snapshot still publishes after close(); a closed manager
    // serves no searches, so its reader is dropped rather than thrown on.
    lock.lock();
    assert(opened->commitId() == snapshot->id.index_gen && opened->schema() == snapshot->schema);
    if (!isClosed()) publishReader(std::move(opened));
  }
  // Hints precede the owning state: a fast-path miss may reopen early, but
  // satisfied searches acquire only the reader and plain scalar atomics.
  lastAdvertisedCommitTime.store(snapshot->commitTime, std::memory_order_relaxed);
  publishedSchemaGen.store(snapshot->schema->gen_, std::memory_order_relaxed);
  published.store(std::move(snapshot), std::memory_order_release);
}

void ReaderManager::testSetFilterCache(std::shared_ptr<FilterCache> cache) {
  std::lock_guard lock(indexReaderMutex);
  filterCache = std::move(cache);
  indexReader.store(nullptr, std::memory_order_release);
}

void ReaderManager::testReset() {
  std::lock_guard lock(indexReaderMutex);
  closed.store(false);
  published.store(nullptr);
  indexReader.store(nullptr, std::memory_order_release);
  filterCache = std::make_shared<FilterCache>(originalFilterCacheConfig);
}

std::shared_ptr<IndexReader> ReaderManager::getReader(uint64_t freshness_us) {
  checkOpen();
  // A newer commit exists and the caller wants it (freshness_us == 0: always
  // the newest; otherwise only once the reader's commit is older than that).
  auto stale = [&](const IndexReader& reader) {
    if (lastAdvertisedCommitTime.load(std::memory_order_relaxed) <= reader.commitTime()) return false;
    if (freshness_us == 0) return true;
    auto now = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return now - reader.commitTime() > freshness_us;
  };
  auto schemaChanged = [&](const IndexReader& reader) {
    return publishedSchemaGen.load(std::memory_order_relaxed) > reader.schema()->gen_;
  };

  // Mark the collection searched, writing the shared line at most once per
  // SEARCH_MARK_US.
  auto nowSteady = steadyUs();
  if (auto last = lastSearchUs.load(std::memory_order_relaxed); nowSteady >= last + SEARCH_MARK_US)
    lastSearchUs.store(nowSteady, std::memory_order_relaxed);

  // Fast path, no lock. Publication opens the reader before it advertises the
  // snapshot, so this normally holds right after every commit.
  auto reader = indexReader.load(std::memory_order_acquire);
  if (reader && !stale(*reader) && !schemaChanged(*reader)) return reader;

  // The publication-time open was skipped (an idle collection, a writer
  // reopening an existing index) or failed. One thread reopens; the others
  // need the new snapshot (freshness or schema), so they wait, and are counted.
  std::unique_lock readerLock(indexReaderMutex, std::try_to_lock);
  if (!readerLock.owns_lock()) {
    MetricWork wait(*metrics, MetricStage::READER_WAIT);
    readerLock.lock();
  }
  reader = indexReader.load(std::memory_order_acquire);
  if (!reader || stale(*reader) || schemaChanged(*reader)) {
    MetricWork metric(*metrics, MetricStage::READER_REFRESH);
    auto openPublished = [&](IndexReader* previous) {
      for (;;) {
        checkOpen();
        auto snapshot = published.load();
        if (!snapshot) throw ApiError(ErrorKind::UNAVAILABLE, "snapshot_unavailable", "no snapshot installed");
        try {
          Signal::emit("indexReaderOpening", this);
          // Every file is secured by handle here, inside the retry: a file a
          // newer commit retired first fails this open, never a later lazy one.
          return std::make_shared<IndexReader>(dir, *snapshot, previous, filterCache, metrics, false, onUnreadable);
        } catch (...) {
          if (snapshot == published.load()) throw;
        }
      }
    };
    auto opened = openPublished(reader.get());
    Signal::emit("indexReaderOpened", this);
    // Hints can run ahead of published; the reopen path follows owning state.
    while (published.load()->schema->gen_ > opened->schema()->gen_) opened = openPublished(opened.get());
    reader = std::move(opened);
    publishReader(reader);
  }
  return reader;
}

void ReaderManager::SegmentStats::readSpaces(const SegmentCore& core) {
  for (size_t i = 1; i < core.spaces().size(); i++) {
    const auto& space = core.spaces()[i];
    const auto& shape = *core.ownership((SpaceOrdinal)i);
    spaces.push_back({space.path, space.maxDoc, shape.endsBytes(), shape.ownersBytes()});
  }
}

ReaderManager::Stats ReaderManager::stats(bool includeSegments, bool includeFiles) {
  Stats out;
  auto current = snapshot();
  std::pmr::monotonic_buffer_resource arena;
  auto info = Manifest::decode(current->bytes, arena);
  auto reader = includeSegments ? indexReader.load() : nullptr;
  out.commitTime = info.commit_time;
  out.indexGen = info.index_gen;
  out.coreGen = info.core_gen;
  out.updateVersion = info.update_version;
  out.schemaGen = info.schema->version.generation();
  out.segments = out.committedSegments = info.segments.size();
  if (includeFiles) out.totalBytes = dir.totalBytes();
  auto copyAux = [](const api::AuxIndexInfo& aux) {
    AuxStats result;
    result.kind = aux.kind;
    result.field = aux.field;
    result.name = aux.name;
    result.gen = aux.gen;
    result.commitTime = aux.commit_time;
    result.builtCoreGen = aux.built_core_gen;
    for (const auto& file : aux.files) {
      result.files.emplace_back(file.name);
      result.bytes += file.size;
    }
    return result;
  };
  for (const auto& aux : info.aux_indexes) out.auxIndexes.push_back(copyAux(aux));
  for (const auto& seg : info.segments) {
    out.maxDocs += seg.max_doc;
    out.liveDocs += seg.live_docs;
    if (!includeSegments) continue;
    SegmentStats result;
    result.segId = seg.seg_id;
    result.creatorEpoch = seg.creator_epoch;
    result.liveGen = seg.live_gen;
    result.minUpdateVersion = seg.min_version;
    result.maxUpdateVersion = seg.max_version;
    result.firstCommitTime = seg.commit_time;
    result.schemaGen = seg.schema.version.generation();
    result.maxDoc = seg.max_doc;
    result.liveDocs = seg.live_docs;
    result.committed = true;
    for (const auto& file : seg.files) result.bytes += file.size;
    for (const auto& aux : seg.overlays) {
      result.overlays.push_back(copyAux(aux));
      result.bytes += result.overlays.back().bytes;
    }
    std::shared_ptr<SegmentCore> core;
    if (reader) {
      if (auto* leaf = reader->findSegment(seg.seg_id)) core = leaf->segmentCoreShared();
    }
    if (!core) core = SegmentCore::create(dir, seg.seg_id, seg.creator_epoch, true);
    if (core) result.readSpaces(*core);
    out.segmentStats.push_back(std::move(result));
  }
  cacheStats(out);
  return out;
}

void ReaderManager::cacheStats(CacheStats& out) const {
  auto cache = filterCache;
  if (!cache) return;
  auto config = cache->configuration();
  out.filterCacheEnabled = config.maxBytes != 0;
  out.filterCacheMaxBytes = config.maxBytes;
  out.filterCacheResidentBytes = cache->bytesUsed();
  out.filterCacheMetadataBytes = cache->metadataBytesUsed();
  out.filterCacheCounters = cache->counters();
}

}
