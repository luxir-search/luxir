// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "PostingsReader.h"

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

} // namespace


// Initialize from files, returns false if missing files and missingFileOK=true
bool PostingsReader::initializeFromFiles(Directory& dir, uint64_t segId, bool missingFileOK, bool expectSynced) {
  std::string segStr = Postings::getSortableString(segId);
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

  // seek to the end of firstIs and read the size of the segmentInfo
  // See PostingsWriter.writeSegmentInfo
  firstIS.seek(firstIS.size() - sizeof(int32_t));
  auto segInfoSize = firstIS.readInt();
  segInfoOffset = firstIS.size() - sizeof(int32_t) - segInfoSize;
  firstIS.seek(segInfoOffset);
  maxdoc = firstIS.readVint();
  uint32_t physicalFileCount = firstIS.readVint();
  if (physicalFileCount == 0) {
    throw std::runtime_error("Segment physical-file list is empty");
  }
  std::vector<uint32_t> physicalFileNums;
  physicalFileNums.reserve(physicalFileCount);
  uint32_t previous = 0;
  for (uint32_t i = 0; i < physicalFileCount; i++) {
    uint32_t fileNum = firstIS.readVint();
    if ((i == 0 && fileNum != 0) || (i != 0 && fileNum <= previous)) {
      throw std::runtime_error("Segment physical-file list is not strictly sorted from file 0");
    }
    physicalFileNums.push_back(fileNum);
    previous = fileNum;
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
  return true;
}

std::shared_ptr<PostingsReader> PostingsReader::create(Directory& dir, uint64_t segId, bool missingFileOK, bool expectSynced) {
  std::string segStr = Postings::getSortableString(segId);
  auto segInfoFile = Postings::getIndexFileName(segStr, 0);
  auto firstFile = dir.openFile(segInfoFile, expectSynced);

  if (firstFile == nullptr) {
    if (missingFileOK) {
      return nullptr;
    }
    throw std::filesystem::filesystem_error(
            std::format("Can't find/open first segment file '{}'", segInfoFile),
            std::make_error_code(std::errc::no_such_file_or_directory));
  }

  // Try to create the PostingsReader - use private constructor
  auto reader = std::shared_ptr<PostingsReader>(new PostingsReader());
  if (!reader->initializeFromFiles(dir, segId, missingFileOK, expectSynced)) {
    return nullptr;  // Missing files and missingFileOK=true
  }
  return reader;
}

PostingsReader::PostingsReader(Directory& dir, uint64_t segId) {
  auto reader = create(dir, segId, false);
  // This should never fail (should throw exception on failure) since missingFileOK=false, but just in case
  if (!reader) {
    throw std::filesystem::filesystem_error(
            "Failed to create PostingsReader",
            std::make_error_code(std::errc::no_such_file_or_directory));
  }
  *this = std::move(*reader);
}

} // namespace luxir
