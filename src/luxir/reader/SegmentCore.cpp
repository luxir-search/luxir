// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "SegmentCore.h"

#include <cstring>
#include <filesystem>
#include <format>
#include <stdexcept>
#include "Postings.h"

namespace luxir {

namespace {

void validateSegmentFileHeader(InputFile& file, std::string_view fileName) {
  InputStream is = file.getInputStream();
  std::string_view header = Postings::LUXIR_HEADER;
  if (is.size() < (int64_t) header.size()
      || std::memcmp(is.ptr(0), header.data(), header.size()) != 0) {
    throw std::runtime_error(std::format(
        "Segment file '{}' has invalid Luxir magic; expected '{}'",
        fileName, header));
  }
}

[[noreturn]] void invalidSegment(std::string_view fileName, std::string_view what) {
  throw std::runtime_error(std::format("Segment file '{}' is corrupt or in an unsupported format: {}", fileName, what));
}

} // namespace

// Every region a field's metadata points to lies within one of the segment's files.
void SegmentCore::checkFieldLocations(const SegFieldInfo& field) const {
  auto check = [&](seg_location loc, uint64_t length = 0) {
    if (loc.isNull()) return;
    uint32_t fnum = loc.filenum();
    if (fnum >= files.size() || !files[fnum] || loc.offset() > files[fnum]->size()
        || length > files[fnum]->size() - loc.offset()) {
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

// Initialize from files, returns false if missing files and missingFileOK=true
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

  inputStreams.emplace_back(files[0]->getInputStream());
  InputStream firstIS = inputStreams[0];

  // File 0 ends with: ... field metadata, field offsets table, field count,
  // segment info, segment info size. See PostingsWriter.writeSegmentInfo.
  const int64_t headerSize = (int64_t)Postings::LUXIR_HEADER.size();
  if (firstIS.size() < headerSize + 2 * (int64_t)sizeof(int32_t)) invalidSegment(segInfoFile, "no segment info");
  firstIS.seek(firstIS.size() - sizeof(int32_t));
  int64_t segInfoSize = firstIS.readInt();
  if (segInfoSize < 0 || segInfoSize > firstIS.size() - headerSize - 2 * (int64_t)sizeof(int32_t)) {
    invalidSegment(segInfoFile, "invalid segment info size");
  }
  segInfoOffset = firstIS.size() - sizeof(int32_t) - segInfoSize;
  InputStream segInfo(firstIS.ptr(segInfoOffset), firstIS.ptr(segInfoOffset + segInfoSize));
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
  } catch (const std::runtime_error& e) {
    invalidSegment(segInfoFile, e.what());
  }
  files.resize((size_t) physicalFileNums.back() + 1);
  inputStreams.resize((size_t) physicalFileNums.back() + 1);

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
    inputStreams[fileNum] = files[fileNum]->getInputStream();
  }
  if (maxdoc > 0) {
    uint32_t fileNum = versionsLoc.filenum();
    if (versionCount == 0 || versionsLoc.isNull() || fileNum >= inputStreams.size() || !files[fileNum]) {
      throw std::runtime_error("Segment has invalid request birth versions");
    }
    const auto& input = inputStreams[fileNum];
    versions = VersionRuns(input.ptr(0), input.size(), versionsLoc.offset(), versionCount, maxdoc);
  } else if (maxdoc != 0 || versionCount != 0 || !versionsLoc.isNull()) {
    throw std::runtime_error("Segment has invalid request birth versions");
  }
  auto fields = getInputStream(0);
  fields.seek(segInfoOffset - sizeof(int32_t));
  int64_t count = fields.readInt();
  if (count < 0 || count > (segInfoOffset - (int64_t)sizeof(int32_t) - headerSize) / (int64_t)sizeof(uint32_t)) {
    invalidSegment(segInfoFile, "invalid field count");
  }
  int64_t fieldOffsetsLoc = segInfoOffset - sizeof(int32_t) - count * (int64_t)sizeof(uint32_t);
  const uint32_t* fieldOffsets = (const uint32_t*)fields.ptr(fieldOffsetsLoc);
  // Each offset is back from the table to a field's metadata, which starts with its name.
  for (int64_t i = 0; i < count; i++) {
    if (fieldOffsets[i] == 0 || fieldOffsets[i] > fieldOffsetsLoc - headerSize) {
      invalidSegment(segInfoFile, "invalid field offset");
    }
  }
  root.initialize(maxdoc, fieldOffsetsLoc, fieldOffsets, (int32_t)count);
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
