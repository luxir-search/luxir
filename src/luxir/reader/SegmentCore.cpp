// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "SegmentCore.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <format>
#include <stdexcept>
#include "Postings.h"
#include "ReaderErrors.h"
#include "OwnershipShape.h"
#include "luxir/store/AcquireJoin.h"
#include "luxir/store/FileIOException.h"
#include "luxir/api/luxir_index.hpp"
#include "luxir/util/IoContext.h"

namespace luxir {

namespace {

void validateSegmentFileHeader(std::span<const char> bytes, std::string_view segment, uint32_t fileNumber) {
  std::string_view header = Postings::LUXIR_HEADER;
  if (bytes.size() < header.size() || std::memcmp(bytes.data(), header.data(), header.size()) != 0) {
    corruptSegment(segment, std::format("invalid Luxir magic; expected '{}'", header), {}, fileNumber);
  }
}

} // namespace

struct SegmentCore::ChildSpace {
  PostingsReader reader;
  SpaceOrdinal parent;
  SharedLazyCell<OwnershipShape> shape;

  ChildSpace(SegmentCore& core, SpaceOrdinal ordinal, SpaceOrdinal parent)
      : reader(core, ordinal), parent(parent) {}
};

SegmentCore::SegmentCore(uint64_t segId, uint64_t creatorEpoch)
    : segId(segId), creatorEpoch(creatorEpoch), root(*this, SpaceOrdinal::ROOT) {}

SegmentCore::~SegmentCore() = default;

void SegmentCore::acquireSlices(std::span<const field_parts::Slice> parts, ReadClass readClass, AcquireBatches& join) {
  std::vector<Part> classed;
  classed.reserve(parts.size());
  for (auto part : parts) classed.push_back({part, readClass});
  acquireParts(classed, join);
}

void SegmentCore::acquireParts(std::span<const Part> parts, AcquireBatches& join) {
  using Key = std::pair<uint64_t, uint64_t>;
  std::map<uint32_t, std::vector<std::pair<Key, ReadClass>>> batches;
  std::unique_lock lock(holdsMutex, std::defer_lock);
  for (auto [part, readClass] : parts) {
    auto number = part.location.filenum();
    assert(number < files.size() && files[number].file);
    if (files[number].holdsFree) continue;  // slice() hands out a free view without a request
    if (!lock.owns_lock()) lock.lock();
    Key key(part.location.raw(), part.bytes);
    if (holds.contains(key)) continue;
    batches[number].emplace_back(key, readClass);
  }
  if (lock.owns_lock()) lock.unlock();
  for (auto& [number, batch] : batches) {
    // A part asked for twice goes once, under its first read class.
    std::ranges::stable_sort(batch, {}, &std::pair<Key, ReadClass>::first);
    auto [first, last] = std::ranges::unique(batch, {}, &std::pair<Key, ReadClass>::first);
    batch.erase(first, last);
    std::vector<ByteRange> ranges;
    std::vector<Key> keys;
    ranges.reserve(batch.size());
    keys.reserve(batch.size());
    uint64_t total = 0;
    for (auto [key, readClass] : batch) {
      ranges.push_back({seg_location::fromRaw(key.first).offset(), key.second, readClass});
      keys.push_back(key);
      total += key.second;
    }
    // One batch: the request counts its ranges and one round trip.
    countAcquire(total, ranges.size());
    join.acquire(*files[number].file, ranges, [this, keys = std::move(keys)](std::vector<HeldRange>&& acquired) {
      if (acquired.size() != keys.size()) throw FileIOException("batched acquire returned the wrong number of holds");
      uint64_t added = 0;
      {
        std::lock_guard lock(holdsMutex);
        for (size_t i = 0; i < keys.size(); ++i) {
          if (holds.try_emplace(keys[i], std::move(acquired[i])).second) added += keys[i].second;
        }
      }
      if (added) countFirstHold(added);
    });
  }
}

bool SegmentCore::held(field_parts::Slice part) {
  auto number = part.location.filenum();
  assert(number < files.size() && files[number].file);
  if (files[number].holdsFree) return true;
  std::lock_guard lock(holdsMutex);
  return holds.contains({part.location.raw(), part.bytes});
}

std::vector<std::span<const char>> SegmentCore::slices(std::span<const field_parts::Slice> parts, ReadClass readClass) {
  {
    AcquireJoin join;
    acquireSlices(parts, readClass, join);
    countAcquireRound();
    join.wait();
  }
  std::vector<std::span<const char>> result;
  result.reserve(parts.size());
  for (auto part : parts) result.push_back(slice(part, readClass));
  return result;
}

std::span<const char> SegmentCore::retain(InputFile& file, field_parts::Slice part, ReadClass readClass) {
  auto key = std::pair(part.location.raw(), part.bytes);
  {
    std::lock_guard lock(holdsMutex);
    auto found = holds.find(key);
    if (found != holds.end()) return found->second.bytes();
  }
  // Acquire outside the lock: on a backend where an acquire is a request, a
  // miss must not hold up other slices of the segment, hits included. Two
  // threads missing the same part both acquire it; the first to store wins.
  auto hold = file.acquire({part.location.offset(), part.bytes, readClass});
  countAcquire(part.bytes);
  countAcquireRound();
  std::span<const char> bytes;
  bool inserted;
  {
    std::lock_guard lock(holdsMutex);
    auto [stored, added] = holds.try_emplace(key, std::move(hold));
    bytes = stored->second.bytes();
    inserted = added;
  }
  // A duplicate releases here, outside the lock.
  if (inserted) countFirstHold(part.bytes);
  return bytes;
}

void SegmentCore::countFirstHold(uint64_t bytes) noexcept {
  if (!opened) return;  // the open's own headers and catalog
  bytesBeyondCatalog.fetch_add(bytes, std::memory_order_relaxed);
  segmentOpenCounters.segmentBytesMapped.fetch_add(bytes, std::memory_order_relaxed);
  if (!readBeyondCatalog.exchange(true, std::memory_order_relaxed)) {
    segmentOpenCounters.segmentsOpened.fetch_add(1, std::memory_order_relaxed);
  }
}

const VersionRuns& SegmentCore::buildVersionRuns() const {
  return *versions.getOrCreate([this] {
    const auto& version = catalog.versionRuns();
    if (!version.slice) return std::make_shared<VersionRuns>();
    auto bytes = const_cast<SegmentCore*>(this)->slice(version.slice, ReadClass::METADATA);
    SegmentReadContext context{this, PackedTerm(nullptr), version.slice.location.filenum()};
    auto runs = std::make_shared<VersionRuns>(bytes.data(), bytes.size(), version.trailerOffset, version.count,
                                              spaces()[0].maxDoc, context);
    if (runs->byteSize() != bytes.size()) context.invalid("version runs slice does not match its header");
    return runs;
  });
}

std::optional<SpaceOrdinal> SegmentCore::spaceOrdinal(std::string_view path) const noexcept {
  return catalog.findSpace(path);
}

PostingsReader* SegmentCore::spaceReader(SpaceOrdinal ordinal) noexcept {
  if (ordinal == SpaceOrdinal::ROOT) return &root;
  return (size_t)ordinal < spaces().size() ? &children[(size_t)ordinal - 1]->reader : nullptr;
}

PostingsReader* SegmentCore::spaceReader(std::string_view path) noexcept {
  auto ordinal = spaceOrdinal(path);
  return ordinal ? spaceReader(*ordinal) : nullptr;
}

const OwnershipShape* SegmentCore::ownership(SpaceOrdinal child) const {
  if (child == SpaceOrdinal::ROOT || (size_t)child >= spaces().size()) return nullptr;
  auto* built = children[(size_t)child - 1]->shape.peek();
  return built ? built : buildOwnership(child);
}

const OwnershipShape* SegmentCore::buildOwnership(SpaceOrdinal child) const {
  auto& core = const_cast<SegmentCore&>(*this);
  auto& space = *children[(size_t)child - 1];
  // A mapping walks from the root, so the shapes above this one are likely
  // next: acquire the columns of every unbuilt shape on the path in one batch.
  std::vector<field_parts::Slice> parts;
  for (auto ordinal = child; ordinal != SpaceOrdinal::ROOT; ordinal = children[(size_t)ordinal - 1]->parent) {
    if (children[(size_t)ordinal - 1]->shape.peek()) continue;
    const auto& descriptor = spaces()[(size_t)ordinal].ownership;
    for (auto part : {descriptor.ends, descriptor.owners}) {
      if (part) parts.push_back(part);
    }
  }
  if (parts.size() > 1) core.slices(parts);
  return space.shape.getOrCreate([&] {
    return std::make_shared<OwnershipShape>(core, spaces()[(size_t)child].ownership,
                                            spaces()[(size_t)space.parent].maxDoc, spaces()[(size_t)child].maxDoc);
  }).get();
}

const OwnershipShape* SegmentCore::ownership(std::string_view path) const {
  auto ordinal = spaceOrdinal(path);
  return ordinal ? ownership(*ordinal) : nullptr;
}

namespace {
template <class File>
SegmentCore::Source makeSource(uint64_t id, uint64_t epoch, CatalogRange range,
                               int32_t maxDoc, std::span<const File> files) {
  SegmentCore::Source result{range, maxDoc, {}};
  auto prefix = Postings::getIndexFileNamePrefix(id, epoch);
  for (const auto& file : files) {
    std::string_view name(file.name);
    if (!name.starts_with(prefix)) continue;
    auto suffix = name.substr(prefix.size());
    auto number = Postings::parseSortableString(suffix);
    if (number > seg_location::maxFilenum() || Postings::getSortableString(number) != suffix) continue;
    if (number >= result.files.size()) result.files.resize(number + 1);
    if (result.files[number]) {
      corruptSegment(Postings::segmentKey(id, epoch), "duplicate segment file in manifest", {}, (uint32_t)number);
    }
    if (!file.size) corruptSegment(Postings::segmentKey(id, epoch), "zero segment file size", {}, (uint32_t)number);
    result.files[number] = file.size;
  }
  return result;
}
} // namespace

SegmentCore::Source SegmentCore::source(uint64_t id, uint64_t epoch, CatalogRange range,
                                        int32_t maxDoc, std::span<const FileDescriptor> files) {
  return makeSource(id, epoch, range, maxDoc, files);
}

bool SegmentCore::openBaseFile(Directory& dir, std::string_view segment, uint32_t number, uint64_t expected,
                               bool missingFileOK, bool expectSynced, bool checkHeader) {
  auto name = Postings::getIndexFileName(segment, number);
  auto file = dir.openFile(name, expectSynced, expected);
  if (!file) {
    if (missingFileOK) return false;
    throw std::filesystem::filesystem_error(
        std::format("Can't find/open segment file '{}'", name),
        std::make_error_code(std::errc::no_such_file_or_directory));
  }
  // Trust the manifest's size so a remote open needs no size request. A
  // file with free holds is local and knows its size, so check it there.
  bool holdsFree = file->holdsFree();
  auto size = expected && !holdsFree ? expected : (uint64_t)file->size();
  if (expected && size != expected) {
    corruptSegment(segment, "file size differs from manifest", {}, number);
  }
  if ((expected || checkHeader) && size <= Postings::LUXIR_HEADER.size()) {
    corruptSegment(segment, "segment file is too short", {}, number);
  }
  if (checkHeader) {
    auto header = file->acquire({0, Postings::LUXIR_HEADER.size(), ReadClass::METADATA});
    if (!holdsFree) countAcquire(header.getLength());
    validateSegmentFileHeader(header.bytes(), segment, number);
  }
  if (number >= files.size()) files.resize(number + 1);
  files[number] = {std::move(file), size, holdsFree};
  return true;
}

bool SegmentCore::beginOpen(Directory& dir, std::string_view segment, const Source& source, bool missingFileOK,
                            bool expectSynced, AcquireBatches& join) {
  constexpr uint32_t primary = Postings::PRIMARY_FILE;
  if (source.files.empty() || !source.files[primary]) corruptSegment(segment, "missing primary file in manifest");
  if (!openBaseFile(dir, segment, primary, source.files[primary], missingFileOK, expectSynced, false)) return false;
  auto primarySize = files[primary].size;
  range = source.range;
  SegmentReadContext primaryContext{this, PackedTerm(nullptr), primary};
  if (range.offset > seg_location::maxOffset() || range.offset > primarySize
      || range.length > primarySize - range.offset) {
    primaryContext.invalid("catalog range lies outside file 0", "invalid segment catalog");
  }
  if (range.offset < Postings::LUXIR_HEADER.size()) {
    primaryContext.invalid("catalog starts inside file header", "invalid segment catalog");
  }
  // The header goes with the catalog, so checking it costs no round of its own.
  field_parts::Slice parts[] = {{{primary, 0}, Postings::LUXIR_HEADER.size()}, {{primary, range.offset}, range.length}};
  acquireSlices(parts, ReadClass::METADATA, join);
  return true;
}

bool SegmentCore::finishOpen(Directory& dir, std::string_view segment, const Source& source, bool missingFileOK,
                             bool expectSynced) {
  constexpr uint32_t primary = Postings::PRIMARY_FILE;
  SegmentReadContext primaryContext{this, PackedTerm(nullptr), primary};
  validateSegmentFileHeader(slice({{primary, 0}, Postings::LUXIR_HEADER.size()}, ReadClass::METADATA), segment, primary);
  auto bytes = slice({{primary, range.offset}, range.length}, ReadClass::METADATA);
  auto head = SegmentCatalog::readDirectory(bytes, range, files[primary].size, primaryContext);
  catalog = SegmentCatalog::decode(head, source.files, primaryContext);
  if (spaces().front().maxDoc != source.maxDoc) {
    corruptSegment(segment, "segment row count differs from manifest");
  }
  files.resize(source.files.size());
  for (uint32_t i = 1; i < files.size(); ++i) {
    if (source.files[i] && !openBaseFile(dir, segment, i, source.files[i], missingFileOK, expectSynced, true)) return false;
  }
  initializeSpaces();
  return true;
}

bool SegmentCore::initializeFromFiles(Directory& dir, bool missingFileOK, bool expectSynced) {
  constexpr uint32_t primary = Postings::PRIMARY_FILE;
  auto segment = segmentName();
  SegmentReadContext primaryContext{this, PackedTerm(nullptr), primary};
  if (!openBaseFile(dir, segment, primary, 0, missingFileOK, expectSynced, false)) return false;
  auto file0 = slice({{primary, 0}, files[primary].size});
  validateSegmentFileHeader(file0, segment, primary);
  auto head = SegmentCatalog::readDirectory(file0, primaryContext);
  auto offset = head.catalogOffset();
  range = {offset, file0.size() - offset, std::nullopt};
  auto numbers = head.fileNumbers();
  files.resize((size_t)numbers.back() + 1);
  std::vector<uint64_t> sizes(files.size());
  sizes[primary] = files[primary].size;
  for (auto number : numbers.subspan(1)) {
    if (!openBaseFile(dir, segment, number, 0, missingFileOK, expectSynced, true)) return false;
    sizes[number] = files[number].size;
  }
  catalog = SegmentCatalog::decode(head, sizes, primaryContext);
  initializeSpaces();
  return true;
}

// Reads nothing beyond the catalog: version runs and ownership shapes are
// built on first use.
void SegmentCore::initializeSpaces() {
  everyHoldFree = std::ranges::all_of(files, [](const BaseFile& base) { return !base.file || base.holdsFree; });
  root.initialize(spaces()[0].maxDoc, spaces()[0].fields.size());
  children.reserve(spaces().size() - 1);
  for (size_t i = 1; i < spaces().size(); ++i) {
    const auto& space = spaces()[i];
    auto parent = spaceOrdinal(spaceParentPath(space.path));
    auto child = std::make_unique<ChildSpace>(*this, (SpaceOrdinal)i, *parent);
    child->reader.initialize(space.maxDoc, space.fields.size());
    children.push_back(std::move(child));
  }
}

void SegmentCore::listParts(const std::function<bool(std::string_view space, std::string_view field)>& data,
                            const std::function<void(const ListedPart&)>& visit) {
  using Kind = ListedPart::Kind;
  auto emit = [&](Kind kind, ReadClass readClass, field_parts::Slice slice, std::string_view space,
                  std::string_view field = {}) {
    if (slice) visit({kind, readClass, slice, space, field});
  };
  forEachBaseFile([&](uint32_t number, uint64_t) {
    // File 0's header sits at the null location, which emit() skips.
    visit({Kind::CATALOG, ReadClass::METADATA, {{number, 0}, Postings::LUXIR_HEADER.size()}, {}, {}});
  });
  emit(Kind::CATALOG, ReadClass::METADATA, {{Postings::PRIMARY_FILE, range.offset}, range.length}, {});
  emit(Kind::NAVIGATION, ReadClass::METADATA, catalog.versionRuns().slice, {});
  std::vector<std::pair<SpaceOrdinal, std::vector<const DecodedFieldRecord*>>> listed;
  for (size_t ordinal = 0; ordinal < spaces().size(); ++ordinal) {
    const auto& space = spaces()[ordinal];
    std::string_view path = space.path;
    if (ordinal) {
      emit(Kind::NAVIGATION, ReadClass::DATA, space.ownership.ends, path);
      emit(Kind::NAVIGATION, ReadClass::DATA, space.ownership.owners, path);
    }
    auto* reader = spaceReader((SpaceOrdinal)ordinal);
    auto& selected = listed.emplace_back((SpaceOrdinal)ordinal, std::vector<const DecodedFieldRecord*>()).second;
    for (int32_t i = 0; i < reader->numFields(); ++i) {
      const auto* record = reader->fieldInfo(i);
      if (!record) continue;
      std::string_view name(record->name);
      auto navigation = [&](field_parts::Slice slice, ReadClass readClass = ReadClass::METADATA) {
        emit(Kind::NAVIGATION, readClass, slice, path, name);
      };
      navigation(record->termNavigation());
      if (const auto* number = record->numberColumn()) navigation(number->index());
      navigation(record->ordBlockMetadata(space.maxDoc));
      if (record->points()) navigation(record->pointsDirectory());
      if (const auto* stored = record->stored()) navigation(stored->names());
      navigation(record->idFilter(), ReadClass::DATA);
      if (data(path, name)) selected.push_back(record);
    }
  }
  for (auto& [ordinal, records] : listed) {
    const auto& space = spaces()[(size_t)ordinal];
    std::string_view path = space.path;
    for (const auto* record : records) {
      std::string_view name(record->name);
      auto extent = [&](field_parts::Slice slice) { emit(Kind::DATA, ReadClass::DATA, slice, path, name); };
      if (const auto* terms = record->terms()) {
        extent(terms->dictionaryBlocks());
        for (const auto& run : terms->runs) {
          extent(run.docs);
          extent(run.positions);
        }
      }
      extent(record->docsBitmap());
      extent(record->normsValues(space.maxDoc));
      if (const auto* number = record->numberColumn()) extent(number->blocks());
      if (const auto* ord = record->ordColumn()) extent(ord->values());
      if (const auto* bytes = record->bytesColumn()) {
        extent(bytes->values);
        extent(bytes->endOffsets);
      }
      if (const auto* points = record->points()) extent(points->leaves());
      if (const auto* stored = record->stored()) {
        extent(stored->chunks());
        extent(stored->dictionary());
        extent(stored->firstDocs);
        extent(stored->chunkOffsets);
      }
      extent(record->endValueRanks());
      extent(record->owners());
    }
  }
}

bool SegmentCore::mayContainId(std::string_view term, uint64_t hash) {
  const auto* info = root.fieldInfo("id");
  if (!info || !info->terms()) return false;
  if (const auto* bounds = info->dictionaryBounds()) {
    // A root id dictionary always records exact inclusive bounds.
    if (term < std::string_view(bounds->lower) || term > std::string_view(bounds->upper)) return false;
  }
  auto part = info->idFilter();
  if (!part) return true;
  // A filter this build cannot read proves nothing.
  auto filter = IdFilter::parse(slice(part));
  return !filter || filter->mayContainHash(hash);
}

std::shared_ptr<SegmentCore> SegmentCore::create(Directory& dir, const api::SegmentInfo& segment,
                                                 bool missingFileOK, bool expectSynced) {
  auto source = makeSource(segment.seg_id, segment.creator_epoch, catalogRangeFromManifest(segment.catalog), segment.max_doc, segment.files);
  return create(dir, segment.seg_id, segment.creator_epoch, source, missingFileOK, expectSynced);
}

std::shared_ptr<SegmentCore> SegmentCore::create(Directory& dir, uint64_t segId, uint64_t creatorEpoch,
                                                 const Source& source, bool missingFileOK, bool expectSynced) {
  auto core = std::shared_ptr<SegmentCore>(new SegmentCore(segId, creatorEpoch));
  auto segment = core->segmentName();
  {
    AcquireJoin join;
    if (!core->beginOpen(dir, segment, source, missingFileOK, expectSynced, join)) return nullptr;
    join.wait();
  }
  if (!core->finishOpen(dir, segment, source, missingFileOK, expectSynced)) return nullptr;
  core->opened = true;
  return core;
}

std::vector<std::shared_ptr<SegmentCore>> SegmentCore::create(Directory& dir,
                                                              std::span<const api::SegmentInfo* const> segments,
                                                              bool missingFileOK, bool expectSynced) {
  std::vector<Opening> openings;
  openings.reserve(segments.size());
  for (const auto* segment : segments) {
    openings.push_back({segment->seg_id, segment->creator_epoch,
                        makeSource(segment->seg_id, segment->creator_epoch, catalogRangeFromManifest(segment->catalog),
                                   segment->max_doc, segment->files)});
  }
  return create(dir, openings, missingFileOK, expectSynced);
}

std::vector<std::shared_ptr<SegmentCore>> SegmentCore::create(Directory& dir, std::span<const Opening> segments,
                                                              bool missingFileOK, bool expectSynced) {
  std::vector<std::shared_ptr<SegmentCore>> cores;
  std::vector<std::string> names;
  cores.reserve(segments.size());
  names.reserve(segments.size());
  {
    AcquireJoin join;
    for (const auto& segment : segments) {
      auto& core = cores.emplace_back(new SegmentCore(segment.segId, segment.creatorEpoch));
      names.push_back(core->segmentName());
      if (!core->beginOpen(dir, names.back(), segment.source, missingFileOK, expectSynced, join)) core = nullptr;
    }
    join.wait();
  }
  for (size_t i = 0; i < cores.size(); ++i) {
    if (cores[i] && !cores[i]->finishOpen(dir, names[i], segments[i].source, missingFileOK, expectSynced)) {
      cores[i] = nullptr;
    }
    if (cores[i]) cores[i]->opened = true;
  }
  return cores;
}

std::shared_ptr<SegmentCore> SegmentCore::create(Directory& dir, uint64_t segId, uint64_t creatorEpoch,
                                                 bool missingFileOK, bool expectSynced) {
  auto core = std::shared_ptr<SegmentCore>(new SegmentCore(segId, creatorEpoch));
  if (!core->initializeFromFiles(dir, missingFileOK, expectSynced)) return nullptr;
  core->opened = true;
  return core;
}

SegmentCore::SegmentCore(Directory& dir, uint64_t segId, uint64_t creatorEpoch)
    : SegmentCore(segId, creatorEpoch) {
  initializeFromFiles(dir, false, false);
  opened = true;
}

} // namespace luxir
