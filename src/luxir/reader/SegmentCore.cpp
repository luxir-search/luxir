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
#include "luxir/api/luxir_index.hpp"
#include "luxir/util/ApiError.h"
#include "luxir/util/IoContext.h"

namespace luxir {

namespace {

void validateSegmentFileHeader(std::span<const char> bytes, std::string_view segment, uint32_t fileNumber) {
  std::string_view header = Postings::LUXIR_HEADER;
  if (bytes.size() < header.size() || std::memcmp(bytes.data(), header.data(), header.size()) != 0) {
    corruptSegment(segment, std::format("invalid Luxir magic; expected '{}'", header), {}, fileNumber);
  }
}

field_parts::Slice headerOf(uint32_t fileNumber) {
  return {{fileNumber, 0}, Postings::LUXIR_HEADER.size()};
}

// A backend's error does not know the segment, so it is rethrown with the
// segment named and the original nested. An ApiError keeps its kind and code.
[[noreturn]] void rethrowForSegment(std::exception_ptr error, std::string_view segment) {
  auto named = [&](const std::exception& e) {
    return std::format("Segment '{}' failed to open: {}", segment, e.what());
  };
  try {
    std::rethrow_exception(error);
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const ApiError& e) {
    std::throw_with_nested(ApiError(e.kind, e.code, named(e)));
  } catch (const std::exception& e) {
    std::throw_with_nested(std::runtime_error(named(e)));
  }
}

} // namespace

// One segment of a manifest open, from beginOpen() to finishOpen().
struct SegmentCore::PendingOpen {
  std::shared_ptr<SegmentCore> core;
  Source source;
  std::string name;     // The segment's name, formatted once for file names and errors.
  size_t slot;          // Where the caller stores the core.
  std::exception_ptr error;  // The first failure its batches reported.

  PendingOpen(uint64_t segId, uint64_t creatorEpoch, Source source, size_t slot = 0)
      : core(new SegmentCore(segId, creatorEpoch)), source(std::move(source)), name(core->segmentName()),
        slot(slot) {}
};

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

template <class Issue>
void SegmentCore::issueParts(std::span<const Part> parts, Issue&& issue) {
  using Key = std::pair<uint64_t, uint64_t>;
  std::vector<std::pair<Key, ReadClass>> keys;
  for (auto [part, readClass] : parts) {
    auto number = part.location.filenum();
    assert(number < files.size() && files[number].file);
    if (!files[number].holdsFree) keys.push_back({{part.location.raw(), part.bytes}, readClass});  // See BaseFile.
  }
  if (keys.empty()) return;
  // raw() keeps the file number in its high bits, so sorted keys run by file.
  // A part listed twice goes out once, with the class it was first listed with.
  auto key = &std::pair<Key, ReadClass>::first;
  std::ranges::stable_sort(keys, {}, key);
  keys.erase(std::ranges::unique(keys, {}, key).begin(), keys.end());
  {
    std::lock_guard lock(holdsMutex);
    std::erase_if(keys, [&](const auto& entry) { return holds.contains(entry.first); });
  }
  auto fileOf = [](const auto& entry) { return seg_location::fromRaw(entry.first.first).filenum(); };
  for (auto begin = keys.begin(); begin != keys.end();) {
    auto number = fileOf(*begin);
    auto end = std::find_if(begin, keys.end(), [&](const auto& entry) { return fileOf(entry) != number; });
    std::vector<ByteRange> ranges;
    std::vector<Key> batch;
    ranges.reserve((size_t)(end - begin));
    batch.reserve((size_t)(end - begin));
    uint64_t total = 0;
    for (auto entry = begin; entry != end; ++entry) {
      auto [raw, bytes] = entry->first;
      ranges.push_back({seg_location::fromRaw(raw).offset(), bytes, entry->second});
      batch.push_back(entry->first);
      total += bytes;
    }
    // One batch: the request counts its ranges and one call.
    countAcquire(total, ranges.size());
    auto take = [this, batch = std::move(batch)](std::vector<HeldRange>&& acquired) {
      std::lock_guard lock(holdsMutex);
      for (size_t i = 0; i < batch.size(); ++i) holds.try_emplace(batch[i], std::move(acquired[i]));
    };
    issue(*files[number].file, std::span<const ByteRange>(ranges), std::move(take));
    begin = end;
  }
}

void SegmentCore::acquireSlices(std::span<const field_parts::Slice> parts, ReadClass readClass, AcquireJoin& join,
                                std::exception_ptr* error) {
  std::vector<Part> classed;
  classed.reserve(parts.size());
  for (auto part : parts) classed.push_back({part, readClass});
  issueParts(classed, [&](InputFile& file, std::span<const ByteRange> ranges, AcquireBatches::Take take) {
    join.acquire(file, ranges, std::move(take), error);
  });
}

void SegmentCore::acquireSlices(std::span<const field_parts::Slice> parts, ReadClass readClass) {
  AcquireJoin join;
  acquireSlices(parts, readClass, join);
  countAcquireRound();
  join.wait();
}

void SegmentCore::acquireParts(std::span<const Part> parts, AcquireBatches& batches) {
  issueParts(parts, [&](InputFile& file, std::span<const ByteRange> ranges, AcquireBatches::Take take) {
    batches.acquire(file, ranges, std::move(take));
  });
}

bool SegmentCore::held(field_parts::Slice part) {
  auto number = part.location.filenum();
  assert(number < files.size() && files[number].file);
  if (files[number].holdsFree) return true;
  std::lock_guard lock(holdsMutex);
  return holds.contains({part.location.raw(), part.bytes});
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
  // Declared after `hold`, so it unlocks before a duplicate hold releases.
  std::lock_guard lock(holdsMutex);
  return holds.try_emplace(key, std::move(hold)).first->second.bytes();
}

void SegmentCore::releaseHold(field_parts::Slice part) {
  decltype(holds)::node_type node;  // Declared before the lock, so the hold releases after it.
  std::lock_guard lock(holdsMutex);
  node = holds.extract(std::pair(part.location.raw(), part.bytes));
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
  return space.shape.getOrCreate([&] {
    // A mapping binds every shape from its descendant up, so the shapes above
    // this one are likely next: acquire the columns of every unbuilt shape on
    // the path in one batch. Inside the build, so concurrent first users of
    // this shape wait on one batch instead of each issuing it.
    std::vector<field_parts::Slice> parts;
    for (auto ordinal = child; ordinal != SpaceOrdinal::ROOT; ordinal = children[(size_t)ordinal - 1]->parent) {
      if (children[(size_t)ordinal - 1]->shape.peek()) continue;
      const auto& descriptor = spaces()[(size_t)ordinal].ownership;
      for (auto part : {descriptor.ends, descriptor.owners}) {
        if (part) parts.push_back(part);
      }
    }
    if (parts.size() > 1) core.acquireSlices(parts);
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
  // With the manifest's size, size() needs no request; see Directory::openFile.
  auto file = dir.openFile(name, {.expectSynced = expectSynced, .size = expected});
  if (!file) {
    if (missingFileOK) return false;
    throw std::filesystem::filesystem_error(
        std::format("Can't find/open segment file '{}'", name),
        std::make_error_code(std::errc::no_such_file_or_directory));
  }
  auto size = (uint64_t)file->size();
  if (expected && size != expected) {
    corruptSegment(segment, "file size differs from manifest", {}, number);
  }
  if ((expected || checkHeader) && size <= Postings::LUXIR_HEADER.size()) {
    corruptSegment(segment, "segment file is too short", {}, number);
  }
  if (checkHeader) {
    auto header = file->acquire({0, Postings::LUXIR_HEADER.size(), ReadClass::METADATA});
    if (!file->holdsFree()) countAcquire(header.getLength());
    validateSegmentFileHeader(header.bytes(), segment, number);
  }
  bool holdsFree = file->holdsFree();
  if (number >= files.size()) files.resize(number + 1);
  files[number] = {std::move(file), size, holdsFree};
  return true;
}

// Opens every file the manifest lists, so each file's header can go out with
// file 0's catalog: one batch per file, issued on `join`.
bool SegmentCore::beginOpen(Directory& dir, PendingOpen& pending, bool missingFileOK, bool expectSynced,
                            AcquireJoin& join) {
  constexpr uint32_t primary = Postings::PRIMARY_FILE;
  const auto& source = pending.source;
  if (source.files.empty() || !source.files[primary]) corruptSegment(pending.name, "missing primary file in manifest");
  files.resize(source.files.size());
  std::vector<field_parts::Slice> parts;
  for (uint32_t number = 0; number < files.size(); ++number) {
    if (!source.files[number]) continue;
    if (!openBaseFile(dir, pending.name, number, source.files[number], missingFileOK, expectSynced, false)) {
      return false;
    }
    parts.push_back(headerOf(number));
  }
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
  parts.push_back({{primary, range.offset}, range.length});
  acquireSlices(parts, ReadClass::METADATA, join, &pending.error);
  return true;
}

void SegmentCore::finishOpen(const PendingOpen& pending) {
  constexpr uint32_t primary = Postings::PRIMARY_FILE;
  // A header is read only here, so its hold goes once checked.
  for (uint32_t number = 0; number < files.size(); ++number) {
    if (!files[number].file) continue;
    validateSegmentFileHeader(slice(headerOf(number), ReadClass::METADATA), pending.name, number);
    releaseHold(headerOf(number));
  }
  SegmentReadContext primaryContext{this, PackedTerm(nullptr), primary};
  auto bytes = slice({{primary, range.offset}, range.length}, ReadClass::METADATA);
  auto head = SegmentCatalog::readDirectory(bytes, range, files[primary].size, primaryContext);
  catalog = SegmentCatalog::decode(head, pending.source.files, primaryContext);
  if (spaces().front().maxDoc != pending.source.maxDoc) {
    corruptSegment(pending.name, "segment row count differs from manifest");
  }
  initializeSpaces();
}

// Segments fail in order, as if opened one at a time: issuing stops at the
// first that cannot open (a throw, or a missing file the caller retries on),
// and after the wait the lowest segment that failed decides, whichever batch
// completed first.
void SegmentCore::openPending(Directory& dir, std::span<PendingOpen> opens, bool missingFileOK, bool expectSynced) {
  size_t stop = opens.size();
  std::exception_ptr thrown;
  {
    // The caller owns the cores, so they outlive the join, and every batch
    // lands in a live core even when an open throws.
    AcquireJoin join;
    for (size_t i = 0; i < opens.size(); ++i) {
      try {
        if (opens[i].core->beginOpen(dir, opens[i], missingFileOK, expectSynced, join)) continue;
      } catch (...) {
        thrown = std::current_exception();
      }
      stop = i;
      break;
    }
    join.wait();
  }
  for (size_t i = 0; i < opens.size(); ++i) {
    auto& pending = opens[i];
    if (pending.error) rethrowForSegment(pending.error, pending.name);
    if (i == stop) {
      if (thrown) std::rethrow_exception(thrown);
      for (; i < opens.size(); ++i) opens[i].core = nullptr;
      return;
    }
    pending.core->finishOpen(pending);
  }
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
      if (const auto* stored = record->stored()) {
        navigation(stored->names());
        navigation(stored->firstDocs);  // the chunk index
        navigation(stored->chunkOffsets);
      }
      navigation(record->idFilter());
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
      }
      extent(record->endValueRanks());
      extent(record->owners());
    }
  }
}

std::optional<IdFilter> SegmentCore::idFilter() {
  const auto* info = root.fieldInfo("id");
  auto part = info ? info->idFilter() : field_parts::Slice();
  if (!part) return std::nullopt;
  // Writers only write filters that parse, so one that does not is damaged.
  auto filter = IdFilter::parse(slice(part, ReadClass::METADATA));
  if (!filter) segmentReadContext(*this, *info, part.location.filenum()).invalid("id filter does not parse");
  return filter;
}

std::shared_ptr<SegmentCore> SegmentCore::create(Directory& dir, const api::SegmentInfo& segment,
                                                 bool missingFileOK, bool expectSynced) {
  auto source = makeSource(segment.seg_id, segment.creator_epoch, catalogRangeFromManifest(segment.catalog), segment.max_doc, segment.files);
  return create(dir, segment.seg_id, segment.creator_epoch, source, missingFileOK, expectSynced);
}

std::shared_ptr<SegmentCore> SegmentCore::create(Directory& dir, uint64_t segId, uint64_t creatorEpoch,
                                                 const Source& source, bool missingFileOK, bool expectSynced) {
  PendingOpen pending(segId, creatorEpoch, source);
  openPending(dir, std::span(&pending, 1), missingFileOK, expectSynced);
  return std::move(pending.core);
}

void SegmentCore::open(Directory& dir, std::span<const api::SegmentInfo> segments,
                       std::span<std::shared_ptr<SegmentCore>> cores, bool missingFileOK, bool expectSynced) {
  assert(segments.size() == cores.size());
  auto missing = (size_t)std::ranges::count(cores, nullptr);
  if (!missing) return;  // A reopen that carries every core over allocates nothing.
  std::vector<PendingOpen> opens;
  opens.reserve(missing);
  for (size_t i = 0; i < segments.size(); ++i) {
    if (cores[i]) continue;
    const auto& segment = segments[i];
    opens.emplace_back(segment.seg_id, segment.creator_epoch,
                       makeSource(segment.seg_id, segment.creator_epoch, catalogRangeFromManifest(segment.catalog),
                                  segment.max_doc, segment.files), i);
  }
  openPending(dir, opens, missingFileOK, expectSynced);
  for (auto& pending : opens) cores[pending.slot] = std::move(pending.core);
}

std::shared_ptr<SegmentCore> SegmentCore::create(Directory& dir, uint64_t segId, uint64_t creatorEpoch,
                                                 bool missingFileOK, bool expectSynced) {
  auto core = std::shared_ptr<SegmentCore>(new SegmentCore(segId, creatorEpoch));
  if (!core->initializeFromFiles(dir, missingFileOK, expectSynced)) return nullptr;
  return core;
}

SegmentCore::SegmentCore(Directory& dir, uint64_t segId, uint64_t creatorEpoch)
    : SegmentCore(segId, creatorEpoch) {
  initializeFromFiles(dir, false, false);
}

} // namespace luxir
