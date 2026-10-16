// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "Stats.h"
#include "luxir/query/QueryStats.h"
#include "luxir/util/ApiError.h"
#include "luxir/util/ProcessAllocator.h"
#include "ReplicationFollower.h"

#include <cassert>
#include <chrono>
#include <string_view>
#include <vector>

#include "LuxirNode.h"
#include "luxir/api/build.h"
#include "luxir/index/IndexWriter.h"
#include "luxir/reader/Postings.h"
#include "luxir/reader/FieldMetadataMemory.h"
#include "luxir/schema/Schema.h"

namespace luxir {
namespace {

// Filesystem spelling for a generation that appears in filenames; empty
// (omitted in JSON) when there is no generation and therefore no file.
std::string_view sortableGen(uint64_t gen, std::pmr::memory_resource& resource) {
  if (gen == 0) return {};
  return api::build::arenaStr(resource, Postings::getSortableString(gen));
}

void fillMemoryStats(LuxirNode& node, api::MemoryStats& memory) {
  auto& ram = node.getIndexRamBudget();
  memory.indexing_budget.limit_bytes = (uint64_t)ram.totalBytes();
  memory.indexing_budget.reserved_bytes = (uint64_t)ram.reservedBytes();
  memory.indexing_budget.draining_bytes = (uint64_t)ram.drainingBytesCount();
  memory.indexing_budget.pending_merge_bytes = (uint64_t)ram.pendingMergeDemandBytes();
  memory.node_budget_bytes = node.getConfig().max_ram_mb * 1024 * 1024;
  memory.search_request_limit_bytes = node.getConfig().search.request_memory_max_bytes;
  memory.storage_ram.used_bytes = node.storageBytes();
  memory.storage_ram.limit_bytes = node.getConfig().store.backend == "ram"
      ? node.getConfig().store.ram_limit_mb * 1024 * 1024 : 0;
  auto& fields = memory.field_metadata;
  fields.directories = fieldMetadataMemory.directories.load(std::memory_order_relaxed);
  fields.directory_bytes = fieldMetadataMemory.directoryBytes.load(std::memory_order_relaxed);
  fields.pages = fieldMetadataMemory.pages.load(std::memory_order_relaxed);
  fields.page_bytes = fieldMetadataMemory.pageBytes.load(std::memory_order_relaxed);
  fields.entries = fieldMetadataMemory.entries.load(std::memory_order_relaxed);
  fields.entry_bytes = fieldMetadataMemory.entryBytes.load(std::memory_order_relaxed);
  fields.names = fieldMetadataMemory.names.load(std::memory_order_relaxed);
  fields.name_bytes = fieldMetadataMemory.nameBytes.load(std::memory_order_relaxed);
  fields.bytes = fields.directory_bytes + fields.page_bytes + fields.entry_bytes + fields.name_bytes;
  auto process = processAllocatorStats();
  if (!process) return;
  auto& jemalloc = memory.jemalloc.emplace();
  jemalloc.version = process->version;
  jemalloc.allocated_bytes = process->allocated;
  jemalloc.active_bytes = process->active;
  jemalloc.metadata_bytes = process->metadata;
  jemalloc.resident_bytes = process->resident;
  jemalloc.mapped_bytes = process->mapped;
  jemalloc.retained_bytes = process->retained;
  jemalloc.dirty_bytes = process->dirty;
  jemalloc.indexing = {process->indexing.allocated, process->indexing.resident};
  jemalloc.big_buffer = {process->bigBuffer.allocated, process->bigBuffer.resident};
}

void addTotals(api::StatsTotals& dst, const api::StatsTotals& src) {
  dst.collections += src.collections;
  dst.shards += src.shards;
  dst.segments += src.segments;
  dst.committed_segments += src.committed_segments;
  dst.max_docs += src.max_docs;
  dst.live_docs += src.live_docs;
  dst.deleted_docs += src.deleted_docs;
  dst.bytes += src.bytes;
  dst.committed_bytes += src.committed_bytes;
}

void fillHistogram(api::HistogramStats& dst, const MetricHistogram& src,
                   std::pmr::memory_resource& resource) {
  auto snapshot = src.snapshot();
  uint64_t count = 0;
  for (auto bucket : snapshot.buckets) count += bucket;
  dst.sum = snapshot.sum;
  dst.count = count;
  if (count == 0) return;
  auto* counts = api::build::allocArray(dst.counts, snapshot.buckets.size(), resource);
  uint64_t cumulative = 0;
  for (std::size_t i = 0; i < snapshot.buckets.size(); i++) counts[i] = cumulative += snapshot.buckets[i];
}

void fillActivity(api::ActivityStats& dst, const MetricActivity& src,
                  std::pmr::memory_resource& resource) {
  dst.in_flight = src.active.load(std::memory_order_relaxed);
  dst.ok = src.completed[(size_t)MetricOutcome::OK].load(std::memory_order_relaxed);
  dst.error = src.completed[(size_t)MetricOutcome::ERROR].load(std::memory_order_relaxed);
  dst.cancelled = src.completed[(size_t)MetricOutcome::CANCELLED].load(std::memory_order_relaxed);
  fillHistogram(dst.duration, src.duration, resource);
}

void fillTransport(api::TransportStats& dst, Metrics& metrics, MetricTransport transport,
                   std::pmr::memory_resource& resource) {
  auto t = (size_t)transport;
  using Operation = std::pair<std::string_view, api::ActivityStats>;
  std::span<const Operation> operations;
  auto* ops = api::build::allocArray(operations, metricOperations.size(), resource);
  for (std::size_t op = 0; op < metricOperations.size(); op++) {
    ops[op].first = metricOperations[op];
    fillActivity(ops[op].second, metrics.requests[t][op], resource);
  }
  dst.operations = {operations};
  using Error = std::pair<std::string_view, uint64_t>;
  std::span<const Error> errors;
  auto* kinds = api::build::allocArray(errors, metrics.errors[t].size(), resource);
  for (std::size_t kind = 0; kind < metrics.errors[t].size(); kind++) {
    kinds[kind] = {errorKindName((ErrorKind)kind), metrics.errors[t][kind].load(std::memory_order_relaxed)};
  }
  dst.errors = {errors};
  dst.received_bytes = metrics.receivedBytes[t].load(std::memory_order_relaxed);
  dst.sent_bytes = metrics.sentBytes[t].load(std::memory_order_relaxed);
  dst.stream_pauses = metrics.streamPauses[t].load(std::memory_order_relaxed);
}

void fillServiceStats(LuxirNode& node, api::ServiceStats& dst, std::pmr::memory_resource& resource) {
  auto& metrics = *node.metrics;
#ifdef LUXIR_VERSION
  dst.version = LUXIR_VERSION;
#endif
  dst.read_only = node.readOnly();
  dst.duration_bounds = MetricHistogram::bounds;
  fillTransport(dst.http, metrics, MetricTransport::HTTP, resource);
  dst.http.connections = metrics.httpConnections.load(std::memory_order_relaxed);
  dst.http.io_threads = metrics.httpThreads.load(std::memory_order_relaxed);
  fillTransport(dst.grpc, metrics, MetricTransport::GRPC, resource);
  fillActivity(dst.search, metrics.searches, resource);
  fillHistogram(dst.search_execution, metrics.searchExecution, resource);
  dst.search_resource_exhausted = metrics.searchResourceExhausted.load(std::memory_order_relaxed);
  dst.search_warnings = metrics.searchWarnings.load(std::memory_order_relaxed);
  fillActivity(dst.index_batches, metrics.updates, resource);
  dst.documents_accepted = metrics.documentsAccepted.load(std::memory_order_relaxed);
  dst.documents_rejected = metrics.documentsRejected.load(std::memory_order_relaxed);
  dst.delete_ids_accepted = metrics.deletesAccepted.load(std::memory_order_relaxed);
  fillActivity(dst.flush, metrics.flushes, resource);
  fillActivity(dst.commit, metrics.commits, resource);
  fillActivity(dst.merge, metrics.merges, resource);
  fillActivity(dst.vector_build, metrics.vectorBuilds, resource);
  fillActivity(dst.reader_refresh, metrics.readerRefreshes, resource);
}

void copyAuxStats(api::AuxStats& dst, const ReaderManager::AuxStats& src,
                  std::pmr::memory_resource& resource) {
  dst.kind = api::build::arenaStr(resource, src.kind);
  dst.field = api::build::arenaStr(resource, src.field);
  dst.name = api::build::arenaStr(resource, src.name);
  dst.gen = sortableGen(src.gen, resource);
  dst.commit_time = src.commitTime;
  dst.built_core_gen = src.builtCoreGen;
  dst.bytes = src.bytes;
  auto* files = api::build::allocArray(dst.files, src.files.size(), resource);
  for (std::size_t i = 0; i < src.files.size(); i++) {
    files[i] = api::build::arenaStr(resource, src.files[i]);
  }
}

void fillQueryCacheStats(api::QueryCacheStats& dst, const ReaderManager::CacheStats& src) {
  dst.enabled = src.filterCacheEnabled;
  dst.max_bytes = src.filterCacheMaxBytes;
  dst.resident_bytes = src.filterCacheResidentBytes;
  dst.metadata_bytes = src.filterCacheMetadataBytes;
  dst.hits = src.filterCacheCounters.hits;
  dst.misses = src.filterCacheCounters.misses;
  dst.admissions = src.filterCacheCounters.admissions;
  dst.builds = src.filterCacheCounters.builds;
  dst.byproduct_inserts = src.filterCacheCounters.byproductInserts;
  dst.publish_rejects = src.filterCacheCounters.publishRejects;
  dst.evictions = src.filterCacheCounters.evictions;
  dst.purges = src.filterCacheCounters.purges;
  dst.oversized_key_bypasses = src.filterCacheCounters.oversizedKeyBypasses;
  dst.reader_stable_hits = src.filterCacheCounters.readerStableHits;
  dst.reader_stable_refreshes = src.filterCacheCounters.readerStableRefreshes;
  dst.reader_stable_retires = src.filterCacheCounters.readerStableRetires;
  dst.build_attempts = src.filterCacheCounters.buildAttempts;
  dst.capacity_dead_builds = src.filterCacheCounters.capacityDeadBuilds;
  dst.thrash_build_skips = src.filterCacheCounters.thrashBuildSkips;
  dst.reader_dead_builds = src.filterCacheCounters.readerDeadBuilds;
  dst.reader_thrash_build_skips = src.filterCacheCounters.readerThrashBuildSkips;
}

template<class Stats>
void fillIndexStats(api::IndexStats& dst, const Stats& src,
                    std::pmr::memory_resource& resource) {
  dst.commit_time = src.commitTime;
  dst.index_gen = src.indexGen;
  dst.core_gen = src.coreGen;
  dst.update_version = src.updateVersion;
  dst.schema_gen = src.schemaGen;
  if constexpr (requires { src.activeMerges; }) {
    dst.active_merges = src.activeMerges;
    dst.inverters_busy = src.indexing;
    dst.inverters_idle = src.idle;
    dst.inverters_flushing = src.flushing;
    dst.commits = src.commits;
  }

  auto* aux = api::build::allocArray(dst.aux_indexes, src.auxIndexes.size(), resource);
  for (std::size_t i = 0; i < src.auxIndexes.size(); i++) {
    copyAuxStats(aux[i], src.auxIndexes[i], resource);
  }
  fillQueryCacheStats(dst.query_cache, src);

  auto* segments = api::build::allocArray(dst.segments, src.segmentStats.size(), resource);
  for (std::size_t i = 0; i < src.segmentStats.size(); i++) {
    const auto& in = src.segmentStats[i];
    auto& out = segments[i];
    out.seg = api::build::arenaStr(resource, Postings::getIndexFileNamePrefix(in.segId));
    out.live_gen = sortableGen(in.liveGen, resource);
    out.min_update_version = in.minUpdateVersion;
    out.max_update_version = in.maxUpdateVersion;
    out.first_commit_time = in.firstCommitTime;
    out.max_doc = in.maxDoc;
    out.live_docs = in.liveDocs;
    out.deleted_docs = in.maxDoc - in.liveDocs;
    out.schema_gen = in.schemaGen;
    out.committed = in.committed;
    if constexpr (requires { in.merging; }) {
      out.merging = in.merging;
      out.merge_level = (uint32_t)in.mergeLevel;
    }
    out.bytes = in.bytes;

    auto* overlays = api::build::allocArray(out.overlays, in.overlays.size(), resource);
    for (std::size_t oi = 0; oi < in.overlays.size(); oi++) {
      copyAuxStats(overlays[oi], in.overlays[oi], resource);
    }
  }
}

} // namespace

void gatherStats(LuxirNode& node, const api::StatsRequest& request,
                 api::StatsResponse& response, std::pmr::memory_resource& resource) {
  auto start = std::chrono::steady_clock::now();
  bool nodeScope = request.collection.empty() && request.tenant.empty();
  bool files = request.files || request.segments;
  if (!nodeScope && request.system) {
    throw RequestError("system stats are node-wide: request them without a collection or tenant");
  }
  if (nodeScope) {
    // Atomics and allocator statistics only: cheap enough to poll.
    fillServiceStats(node, response.service.emplace(), resource);
    fillMemoryStats(node, response.memory.emplace());
    if (request.system) {
      const auto& store = node.getConfig().store;
      gatherSystemStats(response.system.emplace(), store.backend == "fs" ? store.data_dir : "", resource);
    }
  }
  response.multiterm_expansion_refills =
      multitermExpansionRefills.load(std::memory_order_relaxed);

  if (!nodeScope || request.collections || files) {
    std::vector<LuxirNode::CollectionEntry> entries;
    if (!request.collection.empty()) {
      auto collection = node.getCollection(LuxirNode::target(request.tenant, request.collection));
      entries.push_back({collection->getId(), std::move(collection), {}});
    } else {
      // An empty tenant covers every tenant (administration).
      entries = request.tenant.empty() ? node.collectionEntries() : node.collectionEntries(request.tenant);
    }

    auto& totals = response.totals.emplace();
    auto* collections = api::build::allocArray(response.collections, entries.size(), resource);
    std::size_t filled = 0;
    for (const auto& entry : entries) {
      auto& collectionStats = collections[filled];
      collectionStats = {};
      collectionStats.name = api::build::arenaStr(resource, entry.id.name);
      collectionStats.tenant = api::build::arenaStr(resource, entry.id.tenant);
      collectionStats.storage_ram_bytes = node.storageBytes(entry.id);
      if (!entry.error.empty()) {
        collectionStats.error = api::build::arenaError(
            resource, {ErrorKind::UNAVAILABLE, "collection_unavailable", entry.error});
        filled++;
        continue;
      }
      try {
        auto schema = entry.collection->getSchema();
        collectionStats.schema_gen = schema ? schema->gen_ : 0;

        auto shard = entry.collection->getShard();
        assert(shard);
        auto writer = shard->getIndexWriter();
        auto* shards = api::build::allocArray(collectionStats.shards, 1, resource);
        auto& shardStats = shards[0];
        shardStats.shard_id = 0;
        // Never expire pins from a stats read: expiry can unlink files.
        auto pins = shard->getSnapshots().stats(false);
        shardStats.index.snapshot_pins = pins.pins;
        shardStats.index.pin_retained_bytes = pins.retainedBytes;
        shardStats.index.pin_idle_drops = pins.idleDrops;
        shardStats.index.pin_budget_drops = pins.budgetDrops;
        auto& indexTotals = shardStats.index.totals;
        if (auto snapshot = shard->getReaderManager().snapshot()) {
          for (const auto& file : snapshot->files) indexTotals.committed_bytes += file.size;
        }
        auto fill = [&](const auto& stats) {
          fillIndexStats(shardStats.index, stats, resource);
          // Counts for this index alone.  "collections" is meaningless below the node
          // total and "shards" below the collection total, so neither is set here.
          indexTotals.segments = stats.segments;
          indexTotals.committed_segments = stats.committedSegments;
          indexTotals.max_docs = stats.maxDocs;
          indexTotals.live_docs = stats.liveDocs;
          assert(stats.liveDocs <= stats.maxDocs);
          indexTotals.deleted_docs = stats.maxDocs - stats.liveDocs;
          indexTotals.bytes = stats.totalBytes;
        };
        if (writer) fill(writer->stats(request.segments, files));
        else fill(shard->getReaderManager().stats(request.segments, files));
      } catch (const std::exception&) {
        // A collection deleted or closed while walking a node-wide listing must
        // not fail the whole response; a named collection reports the failure.
        if (!request.collection.empty()) throw;
        response.collection_errors++;
        continue;
      }

      collectionStats.totals = collectionStats.shards[0].index.totals;
      collectionStats.totals.shards = 1;
      addTotals(totals, collectionStats.totals);
      totals.collections++;
      filled++;
    }
    response.collections = response.collections.first(filled);
  }
  response.duration_seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

void gatherCacheControl(LuxirNode& node, const api::CacheControlRequest& request,
                        api::CacheControlResponse& response,
                        std::pmr::memory_resource& resource) {
  std::vector<LuxirNode::CollectionEntry> entries;
  if (!request.collection.empty()) {
    auto collection = node.getCollection(LuxirNode::target(request.tenant, request.collection));
    entries.push_back({collection->getId(), std::move(collection), {}});
  } else {
    entries = request.tenant.empty() ? node.collectionEntries() : node.collectionEntries(request.tenant);
  }

  size_t dumpLimit = request.dump_limit != 0 ? request.dump_limit : 100;

  auto* collections = api::build::allocArray(response.collections, entries.size(), resource);
  for (std::size_t i = 0; i < entries.size(); i++) {
    const auto& entry = entries[i];
    auto& collectionControl = collections[i];
    collectionControl.name = api::build::arenaStr(resource, entry.id.name);
    collectionControl.tenant = api::build::arenaStr(resource, entry.id.tenant);
    if (!entry.error.empty()) {
      collectionControl.error = api::build::arenaError(
          resource, {ErrorKind::UNAVAILABLE, "collection_unavailable", entry.error});
      continue;
    }

    auto shard = entry.collection->getShard();
    assert(shard);
    auto cache = shard->getReaderManager().filterCache;

    auto* shards = api::build::allocArray(collectionControl.shards, 1, resource);
    auto& shardControl = shards[0];
    shardControl.shard_id = 0;
    if (cache == nullptr) continue;

    if (request.flush) cache->clear();
    if (request.reset_admission) cache->resetAdmission();
    if (request.reset_counters) cache->resetCounters();

    auto counters = cache->counters();
    auto& stats = shardControl.stats;
    stats.enabled = cache->enabled();
    stats.max_bytes = cache->configuration().maxBytes;
    stats.resident_bytes = cache->bytesUsed();
    stats.metadata_bytes = cache->metadataBytesUsed();
    stats.hits = counters.hits;
    stats.misses = counters.misses;
    stats.admissions = counters.admissions;
    stats.builds = counters.builds;
    stats.byproduct_inserts = counters.byproductInserts;
    stats.publish_rejects = counters.publishRejects;
    stats.evictions = counters.evictions;
    stats.purges = counters.purges;
    stats.oversized_key_bypasses = counters.oversizedKeyBypasses;
    stats.reader_stable_hits = counters.readerStableHits;
    stats.reader_stable_refreshes = counters.readerStableRefreshes;
    stats.reader_stable_retires = counters.readerStableRetires;

    if (!request.dump) continue;
    size_t resident = 0;
    auto rows = cache->dump(dumpLimit, &resident);
    shardControl.entries_resident = resident;
    auto* dumped = api::build::allocArray(shardControl.entries, rows.size(), resource);
    for (std::size_t r = 0; r < rows.size(); r++) {
      const auto& row = rows[r];
      auto& out = dumped[r];
      out.key_hash = row.keyHash;
      out.key_bytes = (uint32_t)row.keyBytes;
      out.key_text = api::build::arenaStr(resource, row.keyText);
      std::string_view scope = row.scope == FilterKeyScope::READER_STABLE
          ? "READER_STABLE"
          : row.scope == FilterKeyScope::CORE_STABLE ? "CORE_STABLE"
                                                     : "SEGMENT_STABLE";
      out.scope = scope;
      out.reader_value = row.readerValue;
      out.bytes = row.bytes;
      out.segments_resident = row.segmentsResident;
      out.hits = row.hits;
      out.last_used_epoch = row.lastUsed;
    }
  }
}

} // namespace luxir
