// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "SegmentCore.h"

#include <cstring>
#include <filesystem>
#include <format>
#include <stdexcept>
#include "Postings.h"
#include "OwnershipShape.h"
#include "luxir/util/log.h"
#include <algorithm>

namespace luxir {

namespace {

void validateSegmentFileHeader(InputFile& file, std::string_view fileName) {
  // Open-time reads go through ranges: a lazy file fetches only these bytes.
  std::string_view header = Postings::LUXIR_HEADER;
  if (file.size() < header.size()
      || std::memcmp(file.rangeStream(0, header.size()).ptr(), header.data(), header.size()) != 0) {
    throw std::runtime_error(std::format(
        "Segment file '{}' has invalid Luxir magic; expected '{}'",
        fileName, header));
  }
}

[[noreturn]] void invalidSegment(std::string_view fileName, std::string_view what) {
  throw std::runtime_error(std::format("Segment file '{}' is corrupt or in an unsupported format: {}", fileName, what));
}

} // namespace

void SegmentCore::checkLocation(seg_location loc, uint64_t length) const {
  uint32_t fnum = loc.filenum();
  if (fnum >= files.size() || !files[fnum] || loc.offset() > files[fnum]->size()
      || length > files[fnum]->size() - loc.offset()) {
    throw std::runtime_error("segment data location out of range");
  }
}

// Every region a field's metadata points to lies within one of the segment's files.
void SegmentCore::checkFieldLocations(const SegFieldInfo& field) const {
  auto check = [&](seg_location loc, uint64_t length = 0) {
    if (loc.isNull()) return;
    try {
      checkLocation(loc, length);
    } catch (const std::runtime_error&) {
      throw std::runtime_error(std::format(
          "Field '{}' of a segment is corrupt or in an unsupported format: data location out of range",
          std::string_view(field.fieldname)));
    }
  };
  check(field.termBlockIndexLoc);
  check(field.termsLoc);
  check(field.docsLoc);
  check(field.posLoc);
  check(field.trieLoc);
  check(field.rangeTableLoc);
  check(field.docsWithFieldEndLoc);
  check(field.columnLoc, field.columnMetaOff);
  check(field.pointsLoc, field.pointsMetaOff);
  check(field.normsLoc, field.normsLen);
  check(field.monoLoc, field.monoMetaOff);
  check(field.mono2Loc, field.mono2MetaOff);
  check(field.valDocLoc, field.valDocMetaOff);
}

struct SegmentCore::ChildSpace {
  PostingsReader reader;
  OwnershipShape shape;

  ChildSpace(SegmentCore& core, SpaceOrdinal ordinal, OwnershipShape shape)
      : reader(core, ordinal), shape(std::move(shape)) {}
};

SegmentCore::SegmentCore(uint64_t segId, uint64_t creatorEpoch)
    : segId(segId), creatorEpoch(creatorEpoch), root(*this, SpaceOrdinal::ROOT) {}

SegmentCore::~SegmentCore() = default;

std::string SegmentCore::fileName(uint32_t fnum) const {
  return Postings::getIndexFileName(Postings::segmentKey(segId, creatorEpoch), fnum);
}

InputStream SegmentCore::materializeStream(uint32_t fnum, std::string_view caller) {
  StreamSlot& slot = streams[fnum];
  std::lock_guard lock(slot.mutex);  // other files materialize in parallel
  if (!slot.ready.load(std::memory_order_relaxed)) {
    InputFile& file = *files[fnum];
    uint64_t limit = maxMaterializeBytes.load(std::memory_order_relaxed);
    if (!slot.warned && file.lazy() && limit > 0 && file.size() > limit) {
      // Every concurrent reader of this file waits on this fetch.
      slot.warned = true;
      largeMaterializations.fetch_add(1, std::memory_order_relaxed);
      LOG_WARN("Materializing the whole of lazy segment file '{}' ({} bytes, over reader.max-materialize-bytes={}){}{}",
               fileName(fnum), file.size(), limit, caller.empty() ? "" : " for ", caller);
    }
    // May fetch the whole file and throw; the slot stays unready then and the
    // next caller retries.
    slot.stream = files[fnum]->getInputStream();
    slot.ready.store(true, std::memory_order_release);
  }
  return slot.stream;
}

std::optional<SpaceOrdinal> SegmentCore::spaceOrdinal(std::string_view path) const noexcept {
  auto found = std::lower_bound(spaceDescriptors.begin(), spaceDescriptors.end(), path,
                               [](const auto& space, std::string_view target) { return space.path < target; });
  if (found == spaceDescriptors.end() || found->path != path) return {};
  return (SpaceOrdinal)(found - spaceDescriptors.begin());
}

PostingsReader* SegmentCore::spaceReader(SpaceOrdinal ordinal) noexcept {
  if (ordinal == SpaceOrdinal::ROOT) return &root;
  return (size_t)ordinal < spaceDescriptors.size() ? &children[(size_t)ordinal - 1]->reader : nullptr;
}

PostingsReader* SegmentCore::spaceReader(std::string_view path) noexcept {
  auto ordinal = spaceOrdinal(path);
  return ordinal ? spaceReader(*ordinal) : nullptr;
}

const OwnershipShape* SegmentCore::ownership(SpaceOrdinal child) const noexcept {
  if (child == SpaceOrdinal::ROOT) return nullptr;
  return (size_t)child < spaceDescriptors.size() ? &children[(size_t)child - 1]->shape : nullptr;
}

const OwnershipShape* SegmentCore::ownership(std::string_view path) const noexcept {
  auto ordinal = spaceOrdinal(path);
  return ordinal ? ownership(*ordinal) : nullptr;
}

void SegmentCore::initializeSpace(PostingsReader& reader, const SpaceDescriptor& space) {
  // Field offset tables live in the base footer, before segment metadata.
  if (space.fieldOffsets.filenum() != 0 || space.fieldCount > INT32_MAX
      || space.fieldOffsets.offset() < Postings::LUXIR_HEADER.size()
      || (space.fieldOffsets.offset() & 3) != 0
      || space.fieldOffsets.offset() > (uint64_t)segInfoOffset
      || ((uint64_t)space.fieldCount + 1) * sizeof(uint32_t) > (uint64_t)segInfoOffset - space.fieldOffsets.offset()) {
    throw std::runtime_error("invalid row space field table");
  }
  auto offsetsLoc = (int64_t)space.fieldOffsets.offset();
  InputStream fields = files[0]->rangeStream((uint64_t)offsetsLoc, ((uint64_t)space.fieldCount + 1) * sizeof(uint32_t));
  const auto* offsets = (const uint32_t*)fields.ptr(0);
  fields.seek((int64_t)space.fieldCount * sizeof(uint32_t));
  if (fields.checkedInt() != (int32_t)space.fieldCount) throw std::runtime_error("inconsistent row space field count");
  catalogStart = std::min(catalogStart, offsetsLoc);
  // Each offset is back from the table to a field's metadata, which starts with
  // its name; the largest offset is the first entry, where the space's metadata begins.
  for (uint32_t i = 0; i < space.fieldCount; i++) {
    if (offsets[i] == 0 || offsets[i] > offsetsLoc - (int64_t)Postings::LUXIR_HEADER.size()) {
      throw std::runtime_error("invalid row space field offset");
    }
    catalogStart = std::min(catalogStart, offsetsLoc - (int64_t)offsets[i]);
  }
  reader.initialize(space.maxDoc, offsetsLoc, offsets, (int32_t)space.fieldCount);
}

std::vector<ByteRange> SegmentCore::openReads(uint32_t fileNum, uint64_t fileSize) {
  static_assert(OPEN_HEADER_BYTES >= Postings::LUXIR_HEADER.size());
  // validateSegmentFileHeader reads every file's header.
  uint64_t header = std::min(fileSize, OPEN_HEADER_BYTES);
  if (fileNum != 0 || fileSize <= header) return {{0, header}};
  // File 0 also ends with what initializeFromFiles reads back from its end:
  // the segment info and its size, each space's field count and offsets, the
  // catalog.
  uint64_t tail = std::max(header, fileSize - std::min(fileSize, OPEN_TAIL_BYTES));
  if (tail == header) return {{0, fileSize}};
  return {{0, header}, {tail, fileSize - tail}};
}

// Initialize from files, returns false if missing files and missingFileOK=true
// (openReads lists what this reads before it reads anything)
bool SegmentCore::initializeFromFiles(Directory& dir, bool missingFileOK, bool expectSynced) {
  std::string segStr = Postings::segmentKey(segId, creatorEpoch);
  auto segInfoFile = Postings::getIndexFileName(segStr, 0);
  files.emplace_back(dir.openFile(segInfoFile, expectSynced));

  if (files.back().get() == nullptr) {
    if (missingFileOK) {
      return false;
    }
    throw std::filesystem::filesystem_error(
            std::format("Can't find/open first segment file '{}'", segInfoFile),
            std::make_error_code(std::errc::no_such_file_or_directory));
  }
  validateSegmentFileHeader(*files.back(), segInfoFile);

  InputFile& first = *files[0];
  const int64_t firstSize = (int64_t)first.size();

  // File 0 ends with: ... field metadata, field offsets table, field count,
  // segment info, segment info size. See PostingsWriter.writeSegmentInfo.
  // Each space has its own field table; segment info catalogs their locations.
  const int64_t headerSize = (int64_t)Postings::LUXIR_HEADER.size();
  if (firstSize < headerSize + 2 * (int64_t)sizeof(int32_t)) invalidSegment(segInfoFile, "no segment info");
  int64_t segInfoSize = first.rangeStream((uint64_t)firstSize - sizeof(int32_t), sizeof(int32_t)).readInt();
  if (segInfoSize < 0 || segInfoSize > firstSize - headerSize - 2 * (int64_t)sizeof(int32_t)) {
    invalidSegment(segInfoFile, "invalid segment info size");
  }
  segInfoOffset = firstSize - sizeof(int32_t) - segInfoSize;
  InputStream segInfo = first.rangeStream((uint64_t)segInfoOffset, (uint64_t)segInfoSize);
  std::vector<uint32_t> physicalFileNums;
  seg_location versionsLoc;
  uint32_t versionCount;
  int32_t maxdoc;
  try {
    maxdoc = (int32_t)segInfo.checkedVint();
    uint32_t physicalFileCount = segInfo.checkedVint();
    if (physicalFileCount == 0 || physicalFileCount > segInfo.left()) {
      throw std::runtime_error("invalid physical-file count");
    }
    physicalFileNums.reserve(physicalFileCount);
    uint32_t previous = 0;
    for (uint32_t i = 0; i < physicalFileCount; i++) {
      uint32_t fileNum = segInfo.checkedVint();
      if ((i == 0 && fileNum != 0) || (i != 0 && fileNum <= previous) || fileNum > seg_location::maxFilenum()) {
        throw std::runtime_error("physical-file list is not strictly sorted from file 0");
      }
      physicalFileNums.push_back(fileNum);
      previous = fileNum;
    }
    versionsLoc = seg_location::checkedRead(segInfo);
    versionCount = segInfo.checkedVint();
    uint32_t spaceCount = segInfo.checkedVint();
    if (spaceCount == 0 || spaceCount > segInfo.left()) throw std::runtime_error("invalid row space count");
    spaceDescriptors.reserve(spaceCount);
    for (uint32_t i = 0; i < spaceCount; i++) {
      SpaceDescriptor space;
      uint32_t pathSize = segInfo.checkedVint();
      segInfo.require(pathSize);
      space.path.assign(segInfo.ptr(), pathSize);
      segInfo.skip(pathSize);
      uint32_t count = segInfo.checkedVint();
      if (count > INT32_MAX || (i != 0 && count >= (uint32_t)PostingsReader::HARD_MAX_DOC)) {
        throw std::runtime_error("row space count exceeds row id limit");
      }
      space.maxDoc = (int32_t)count;
      space.fieldOffsets = seg_location::checkedRead(segInfo);
      space.fieldCount = segInfo.checkedVint();
      if ((i == 0 && (!space.path.empty() || space.maxDoc != maxdoc))
          || (i != 0 && (space.path <= spaceDescriptors.back().path || count == 0))) {
        throw std::runtime_error("row space catalog is not strictly sorted from root");
      }
      if (i != 0) {
        OwnershipDescriptor shape;
        shape.ends = seg_location::checkedRead(segInfo);
        shape.endsMetaOff = (int64_t)segInfo.checkedVlong();
        shape.owners = seg_location::checkedRead(segInfo);
        shape.ownersMetaOff = (int64_t)segInfo.checkedVlong();
        space.ownership = shape;
      }
      spaceDescriptors.push_back(space);
    }
    if (segInfo.left() != 0) throw std::runtime_error("trailing segment catalog bytes");
  } catch (const std::runtime_error& e) {
    invalidSegment(segInfoFile, e.what());
  }
  files.resize((size_t) physicalFileNums.back() + 1);
  streams = std::make_unique<StreamSlot[]>(files.size());

  for (size_t i = 1; i < physicalFileNums.size(); i++) {
    uint32_t fileNum = physicalFileNums[i];
    std::string fileName = Postings::getIndexFileName(segStr, fileNum);
    files[fileNum] = dir.openFile(fileName, expectSynced);
    if (files[fileNum] == nullptr) {
      if (missingFileOK) {
        return false;
      }
      throw std::filesystem::filesystem_error(
              std::format("Can't find/open segment file '{}'", fileName),
              std::make_error_code(std::errc::no_such_file_or_directory));
    }
    validateSegmentFileHeader(*files[fileNum], fileName);
  }
  // A file that is already resident publishes its stream now; a lazy file's
  // stream is made by its first bulk reader.
  for (size_t fileNum = 0; fileNum < files.size(); fileNum++) {
    if (files[fileNum] && !files[fileNum]->lazy()) {
      streams[fileNum].stream = files[fileNum]->getInputStream();
      streams[fileNum].ready.store(true, std::memory_order_relaxed);
    }
  }
  if (maxdoc > 0) {
    uint32_t fileNum = versionsLoc.filenum();
    if (versionCount == 0 || versionsLoc.isNull() || fileNum >= files.size() || !files[fileNum]) {
      throw std::runtime_error("Segment has invalid request birth versions");
    }
    // The run directory ends at the header; the blocks precede it. Both are
    // validated and then read lazily through pointers, so make them resident.
    InputFile& file = *files[fileNum];
    uint64_t fileSize = file.size(), offset = versionsLoc.offset();
    if (offset > fileSize || fileSize - offset < sizeof(VersionRunsHeader)) {
      throw std::runtime_error("Invalid request-version run coverage or encoding");
    }
    VersionRunsHeader header;
    std::memcpy(&header, file.rangeStream(offset, sizeof(VersionRunsHeader)).ptr(), sizeof(header));
    if (header.bytes < sizeof(VersionRunsHeader) || header.bytes > offset + sizeof(VersionRunsHeader)) {
      throw std::runtime_error("Invalid request-version run coverage or encoding");
    }
    // One range holds the blocks and the header after them; the runs read
    // only within it, so no two ranges need to share a base address.
    InputStream runs = file.rangeStream(offset + sizeof(VersionRunsHeader) - header.bytes, header.bytes);
    versions = VersionRuns(runs.ptr(), header.bytes, header.bytes - sizeof(VersionRunsHeader), versionCount, maxdoc);
  } else if (maxdoc != 0 || versionCount != 0 || !versionsLoc.isNull()) {
    throw std::runtime_error("Segment has invalid request birth versions");
  }
  catalogStart = segInfoOffset;
  initializeSpace(root, spaceDescriptors[0]);
  children.reserve(spaceDescriptors.size() - 1);
  for (size_t i = 1; i < spaceDescriptors.size(); i++) {
    const auto& space = spaceDescriptors[i];
    auto parent = spaceOrdinal(spaceParentPath(space.path));
    if (!parent || (size_t)*parent >= i) invalidSegment(segInfoFile, "missing row space path prefix");
    auto child = std::make_unique<ChildSpace>(*this, (SpaceOrdinal)i,
        OwnershipShape(*this, *space.ownership, spaceDescriptors[(size_t)*parent].maxDoc, space.maxDoc));
    initializeSpace(child->reader, space);
    children.push_back(std::move(child));
  }
  catalog = first.rangeStream((uint64_t)catalogStart, (uint64_t)(firstSize - catalogStart));
  return true;
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
