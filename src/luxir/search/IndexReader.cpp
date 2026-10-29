// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "IndexReader.h"
#include "luxir/store/Manifest.h"
#include "luxir/index/CommitSnapshot.h"
#include "luxir/reader/FieldReader.h"
#include "luxir/reader/FieldMetadataMemory.h"
#include "luxir/reader/Postings.h"
#include "luxir/reader/TestOverlayAuxReader.h"
#include "luxir/reader/VectorAuxReader.h"
#include "luxir/schema/Schema.h"
#include "luxir/util/MemPool.h"

#include "luxir/api/padded_input.h"
#include "luxir/api/luxir_index.hpp"
#include <boost/unordered/unordered_flat_map.hpp>
#include <algorithm>
#include <bit>
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
        totalMaxDoc += segs.back().maxDoc();
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
                   && info.kind != TestOverlayAuxReader::KIND) {
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

  // Base-segment metadata is unchanged by deletes and overlays.
  if (previous && previous->coreGeneration == core->coreGeneration) {
    core->fields = previous->fields;
  } else {
    core->fields = std::make_shared<FieldRecords>();
  }

  IREADER_DEBUG("IndexReader opened with {} segments and {} docs, commitTime={}", segs.size(), totalMaxDoc, commitTimeUs);
  return {std::move(core), std::move(schema), commitTimeUs, indexGen};
}




const Segment* IndexReader::PhysicalCore::segment(uint64_t segId) const {
  auto it = std::ranges::lower_bound(segOrds, segId, {}, &std::pair<uint64_t, int32_t>::first);
  return it != segOrds.end() && it->first == segId ? &segs[(size_t)it->second] : nullptr;
}

IndexReader::FieldRecord::FieldRecord(std::string_view name, size_t hash, size_t numSegments,
                                      size_t first, int32_t ordinal)
    : name(name), hash(hash), numSegments(numSegments),
      ordinals(std::make_unique<std::atomic<int32_t>[]>(numSegments)) {
  for (size_t i = 0; i < numSegments; i++) {
    ordinals[i].store(i < first ? -1 : UNRESOLVED, std::memory_order_relaxed);
  }
  ordinals[first].store(ordinal, std::memory_order_relaxed);
  bytes = sizeof(FieldRecord) + numSegments * sizeof(std::atomic<int32_t>)
      + FieldRecords::BYTES_PER_RECORD
      + (name.size() > std::string().capacity() ? name.size() + 1 : 0);
  fieldMetadataMemory.names.fetch_add(1, std::memory_order_relaxed);
  fieldMetadataMemory.nameBytes.fetch_add(bytes, std::memory_order_relaxed);
}

IndexReader::FieldRecord::~FieldRecord() {
  auto* resolved = totals.load(std::memory_order_relaxed);
  fieldMetadataMemory.names.fetch_sub(1, std::memory_order_relaxed);
  fieldMetadataMemory.nameBytes.fetch_sub(bytes + (resolved ? sizeof(*resolved) : 0), std::memory_order_relaxed);
  delete resolved;
}

IndexReader::FieldRecords::Table::Table(size_t capacity)
    : mask(capacity - 1), slots(std::make_unique<std::atomic<const FieldRecord*>[]>(capacity)) {
  assert(std::has_single_bit(capacity));
}

// Linear probing; tables stay at most half full, so every probe ends.
const IndexReader::FieldRecord* IndexReader::FieldRecords::Table::find(
    std::string_view name, size_t hash) const noexcept {
  for (size_t i = hash & mask;; i = (i + 1) & mask) {
    const FieldRecord* record = slots[i].load(std::memory_order_acquire);
    if (!record) return nullptr;
    if (record->hash == hash && record->name == name) return record;
  }
}

void IndexReader::FieldRecords::Table::place(const FieldRecord* record) noexcept {
  size_t i = record->hash & mask;
  while (slots[i].load(std::memory_order_relaxed)) i = (i + 1) & mask;
  slots[i].store(record, std::memory_order_release);
}

const IndexReader::FieldRecord* IndexReader::FieldRecords::insert(std::unique_ptr<FieldRecord> record) {
  std::lock_guard lock(insertMutex);
  Table* table = tables.empty() ? nullptr : tables.back().get();
  if (table) {
    if (auto* existing = table->find(record->name, record->hash)) return existing;
  }
  size_t needed = 2 * (records.size() + 1);
  if (!table || needed > table->mask + 1) {
    tables.push_back(std::make_unique<Table>(std::max<size_t>(16, std::bit_ceil(needed))));
    table = tables.back().get();
    for (const auto& kept : records) table->place(kept.get());
  }
  const FieldRecord* kept = record.get();
  records.push_back(std::move(record));
  table->place(kept);
  current.store(table, std::memory_order_release);
  return kept;
}

IndexReader::FieldSegments IndexReader::field(std::string_view name) const {
  size_t hash = PackedTermHash{}(name);
  const FieldRecord* record = core->fields->find(name, hash);
  if (!record) {
    for (size_t i = 0; i < core->segs.size(); i++) {
      int32_t ordinal = core->segs[i].postingsReader().findOrdinal(name);
      if (ordinal < 0) continue;
      record = core->fields->insert(
          std::make_unique<FieldRecord>(name, hash, core->segs.size(), i, ordinal));
      break;
    }
  }
  if (!record) return {};
  FieldSegments result(record, core->segs.data());
  if (sharedSchema) {
    if (auto* expected = sharedSchema->getFieldTypePtr(name)) {
      const auto& persisted = result.totals();
      if (expected->persistedType() != persisted.type
          || expected->segmentTypeFlags() != FieldType::typeParameters(persisted.type, persisted.typeFlags)) {
        throw RequestError(std::format(
            "Field '{}': schema {} does not match persisted {}",
            name, FieldType::describeType(expected->persistedType(), expected->segmentTypeFlags()),
            FieldType::describeType(persisted.type, persisted.typeFlags)),
            "field_type_mismatch");
      }
    }
  }
  return result;
}

std::shared_ptr<OrdMap> IndexReader::getOrdMap(std::string_view name) {
  auto fields = field(name);
  if (!fields.exists()) return {};
  return fields.record->ordMap.getOrCreate([&] { return OrdMap::build(fields, *this); });
}

std::string_view IndexReader::FieldSegments::name() const noexcept {
  return record ? std::string_view(record->name) : std::string_view{};
}

int32_t IndexReader::FieldSegments::ordinalAt(size_t segment) const {
  assert(record);
  assert(segment < record->numSegments);
  auto& postings = segments[segment].postingsReader();
  int32_t ordinal = record->ordinals[segment].load(std::memory_order_relaxed);
  if (ordinal == FieldRecord::UNRESOLVED) {
    ordinal = postings.findOrdinal(record->name);
    record->ordinals[segment].store(ordinal, std::memory_order_relaxed);
  }
  return ordinal;
}

const PostingsReader::Field* IndexReader::FieldSegments::at(size_t segment) const {
  return record ? segments[segment].postingsReader().fieldInfo(ordinalAt(segment)) : nullptr;
}

bool IndexReader::FieldSegments::exists() const noexcept {
  return record != nullptr;
}

size_t IndexReader::FieldSegments::size() const noexcept {
  return record ? record->numSegments : 0;
}

const IndexReader::HeaderTotals& IndexReader::FieldSegments::totals() const {
  assert(record);
  auto* totals = record->totals.load(std::memory_order_acquire);
  if (!totals) {
    auto fresh = std::make_unique<HeaderTotals>();
    std::optional<SegFieldInfo> first;
    for (size_t i = 0; i < size(); i++) {
      fresh->stats.maxDoc += segments[i].maxDoc();
      int32_t ordinal = ordinalAt(i);
      if (ordinal < 0) continue;
      SegFieldInfo info{};
      FieldReader fields(segments[i].postingsReader(), ordinal);
      fields.readFieldInfo(info);
      if (first) first->requireCompatibleType(info);
      else {
        first = info;
        fresh->type = info.type;
        fresh->typeFlags = FieldType::typeParameters(info.type, info.typeFlags);
      }
      fresh->stats.docsWithField += info.docsWithField;
      fresh->stats.sumDocFreq += info.sumDocFreq;
      fresh->stats.sumTotalTermFreq += info.sumTotalTermFreq;
      fresh->flags |= info.flags;
    }
    if (record->totals.compare_exchange_strong(totals, fresh.get(),
                                               std::memory_order_release,
                                               std::memory_order_acquire)) {
      totals = fresh.release();
      fieldMetadataMemory.nameBytes.fetch_add(sizeof(*totals), std::memory_order_relaxed);
    }
  }
  return *totals;
}

const Similarity::FieldStats& IndexReader::FieldSegments::stats() const {
  return totals().stats;
}

FieldType::flag_type IndexReader::FieldSegments::flagsUnion() const {
  return record ? totals().flags : 0;
}

void IndexReader::PhysicalCore::buildFieldCatalogs() {
  callOnce(fieldCatalogsOnce, [this] {
    // Both name sources are views into segment files held open by the
    // segment's PostingsReader: the field index (fi.fieldname) and the stored
    // resource's cached metadata. No copies needed.
    std::map<std::string_view, ProjectableField> sources;
    for (auto& seg : segs) {
      auto& postingsReader = seg.postingsReader();
      auto poolGuard = MemPool::threadLocalPoolGuard();
      FieldReader fieldReader(postingsReader);
      for (int32_t ordinal = 0; fieldReader.readNextField(); ordinal++) {
        SegFieldInfo fi;
        fieldReader.readFieldInfo(fi);
        std::string_view physicalName(fi.fieldname.data(), fi.fieldname.size());
        if (fi.type == FieldType::BIN && (fi.flags & FieldType::STORED)) {
          // A stored-fields resource: its metadata lists the fields it holds.
          // An empty resource is never written, but guard the reader's
          // precondition anyway.
          if (fi.numValues <= 0) continue;
          const auto* field = postingsReader.fieldInfo(ordinal);
          for (std::string_view name : field->stored->fieldNames) {
            auto& resources = sources[name].storedResources;
            if (std::ranges::find(resources, physicalName) == resources.end()) {
              resources.push_back(physicalName);
            }
          }
        } else if (fi.type != FieldType::BIN) {
          sources[physicalName].column |= (fi.flags & FieldType::COLUMN_STORED) != 0;
        }
      }
    }
    std::vector<ProjectableField> fields;
    std::vector<std::string_view> names;
    fields.reserve(sources.size());
    names.reserve(sources.size());
    for (auto& [name, source] : sources) {
      names.push_back(name);
      if (source.column || !source.storedResources.empty()) {
        source.name = name;
        fields.push_back(std::move(source));
      }
    }
    projectable = std::move(fields);
    physicalNames = std::move(names);
  });
}

std::span<const IndexReader::RetrievableField> IndexReader::retrievableFields() {
  assert(sharedSchema);
  callOnce(retrievableOnce, [this] {
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

std::span<const std::string> IndexReader::searchableFields() {
  assert(sharedSchema);
  callOnce(searchableOnce, [this] {
    searchable = sharedSchema->searchableFields(core->physicalFieldNames());
  });
  return searchable;
}

} // namespace luxir
