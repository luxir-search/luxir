// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "IndexReader.h"
#include "OrdMapStore.h"
#include "luxir/store/Manifest.h"
#include "luxir/index/CommitSnapshot.h"
#include "luxir/reader/FieldReader.h"
#include "luxir/reader/Postings.h"
#include "luxir/reader/StoredFieldsReader.h"
#include "luxir/reader/TestOverlayAuxReader.h"
#include "luxir/reader/VectorAuxReader.h"
#include "luxir/schema/Schema.h"
#include "luxir/util/MemPool.h"

#include "luxir/api/padded_input.h"
#include "luxir/api/luxir_index.hpp"
#include <boost/unordered/unordered_flat_map.hpp>
#include <algorithm>
#include <map>
#include <memory_resource>
#include <span>

#include "OrdMapImpl.h"

namespace luxir {

namespace {

std::string segmentAuxKey(uint64_t segId, std::string_view name) {
  std::string key = std::to_string(segId);
  key.push_back(':');
  key.append(name);
  return key;
}

std::shared_ptr<AuxReader> openKnownAux(Directory& dir,
                                        const luxir::api::AuxIndexInfo& info,
                                        bool missingFileOK) {
  if (info.kind == VectorAuxReader::KIND) {
    return VectorAuxReader::open(dir, info, missingFileOK);
  }
  if (info.kind == TestOverlayAuxReader::KIND) {
    return TestOverlayAuxReader::open(dir, info, missingFileOK);
  }
  if (info.kind == OrdMapStore::KIND) {
    return OrdMapStore::open(dir, info, missingFileOK);
  }
  IREADER_DEBUG("Skipping unknown aux kind '{}' for '{}'", info.kind, info.name);
  return nullptr;
}

} // namespace

std::shared_ptr<LiveDocs> LiveDocs::create(Directory& dir, uint64_t segId, uint64_t liveGen, int32_t maxDoc, bool missingFileOK, bool expectSynced) {
  assert(liveGen > 0 && maxDoc > 0);
  std::string deleteFileName = Postings::getLiveDocsFileName(
    Postings::getSortableString(segId), liveGen);

  auto deleteFile = dir.openFile(deleteFileName, expectSynced);
  if (deleteFile == nullptr) {
    if (missingFileOK) {
      // Delete file not found - return nullptr instead of throwing
      IREADER_DEBUG("Delete file {} not found for segment {} (liveGen={})",
                deleteFileName, segId, liveGen);
      return nullptr;
    } else {
      throw std::filesystem::filesystem_error(
              std::format("Delete file {} not found for segment {} (liveGen={})",
                         deleteFileName, segId, liveGen),
              std::make_error_code(std::errc::no_such_file_or_directory));
    }
  }
  IREADER_DEBUG("Successfully opened delete file {} for segment {}", deleteFileName, segId);

  InputStream deleteStream = deleteFile->getInputStream();

  // Validate file format
  const char* data = deleteStream.ptr();
  size_t dataSize = deleteStream.size();

  // Define offsets based on field sizes
  auto headerSize =  Postings::LUXIR_HEADER.size() + sizeof(uint64_t) + sizeof(int32_t) + sizeof(int32_t);
  // Minimum size check
  if (dataSize < headerSize) {
    throw std::runtime_error(std::format("Delete file {} is too small: {} bytes", deleteFileName, dataSize));
  }

  // Check magic number
  if (std::memcmp(data, Postings::LUXIR_HEADER.data(), Postings::LUXIR_HEADER.size()) != 0) {
    throw std::runtime_error(std::format("Delete file {} has invalid magic number", deleteFileName));
  }

  deleteStream.skip(Postings::LUXIR_HEADER.size());
  uint64_t format = (uint64_t)deleteStream.readLong();
  int32_t fileMaxDoc = deleteStream.readInt();
  int32_t numLiveDocs = deleteStream.readInt();

  if (format != 1) {
    throw std::runtime_error(std::format("Delete file {} has unsupported format: {}", deleteFileName, format));
  }

  if (fileMaxDoc != maxDoc) {
    throw std::runtime_error(std::format("Delete file {} maxDoc mismatch: {} (expected {})",
              deleteFileName, fileMaxDoc, maxDoc));
  }

  if (numLiveDocs < 0 || numLiveDocs > maxDoc) {
    throw std::runtime_error(std::format("Delete file {} has invalid numLiveDocs: {} (maxDoc={})",
              deleteFileName, numLiveDocs, maxDoc));
  }

  // Validate file size
  int64_t expectedSize = deleteStream.offset() + screaming::FixedBitSet::sizeInWords(maxDoc) * sizeof(uint64_t);
  if (deleteStream.size() != expectedSize) {
    throw std::runtime_error(std::format("Delete file {} size mismatch: {} bytes (expected {})",
              deleteFileName, dataSize, expectedSize));
  }

  // FixedBitSet with live docs - memory map directly!
  uint64_t* liveBitsMemory = (uint64_t*)deleteStream.ptr();

  IREADER_DEBUG("Loaded {} deleted documents from {} for segment {}, {} live docs",
            maxDoc - numLiveDocs, deleteFileName, segId, numLiveDocs);

  return std::make_shared<LiveDocs>(liveBitsMemory, maxDoc, std::move(deleteFile), numLiveDocs);
}


IndexReader::IndexReader(Snapshot snapshot, std::shared_ptr<FilterCache> filterCache)
  : snapshotTime(snapshot.commitTime), snapshotGen(snapshot.indexGen),
    core(std::move(snapshot.core)), sharedSchema(std::move(snapshot.schema)),
    sharedFilterCache(std::move(filterCache)) {}

IndexReader::IndexReader(Directory& dir, const CommitSnapshot& snapshot, IndexReader* previousReader,
                         std::shared_ptr<FilterCache> filterCache)
  : IndexReader(openSnapshot(dir, previousReader, snapshot.schema, snapshot.bytes, snapshot.openReaders),
                filterCache ? std::move(filterCache)
                    : (previousReader ? previousReader->sharedFilterCache : nullptr)) {}

IndexReader::IndexReader(Directory& dir, IndexReader* previousReader, std::shared_ptr<FilterCache> filterCache)
  : IndexReader(openSnapshot(dir, previousReader, nullptr, nullptr),
                filterCache ? std::move(filterCache)
                    : (previousReader ? previousReader->sharedFilterCache : nullptr)) {}

IndexReader::Snapshot IndexReader::openSnapshot(Directory& dir, const IndexReader* previousReader,
                                                std::shared_ptr<Schema> preferredSchema,
                                                std::shared_ptr<const std::vector<std::byte>> manifest,
                                                std::span<const std::weak_ptr<PostingsReader>> published) {
  assert(published.empty() || manifest);  // aligned with that manifest's segments
  auto core = std::make_shared<PhysicalCore>();
  const auto* previous = previousReader ? previousReader->core.get() : nullptr;
  auto& segs = core->segs;
  auto& auxReadersList = core->auxReadersList;
  auto& totalMaxDoc = core->totalMaxDoc;
  auto& livedocs = core->livedocs;
  uint64_t commitTimeUs = 0;
  uint64_t indexGen = 0;
  std::shared_ptr<Schema> schema;
  // because old segments could be merged away before we have a chance to read them, we need
  // to check if there is a new index info file and retry the open if so.
  // This could be optimized by saving the segments we did read properly in case they are still in the index.
  uint64_t lastCommitTime = 0;
  bool retry = false;

  // Index previous reader's aux readers by name for cheap reuse when the new
  // commit carried the entry forward (same gen + built_core_gen).  FAISS
  // deserialization is the expensive part; reuse keeps reopens cheap.
  boost::unordered_flat_map<std::string, std::shared_ptr<AuxReader>> prevAuxByName;
  boost::unordered_flat_map<std::string, std::shared_ptr<AuxReader>> prevSegAuxByKey;
  if (previous) {
    assert(previous->segmentFiles.size() == previous->segs.size());
    for (const auto& r : previous->auxReadersList) {
      prevAuxByName.emplace(std::string(r->getName()), r);
    }
    for (const auto& seg : previous->segs) {
      for (const auto& r : seg.auxReaders()) {
        prevSegAuxByKey.emplace(segmentAuxKey(seg.segInfo.seg_id, r->getName()), r);
      }
    }
  }

  do {
    if (retry) {
      IREADER_DEBUG("Retrying IndexReader open");
      segs.clear();
      auxReadersList.clear();
      totalMaxDoc = 0;
      livedocs = 0;
      retry = false;
    }

    // Non-owning IndexInfo view; its repeated messages and padded input copy live in
    // indexInfoArena, which is alive through the loop.
    std::pmr::monotonic_buffer_resource indexInfoArena;
    luxir::api::IndexInfo indexInfo;

    auto bytes = manifest ? manifest : Manifest::load(dir).bytes;
    if (!bytes) {
      schema = preferredSchema ? preferredSchema : Schema::createDefaultSchema();
    } else {
      indexInfo = Manifest::decode(bytes, indexInfoArena);
      if (!indexInfo.schema || indexInfo.incarnation.empty()) {
        throw std::runtime_error("Incomplete commit manifest");
      }
      core->incarnation = indexInfo.incarnation;
      if (previous && previous->incarnation != indexInfo.incarnation) {
        previous = nullptr;
        prevAuxByName.clear();
        prevSegAuxByKey.clear();
      }
      if (preferredSchema && preferredSchema->gen_ == indexInfo.schema_gen) {
        schema = preferredSchema;
      } else if (previous && previousReader->schema()->gen_ == indexInfo.schema_gen) {
        schema = previousReader->schema();
      } else {
        schema = Schema::fromStored(*indexInfo.schema, indexInfo.schema_gen);
      }
      indexGen = indexInfo.index_gen;
      core->segmentFiles.clear();
      core->auxFiles.clear();
      for (const auto& segment : indexInfo.segments) {
        auto& files = core->segmentFiles.emplace_back();
        files.segId = segment.seg_id;
        std::string liveName = segment.live_gen == 0 ? std::string()
            : Postings::getLiveDocsFileName(Postings::getSortableString(segment.seg_id), segment.live_gen);
        for (const auto& file : segment.files) {
          (file.name == liveName ? files.liveDocs : files.files)
              .push_back({std::string(file.name), file.size, file.xxh3});
        }
        for (const auto& overlay : segment.overlays) files.overlays.push_back(fromWire(overlay.files));
      }
      for (const auto& aux : indexInfo.aux_indexes) core->auxFiles.push_back(fromWire(aux.files));
      if (previous && previous->segmentFiles == core->segmentFiles && previous->auxFiles == core->auxFiles) {
        return {previousReader->core, std::move(schema), indexInfo.commit_time, indexGen};
      }
      commitTimeUs = indexInfo.commit_time;
      core->coreGeneration = indexInfo.core_gen;
      bool missingFileOK = !manifest; // Allow missing files on first attempt
      IREADER_DEBUG("\tOpening IndexReader, commitTime={} nSegs={} gen={}", indexInfo.commit_time,
                    indexInfo.segments.size(), indexInfo.index_gen);
      if (commitTimeUs == lastCommitTime) {
        // No new commit, continue with missingFileOK=false so we get proper exceptions
        IREADER_DEBUG("Retry index open did not get new IndexInfo file, will try with missingFileOK=false.");
        missingFileOK = false; // On retry, we expect files to be present
      }
      lastCommitTime = commitTimeUs;

      segs.reserve(indexInfo.segments.size());
      for (int i = 0; i < (int)indexInfo.segments.size(); i++) {
        const auto& segment = indexInfo.segments[i];
        uint64_t segId = segment.seg_id;
        uint64_t liveGen = segment.live_gen;
        int32_t nDocs = segment.max_doc;
        const auto& files = core->segmentFiles[i];

        // Every holder of a physical segment shares one open PostingsReader.
        // The publisher's (the writer's) comes first: it opened the segment for
        // deletes, a merge or an overlay build, possibly while a reader opened
        // it too, so preferring it converges all holders by the next reopen.
        // Then the previous snapshot's, when the base files are identical (a
        // delete or overlay commit leaves them unchanged); without descriptors
        // the identity is unknown and nothing is shared. Live docs carry over
        // when the generation's file is unchanged.
        const Segment* prevSeg = previous ? previous->segment(segId) : nullptr;
        const auto* prevFiles = prevSeg ? &previous->segmentFiles[(size_t)prevSeg->ord] : nullptr;
        std::shared_ptr<PostingsReader> postingsReader;
        if ((size_t)i < published.size()) {
          postingsReader = published[i].lock();
          if (postingsReader && postingsReader->maxDoc() != nDocs) postingsReader = nullptr;
        }
        if (!postingsReader && prevSeg && !files.files.empty() && prevFiles->files == files.files) {
          postingsReader = prevSeg->sharedPostingsReader;
        }
        if (!postingsReader) postingsReader = PostingsReader::create(dir, segId, missingFileOK, true);
        if (!postingsReader) {
          // Failed to create PostingsReader (segment files not found) - trigger retry
          retry = true;
          break; // Break out of the segments loop to retry
        }

        std::shared_ptr<LiveDocs> liveDocs;
        if (liveGen > 0 && prevSeg && prevSeg->sharedLiveDocs && !files.liveDocs.empty()
            && prevFiles->liveDocs == files.liveDocs) {
          liveDocs = prevSeg->sharedLiveDocs;
        } else if (liveGen > 0) {
          liveDocs = LiveDocs::create(dir, segId, liveGen, nDocs, missingFileOK, true);
          if (!liveDocs) {
            // Failed to create LiveDocs (delete file not found) - trigger retry
            retry = true;
            break; // Break out of the segments loop to retry
          }
        }
        // If liveGen == 0, liveDocs remains nullptr (no deletes)

        std::vector<std::shared_ptr<AuxReader>> segmentAuxReaders;
        segmentAuxReaders.reserve(segment.overlays.size());
        for (int j = 0; j < (int)segment.overlays.size(); j++) {
          const auto& info = segment.overlays[j];

          auto prevIt = prevSegAuxByKey.find(segmentAuxKey(segId, info.name));
          if (prevIt != prevSegAuxByKey.end()
              && prevIt->second->getGen() == info.gen
              && prevIt->second->getBuiltCoreGen() == info.built_core_gen) {
            IREADER_DEBUG("Reusing segment overlay '{}' for seg={} (gen={}) from previous IndexReader",
                          info.name, segId, info.gen);
            segmentAuxReaders.push_back(prevIt->second);
            continue;
          }

          auto aux = openKnownAux(dir, info, missingFileOK);
          if (!aux && (info.kind == VectorAuxReader::KIND
                       || info.kind == TestOverlayAuxReader::KIND)) {
            IREADER_DEBUG("Segment overlay file missing for '{}' seg={} - triggering retry",
                          info.name, segId);
            retry = true;
            break;
          }
          if (aux) {
            segmentAuxReaders.push_back(std::move(aux));
          }
        }
        if (retry) break;

        Segment::SegmentInfo segmentInfo;
        segmentInfo.seg_id = segId;
        segmentInfo.live_gen = liveGen;
        segmentInfo.min_version = segment.min_version;
        segmentInfo.max_version = segment.max_version;
        segmentInfo.max_doc = nDocs;
        segmentInfo.commit_time = segment.commit_time;
        segmentInfo.live_docs = segment.live_docs;

        segs.emplace_back(std::move(postingsReader), std::move(liveDocs),
                          std::move(segmentAuxReaders), segmentInfo, totalMaxDoc, i);
        totalMaxDoc += segs.back().postingsReader().maxDoc();
        livedocs += segs.back().numLive();
        assert(nDocs == segs.back().postingsReader().maxDoc());
      }

      // Aux indexes: open inside the same retry loop so a missing aux file
      // converts to a re-parse of IndexInfo (writer deletes orphaned aux files
      // only after publishing the new IndexInfo, so the retry will see a
      // referenceable list).  Unknown-kind entries are skipped silently.
      if (!retry) {
        for (int i = 0; i < (int)indexInfo.aux_indexes.size(); i++) {
          const auto& info = indexInfo.aux_indexes[i];

          // Reuse from the previous reader when name + gen + built_core_gen
          // all match - the writer carry-forward logic guarantees the files
          // are byte-identical in that case.
          auto prevIt = prevAuxByName.find(std::string(info.name));
          if (prevIt != prevAuxByName.end()
              && prevIt->second->getGen() == info.gen
              && prevIt->second->getBuiltCoreGen() == info.built_core_gen) {
            IREADER_DEBUG("Reusing aux reader '{}' (gen={}) from previous IndexReader",
                          info.name, info.gen);
            auxReadersList.push_back(prevIt->second);
            continue;
          }

          // Dispatch by kind.  Unknown kinds are silently skipped so older
          // binaries can read indexes that contain newer aux kinds.
          std::shared_ptr<AuxReader> aux = openKnownAux(dir, info, missingFileOK);
          if (!aux && info.kind != VectorAuxReader::KIND
                   && info.kind != TestOverlayAuxReader::KIND && info.kind != OrdMapStore::KIND) {
            continue;
          }

          if (!aux) {
            IREADER_DEBUG("Aux file missing for '{}' - triggering retry", info.name);
            retry = true;
            break;
          }
          auxReadersList.push_back(std::move(aux));
        }
      }
    }
  }
  while (retry);

  core->segOrds.reserve(segs.size());
  for (const auto& seg : segs) core->segOrds.emplace_back(seg.segInfo.seg_id, seg.ord);
  std::ranges::sort(core->segOrds);

  // Check if we can share ordMaps from the previous reader
  if (previous && previous->coreGeneration == core->coreGeneration) {
    IREADER_DEBUG("Sharing ordMaps from previous IndexReader (coreGen={})", core->coreGeneration);
    core->ordMaps = previous->ordMaps;
  } else {
    IREADER_DEBUG("Creating new ordMaps cache (coreGen={})", core->coreGeneration);
    core->ordMaps = std::make_shared<SharedLazyMap<std::string, OrdMap>>();
  }
  // Persisted maps serve this core only if built for exactly its segments.
  for (const auto& aux : auxReadersList) {
    if (aux->getKind() != OrdMapStore::KIND) continue;
    std::vector<OrdMapStore::Segment> segments;
    segments.reserve(segs.size());
    for (size_t i = 0; i < segs.size(); i++) {
      segments.push_back({segs[i].segInfo.seg_id, segs[i].maxDoc(),
                          OrdMapStore::segmentDigest(core->segmentFiles[i].files)});
    }
    auto store = std::static_pointer_cast<OrdMapStore>(aux);
    if (store->matches(core->coreGeneration, segments)) {
      core->ordMapStore = std::move(store);
    } else {
      LOG_WARN("Ignoring persisted OrdMaps '{}': built for another segment set (coreGen={})",
               aux->getName(), core->coreGeneration);
    }
  }

  IREADER_DEBUG("IndexReader opened with {} segments and {} docs, commitTime={}", segs.size(), totalMaxDoc, commitTimeUs);
  return {std::move(core), std::move(schema), commitTimeUs, indexGen};
}




std::shared_ptr<OrdMap> IndexReader::getOrdMap(std::string_view field) {
  return core->ordMaps->getOrCreate(std::string(field), [this, field]() {
    if (core->ordMapStore && core->ordMapStore->contains(field)) {
      if (auto loaded = core->ordMapStore->load(field)) return loaded;
    }
    return OrdMap::build(field, *this);
  });
}

std::vector<std::string> IndexReader::cachedOrdMapFields() const {
  std::vector<std::string> fields;
  core->ordMaps->dataMap.cvisit_all([&](const auto& entry) {
    if (std::holds_alternative<std::shared_ptr<OrdMap>>(entry.second)) fields.push_back(entry.first);
  });
  std::ranges::sort(fields);
  return fields;
}

const Segment* IndexReader::PhysicalCore::segment(uint64_t segId) const {
  auto it = std::ranges::lower_bound(segOrds, segId, {}, &std::pair<uint64_t, int32_t>::first);
  return it != segOrds.end() && it->first == segId ? &segs[(size_t)it->second] : nullptr;
}

std::span<const IndexReader::ProjectableField> IndexReader::PhysicalCore::projectableFields() {
  std::call_once(projectableOnce, [this] {
    // Both name sources are views into segment files held open by the
    // segment's PostingsReader: the field index (fi.fieldname) and the stored
    // resource's metadata (StoredFieldsReader::fieldNames).  No copies needed.
    std::map<std::string_view, ProjectableField> sources;
    for (auto& seg : segs) {
      auto& postingsReader = seg.postingsReader();
      auto poolGuard = MemPool::threadLocalPoolGuard();
      FieldReader fieldReader(postingsReader);
      while (fieldReader.readNextField()) {
        SegFieldInfo fi;
        fieldReader.readFieldInfo(fi);
        std::string_view physicalName(fi.fieldname.data(), fi.fieldname.size());
        if (fi.type == FieldType::BIN && (fi.flags & FieldType::STORED)) {
          // A stored-fields resource: its metadata lists the fields it holds.
          // An empty resource is never written, but guard the reader's
          // precondition anyway.
          if (fi.numValues <= 0) continue;
          StoredFieldsReader sfr(postingsReader, fi);
          for (std::string_view name : sfr.fieldNames()) {
            auto& resources = sources[name].storedResources;
            if (std::ranges::find(resources, physicalName) == resources.end()) {
              resources.push_back(physicalName);
            }
          }
        } else if (fi.flags & FieldType::COLUMN_STORED) {
          sources[physicalName].column = true;
        }
      }
    }
    std::vector<ProjectableField> fields;
    fields.reserve(sources.size());
    for (auto& [name, source] : sources) {
      source.name = name;
      fields.push_back(std::move(source));
    }
    projectable = std::move(fields);
  });
  return projectable;
}

std::span<const IndexReader::RetrievableField> IndexReader::retrievableFields() {
  assert(sharedSchema);
  std::call_once(retrievableOnce, [this] {
    std::vector<RetrievableField> fields;
    for (const auto& source : projectableFields()) {
      auto split = source.name.rfind("__");
      bool derived = split != std::string_view::npos;
      auto root = derived ? source.name.substr(0, split) : source.name;
      if (root.empty() || root[0] == '_') continue;
      FieldType* type;
      try {
        type = sharedSchema->getFieldTypePtr(source.name);  // physical primary or variant, never a binding
      } catch (const RequestError&) {
        // A template edit may make an old concrete root invalid for this schema.
        continue;
      }
      if (!type) continue;
      // Variants have no stored copy; only a primary's own source admits a root.
      bool stored = !derived && type->isStored() && std::ranges::find(source.storedResources,
          std::string_view(type->storedResource_)) != source.storedResources.end();
      bool retrievable = false;
      switch (type->type()) {
        case FieldType::TEXT:
          retrievable = stored;
          break;
        case FieldType::STRING:
        case FieldType::ID:
          // Stored STRING/ID also fall back to their own older segment columns
          // even if the current schema no longer enables a column.
          retrievable = stored || (source.column && (type->isStored() || type->hasColumn()));
          break;
        case FieldType::INT:
        case FieldType::FLOAT:
        case FieldType::DOUBLE:
        case FieldType::DATE:
          retrievable = source.column && type->hasColumn();
          break;
        default:
          break;
      }
      if (retrievable) fields.push_back({source.name, type, derived});
    }
    retrievable = std::move(fields);
  });
  return retrievable;
}

}
