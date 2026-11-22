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
#include <algorithm>

namespace luxir {

namespace {

void validateSegmentFileHeader(InputFile& file, std::string_view segment, uint32_t fileNumber) {
  InputStream is = file.getInputStream();
  std::string_view header = Postings::LUXIR_HEADER;
  if (is.size() < (int64_t) header.size()
      || std::memcmp(is.ptr(0), header.data(), header.size()) != 0) {
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
    auto file = dir.openFile(name, expectSynced);
    if (!file) {
      if (missingFileOK) return false;
      throw std::filesystem::filesystem_error(
          std::format("Can't find/open segment file '{}'", name),
          std::make_error_code(std::errc::no_such_file_or_directory));
    }
    validateSegmentFileHeader(*file, segment, number);
    auto input = file->getInputStream();
    if (expected && (uint64_t)input.size() != expected) {
      corruptSegment(segment, "file size differs from manifest", {}, number);
    }
    files[number] = std::move(file);
    inputStreams[number] = input;
    return true;
  };

  files.resize(1);
  inputStreams.resize(1);
  if (source && (source->files.empty() || !source->files[primary])) {
    corruptSegment(segment, "missing primary file in manifest");
  }
  if (!open(primary, source ? source->files[primary] : 0)) return false;
  auto first = inputStreams[primary];
  std::span<const char> file0(first.ptr(), first.size());

  auto head = source ? SegmentCatalog::readDirectory(file0, source->range, primaryContext)
                     : SegmentCatalog::readDirectory(file0, primaryContext);
  if (source) {
    range = source->range;
    catalog = SegmentCatalog::decode(head, source->files, primaryContext);
    if (spaces().front().maxDoc != source->maxDoc) {
      corruptSegment(segment, "segment row count differs from manifest");
    }
    files.resize(source->files.size());
    inputStreams.resize(source->files.size());
    for (uint32_t i = 1; i < files.size(); ++i) {
      if (source->files[i] && !open(i, source->files[i])) return false;
    }
  } else {
    auto offset = head.catalogOffset();
    range = {offset, file0.size() - offset, std::nullopt};
    auto numbers = head.fileNumbers();
    files.resize((size_t)numbers.back() + 1);
    inputStreams.resize(files.size());
    std::vector<uint64_t> sizes(files.size());
    sizes[Postings::PRIMARY_FILE] = file0.size();
    for (auto number : numbers.subspan(1)) {
      if (!open(number, 0)) return false;
      sizes[number] = (uint64_t)inputStreams[number].size();
    }
    catalog = SegmentCatalog::decode(head, sizes, primaryContext);
  }

  const auto& version = catalog.versionRuns();
  if (version.slice) {
    const auto& slice = version.slice;
    auto input = getInputStreamSeek(slice.location);
    SegmentReadContext context{this, PackedTerm(nullptr), slice.location.filenum()};
    versions = VersionRuns(input.ptr(), slice.bytes, version.trailerOffset, version.count, spaces()[0].maxDoc, context);
    if (versions.byteSize() != slice.bytes) context.invalid("version runs slice does not match its header");
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
