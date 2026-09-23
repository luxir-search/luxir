// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "ReaderManager.h"
#include "luxir/store/Manifest.h"
#include "luxir/util/ApiError.h"
#include "luxir/util/Signal.h"

namespace luxir {

ReaderManager::ReaderManager(Directory& dir, std::atomic<std::shared_ptr<const CommitSnapshot>>& published,
                             FilterCacheConfig config)
    : dir(dir), published(published), originalFilterCacheConfig(config), filterCache(std::make_shared<FilterCache>(config)) {}

void ReaderManager::throwClosed() {
  throw ApiError(ErrorKind::UNAVAILABLE, "reader_closed", "reader manager is closed");
}

void ReaderManager::install(std::shared_ptr<const CommitSnapshot> snapshot) noexcept {
  // Hints precede the owning state: a fast-path miss may reopen early, but
  // satisfied searches acquire only the reader and plain scalar atomics.
  lastAdvertisedCommitTime.store(snapshot->commitTime, std::memory_order_relaxed);
  publishedSchemaGen.store(snapshot->schema->gen_, std::memory_order_relaxed);
  published.store(std::move(snapshot), std::memory_order_release);
}

std::shared_ptr<IndexReader> ReaderManager::prepare(const CommitSnapshot& snapshot) {
  std::lock_guard lock(indexReaderMutex);
  checkOpen();
  auto previous = indexReader.load();
  return std::make_shared<IndexReader>(dir, previous.get(), filterCache, snapshot.schema, snapshot.bytes);
}

void ReaderManager::publishReader(std::shared_ptr<IndexReader> reader) {
  auto previous = indexReader.load(std::memory_order_relaxed);
  if (!previous || reader->core != previous->core) filterCache->onReaderPublished(*reader);
  indexReader.store(std::move(reader), std::memory_order_release);
}

void ReaderManager::installOpened(std::shared_ptr<const CommitSnapshot> snapshot, std::shared_ptr<IndexReader> reader) {
  std::lock_guard lock(indexReaderMutex);
  checkOpen();
  assert(reader->commitId() == snapshot->id.index_gen && reader->schema() == snapshot->schema);
  publishReader(std::move(reader));
  install(std::move(snapshot));
}

void ReaderManager::testReset() {
  std::lock_guard lock(indexReaderMutex);
  closed.store(false);
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

  // Fast path, no lock: the published reader is acceptable as it is, so a
  // reopen in progress on another thread never delays this request.
  auto reader = indexReader.load(std::memory_order_acquire);
  if (reader && !stale(*reader) && !schemaChanged(*reader)) return reader;

  // One thread reopens or swaps the schema; the others that need the result
  // wait here and then find it published, so re-check under the lock.
  const std::lock_guard<std::mutex> readerLock(indexReaderMutex);
  reader = indexReader.load(std::memory_order_acquire);
  if (!reader || stale(*reader) || schemaChanged(*reader)) {
    auto openPublished = [&](IndexReader* previous) {
      for (;;) {
        checkOpen();
        auto snapshot = published.load();
        if (!snapshot) throw ApiError(ErrorKind::UNAVAILABLE, "snapshot_unavailable", "no snapshot installed");
        try {
          Signal::emit("indexReaderOpening", this);
          return std::make_shared<IndexReader>(dir, previous, filterCache, snapshot->schema, snapshot->bytes);
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

ReaderManager::Stats ReaderManager::stats(bool includeSegments) {
  Stats out;
  auto current = snapshot();
  std::pmr::monotonic_buffer_resource arena;
  auto info = Manifest::decode(current->bytes, arena);
  out.commitTime = info.commit_time;
  out.indexGen = info.index_gen;
  out.coreGen = info.core_gen;
  out.updateVersion = info.update_version;
  out.schemaGen = info.schema_gen;
  out.segments = out.committedSegments = info.segments.size();
  out.totalBytes = dir.totalBytes();
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
    result.liveGen = seg.live_gen;
    result.minUpdateVersion = seg.min_version;
    result.maxUpdateVersion = seg.max_version;
    result.firstCommitTime = seg.commit_time;
    result.schemaGen = seg.schema_gen;
    result.maxDoc = seg.max_doc;
    result.liveDocs = seg.live_docs;
    result.committed = true;
    for (const auto& file : seg.files) result.bytes += file.size;
    for (const auto& aux : seg.overlays) {
      result.overlays.push_back(copyAux(aux));
      result.bytes += result.overlays.back().bytes;
    }
    out.segmentStats.push_back(std::move(result));
  }
  cacheStats(out);
  return out;
}

void ReaderManager::cacheStats(CacheStats& out) const {
  auto cache = filterCache;
  if (!cache) return;
  out.filterCacheEnabled = cache->enabled();
  out.filterCacheMaxBytes = cache->configuration().maxBytes;
  out.filterCacheResidentBytes = cache->bytesUsed();
  out.filterCacheMetadataBytes = cache->metadataBytesUsed();
  out.filterCacheCounters = cache->counters();
}

}
