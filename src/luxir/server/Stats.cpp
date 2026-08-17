#include "Stats.h"

#include <cassert>
#include <string_view>
#include <vector>

#include "LuxirNode.h"
#include "luxir/api/build.h"
#include "luxir/index/IndexWriter.h"
#include "luxir/reader/Postings.h"
#include "luxir/schema/Schema.h"

namespace luxir {
namespace {

// Filesystem spelling for a generation that appears in filenames; empty
// (omitted in JSON) when there is no generation and therefore no file.
std::string_view sortableGen(uint64_t gen, std::pmr::memory_resource& resource) {
  if (gen == 0) return {};
  return api::build::arenaStr(resource, Postings::getSortableString(gen));
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
}

void copyAuxStats(api::AuxStats& dst, const IndexWriter::AuxStats& src,
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

void fillFilterCacheStats(api::FilterCacheStats& dst, const IndexWriter::Stats& src) {
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
}

void fillIndexStats(api::IndexStats& dst, const IndexWriter::Stats& src,
                    std::pmr::memory_resource& resource) {
  dst.commit_time = src.commitTime;
  dst.index_gen = src.indexGen;
  dst.core_gen = src.coreGen;
  dst.update_version = src.updateVersion;
  dst.schema_gen = sortableGen(src.schemaGen, resource);
  dst.active_merges = src.activeMerges;

  auto* aux = api::build::allocArray(dst.aux_indexes, src.auxIndexes.size(), resource);
  for (std::size_t i = 0; i < src.auxIndexes.size(); i++) {
    copyAuxStats(aux[i], src.auxIndexes[i], resource);
  }
  fillFilterCacheStats(dst.filter_cache, src);

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
    out.schema_gen = sortableGen(in.schemaGen, resource);
    out.committed = in.committed;
    out.merging = in.merging;
    out.merge_level = (uint32_t)in.mergeLevel;
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
  std::vector<LuxirNode::CollectionEntry> entries;
  if (request.collection) {
    auto collection = node.resolveCollection(&*request.collection);
    std::string_view name = LuxirNode::kDefaultCollectionName;
    if (!request.collection->name.empty()) name = request.collection->name.back();
    entries.push_back({std::string(name), std::move(collection), {}});
  } else {
    entries = node.collectionEntries();
  }

  auto* collections = api::build::allocArray(response.collections, entries.size(), resource);
  for (std::size_t i = 0; i < entries.size(); i++) {
    const auto& entry = entries[i];
    auto& collectionStats = collections[i];
    collectionStats.name = api::build::arenaStr(resource, entry.name);
    if (!entry.error.empty()) {
      collectionStats.error = api::build::arenaStr(resource, entry.error);
      continue;
    }

    auto schema = entry.collection->getSchema();
    collectionStats.schema_gen = sortableGen(schema ? schema->gen_ : 0, resource);

    auto shard = entry.collection->getShard();
    assert(shard);
    auto writer = shard->getIndexWriter();
    assert(writer);
    auto writerStats = writer->stats(request.segments);

    auto* shards = api::build::allocArray(collectionStats.shards, 1, resource);
    auto& shardStats = shards[0];
    shardStats.shard_id = 0;
    fillIndexStats(shardStats.index, writerStats, resource);

    // Counts for this index alone.  "collections" is meaningless below the node
    // total and "shards" below the collection total, so neither is set here.
    auto& indexTotals = shardStats.index.totals;
    indexTotals.segments = writerStats.segments;
    indexTotals.committed_segments = writerStats.committedSegments;
    indexTotals.max_docs = writerStats.maxDocs;
    indexTotals.live_docs = writerStats.liveDocs;
    assert(writerStats.liveDocs <= writerStats.maxDocs);
    indexTotals.deleted_docs = writerStats.maxDocs - writerStats.liveDocs;
    indexTotals.bytes = writerStats.totalBytes;

    collectionStats.totals = indexTotals;
    collectionStats.totals.shards = 1;
    addTotals(response.totals, collectionStats.totals);
    response.totals.collections++;
  }

  auto& ram = node.getIndexRamBudget();
  response.index_ram.limit_bytes = (uint64_t)ram.totalBytes();
  response.index_ram.reserved_bytes = (uint64_t)ram.reservedBytes();
}

} // namespace luxir
