// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "Stats.h"
#include "luxir/query/QueryStats.h"
#include "luxir/util/ApiError.h"
#include "luxir/util/ProcessAllocator.h"
#include "luxir/util/TaskWork.h"
#include "luxir/util/log.h"
#include "ReplicationFollower.h"
#include "Scrubber.h"

#include <cassert>
#include <chrono>
#include <string_view>
#include <vector>

#include "LuxirNode.h"
#include "luxir/BuildInfo.h"
#include "luxir/KernelInfo.h"
#include "luxir/api/build.h"
#include "luxir/index/IndexWriter.h"
#include "luxir/reader/Postings.h"
#include "luxir/reader/FieldMetadataMemory.h"
#include "luxir/schema/Schema.h"
#include "luxir/search/LiveDocsMemory.h"

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
  memory.indexing_budget.retained_delete_bytes = (uint64_t)ram.retainedDeleteBytesCount();
  memory.node_budget_bytes = node.getConfig().max_ram_mb * 1024 * 1024;
  memory.search_request_limit_bytes = node.getConfig().search.request_memory_max_bytes;
  memory.schema_cache_bytes = node.collections().schemaBytes();
  memory.live_docs_heaps = liveDocsMemory.sparseRoots.count.load(std::memory_order_relaxed);
  memory.live_docs_heap_bytes = liveDocsMemory.sparseRoots.bytes.load(std::memory_order_relaxed);
  memory.storage_ram.used_bytes = node.storageBytes();
  auto storageMemory = node.collections().storage().memory();
  memory.storage_ram.limit_bytes = storageMemory ? storageMemory->limitBytes() : 0;
  auto& fields = memory.field_metadata;
  // Keep the exported API names for the field-cache page-pointer arrays.
  fields.directories = fieldMetadataMemory.pagePointerArrays.load(std::memory_order_relaxed);
  fields.directory_bytes = fieldMetadataMemory.pagePointerBytes.load(std::memory_order_relaxed);
  fields.pages = fieldMetadataMemory.pages.load(std::memory_order_relaxed);
  fields.page_bytes = fieldMetadataMemory.pageBytes.load(std::memory_order_relaxed);
  fields.entries = fieldMetadataMemory.entries.load(std::memory_order_relaxed);
  fields.entry_bytes = fieldMetadataMemory.entryBytes.load(std::memory_order_relaxed);
  fields.opened_pages = fieldMetadataMemory.openedPages.load(std::memory_order_relaxed);
  fields.opened_page_bytes = fieldMetadataMemory.openedPageBytes.load(std::memory_order_relaxed);
  fields.opened_entries = fieldMetadataMemory.openedEntries.load(std::memory_order_relaxed);
  fields.opened_entry_bytes = fieldMetadataMemory.openedEntryBytes.load(std::memory_order_relaxed);
  fields.names = fieldMetadataMemory.names.load(std::memory_order_relaxed);
  fields.name_bytes = fieldMetadataMemory.nameBytes.load(std::memory_order_relaxed);
  fields.live_views = liveDocsMemory.views.count.load(std::memory_order_relaxed);
  fields.live_view_bytes = liveDocsMemory.views.bytes.load(std::memory_order_relaxed);
  fields.bytes = fields.directory_bytes + fields.page_bytes + fields.entry_bytes
      + fields.opened_page_bytes + fields.opened_entry_bytes + fields.name_bytes + fields.live_view_bytes;
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
  if (src.bytes) dst.bytes = dst.bytes.value_or(0) + *src.bytes;
  dst.committed_bytes += src.committed_bytes;
}

void fillHistogram(api::HistogramStats& dst, const ActivityCounts& src, std::span<const double> bounds,
                   std::pmr::memory_resource& resource) {
  uint64_t count = 0;
  for (std::size_t i = 0; i <= bounds.size(); i++) count += src.buckets[i];
  dst.sum = (double)src.nanos / 1e9;
  dst.count = count;
  if (count == 0) return;
  auto* counts = api::build::allocArray(dst.counts, bounds.size() + 1, resource);
  uint64_t cumulative = 0;
  for (std::size_t i = 0; i <= bounds.size(); i++) counts[i] = cumulative += src.buckets[i];
}

void fillActivity(api::ActivityStats& dst, const MetricCounts& counts, size_t activity,
                  std::pmr::memory_resource& resource) {
  const auto& src = counts.activities[activity];
  dst.ok = src.completed[(size_t)MetricOutcome::OK];
  dst.error = src.completed[(size_t)MetricOutcome::ERROR];
  dst.cancelled = src.completed[(size_t)MetricOutcome::CANCELLED];
  // Stripes are summed one at a time, so work finishing during the read can
  // briefly count as completed before it counts as started.
  uint64_t completed = dst.ok + dst.error + dst.cancelled;
  dst.in_flight = src.started > completed ? (int64_t)(src.started - completed) : 0;
  fillHistogram(dst.duration, src, metricBounds(activity), resource);
}

// Counts by error kind, keyed by the kind's wire spelling.
api::map_view<std::string_view, uint64_t> errorKinds(const std::array<uint64_t, ERROR_KINDS>& counts,
                                                     std::pmr::memory_resource& resource) {
  using Entry = std::pair<std::string_view, uint64_t>;
  std::span<const Entry> entries;
  auto* kinds = api::build::allocArray(entries, ERROR_KINDS, resource);
  for (std::size_t kind = 0; kind < ERROR_KINDS; kind++) kinds[kind] = {errorKindName((ErrorKind)kind), counts[kind]};
  return {entries};
}

void fillTransport(api::TransportStats& dst, const MetricCounts& counts, MetricTransport transport,
                   std::pmr::memory_resource& resource) {
  auto t = (size_t)transport;
  using Operation = std::pair<std::string_view, api::ActivityStats>;
  std::span<const Operation> operations;
  auto* ops = api::build::allocArray(operations, metricOperations.size(), resource);
  for (std::size_t op = 0; op < metricOperations.size(); op++) {
    ops[op].first = metricOperations[op];
    fillActivity(ops[op].second, counts, metricActivity(transport, (MetricOperation)op), resource);
  }
  dst.operations = {operations};
  dst.errors = errorKinds(counts.requestErrors[t], resource);
  dst.received_bytes = counts.transport[(size_t)MetricTransportCounter::RECEIVED_BYTES][t];
  dst.sent_bytes = counts.transport[(size_t)MetricTransportCounter::SENT_BYTES][t];
  dst.stream_pauses = counts.transport[(size_t)MetricTransportCounter::STREAM_PAUSES][t];
}

void fillServiceStats(LuxirNode& node, api::ServiceStats& dst, std::pmr::memory_resource& resource) {
  auto& metrics = *node.metrics;
  auto counts = metrics.counts();
  auto counter = [&](MetricCounter c) { return counts.counters[(size_t)c]; };
  const auto& build = buildInfo();
  dst.version = build.version;
  dst.cpu_target = build.cpuTarget;
  dst.source_commit = build.sourceCommit;
  dst.faiss_simd = kernelInfo().faissSimd;
  dst.blas_core = kernelInfo().blasCore;
  dst.read_only = node.readOnly();
  dst.request_duration_bounds = requestBounds;
  dst.background_duration_bounds = backgroundBounds;
  fillTransport(dst.http, counts, MetricTransport::HTTP, resource);
  dst.http.connections = (uint64_t)metrics.httpConnections.load(std::memory_order_relaxed);
  dst.http.io_threads = metrics.httpThreads.load(std::memory_order_relaxed);
  fillTransport(dst.grpc, counts, MetricTransport::GRPC, resource);
  fillActivity(dst.search, counts, metricActivity(MetricStage::SEARCH), resource);
  fillHistogram(dst.search_execution, counts.activities[metricActivity(MetricStage::SEARCH_EXECUTION)],
                requestBounds, resource);
  dst.search_resource_exhausted = counter(MetricCounter::SEARCH_RESOURCE_EXHAUSTED);
  dst.search_warnings = counter(MetricCounter::SEARCH_WARNINGS);
  fillActivity(dst.index_batches, counts, metricActivity(MetricStage::INDEX_BATCH), resource);
  dst.documents_accepted = counter(MetricCounter::DOCUMENTS_ACCEPTED);
  dst.documents_rejected = counter(MetricCounter::DOCUMENTS_REJECTED);
  dst.delete_ids_accepted = counter(MetricCounter::DELETE_IDS_ACCEPTED);
  {
    using Entry = std::pair<std::string_view, uint64_t>;
    std::span<const Entry> entries;
    auto* patches = api::build::allocArray(entries, 3, resource);
    patches[0] = {"written", counter(MetricCounter::PATCHES_WRITTEN)};
    patches[1] = {"unchanged", counter(MetricCounter::PATCHES_UNCHANGED)};
    patches[2] = {"failed", counter(MetricCounter::PATCHES_FAILED)};
    dst.patches = {entries};
  }
  dst.document_errors = errorKinds(counts.documentErrors, resource);
  fillActivity(dst.flush, counts, metricActivity(MetricStage::FLUSH), resource);
  fillActivity(dst.commit, counts, metricActivity(MetricStage::COMMIT), resource);
  fillActivity(dst.delete_pass, counts, metricActivity(MetricStage::DELETE_PASS), resource);
  fillActivity(dst.merge, counts, metricActivity(MetricStage::MERGE), resource);
  fillActivity(dst.vector_build, counts, metricActivity(MetricStage::VECTOR_BUILD), resource);
  fillActivity(dst.reader_refresh, counts, metricActivity(MetricStage::READER_REFRESH), resource);
  auto control = node.getControlExecutor().stats();
  dst.control_threads = control.threads;
  dst.control_busy = control.busy;
  dst.control_queued = control.queued;
  dst.control_completed = control.completed;
  // Fan-out counters are process-wide: nodes sharing a process report the same.
  auto& fanout = dst.search_fanout;
  auto load = [](const std::atomic<uint64_t>& c) { return c.load(std::memory_order_relaxed); };
  auto outcome = [&](FanoutOutcome o) { return load(fanoutOutcomes[(size_t)o]); };
  fanout.parallel_searches = load(fanoutParallelSearches);
  fanout.published = outcome(FanoutOutcome::PUBLISHED);
  fanout.shares = load(fanoutPublishedShares);
  fanout.no_split = outcome(FanoutOutcome::NO_SPLIT);
  fanout.capped = outcome(FanoutOutcome::CAPPED);
  fanout.siblings_only = outcome(FanoutOutcome::SIBLINGS_ONLY);
  fanout.saturated = outcome(FanoutOutcome::SATURATED);
}

void fillOutcomes(api::OutcomeStats& dst, const std::array<uint64_t, (size_t)MetricOutcome::COUNT>& outcomes,
                  uint64_t nanos) {
  dst.ok = outcomes[(size_t)MetricOutcome::OK];
  dst.error = outcomes[(size_t)MetricOutcome::ERROR];
  dst.cancelled = outcomes[(size_t)MetricOutcome::CANCELLED];
  dst.seconds = (double)nanos / 1e9;
}

void fillCollectionActivity(api::CollectionActivityStats& dst, const CollectionCounts& src) {
  fillOutcomes(dst.searches, src.searches, src.searchNanos);
  fillOutcomes(dst.index_batches, src.batches, src.batchNanos);
  dst.documents_accepted = src.documentsAccepted;
  dst.documents_rejected = src.documentsRejected;
  dst.delete_ids_accepted = src.deleteIdsAccepted;
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
  dst.unrepaid_evictions = src.filterCacheCounters.unrepaidEvictions;
  dst.population_deferred = src.filterCacheCounters.populationDeferred;
  dst.capacity_deferred = src.filterCacheCounters.capacityDeferred;
  dst.assumed_charges = src.filterCacheCounters.assumedCharges;
  dst.observed_charges = src.filterCacheCounters.observedCharges;
  dst.build_us = src.filterCacheCounters.buildNanos / 1000;
  dst.estimated_saved_us = src.filterCacheCounters.estimatedSavedNanos / 1000;
  dst.unpaid_build_us = src.filterCacheCounters.unpaidBuildNanos / 1000;
  dst.assumed_credits = src.filterCacheCounters.assumedCredits;
  dst.observed_credits = src.filterCacheCounters.observedCredits;
  dst.outstanding_debt_us = src.filterCacheCounters.outstandingDebtNanos / 1000;
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
    dst.retained_delete_bytes = src.retainedDeleteBytes;
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
    out.seg = api::build::arenaStr(resource, "s" + Postings::segmentKey(in.segId, in.creatorEpoch));
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
    auto* spaces = api::build::allocArray(out.spaces, in.spaces.size(), resource);
    for (size_t si = 0; si < in.spaces.size(); si++) {
      spaces[si].path = api::build::arenaStr(resource, in.spaces[si].path);
      spaces[si].max_doc = in.spaces[si].maxDoc;
      spaces[si].ends_bytes = in.spaces[si].endsBytes;
      spaces[si].owners_bytes = in.spaces[si].ownersBytes;
    }

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
  if (!nodeScope && (request.system || request.host)) {
    throw RequestError("system and host stats are node-wide: request them without a collection or tenant");
  }
  if (nodeScope) {
    // Atomics and allocator statistics only: cheap enough to poll.
    fillServiceStats(node, response.service.emplace(), resource);
    fillMemoryStats(node, response.memory.emplace());
    if (request.system || request.host) {
      gatherSystemStats(request.system ? &response.system.emplace() : nullptr,
                        request.host ? &response.host.emplace() : nullptr,
                        node.collections().storage().diskPath(), resource);
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
    for (std::size_t i = 0; i < entries.size(); i++) {
      const auto& entry = entries[i];
      auto& collectionStats = collections[i];
      collectionStats.name = api::build::arenaStr(resource, entry.id.name);
      collectionStats.tenant = api::build::arenaStr(resource, entry.id.tenant);
      collectionStats.storage_ram_bytes = node.collections().storageBytes(*entry.collection);
      auto unavailable = [&](const ErrorInfo& info) {
        collectionStats.schema_gen = 0;
        collectionStats.shards = {};
        collectionStats.error = api::build::arenaError(resource, info);
      };
      if (!entry.error.empty()) {
        unavailable({ErrorKind::UNAVAILABLE, "collection_unavailable", entry.error});
        continue;
      }
      // Deleted since the listing (or the resolution): report it as
      // resolution now would.
      if (entry.collection->getReaderManager().isClosed()) {
        if (!request.collection.empty()) {
          throw CollectionUnavailableError("collection '" + entry.id.label() + "' was deleted");
        }
        unavailable({ErrorKind::UNAVAILABLE, "collection_unavailable", "collection was deleted"});
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
          if (files) indexTotals.bytes = stats.totalBytes;
        };
        if (writer) fill(writer->stats(request.segments, files));
        else fill(shard->getReaderManager().stats(request.segments, files));
      } catch (const std::exception& e) {
        // A named collection reports its failure. A node-wide walk reports the
        // collection in its error slot and goes on: unavailable if it was
        // deleted while being read, otherwise the engine's failure, logged.
        if (!request.collection.empty()) throw;
        if (entry.collection->getReaderManager().isClosed()) {
          unavailable({ErrorKind::UNAVAILABLE, "collection_unavailable", e.what()});
        } else {
          auto info = classifyException(e, ErrorKind::INTERNAL);
          LOG_ERROR("stats for collection '{}' failed: {}", entry.id.label(), info.message);
          unavailable(info);
        }
        continue;
      }

      fillCollectionActivity(collectionStats.activity.emplace(), entry.collection->getMetrics().counts());
      collectionStats.totals = collectionStats.shards[0].index.totals;
      collectionStats.totals.shards = 1;
      addTotals(totals, collectionStats.totals);
      totals.collections++;
    }
  }
  if (nodeScope && node.getScrubber().enabled()) {
    auto scrub = node.getScrubber().stats();
    response.scrub = api::ScrubStats{scrub.passes, scrub.files, scrub.bytes, scrub.mismatches, scrub.errors, scrub.lastPassEnd};
  }
  response.duration_seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

void gatherCacheControl(LuxirNode& node, const api::CacheControlRequest& request,
                        api::CacheControlResponse& response,
                        std::pmr::memory_resource& resource) {
  std::vector<LuxirNode::CollectionEntry> entries;
  if (request.max_bytes) {
    entries =node.collections().setCacheBytes(request.tenant, request.collection, (size_t)*request.max_bytes);
  } else if (!request.collection.empty()) {
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

    ReaderManager::CacheStats cacheStats;
    shard->getReaderManager().cacheStats(cacheStats);
    fillQueryCacheStats(shardControl.stats, cacheStats);

    if (!request.dump) continue;
    size_t resident = 0;
    auto rows = cache->dump(dumpLimit, &resident, request.include_ghosts);
    shardControl.entries_resident = resident;
    shardControl.recent_work_epochs = cache->configuration().recentWorkEpochs;
    shardControl.last_refresh_epoch = cache->lastRefreshEpoch();
    shardControl.current_epoch = cache->currentEpoch();
    auto* dumped = api::build::allocArray(shardControl.entries, rows.size(), resource);
    for (std::size_t r = 0; r < rows.size(); r++) {
      const auto& row = rows[r];
      auto& out = dumped[r];
      out.key_hash = row.keyHash;
      out.key_bytes = (uint32_t)row.keyBytes;
      out.key_text = api::build::arenaStr(resource, row.keyText);
      switch (row.scope) {
        case FilterKeyScope::SEGMENT_STABLE: out.scope = "SEGMENT_STABLE"; break;
        case FilterKeyScope::CORE_STABLE: out.scope = "CORE_STABLE"; break;
        case FilterKeyScope::READER_STABLE: out.scope = "READER_STABLE"; break;
        case FilterKeyScope::REQUEST_STABLE: out.scope = "REQUEST_STABLE"; break;
        case FilterKeyScope::UNCACHEABLE: out.scope = "UNCACHEABLE"; break;
      }
      out.reader_value = row.readerValue;
      out.bytes = row.bytes;
      out.segments_resident = row.segmentsResident;
      out.last_used_epoch = row.lastUsed;
      out.build_us = row.buildNanos / 1000;
      auto* values = api::build::allocArray(out.values, row.values.size(), resource);
      for (size_t v = 0; v < row.values.size(); v++) {
        const auto& value = row.values[v];
        values[v].seg_id = value.segId;
        values[v].space = (uint32_t)value.space;
        values[v].max_doc = value.maxDoc;
        if (value.rawCardinality >= 0) values[v].raw_cardinality = (uint32_t)value.rawCardinality;
        values[v].reader_version = value.readerVersion;
        values[v].resident = value.resident;
        values[v].owed_us = value.owed / 1000;
        values[v].debt_us = value.debt / 1000;
        values[v].credited_us = value.creditedTotal / 1000;
        values[v].production_us = value.productionNanos / 1000;
        values[v].production_source = value.productionSource;
        values[v].absent_mean_us = value.absentNanos / 1000;
        values[v].absent_count = value.absentCount;
        values[v].served_mean_us = value.servedNanos / 1000;
        values[v].served_count = value.servedCount;
        values[v].reference_bytes = value.referenceBytes;
        values[v].recent_us = value.recent / 1000;
        values[v].recent_epochs = value.recentEpochs;
        values[v].work_us = value.work / 1000;
        values[v].residency_bytes = value.residencyBytes;
        values[v].ranked_rate_us_per_epoch = value.rankedRate / 1000;
        values[v].spoken_for = value.spokenFor;
      }
    }
  }
}

} // namespace luxir
