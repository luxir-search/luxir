// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "SegmentCore.h"

#include <cstring>
#include <filesystem>
#include <format>
#include <stdexcept>
#include "Postings.h"
#include "ReaderErrors.h"
#include "OwnershipShape.h"
#include "luxir/api/luxir_index.hpp"

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
  OwnershipShape shape;

  ChildSpace(SegmentCore& core, SpaceOrdinal ordinal, OwnershipShape shape)
      : reader(core, ordinal), shape(std::move(shape)) {}
};

SegmentCore::SegmentCore(uint64_t segId, uint64_t creatorEpoch)
    : segId(segId), creatorEpoch(creatorEpoch), root(*this, SpaceOrdinal::ROOT) {}

SegmentCore::~SegmentCore() = default;

std::span<const char> SegmentCore::retain(InputFile& file, field_parts::Slice part, ReadClass readClass) {
  std::lock_guard lock(holdsMutex);
  auto key = std::pair(part.location.raw(), part.bytes);
  auto found = holds.lower_bound(key);
  if (found != holds.end() && found->first == key) return found->second.bytes();
  auto hold = file.acquire({part.location.offset(), part.bytes, readClass});
  return holds.emplace_hint(found, key, std::move(hold))->second.bytes();
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

const OwnershipShape* SegmentCore::ownership(SpaceOrdinal child) const noexcept {
  if (child == SpaceOrdinal::ROOT) return nullptr;
  return (size_t)child < spaces().size() ? &children[(size_t)child - 1]->shape : nullptr;
}

const OwnershipShape* SegmentCore::ownership(std::string_view path) const noexcept {
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

bool SegmentCore::initializeFromFiles(Directory& dir, const Source* source, bool missingFileOK, bool expectSynced) {
  constexpr uint32_t primary = Postings::PRIMARY_FILE;
  auto segment = segmentName();
  SegmentReadContext primaryContext{this, PackedTerm(nullptr), primary};
  auto open = [&](uint32_t number, uint64_t expected) {
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
    if (source || number != primary) {
      if (size <= Postings::LUXIR_HEADER.size()) {
        corruptSegment(segment, "segment file is too short", {}, number);
      }
      auto header = file->acquire({0, Postings::LUXIR_HEADER.size(), ReadClass::METADATA});
      validateSegmentFileHeader(header.bytes(), segment, number);
    }
    bool holdsFree = file->holdsFree();
    files[number] = {std::move(file), size, holdsFree};
    return true;
  };

  files.resize(1);
  if (source && (source->files.empty() || !source->files[primary])) {
    corruptSegment(segment, "missing primary file in manifest");
  }
  if (!open(primary, source ? source->files[primary] : 0)) return false;
  auto primarySize = files[primary].size;

  if (source) {
    range = source->range;
    if (range.offset > seg_location::maxOffset() || range.offset > primarySize
        || range.length > primarySize - range.offset) {
      primaryContext.invalid("catalog range lies outside file 0", "invalid segment catalog");
    }
    if (range.offset < Postings::LUXIR_HEADER.size()) {
      primaryContext.invalid("catalog starts inside file header", "invalid segment catalog");
    }
    auto bytes = slice({{primary, range.offset}, range.length}, ReadClass::METADATA);
    auto head = SegmentCatalog::readDirectory(bytes, range.offset, primarySize, primaryContext);
    catalog = SegmentCatalog::decode(head, source->files, primaryContext);
    if (spaces().front().maxDoc != source->maxDoc) {
      corruptSegment(segment, "segment row count differs from manifest");
    }
    files.resize(source->files.size());
    for (uint32_t i = 1; i < files.size(); ++i) {
      if (source->files[i] && !open(i, source->files[i])) return false;
    }
  } else {
    auto file0 = slice({{primary, 0}, primarySize});
    validateSegmentFileHeader(file0, segment, primary);
    auto head = SegmentCatalog::readDirectory(file0, primaryContext);
    auto offset = head.catalogOffset();
    range = {offset, file0.size() - offset, std::nullopt};
    auto numbers = head.fileNumbers();
    files.resize((size_t)numbers.back() + 1);
    std::vector<uint64_t> sizes(files.size());
    sizes[primary] = primarySize;
    for (auto number : numbers.subspan(1)) {
      if (!open(number, 0)) return false;
      sizes[number] = files[number].size;
    }
    catalog = SegmentCatalog::decode(head, sizes, primaryContext);
  }

  const auto& version = catalog.versionRuns();
  if (version.slice) {
    auto bytes = slice(version.slice, ReadClass::METADATA);
    SegmentReadContext context{this, PackedTerm(nullptr), version.slice.location.filenum()};
    versions = VersionRuns(bytes.data(), bytes.size(), version.trailerOffset, version.count, spaces()[0].maxDoc, context);
    if (versions.byteSize() != bytes.size()) context.invalid("version runs slice does not match its header");
  }
  root.initialize(spaces()[0].maxDoc, spaces()[0].fields.size());
  children.reserve(spaces().size() - 1);
  for (size_t i = 1; i < spaces().size(); ++i) {
    const auto& space = spaces()[i];
    auto parent = spaceOrdinal(spaceParentPath(space.path));
    OwnershipShape shape(*this, space.ownership, spaces()[(size_t)*parent].maxDoc, space.maxDoc);
    auto child = std::make_unique<ChildSpace>(*this, (SpaceOrdinal)i, std::move(shape));
    child->reader.initialize(space.maxDoc, space.fields.size());
    children.push_back(std::move(child));
  }
  return true;
}

std::shared_ptr<SegmentCore> SegmentCore::create(Directory& dir, const api::SegmentInfo& segment,
                                                 bool missingFileOK, bool expectSynced) {
  auto source = makeSource(segment.seg_id, segment.creator_epoch, catalogRangeFromManifest(segment.catalog), segment.max_doc, segment.files);
  return create(dir, segment.seg_id, segment.creator_epoch, source, missingFileOK, expectSynced);
}

std::shared_ptr<SegmentCore> SegmentCore::create(Directory& dir, uint64_t segId, uint64_t creatorEpoch,
                                                 const Source& source, bool missingFileOK, bool expectSynced) {
  auto core = std::shared_ptr<SegmentCore>(new SegmentCore(segId, creatorEpoch));
  if (!core->initializeFromFiles(dir, &source, missingFileOK, expectSynced)) return nullptr;
  return core;
}

std::shared_ptr<SegmentCore> SegmentCore::create(Directory& dir, uint64_t segId, uint64_t creatorEpoch,
                                                 bool missingFileOK, bool expectSynced) {
  auto core = std::shared_ptr<SegmentCore>(new SegmentCore(segId, creatorEpoch));
  if (!core->initializeFromFiles(dir, nullptr, missingFileOK, expectSynced)) return nullptr;
  return core;
}

SegmentCore::SegmentCore(Directory& dir, uint64_t segId, uint64_t creatorEpoch)
    : SegmentCore(segId, creatorEpoch) {
  initializeFromFiles(dir, nullptr, false, false);
}

} // namespace luxir
