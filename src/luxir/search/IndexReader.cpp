// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "IndexReader.h"
#include "luxir/store/Manifest.h"
#include "luxir/index/CommitSnapshot.h"
#include "luxir/reader/FieldReader.h"
#include "luxir/reader/FieldMetadataMemory.h"
#include "luxir/reader/Postings.h"
#include "luxir/reader/OwnershipMapper.h"
#include "luxir/reader/TestOverlayAuxReader.h"
#include "luxir/reader/VectorAuxReader.h"
#include "luxir/schema/Schema.h"
#include "luxir/schema/SchemaCache.h"
#include "luxir/store/FileIOException.h"
#include "luxir/util/ApiError.h"
#include "luxir/util/MemPool.h"
#include "luxir/util/Signal.h"

#include "luxir/api/padded_input.h"
#include "luxir/api/luxir_index.hpp"
#include <boost/unordered/unordered_flat_map.hpp>
#include <algorithm>
#include <bit>
#include <format>
#include <map>
#include <memory_resource>
#include <span>
#include <system_error>
#include <oneapi/tbb/parallel_for.h>

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

bool isKnownOverlayKind(std::string_view kind) {
  return kind == VectorAuxReader::KIND || kind == TestOverlayAuxReader::KIND;
}

SegmentOverlayPolicy overlayPolicyOf(std::string_view kind) {
  if (kind == TestOverlayAuxReader::KIND) return TestOverlayAuxReader::policyForTests;
  return {};
}

// Secures every file of `files` in `dir` by handle; null when one is absent.
std::unique_ptr<HeldFiles> holdAll(Directory& dir, std::span<const FileDescriptor> files) {
  auto held = std::make_unique<HeldFiles>();
  for (const auto& file : files) {
    if (!held->hold(dir, file.name)) return nullptr;
  }
  return held;
}

} // namespace

LiveDocs::LiveDocs(std::unique_ptr<uint64_t[]> words, int32_t maxDoc, int32_t numLive)
    : docSet(FixedBitSet(words.get(), maxDoc), numLive), ownedWords(std::move(words)),
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
    auto core = base->get();
    auto rootLive = root->get();
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

std::shared_ptr<LiveDocs> LiveDocs::create(Directory& dir, uint64_t segId, uint64_t creatorEpoch, uint64_t liveGen, uint64_t liveEpoch, int32_t maxDoc, bool missingFileOK, bool expectSynced) {
  assert(liveGen > 0 && maxDoc > 0);
  std::string deleteFileName = Postings::getLiveDocsFileName(
    Postings::segmentKey(segId, creatorEpoch), liveGen, liveEpoch);

  auto deleteFile = dir.openFile(deleteFileName, expectSynced);
  if (deleteFile == nullptr) {
    if (missingFileOK) {
      // Delete file not found - return nullptr instead of throwing
      IREADER_DEBUG("Delete file {} not found for segment s{} (liveGen={})",
                deleteFileName, Postings::segmentKey(segId, creatorEpoch), liveGen);
      return nullptr;
    } else {
      throw std::filesystem::filesystem_error(
              std::format("Delete file {} not found for segment s{} (liveGen={})",
                         deleteFileName, Postings::segmentKey(segId, creatorEpoch), liveGen),
              std::make_error_code(std::errc::no_such_file_or_directory));
    }
  }
  IREADER_DEBUG("Successfully opened delete file {} for segment s{}", deleteFileName, Postings::segmentKey(segId, creatorEpoch));

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

  IREADER_DEBUG("Loaded {} deleted documents from {} for segment s{}, {} live docs",
            maxDoc - numLiveDocs, deleteFileName, Postings::segmentKey(segId, creatorEpoch), numLiveDocs);

  return std::make_shared<LiveDocs>(liveBitsMemory, maxDoc, std::move(deleteFile), numLiveDocs);
}


// Whether `schema` types every field `summary` records as the segments
// persist it: the check field() makes on access.
static bool schemaMatchesPersisted(const SegmentSummary& summary, const Schema* schema) {
  if (!schema) return true;
  for (const auto& record : summary.fields()) {
    auto* expected = schema->getFieldTypePtr(record.fieldname);
    if (!expected) continue;
    auto type = (FieldType::Type)record.type;
    if (expected->type() != type
        || expected->segmentTypeFlags() != FieldType::typeParameters(type, record.type_flags)) {
      return false;
    }
  }
  return true;
}

const SegmentSummary* IndexReader::provingSummaryOf(const PhysicalCore& core, const Schema* schema) {
  const auto& summary = core.collectionSummary;
  return summary && schemaMatchesPersisted(*summary, schema) ? summary.get() : nullptr;
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
                         std::shared_ptr<FilterCache> filterCache, std::shared_ptr<Metrics> metrics,
                         bool materialize, std::shared_ptr<const UnreadableHook> onUnreadable)
  : IndexReader(openSnapshot(dir, previousReader, snapshot.schema, snapshot.bytes, snapshot.openCores,
                             metrics ? metrics : Metrics::detached(), materialize, onUnreadable),
                filterCache ? std::move(filterCache)
                    : (previousReader ? previousReader->snapshot->filterCache : nullptr)) {}

IndexReader::IndexReader(Directory& dir, IndexReader* previousReader, std::shared_ptr<FilterCache> filterCache)
  : IndexReader(openSnapshot(dir, previousReader, nullptr, nullptr, {}, Metrics::detached(), false, nullptr),
                filterCache ? std::move(filterCache)
                    : (previousReader ? previousReader->snapshot->filterCache : nullptr)) {}

template <class Value>
template <class Open>
std::shared_ptr<Value> SegmentPart<Value>::getOrOpen(std::string_view what, Open&& open) {
  auto rethrowUnreadable = [&] {
    throw ApiError(ErrorKind::INTERNAL, "segment_unreadable", failure);
  };
  if (isUnreadable()) rethrowUnreadable();
  return cell.getOrCreate([&] {
    if (isUnreadable()) rethrowUnreadable();  // marked while this call waited to build
    assert(files);
    // Unreadable bytes do not heal: record the failure, release the
    // handles, and never build again.
    auto markUnreadable = [&](std::string message) {
      failure = std::move(message);
      unreadable.store(true, std::memory_order_release);
      std::vector<Directory::FileInfo> held;
      files->listFiles(held);
      files.reset();
      LOG_ERROR("{}; it is not retried until the segment's files are replaced", failure);
      if (onUnreadable) {
        std::vector<std::string> suspects;
        suspects.reserve(held.size());
        for (auto& file : held) suspects.push_back(std::move(file.name));
        (*onUnreadable)(std::move(suspects), failure);
      }
      Signal::emit("segmentUnreadable", (void*)this);
      rethrowUnreadable();
    };
    uint64_t mappedBefore = files->mappedBytes();
    std::shared_ptr<Value> value;
    try {
      value = open(*files);
    } catch (const FileEIOError& e) {
      markUnreadable(std::string(what) + " cannot be read: " + e.what());
    } catch (const ApiError&) {
      throw;  // I/O or resource failures (a failed mmap, a memory limit) retry
    } catch (const std::filesystem::filesystem_error& e) {
      // A held file cannot vanish: one absent at decode was never secured.
      markUnreadable(std::string(what) + " has a file missing: " + e.what());
    } catch (const std::system_error& e) {
      // Retried; classified as the server's failure, not the request's.
      throw ApiError(ErrorKind::UNAVAILABLE, "segment_open_failed", std::string(what) + " cannot be opened: " + e.what());
    } catch (const std::bad_alloc&) {
      throw ResourceExhaustedError(std::string(what) + " cannot be opened: out of memory");
    } catch (const std::exception& e) {
      markUnreadable(std::string(what) + " cannot be read: " + e.what());
    }
    if (!value) {
      // A held file cannot vanish; a missing one was never secured.
      markUnreadable(std::string(what) + " has files missing");
    }
    // Only the successful attempt's mappings stay; a failed one's are gone.
    metrics->add(MetricCounter::SEGMENT_BYTES_MAPPED, files->mappedBytes() - mappedBefore);
    files.reset();  // the mappings keep the bytes now
    return value;
  });
}

std::shared_ptr<SegmentCore> SegmentBase::get() {
  if (auto value = cell.peek()) return value;  // the warm path builds no message
  return getOrOpen("segment " + std::to_string(segId), [this](HeldFiles& held) {
    auto core = SegmentCore::create(held, segId, creatorEpoch, false, true);
    if (core->rootReader().maxDoc() != maxDoc) {
      throw std::runtime_error(std::format("maxDoc {} disagrees with the manifest's {}",
                                           core->rootReader().maxDoc(), maxDoc));
    }
    // View sizes and ordinals came from the manifest; the catalog must match.
    auto catalog = core->spaces();
    bool same = catalog.size() == spaces.size() + 1;
    for (size_t i = 0; same && i < spaces.size(); i++) {
      same = catalog[i + 1].path == spaces[i].first && catalog[i + 1].maxDoc == spaces[i].second;
    }
    if (!same) throw std::runtime_error("row spaces disagree with the manifest's");
    metrics->add(MetricCounter::SEGMENTS_OPENED);
    Signal::emit("segmentMaterialized", (void*)&segId);
    return core;
  });
}

std::optional<std::pair<SpaceOrdinal, int32_t>> SegmentBase::space(std::string_view path) const noexcept {
  if (path.empty()) return std::pair{SpaceOrdinal::ROOT, maxDoc};
  auto found = std::ranges::lower_bound(spaces, path, {}, [](const auto& entry) { return std::string_view(entry.first); });
  if (found == spaces.end() || found->first != path) return std::nullopt;
  return std::pair{(SpaceOrdinal)(found - spaces.begin() + 1), found->second};
}

std::shared_ptr<LiveDocs> SegmentLive::get() {
  if (auto value = cell.peek()) return value;  // the warm path builds no message
  return getOrOpen("live docs of segment " + std::to_string(segId), [this](HeldFiles& held) {
    auto live = LiveDocs::create(held, segId, creatorEpoch, liveGen, liveEpoch, maxDoc, false, true);
    if (live->numLive() != numLive) {
      throw std::runtime_error(std::format("{} live docs disagree with the manifest's {}", live->numLive(), numLive));
    }
    return live;
  });
}

std::shared_ptr<AuxReader> SegmentOverlay::get() {
  if (auto value = cell.peek()) return value;  // the warm path builds no message
  return getOrOpen("overlay " + info.name, [this](HeldFiles& held) -> std::shared_ptr<AuxReader> {
    std::pmr::monotonic_buffer_resource arena;
    if (!policy.advisory) return openKnownAux(held, toWire(info, arena), false);
    std::shared_ptr<AuxReader> reader;
    try {
      reader = openKnownAux(held, toWire(info, arena), false);
    } catch (const ApiError&) {
      throw;
    } catch (const std::system_error&) {
      throw;
    } catch (const std::bad_alloc&) {
      throw;
    } catch (const std::exception& e) {
      LOG_WARN("Advisory overlay '{}' is unusable and will answer nothing: {}", info.name, e.what());
    }
    if (reader) return reader;
    return std::make_shared<UnavailableAuxReader>(info.kind, info.name, info.gen, info.built_core_gen);
  });
}

bool Segment::isMaterialized() const noexcept {
  if (!sharedBase->peekRaw() || (sharedLive && !sharedLive->peekRaw())) return false;
  for (const auto& overlay : sharedOverlays) {
    if (!overlay->policy.onDemand && !overlay->peekRaw()) return false;
  }
  return true;
}

void Segment::materialize() const {
  sharedBase->get();
  if (sharedLive) sharedLive->get();
  for (const auto& overlay : sharedOverlays) {
    if (!overlay->policy.onDemand) overlay->get();
  }
}

std::vector<std::shared_ptr<AuxReader>> Segment::auxReaders() const {
  std::vector<std::shared_ptr<AuxReader>> readers;
  readers.reserve(sharedOverlays.size());
  for (const auto& overlay : sharedOverlays) {
    if (!overlay->policy.onDemand) readers.push_back(overlay->get());
  }
  return readers;
}

std::shared_ptr<AuxReader> Segment::getAuxReader(std::string_view name) const {
  for (const auto& overlay : sharedOverlays) {
    if (overlay->info.name == name) return overlay->get();
  }
  return nullptr;
}

namespace {
// Opens segments in parallel: each open maps files and decodes headers independently.
void materializeAll(const std::vector<const Segment*>& pending) {
  if (pending.size() == 1) {
    pending.front()->materialize();
  } else if (!pending.empty()) {
    oneapi::tbb::parallel_for((size_t)0, pending.size(), [&](size_t i) { pending[i]->materialize(); },
                              oneapi::tbb::simple_partitioner());
  }
}

// Opens the not yet materialized segments the mask keeps, all of them without one.
void materializeSegments(const std::vector<Segment>& segs, const SegmentMask* mask) {
  std::vector<const Segment*> pending;
  for (const auto& segment : segs) {
    if ((!mask || mask->test((size_t)segment.ord)) && !segment.isMaterialized()) pending.push_back(&segment);
  }
  materializeAll(pending);
}
}  // namespace

void IndexReader::materialize(const SegmentMask& mask) { materializeSegments(space->segs, &mask); }

IndexReader::OpenedSnapshot IndexReader::openSnapshot(Directory& dir, const IndexReader* previousReader,
                                                std::shared_ptr<Schema> preferredSchema,
                                                std::shared_ptr<const std::vector<std::byte>> manifest,
                                                std::span<const std::weak_ptr<SegmentCore>> published,
                                                const std::shared_ptr<Metrics>& metrics, bool materialize,
                                                const std::shared_ptr<const UnreadableHook>& onUnreadable) {
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
  // Segment overlays are reused as parts, materialized or not.
  boost::unordered_flat_map<std::string, std::shared_ptr<AuxReader>> prevAuxByName;
  boost::unordered_flat_map<std::string, std::shared_ptr<SegmentOverlay>> prevSegAuxByKey;
  if (previous) {
    assert(previous->segmentFiles.size() == previous->bundles.size());
    for (const auto& r : previous->auxReadersList) {
      prevAuxByName.emplace(std::string(r->getName()), r);
    }
    for (const auto& seg : previous->bundles) {
      for (const auto& overlay : seg.aux) {
        prevSegAuxByKey.emplace(segmentAuxKey(seg.info.seg_id, overlay->info.name), overlay);
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
        if (materialize) materializeSegments(previous->root->segs, nullptr);
        return {previousReader->snapshot->core, std::move(schema), indexInfo.commit_time, indexGen};
      }
      commitTimeUs = indexInfo.commit_time;
      core->coreGeneration = indexInfo.core_gen;
      core->commitTime = commitTimeUs;
      core->collectionSummary = nullptr;
      if (indexInfo.summary_complete) {
        try {
          core->collectionSummary = SegmentSummary::fromAggregate(indexInfo.field_summaries, true);
        } catch (const std::exception& e) {
          LOG_WARN("Collection summary unreadable at gen={}: {}", indexInfo.index_gen, e.what());
        }
      }
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

        // Every holder of a physical segment shares one core part,
        // materialized or not: the previous snapshot's when the base files
        // are identical (a delete or overlay commit leaves them unchanged),
        // unless the publisher (the writer) hands over a different open core,
        // which then wins so holders converge on one instance. A new part
        // holds its files by handle; without descriptors the identity is
        // unknown, nothing is shared, and the core opens now by name. Live
        // docs and overlays carry over likewise when their files are unchanged.
        const auto* prevSeg = previous ? previous->bundle(segId) : nullptr;
        const auto* prevFiles = prevSeg ? &previous->segmentFiles[(size_t)prevSeg->ord] : nullptr;
        SegmentBase::SpaceRows spaceRows;
        spaceRows.reserve(segment.spaces.size());
        for (const auto& space : segment.spaces) spaceRows.emplace_back(std::string(space.path), space.rows);
        std::shared_ptr<SegmentCore> handed;
        if ((size_t)i < published.size()) {
          handed = published[i].lock();
          if (handed && handed->rootReader().maxDoc() != nDocs) handed = nullptr;
        }
        std::shared_ptr<SegmentBase> base;
        if (prevSeg && !files.files.empty() && prevFiles->files == files.files
            && (!handed || handed == prevSeg->core->peek())) {
          base = prevSeg->core;
        } else if (handed) {
          base = std::make_shared<SegmentBase>(segId, segment.creator_epoch, nDocs, std::move(spaceRows),
                                               std::move(handed));
        } else if (files.files.empty()) {
          auto opened = SegmentCore::create(dir, segId, segment.creator_epoch, missingFileOK, true);
          if (!opened) {
            retry = true;  // segment files not found
            break;
          }
          base = std::make_shared<SegmentBase>(segId, segment.creator_epoch, nDocs, std::move(spaceRows),
                                               std::move(opened));
        } else {
          auto held = holdAll(dir, files.files);
          if (!held) {
            if (!missingFileOK) {
              throw std::filesystem::filesystem_error(
                  std::format("Segment {} files are missing", segId),
                  std::make_error_code(std::errc::no_such_file_or_directory));
            }
            // A newer root retired the files before their handles opened - retry
            retry = true;
            break;
          }
          dir.expectSynced(files.files);
          base = std::make_shared<SegmentBase>(segId, segment.creator_epoch, nDocs, std::move(spaceRows),
                                               std::move(held), metrics, onUnreadable);
        }

        std::shared_ptr<SegmentLive> live;
        if (liveGen > 0 && prevSeg && prevSeg->live && !files.liveDocs.empty()
            && prevFiles->liveDocs == files.liveDocs) {
          live = prevSeg->live;
        } else if (liveGen > 0) {
          auto held = std::make_unique<HeldFiles>();
          std::string liveName = Postings::getLiveDocsFileName(
              Postings::segmentKey(segId, segment.creator_epoch), liveGen, segment.live_epoch);
          if (!held->hold(dir, liveName)) {
            if (!missingFileOK) {
              throw std::filesystem::filesystem_error(
                  std::format("Delete file {} not found for segment {} (liveGen={})", liveName, segId, liveGen),
                  std::make_error_code(std::errc::no_such_file_or_directory));
            }
            retry = true;
            break;
          }
          dir.expectSynced(files.liveDocs);
          live = std::make_shared<SegmentLive>(segId, segment.creator_epoch, liveGen, segment.live_epoch, nDocs,
                                              segment.live_docs, std::move(held), metrics, onUnreadable);
        }
        // If liveGen == 0, live remains null (no deletes)

        std::vector<std::shared_ptr<SegmentOverlay>> segmentOverlays;
        std::shared_ptr<const SegmentSummary> summary;
        segmentOverlays.reserve(segment.overlays.size());
        for (int j = 0; j < (int)segment.overlays.size(); j++) {
          const auto& info = segment.overlays[j];

          if (info.kind == SegmentSummary::OVERLAY_KIND) {
            if (info.files.empty()) continue;
            // Read now: it is small, and what lets a query skip the segment
            // without materializing anything. A segment's summary is written
            // once with it: reuse the previous snapshot's copy when the
            // segment's files and the summary's own descriptor are unchanged.
            if (prevSeg && prevSeg->summary && !files.files.empty() && prevFiles->files == files.files
                && std::ranges::find(prevFiles->overlays, files.overlays[(size_t)j]) != prevFiles->overlays.end()) {
              summary = prevSeg->summary;
              continue;
            }
            // A file that cannot be opened counts as missing: retried like
            // any segment file while missingFileOK, then skipped.
            std::shared_ptr<InputFile> file;
            try {
              file = dir.openFile(info.files.front().name, true);
            } catch (const std::exception& e) {
              if (!missingFileOK) LOG_WARN("Segment summary not opened for seg={}: {}", segId, e.what());
            }
            if (!file) {
              if (missingFileOK) {
                IREADER_DEBUG("Segment summary file missing for seg={} - triggering retry", segId);
                retry = true;
                break;
              }
              continue;
            }
            try {
              std::string_view image = file->read();
              summary = SegmentSummary::fromBytes(
                  std::span<const std::byte>((const std::byte*)image.data(), image.size()));
            } catch (const std::exception& e) {
              // Unreadable: the segment has no summary, which proves nothing.
              LOG_WARN("Segment summary unreadable for seg={}: {}", segId, e.what());
              continue;
            }
            if (summary->segId() != segId || summary->maxDoc() != nDocs) {
              LOG_WARN("Segment summary for seg={} describes seg={} max_doc={} (expected max_doc={}); ignored",
                       segId, summary->segId(), summary->maxDoc(), nDocs);
              summary = nullptr;
            }
            continue;
          }

          if (!isKnownOverlayKind(info.kind)) {
            IREADER_DEBUG("Skipping unknown aux kind '{}' for '{}'", info.kind, info.name);
            continue;
          }

          auto prevIt = prevSegAuxByKey.find(segmentAuxKey(segId, info.name));
          if (prevIt != prevSegAuxByKey.end()
              && prevIt->second->info.gen == info.gen
              && prevIt->second->info.built_core_gen == info.built_core_gen
              && prevIt->second->info.files == files.overlays[(size_t)j]) {
            IREADER_DEBUG("Reusing segment overlay '{}' for seg={} (gen={}) from previous IndexReader",
                          info.name, segId, info.gen);
            segmentOverlays.push_back(prevIt->second);
            continue;
          }

          const auto& descriptors = files.overlays[(size_t)j];
          auto held = holdAll(dir, descriptors);
          if (!held) {
            if (!missingFileOK) {
              throw std::filesystem::filesystem_error(
                  std::format("Missing overlay file for '{}' seg={}", info.name, segId),
                  std::make_error_code(std::errc::no_such_file_or_directory));
            }
            IREADER_DEBUG("Segment overlay file missing for '{}' seg={} - triggering retry", info.name, segId);
            retry = true;
            break;
          }
          dir.expectSynced(descriptors);
          segmentOverlays.push_back(std::make_shared<SegmentOverlay>(
              fromWire(info), overlayPolicyOf(info.kind), std::move(held), metrics, onUnreadable));
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

        auto childLives = prevSeg && prevSeg->core == base && prevSeg->live == live
            ? prevSeg->childLives : live && !base->spaces.empty()
                ? std::make_shared<PhysicalCore::ChildLives>() : nullptr;
        segs.push_back({std::move(base), std::move(live),
                        std::move(segmentOverlays), segmentInfo, i, std::move(childLives),
                        std::move(summary)});
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
    core->root->fields = std::make_shared<FieldRecords>();
  }

  core->root->segs.reserve(segs.size());
  for (auto& bundle : segs) {
    assert((bundle.info.live_gen != 0) == (bundle.live != nullptr));
    assert(bundle.info.live_gen == 0 || bundle.info.live_docs < bundle.info.max_doc);
    assert(!bundle.live || bundle.info.live_docs == bundle.live->numLive);
    core->root->segs.emplace_back(bundle.core, bundle.live, bundle.aux, bundle.info, totalMaxDoc, bundle.ord,
        SpaceOrdinal::ROOT, bundle.info.max_doc, bundle.info.live_docs, nullptr, bundle.summary);
    totalMaxDoc += bundle.info.max_doc;
    livedocs += bundle.info.live_docs;
  }

  if (materialize) materializeSegments(core->root->segs, nullptr);

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
  state->fields = std::make_shared<FieldRecords>();
  state->segs.reserve(bundles.size());
  for (const auto& bundle : bundles) {
    // Sized from the manifest: building a view opens no segment.
    auto found = bundle.core->space(path);
    std::shared_ptr<ChildLiveDocs> live;
    if (found && bundle.live) {
      std::lock_guard liveLock(bundle.childLives->mutex);
      auto& entry = bundle.childLives->paths[std::string(path)];
      if (!entry) entry = std::make_shared<ChildLiveDocs>(bundle.core, bundle.live, std::string(path));
      live = entry;
    }
    // Segment metadata remains the root manifest metadata. Row counts and
    // readers belong to this space; overlays are shared by the bundle.
    auto [ordinal, rows] = found.value_or(std::pair{Segment::NO_SPACE, 0});
    state->segs.emplace_back(bundle.core, nullptr, bundle.aux, bundle.info,
        state->totalMaxDoc, bundle.ord, ordinal, rows, rows, std::move(live));
    state->totalMaxDoc += state->segs.back().maxDoc();
  }
  spaces.emplace(path, state);
  return state;
}

IndexReader::FieldRecord::FieldRecord(std::string_view name, size_t hash, std::span<const int32_t> initialOrdinals)
    : name(name), hash(hash), numSegments(initialOrdinals.size()),
      ordinals(std::make_unique<std::atomic<int32_t>[]>(initialOrdinals.size())) {
  for (size_t i = 0; i < numSegments; i++) {
    ordinals[i].store(initialOrdinals[i], std::memory_order_relaxed);
  }
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
  const FieldRecord* record = space->fields->find(name, hash);
  if (!record) {
    // A complete summary proves presence or absence without materializing
    // the segment. Other segments are probed only until the field is found;
    // their ordinals, and those of present fields, resolve lazily after.
    std::vector<int32_t> ordinals(space->segs.size(), FieldRecord::UNRESOLVED);
    bool found = false;
    for (size_t i = 0; i < space->segs.size(); i++) {
      const auto& segment = space->segs[i];
      if (!segment.hasSpace()) {
        ordinals[i] = -1;
        continue;
      }
      const SegmentSummary* summary = segment.summary();
      if (summary && summary->complete()) {
        if (summary->field(name) == nullptr) ordinals[i] = -1;
        else found = true;
        continue;
      }
      if (found) continue;
      ordinals[i] = segment.postingsReader().findOrdinal(name);
      found = ordinals[i] >= 0;
    }
    if (found) record = space->fields->insert(std::make_unique<FieldRecord>(name, hash, ordinals));
  }
  if (!record) return {};
  FieldSegments result(record, space->segs.data());
  if (snapshot->schema) {
    if (auto* expected = snapshot->schema->getFieldTypePtr(name)) {
      const auto& persisted = result.totals();
      if (expected->type() != persisted.type
          || expected->segmentTypeFlags() != FieldType::typeParameters(persisted.type, persisted.typeFlags)) {
        throw RequestError(std::format(
            "Field '{}': schema {} does not match persisted {}",
            name, FieldType::describeType(expected->type(), expected->segmentTypeFlags()),
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
  int32_t ordinal = record->ordinals[segment].load(std::memory_order_relaxed);
  if (ordinal == FieldRecord::UNRESOLVED) {
    ordinal = segments[segment].hasSpace()
        ? segments[segment].postingsReader().findOrdinal(record->name) : -1;
    record->ordinals[segment].store(ordinal, std::memory_order_relaxed);
  }
  return ordinal;  // -1, absent, needs no materialization once known
}

const PostingsReader::Field* IndexReader::FieldSegments::at(size_t segment) const {
  if (!record) return nullptr;
  int32_t ordinal = ordinalAt(segment);
  return ordinal < 0 ? nullptr : segments[segment].postingsReader().fieldInfo(ordinal);
}

bool IndexReader::FieldSegments::presentIn(size_t segment) const noexcept {
  if (!record) return false;
  assert(segment < record->numSegments);
  return record->ordinals[segment].load(std::memory_order_relaxed) != -1;
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
    // Segments without a complete summary (child spaces have none) need their
    // headers: open those together rather than one by one in the loop below.
    std::vector<const Segment*> pending;
    for (size_t i = 0; i < size(); i++) {
      const SegmentSummary* summary = segments[i].summary();
      if ((!summary || !summary->complete()) && presentIn(i) && segments[i].hasSpace()
          && !segments[i].isMaterialized()) {
        pending.push_back(&segments[i]);
      }
    }
    materializeAll(pending);
    auto fresh = std::make_unique<HeaderTotals>();
    bool first = true;
    for (size_t i = 0; i < size(); i++) {
      fresh->stats.maxDoc += segments[i].maxDoc();
      // A complete summary carries the header's totals, flags and persisted
      // type, so the segment stays unmaterialized.
      FieldType::Type type;
      uint32_t typeFlags;
      const SegmentSummary* summary = segments[i].summary();
      if (summary && summary->complete()) {
        const auto* field = summary->field(record->name);
        if (!field) continue;
        type = (FieldType::Type)field->type;
        typeFlags = field->type_flags;
        fresh->stats.docsWithField += (int64_t)field->docs_with_field;
        fresh->stats.sumDocFreq += (int64_t)field->sum_doc_freq;
        fresh->stats.sumTotalTermFreq += (int64_t)field->sum_total_term_freq;
        fresh->flags |= (FieldType::flag_type)field->flags;
      } else {
        int32_t ordinal = ordinalAt(i);
        if (ordinal < 0) continue;
        SegFieldInfo info{};
        FieldReader fields(segments[i].postingsReader(), ordinal);
        fields.readFieldInfo(info);
        type = info.type;
        typeFlags = info.typeFlags;
        fresh->stats.docsWithField += info.docsWithField;
        fresh->stats.sumDocFreq += info.sumDocFreq;
        fresh->stats.sumTotalTermFreq += info.sumTotalTermFreq;
        fresh->flags |= info.flags;
      }
      typeFlags = FieldType::typeParameters(type, typeFlags);
      if (first) {
        first = false;
        fresh->type = type;
        fresh->typeFlags = typeFlags;
      } else if (type != fresh->type || typeFlags != fresh->typeFlags) {
        throw std::runtime_error(std::format(
            "Incompatible persisted types for field '{}': {} and {}", record->name,
            FieldType::describeType(fresh->type, fresh->typeFlags), FieldType::describeType(type, typeFlags)));
      }
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

void IndexReader::SpaceState::buildFieldCatalogs() {
  callOnce(fieldCatalogsOnce, [this] {
    // Both name sources are views into segment files held open by the
    // segment core's space reader: the field index (fi.fieldname) and the stored
    // resource's cached metadata. No copies needed.
    std::map<std::string_view, ProjectableField> sources;
    for (auto& seg : segs) {
      if (!seg.hasSpace()) continue;
      auto& postingsReader = seg.postingsReader();
      auto poolGuard = MemPool::threadLocalPoolGuard();
      FieldReader fieldReader(postingsReader);
      for (int32_t ordinal = 0; fieldReader.readNextField(); ordinal++) {
        SegFieldInfo fi;
        fieldReader.readFieldInfo(fi);
        std::string_view physicalName(fi.fieldname.data(), fi.fieldname.size());
        if (FieldType::storage(fi.type) == FieldType::Storage::BIN && (fi.flags & FieldType::STORED)) {
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
        } else if (FieldType::storage(fi.type) != FieldType::Storage::BIN) {
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
