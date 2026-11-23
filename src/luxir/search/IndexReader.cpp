// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "IndexReader.h"
#include "luxir/store/AcquireJoin.h"
#include "luxir/store/Manifest.h"
#include "luxir/index/CommitSnapshot.h"
#include "luxir/reader/FieldMetadataMemory.h"
#include "luxir/reader/Postings.h"
#include "luxir/reader/OwnershipMapper.h"
#include "luxir/reader/ReaderErrors.h"
#include "luxir/reader/TestOverlayAuxReader.h"
#include "luxir/reader/VectorAuxReader.h"
#include "luxir/schema/Schema.h"
#include "luxir/schema/SchemaCache.h"
#include "luxir/util/ApiError.h"
#include "luxir/util/MemPool.h"
#include "luxir/util/IoContext.h"
#include "luxir/util/log.h"

#include "luxir/api/padded_input.h"
#include "luxir/api/luxir_index.hpp"
#include <boost/unordered/unordered_flat_map.hpp>
#include <algorithm>
#include <bit>
#include <cstring>
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

OverlayPolicy overlayPolicyOf(std::string_view kind) {
  if (kind == TestOverlayAuxReader::KIND) return TestOverlayAuxReader::policyForTests;
  return OverlayPolicy::ON_DEMAND;
}

} // namespace

bool SegmentOverlay::knownKind(std::string_view kind) {
  return kind == VectorAuxReader::KIND || kind == TestOverlayAuxReader::KIND;
}

std::shared_ptr<SegmentOverlay> SegmentOverlay::open(Directory& dir, const luxir::api::AuxIndexInfo& entry,
                                                     std::string segment, bool missingFileOK) {
  assert(knownKind(entry.kind));
  std::vector<std::shared_ptr<InputFile>> files;
  files.reserve(entry.files.size());
  for (const auto& descriptor : entry.files) {
    // Overlay files were synced before the manifest naming them was published.
    auto file = dir.openFile(descriptor.name, /*expectSynced=*/true, descriptor.size);
    if (!file) {
      if (missingFileOK) {
        IREADER_DEBUG("Overlay file {} missing for '{}', will retry", descriptor.name, entry.name);
        return nullptr;
      }
      throw std::filesystem::filesystem_error(
          std::format("Missing overlay file '{}' for overlay '{}'", descriptor.name, entry.name),
          std::make_error_code(std::errc::no_such_file_or_directory));
    }
    files.push_back(std::move(file));
  }
  return std::make_shared<SegmentOverlay>(fromWire(entry), overlayPolicyOf(entry.kind), std::move(segment),
                                          std::move(files));
}

std::shared_ptr<AuxReader> SegmentOverlay::get() {
  return reader.getOrCreate([this]() -> std::shared_ptr<AuxReader> {
    try {
      std::pmr::monotonic_buffer_resource arena;
      auto entry = toWire(info, arena);
      if (files.size() != 1) throw std::runtime_error(std::format("expected 1 file, got {}", files.size()));
      if (info.kind == VectorAuxReader::KIND) return VectorAuxReader::open(files.front(), entry);
      return TestOverlayAuxReader::open(files.front(), entry);
    } catch (const ApiError&) {
      throw;  // a failed read or a resource limit: the next use retries
    } catch (const std::system_error&) {
      throw;
    } catch (const std::bad_alloc&) {
      throw;
    } catch (const std::exception& e) {
      if (policy == OverlayPolicy::ADVISORY) {
        LOG_WARN("Advisory overlay '{}' of segment {} is unusable and will answer nothing: {}",
                 info.name, segment, e.what());
        return std::make_shared<UnavailableAuxReader>(info.kind, info.name, info.gen, info.built_core_gen);
      }
      if (dynamic_cast<const SegmentCorruptError*>(&e)) throw;
      corruptSegment(segment, std::format("overlay '{}': {}", info.name, e.what()));
    }
  });
}

LiveDocs::LiveDocs(std::unique_ptr<uint64_t[]> words, int32_t maxDoc, int32_t numLive)
    : ownedWords(std::move(words)), docSet(FixedBitSet(ownedWords.get(), maxDoc), numLive),
      derivedBytes(sizeof(*this) + FixedBitSet::sizeInWords(maxDoc) * sizeof(uint64_t)) {
  fieldMetadataMemory.liveViews.fetch_add(1, std::memory_order_relaxed);
  fieldMetadataMemory.liveViewBytes.fetch_add(derivedBytes, std::memory_order_relaxed);
}

LiveDocs::~LiveDocs() {
  if (derivedBytes) {
    fieldMetadataMemory.liveViews.fetch_sub(1, std::memory_order_relaxed);
    fieldMetadataMemory.liveViewBytes.fetch_sub(derivedBytes, std::memory_order_relaxed);
  }
}

std::shared_ptr<LiveDocs> ChildLiveDocs::get() {
  return value.getOrCreate([&]() -> std::shared_ptr<LiveDocs> {
    auto rootLive = root->shared();
    OwnershipMapper mapper(*core, "", path);
    int32_t count = core->spaceReader(path)->maxDoc();
    auto words = FixedBitSet::allocate(count, true);
    int32_t live = count;
    // Only dead roots decode individual ranges; all-live words need no mapping.
    for (int32_t word = 0; word < (int32_t)FixedBitSet::sizeInWords(rootLive->size()); word++) {
      int32_t end = (int32_t)std::min((int64_t)(word + 1) * 64, (int64_t)rootLive->size());
      uint64_t dead = ~rootLive->bitset().words[word];
      if (end & 63) dead &= (1ULL << (end & 63)) - 1;
      while (dead) {
        int32_t parent = word * 64 + (int32_t)std::countr_zero(dead);
        dead &= dead - 1;
        auto [first, last] = mapper.range(parent);
        live -= last - first;
        // Disjoint ranges clear each full child word at most once. Partial
        // words handle the family boundaries without visiting child rows.
        while (first < last && (first & 63)) {
          words[first >> 6] &= ~(1ULL << (first & 63));
          ++first;
        }
        int32_t fullEnd = last & ~63;
        if (first < fullEnd) {
          std::fill(words.get() + (first >> 6), words.get() + (fullEnd >> 6), 0);
          first = fullEnd;
        }
        if (first < last) words[first >> 6] &= ~((1ULL << (last - first)) - 1);
      }
    }
    auto result = live == count ? nullptr : std::make_shared<LiveDocs>(std::move(words), count, live);
    knownCard.store(live, std::memory_order_release);
    return result;
  });
}

std::shared_ptr<SegmentLive> SegmentLive::open(Directory& dir, uint64_t segId, uint64_t creatorEpoch, uint64_t liveGen,
                                               uint64_t liveEpoch, int32_t maxDoc, int32_t numLive, uint64_t size,
                                               bool missingFileOK, bool expectSynced) {
  assert(liveGen > 0 && maxDoc > 0);
  auto segment = Postings::segmentKey(segId, creatorEpoch);
  auto name = Postings::getLiveDocsFileName(segment, liveGen, liveEpoch);
  auto file = dir.openFile(name, expectSynced, size);
  if (!file) {
    if (missingFileOK) {
      IREADER_DEBUG("Delete file {} not found for segment s{} (liveGen={})", name, segment, liveGen);
      return nullptr;
    }
    throw std::filesystem::filesystem_error(
        std::format("Delete file {} not found for segment s{} (liveGen={})", name, segment, liveGen),
        std::make_error_code(std::errc::no_such_file_or_directory));
  }
  // As for segment files: trust the manifest's size unless the file is local.
  if (!size || file->holdsFree()) size = file->size();
  return std::make_shared<SegmentLive>(std::move(file), std::move(name), std::move(segment), size, maxDoc, numLive);
}

std::shared_ptr<LiveDocs> LiveDocs::create(Directory& dir, uint64_t segId, uint64_t creatorEpoch, uint64_t liveGen,
                                           uint64_t liveEpoch, int32_t maxDoc, bool missingFileOK, bool expectSynced) {
  auto live = SegmentLive::open(dir, segId, creatorEpoch, liveGen, liveEpoch, maxDoc, -1, 0, missingFileOK, expectSynced);
  return live ? live->shared() : nullptr;
}

std::shared_ptr<LiveDocs> SegmentLive::shared() {
  return value.getOrCreate([this] { return LiveDocs::decode(file, fileSize, name, segment, maxDoc, liveCount); });
}

bool SegmentLive::acquireAhead(AcquireBatches& batches) {
  if (!file || decoded() || file->holdsFree() || fileSize < Postings::LUXIR_HEADER.size()) return false;
  ByteRange whole[] = {{0, fileSize}};
  countAcquire(fileSize);
  batches.acquire(*file, whole, [this](std::vector<HeldRange>&& holds) {
    if (holds.size() != 1) return;
    try {
      value.getOrCreate([&] { return LiveDocs::decode(file, std::move(holds[0]), name, segment, maxDoc, liveCount); });
    } catch (const std::exception& e) {
      LOG_DEBUG("live docs '{}' acquired ahead failed to decode; the first use retries: {}", name, e.what());
    }
  });
  return true;
}

std::shared_ptr<LiveDocs> LiveDocs::decode(std::shared_ptr<InputFile> file, uint64_t size, std::string_view name,
                                           std::string_view segment, int32_t maxDoc, int32_t numLive) {
  constexpr auto headerSize = Postings::LUXIR_HEADER.size() + sizeof(uint64_t) + sizeof(int32_t) + sizeof(int32_t);
  if (size < headerSize) corruptSegment(segment, std::format("live docs file '{}': too small: {} bytes", name, size));
  auto hold = file->acquire({0, size});
  return decode(std::move(file), std::move(hold), name, segment, maxDoc, numLive);
}

std::shared_ptr<LiveDocs> LiveDocs::decode(std::shared_ptr<InputFile> file, HeldRange hold, std::string_view name,
                                           std::string_view segment, int32_t maxDoc, int32_t numLive) {
  auto invalid = [&](std::string_view reason) {
    corruptSegment(segment, std::format("live docs file '{}': {}", name, reason));
  };
  uint64_t size = hold.bytes().size();
  constexpr auto headerSize = Postings::LUXIR_HEADER.size() + sizeof(uint64_t) + sizeof(int32_t) + sizeof(int32_t);
  if (size < headerSize) invalid(std::format("too small: {} bytes", size));
  InputStream in = hold.stream();
  if (std::memcmp(in.ptr(), Postings::LUXIR_HEADER.data(), Postings::LUXIR_HEADER.size()) != 0) {
    invalid("invalid magic number");
  }
  in.skip(Postings::LUXIR_HEADER.size());
  uint64_t format = (uint64_t)in.readLong();
  int32_t fileMaxDoc = in.readInt();
  int32_t fileLive = in.readInt();
  if (format != 1) invalid(std::format("unsupported format {}", format));
  if (fileMaxDoc != maxDoc) invalid(std::format("maxDoc {} differs from the segment's {}", fileMaxDoc, maxDoc));
  if (numLive < 0) {
    if (fileLive < 0 || fileLive > maxDoc) invalid(std::format("invalid live count {} (maxDoc={})", fileLive, maxDoc));
    numLive = fileLive;
  } else if (fileLive != numLive) {
    invalid(std::format("live count {} differs from the manifest's {}", fileLive, numLive));
  }
  uint64_t expectedSize = (uint64_t)in.offset() + FixedBitSet::sizeInWords(maxDoc) * sizeof(uint64_t);
  if (size != expectedSize) invalid(std::format("size {} bytes, expected {}", size, expectedSize));

  // The bitmap borrows the bytes from the hold.
  auto* bits = (uint64_t*)in.ptr();
  IREADER_DEBUG("Loaded {} deleted documents from {} for segment s{}, {} live docs",
                maxDoc - numLive, name, segment, numLive);
  return std::make_shared<LiveDocs>(bits, maxDoc, std::move(file), std::move(hold), numLive);
}


IndexReader::IndexReader(OpenedSnapshot opened, std::shared_ptr<FilterCache> filterCache)
  : snapshot(std::make_shared<Snapshot>(std::move(opened), std::move(filterCache))),
    space(snapshot->core->root) {}

IndexReader::IndexReader(std::shared_ptr<Snapshot> snapshot, std::shared_ptr<SpaceState> space)
  : snapshot(std::move(snapshot)), space(std::move(space)) {}

std::shared_ptr<IndexReader> IndexReader::view(std::string_view path) {
  std::lock_guard lock(snapshot->viewsMutex);
  if (auto self = weak_from_this().lock()) snapshot->views[std::string(this->path())] = self;
  auto& cached = snapshot->views[std::string(path)];
  if (auto reader = cached.lock()) return reader;
  if (path == this->path()) {
    if (auto reader = weak_from_this().lock()) {
      cached = reader;
      return reader;
    }
  }
  auto reader = std::shared_ptr<IndexReader>(new IndexReader(snapshot, snapshot->core->space(path)));
  cached = reader;
  return reader;
}

IndexReader::IndexReader(Directory& dir, const CommitSnapshot& snapshot, IndexReader* previousReader,
                         std::shared_ptr<FilterCache> filterCache)
  : IndexReader(openSnapshot(dir, previousReader, snapshot.schema, snapshot.bytes, snapshot.openCores),
                filterCache ? std::move(filterCache)
                    : (previousReader ? previousReader->snapshot->filterCache : nullptr)) {}

IndexReader::IndexReader(Directory& dir, IndexReader* previousReader, std::shared_ptr<FilterCache> filterCache)
  : IndexReader(openSnapshot(dir, previousReader, nullptr, nullptr),
                filterCache ? std::move(filterCache)
                    : (previousReader ? previousReader->snapshot->filterCache : nullptr)) {}

IndexReader::OpenedSnapshot IndexReader::openSnapshot(Directory& dir, const IndexReader* previousReader,
                                                std::shared_ptr<Schema> preferredSchema,
                                                std::shared_ptr<const std::vector<std::byte>> manifest,
                                                std::span<const std::weak_ptr<SegmentCore>> published) {
  assert(published.empty() || manifest);  // aligned with that manifest's segments
  auto core = std::make_shared<PhysicalCore>();
  const auto* previous = previousReader ? previousReader->snapshot->core.get() : nullptr;
  core->root = std::make_shared<SpaceState>();
  auto& segs = core->bundles;
  auto& auxReadersList = core->auxReadersList;
  auto& totalMaxDoc = core->root->totalMaxDoc;
  auto& livedocs = core->root->livedocs;
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
  boost::unordered_flat_map<std::string, std::shared_ptr<SegmentOverlay>> prevSegAuxByKey;
  if (previous) {
    assert(previous->segmentFiles.size() == previous->bundles.size());
    for (const auto& r : previous->auxReadersList) {
      prevAuxByName.emplace(std::string(r->getName()), r);
    }
    for (const auto& seg : previous->bundles) {
      for (const auto& overlay : seg.aux) {
        prevSegAuxByKey.emplace(segmentAuxKey(seg.info.seg_id, overlay->name()), overlay);
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
      if (preferredSchema && preferredSchema->binding == SchemaBinding(*indexInfo.schema)) {
        schema = preferredSchema;
      } else if (previous && previousReader->schema()->binding == SchemaBinding(*indexInfo.schema)) {
        schema = previousReader->schema();
      } else {
        SchemaCache schemas; // standalone readers have no node owner
        schema = schemas.load(dir, indexInfo);
      }
      indexGen = indexInfo.index_gen;
      core->segmentFiles.clear();
      core->auxFiles.clear();
      for (const auto& segment : indexInfo.segments) {
        auto& files = core->segmentFiles.emplace_back();
        files.segId = segment.seg_id;
        files.catalog = catalogRangeFromManifest(segment.catalog);
        files.maxDoc = segment.max_doc;
        std::string liveName = segment.live_gen == 0 ? std::string()
            : Postings::getLiveDocsFileName(Postings::segmentKey(segment.seg_id, segment.creator_epoch), segment.live_gen, segment.live_epoch);
        for (const auto& file : segment.files) {
          (file.name == liveName ? files.liveDocs : files.files)
              .push_back({std::string(file.name), file.size, file.xxh3});
        }
        for (const auto& overlay : segment.overlays) files.overlays.push_back(fromWire(overlay.files));
      }
      for (const auto& aux : indexInfo.aux_indexes) core->auxFiles.push_back(fromWire(aux.files));
      if (previous && previous->segmentFiles == core->segmentFiles && previous->auxFiles == core->auxFiles) {
        return {previousReader->snapshot->core, std::move(schema), indexInfo.commit_time, indexGen};
      }
      commitTimeUs = indexInfo.commit_time;
      core->coreGeneration = indexInfo.core_gen;
      core->commitTime = commitTimeUs;
      bool missingFileOK = !manifest; // Allow missing files on first attempt
      IREADER_DEBUG("\tOpening IndexReader, commitTime={} nSegs={} gen={}", indexInfo.commit_time,
                    indexInfo.segments.size(), indexInfo.index_gen);
      if (commitTimeUs == lastCommitTime) {
        // No new commit, continue with missingFileOK=false so we get proper exceptions
        IREADER_DEBUG("Retry index open did not get new IndexInfo file, will try with missingFileOK=false.");
        missingFileOK = false; // On retry, we expect files to be present
      }
      lastCommitTime = commitTimeUs;

      // Every holder of a physical segment shares one open SegmentCore.
      // The publisher's (the writer's) comes first: it opened the segment for
      // deletes, a merge or an overlay build, possibly while a reader opened
      // it too, so preferring it converges all holders by the next reopen.
      // Then the previous snapshot's, when the base files are identical (a
      // delete or overlay commit leaves them unchanged); without descriptors
      // the identity is unknown and nothing is shared. The rest open together,
      // so their catalogs are acquired in one round.
      std::vector<std::shared_ptr<SegmentCore>> segmentCores(indexInfo.segments.size());
      std::vector<const luxir::api::SegmentInfo*> opening;
      std::vector<const PhysicalCore::Bundle*> prevSegs(indexInfo.segments.size());
      for (size_t i = 0; i < indexInfo.segments.size(); i++) {
        const auto& segment = indexInfo.segments[i];
        const auto& files = core->segmentFiles[i];
        const auto* prevSeg = prevSegs[i] = previous ? previous->bundle(segment.seg_id) : nullptr;
        const auto* prevFiles = prevSeg ? &previous->segmentFiles[(size_t)prevSeg->ord] : nullptr;
        auto& segmentCore = segmentCores[i];
        if (i < published.size()) {
          segmentCore = published[i].lock();
          if (segmentCore && (segmentCore->rootReader().maxDoc() != segment.max_doc
                              || segmentCore->catalogRange() != files.catalog)) segmentCore = nullptr;
        }
        if (!segmentCore && prevSeg && !files.files.empty() && prevFiles->files == files.files
            && prevFiles->catalog == files.catalog && prevFiles->maxDoc == segment.max_doc) {
          segmentCore = prevSeg->core;
        }
        if (!segmentCore) opening.push_back(&segment);
      }
      if (!opening.empty()) {
        auto opened = SegmentCore::create(dir, opening, missingFileOK, true);
        auto next = opened.begin();
        for (auto& segmentCore : segmentCores) {
          if (!segmentCore) segmentCore = std::move(*next++);
        }
      }

      segs.reserve(indexInfo.segments.size());
      for (int i = 0; i < (int)indexInfo.segments.size(); i++) {
        const auto& segment = indexInfo.segments[i];
        uint64_t segId = segment.seg_id;
        uint64_t liveGen = segment.live_gen;
        int32_t nDocs = segment.max_doc;
        const auto& files = core->segmentFiles[i];

        // Live docs carry over when the generation's file is unchanged.
        const auto* prevSeg = prevSegs[(size_t)i];
        const auto* prevFiles = prevSeg ? &previous->segmentFiles[(size_t)prevSeg->ord] : nullptr;
        auto segmentCore = std::move(segmentCores[i]);
        if (!segmentCore) {
          // Failed to create SegmentCore (segment files not found) - trigger retry
          retry = true;
          break; // Break out of the segments loop to retry
        }

        // Opened here and decoded on first use; the live count is the manifest's.
        std::shared_ptr<SegmentLive> liveDocs;
        if (liveGen > 0 && prevSeg && prevSeg->live && !files.liveDocs.empty()
            && prevFiles->liveDocs == files.liveDocs) {
          liveDocs = prevSeg->live;
        } else if (liveGen > 0) {
          uint64_t liveSize = files.liveDocs.empty() ? 0 : files.liveDocs.front().size;
          liveDocs = SegmentLive::open(dir, segId, segment.creator_epoch, liveGen, segment.live_epoch, nDocs,
                                       segment.live_docs, liveSize, missingFileOK, true);
          if (!liveDocs) {
            // Failed to create LiveDocs (delete file not found) - trigger retry
            retry = true;
            break; // Break out of the segments loop to retry
          }
        }
        // If liveGen == 0, liveDocs remains nullptr (no deletes)

        // Overlay files are opened here and their readers on first use.
        std::vector<std::shared_ptr<SegmentOverlay>> segmentAuxReaders;
        segmentAuxReaders.reserve(segment.overlays.size());
        for (int j = 0; j < (int)segment.overlays.size(); j++) {
          const auto& info = segment.overlays[j];
          if (!SegmentOverlay::knownKind(info.kind)) {
            IREADER_DEBUG("Skipping unknown overlay kind '{}' for '{}'", info.kind, info.name);
            continue;
          }

          auto prevIt = prevSegAuxByKey.find(segmentAuxKey(segId, info.name));
          if (prevIt != prevSegAuxByKey.end()
              && prevIt->second->gen() == info.gen
              && prevIt->second->builtCoreGen() == info.built_core_gen) {
            IREADER_DEBUG("Reusing segment overlay '{}' for seg=s{} (gen={}) from previous IndexReader",
                          info.name, Postings::segmentKey(segId, segment.creator_epoch), info.gen);
            segmentAuxReaders.push_back(prevIt->second);
            continue;
          }

          auto overlay = SegmentOverlay::open(dir, info, Postings::segmentKey(segId, segment.creator_epoch),
                                              missingFileOK);
          if (!overlay) {
            IREADER_DEBUG("Segment overlay file missing for '{}' seg=s{} - triggering retry",
                          info.name, Postings::segmentKey(segId, segment.creator_epoch));
            retry = true;
            break;
          }
          segmentAuxReaders.push_back(std::move(overlay));
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

        auto childLives = prevSeg && prevSeg->core == segmentCore && prevSeg->live == liveDocs
            ? prevSeg->childLives : liveDocs && segmentCore->spaces().size() > 1
                ? std::make_shared<PhysicalCore::ChildLives>() : nullptr;
        segs.push_back({std::move(segmentCore), std::move(liveDocs),
                        std::move(segmentAuxReaders), segmentInfo, i, std::move(childLives)});
        assert(nDocs == segs.back().core->rootReader().maxDoc());
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
  for (const auto& seg : segs) core->segOrds.emplace_back(seg.info.seg_id, seg.ord);
  std::ranges::sort(core->segOrds);

  // Base-segment metadata is unchanged by deletes and overlays.
  if (previous && previous->coreGeneration == core->coreGeneration) {
    core->root->fields = previous->root->fields;
  } else {
    core->root->fields = std::make_shared<FieldCache>();
  }

  core->root->segs.reserve(segs.size());
  for (auto& bundle : segs) {
    assert((bundle.info.live_gen != 0) == (bundle.live != nullptr));
    assert(bundle.info.live_gen == 0 || bundle.info.live_docs < bundle.info.max_doc);
    assert(bundle.info.live_docs == (bundle.live ? bundle.live->numLive() : bundle.info.max_doc));
    core->root->segs.emplace_back(std::shared_ptr(bundle.core), std::shared_ptr(bundle.live),
        std::vector(bundle.aux), bundle.info, totalMaxDoc, bundle.ord, &bundle.core->rootReader());
    totalMaxDoc += bundle.info.max_doc;
    livedocs += bundle.info.live_docs;
  }

  IREADER_DEBUG("IndexReader opened with {} segments and {} docs, commitTime={}", segs.size(), totalMaxDoc, commitTimeUs);
  return {std::move(core), std::move(schema), commitTimeUs, indexGen};
}




const IndexReader::PhysicalCore::Bundle* IndexReader::PhysicalCore::bundle(uint64_t segId) const {
  auto it = std::ranges::lower_bound(segOrds, segId, {}, &std::pair<uint64_t, int32_t>::first);
  return it != segOrds.end() && it->first == segId ? &bundles[(size_t)it->second] : nullptr;
}

std::shared_ptr<IndexReader::SpaceState> IndexReader::PhysicalCore::space(std::string_view path) {
  if (path.empty()) return root;
  std::lock_guard lock(spacesMutex);
  auto found = spaces.find(path);
  if (found != spaces.end()) return found->second;
  auto state = std::make_shared<SpaceState>();
  state->path = path;
  state->fields = std::make_shared<FieldCache>();
  state->segs.reserve(bundles.size());
  for (const auto& bundle : bundles) {
    auto* postings = bundle.core->spaceReader(path);
    std::shared_ptr<ChildLiveDocs> live;
    if (postings && bundle.live) {
      std::lock_guard liveLock(bundle.childLives->mutex);
      auto& entry = bundle.childLives->paths[std::string(path)];
      if (!entry) entry = std::make_shared<ChildLiveDocs>(bundle.core, bundle.live, std::string(path));
      live = entry;
    }
    // Segment metadata remains the root manifest metadata. Row counts and
    // readers belong to this space; overlays are shared by the bundle.
    state->segs.emplace_back(std::shared_ptr(bundle.core), nullptr,
        std::vector(bundle.aux), bundle.info,
        state->totalMaxDoc, bundle.ord, postings, std::move(live));
    state->totalMaxDoc += state->segs.back().maxDoc();
  }
  spaces.emplace(path, state);
  return state;
}

IndexReader::FieldNameEntry::FieldNameEntry(std::string_view name, size_t hash, size_t numSegments,
                                      size_t first, int32_t ordinal)
    : name(name), hash(hash), numSegments(numSegments),
      ordinals(std::make_unique<std::atomic<int32_t>[]>(numSegments)) {
  for (size_t i = 0; i < numSegments; i++) {
    ordinals[i].store(i < first ? -1 : UNRESOLVED, std::memory_order_relaxed);
  }
  ordinals[first].store(ordinal, std::memory_order_relaxed);
  bytes = sizeof(FieldNameEntry) + numSegments * sizeof(std::atomic<int32_t>)
      + FieldCache::BYTES_PER_FIELD
      + (name.size() > std::string().capacity() ? name.size() + 1 : 0);
  fieldMetadataMemory.names.fetch_add(1, std::memory_order_relaxed);
  fieldMetadataMemory.nameBytes.fetch_add(bytes, std::memory_order_relaxed);
}

IndexReader::FieldNameEntry::~FieldNameEntry() {
  auto* resolved = totals.load(std::memory_order_relaxed);
  fieldMetadataMemory.names.fetch_sub(1, std::memory_order_relaxed);
  fieldMetadataMemory.nameBytes.fetch_sub(bytes + (resolved ? sizeof(*resolved) : 0), std::memory_order_relaxed);
  delete resolved;
}

IndexReader::FieldCache::Table::Table(size_t capacity)
    : mask(capacity - 1), slots(std::make_unique<std::atomic<const FieldNameEntry*>[]>(capacity)) {
  assert(std::has_single_bit(capacity));
}

// Linear probing; tables stay at most half full, so every probe ends.
const IndexReader::FieldNameEntry* IndexReader::FieldCache::Table::find(
    std::string_view name, size_t hash) const noexcept {
  for (size_t i = hash & mask;; i = (i + 1) & mask) {
    const FieldNameEntry* entry = slots[i].load(std::memory_order_acquire);
    if (!entry) return nullptr;
    if (entry->hash == hash && entry->name == name) return entry;
  }
}

void IndexReader::FieldCache::Table::place(const FieldNameEntry* entry) noexcept {
  size_t i = entry->hash & mask;
  while (slots[i].load(std::memory_order_relaxed)) i = (i + 1) & mask;
  slots[i].store(entry, std::memory_order_release);
}

const IndexReader::FieldNameEntry* IndexReader::FieldCache::insert(std::unique_ptr<FieldNameEntry> entry) {
  std::lock_guard lock(insertMutex);
  Table* table = tables.empty() ? nullptr : tables.back().get();
  if (table) {
    if (auto* existing = table->find(entry->name, entry->hash)) return existing;
  }
  size_t needed = 2 * (entries.size() + 1);
  if (!table || needed > table->mask + 1) {
    tables.push_back(std::make_unique<Table>(std::max<size_t>(16, std::bit_ceil(needed))));
    table = tables.back().get();
    for (const auto& kept : entries) table->place(kept.get());
  }
  const FieldNameEntry* kept = entry.get();
  entries.push_back(std::move(entry));
  table->place(kept);
  current.store(table, std::memory_order_release);
  return kept;
}

IndexReader::FieldSegments IndexReader::field(std::string_view name) const {
  size_t hash = PackedTermHash{}(name);
  const FieldNameEntry* entry = space->fields->find(name, hash);
  if (!entry) {
    for (size_t i = 0; i < space->segs.size(); i++) {
      if (!space->segs[i].hasSpace()) continue;
      int32_t ordinal = space->segs[i].postingsReader().findOrdinal(name);
      if (ordinal < 0) continue;
      entry = space->fields->insert(
          std::make_unique<FieldNameEntry>(name, hash, space->segs.size(), i, ordinal));
      break;
    }
  }
  if (!entry) return {};
  FieldSegments result(entry, space->segs.data());
  if (snapshot->schema) {
    if (auto* expected = snapshot->schema->getFieldTypePtr(name)) {
      const auto& persisted = result.totals();
      if (expected->type() != persisted.type
          || expected->segmentTypeParameters() != persisted.typeParameters) {
        throw RequestError(std::format(
            "Field '{}': schema {} does not match persisted {}",
            name, FieldType::describeType(expected->type(), expected->segmentTypeParameters()),
            FieldType::describeType(persisted.type, persisted.typeParameters)),
            "field_type_mismatch");
      }
    }
  }
  return result;
}

std::shared_ptr<OrdMap> IndexReader::getOrdMap(std::string_view name) {
  auto fields = field(name);
  if (!fields.exists()) return {};
  return fields.entry->ordMap.getOrCreate([&] { return OrdMap::build(fields, *this); });
}

std::string_view IndexReader::FieldSegments::name() const noexcept {
  return entry ? std::string_view(entry->name) : std::string_view{};
}

int32_t IndexReader::FieldSegments::ordinalAt(size_t segment) const {
  assert(entry);
  assert(segment < entry->numSegments);
  int32_t ordinal = entry->ordinals[segment].load(std::memory_order_relaxed);
  if (ordinal == FieldNameEntry::UNRESOLVED) {
    ordinal = segments[segment].hasSpace()
        ? segments[segment].postingsReader().findOrdinal(entry->name) : -1;
    entry->ordinals[segment].store(ordinal, std::memory_order_relaxed);
  }
  return ordinal;
}

const DecodedFieldRecord* IndexReader::FieldSegments::at(size_t segment) const {
  if (!entry) return nullptr;
  int32_t ordinal = ordinalAt(segment);
  return ordinal < 0 ? nullptr : segments[segment].postingsReader().fieldInfo(ordinal);
}

bool IndexReader::FieldSegments::exists() const noexcept {
  return entry != nullptr;
}

size_t IndexReader::FieldSegments::size() const noexcept {
  return entry ? entry->numSegments : 0;
}

const IndexReader::FieldTotals& IndexReader::FieldSegments::totals() const {
  assert(entry);
  auto* totals = entry->totals.load(std::memory_order_acquire);
  if (!totals) {
    auto fresh = std::make_unique<FieldTotals>();
    std::optional<FieldRecordHeader> first;
    for (size_t i = 0; i < size(); i++) {
      fresh->stats.maxDoc += segments[i].maxDoc();
      int32_t ordinal = ordinalAt(i);
      if (ordinal < 0) continue;
      auto header = segments[i].postingsReader().fieldHeader(ordinal);
      if (first) requireCompatibleType(*first, header);
      else {
        first = header;
        fresh->type = header.type;
        fresh->typeParameters = header.typeParameters;
      }
      fresh->stats.docsWithField += header.docsWithField;
      fresh->stats.sumDocFreq += header.indexedTotals.sumDocFreq;
      fresh->stats.sumTotalTermFreq += header.indexedTotals.sumTotalTermFreq;
      fresh->segFlags |= header.segFlags;
    }
    if (entry->totals.compare_exchange_strong(totals, fresh.get(),
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

uint8_t IndexReader::FieldSegments::segFlags() const {
  return entry ? totals().segFlags : 0;
}

void IndexReader::SpaceState::buildFieldNameLists() {
  callOnce(fieldNamesOnce, [this] {
    // Both name sources are views into segment files held open by the
    // segment core's space reader: the field table and the stored
    // resource's cached metadata. No copies needed.
    std::map<std::string_view, ProjectableField> sources;
    for (auto& seg : segs) {
      if (!seg.hasSpace()) continue;
      auto& postingsReader = seg.postingsReader();
      for (int32_t ordinal = 0; ordinal < postingsReader.numFields(); ++ordinal) {
        auto header = postingsReader.fieldHeader(ordinal);
        auto physicalName = std::string_view(header.name);
        if (header.parts.has(FieldPart::STORED)) {
          const auto* field = postingsReader.fieldInfo(ordinal);
          for (auto term : postingsReader.openStoredMetadata(*field)->fieldNames()) {
            auto name = std::string_view(term);
            auto& resources = sources[name].storedResources;
            if (std::ranges::find(resources, physicalName) == resources.end()) {
              resources.push_back(physicalName);
            }
          }
        } else if (FieldType::storage(header.type) != FieldType::Storage::BIN) {
          sources[physicalName].column |= (header.flags & FieldType::COLUMN_STORED) != 0;
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
  assert(snapshot->schema);
  callOnce(retrievableOnce, [this] {
    std::vector<RetrievableField> fields;
    for (const auto& source : projectableFields()) {
      auto split = source.name.rfind("__");
      bool derived = split != std::string_view::npos;
      auto root = derived ? source.name.substr(0, split) : source.name;
      if (root.empty() || root[0] == '_') continue;
      FieldType* type;
      try {
        type = snapshot->schema->getFieldTypePtr(source.name);  // physical primary or variant, never a binding
      } catch (const RequestError&) {
        // A template edit may make an old concrete root invalid for this schema.
        continue;
      }
      if (!type) continue;
      // Variants have no stored copy; only a primary's own source admits a root.
      bool stored = !derived && type->isStored() && std::ranges::find(source.storedResources,
          std::string_view(type->storedResource_)) != source.storedResources.end();
      bool retrievable = false;
      switch (FieldType::storage(type->type())) {
        case FieldType::Storage::TEXT:
          retrievable = stored;
          break;
        case FieldType::Storage::TERMS:
          // Stored STRING/ID also fall back to their own older segment columns
          // even if the current schema no longer enables a column.
          retrievable = stored || (source.column && (type->isStored() || type->hasColumn()));
          break;
        case FieldType::Storage::INT64:
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
  assert(snapshot->schema);
  callOnce(searchableOnce, [this] {
    searchable = snapshot->schema->searchableFields(space->physicalFieldNames(), path());
  });
  return searchable;
}

} // namespace luxir
