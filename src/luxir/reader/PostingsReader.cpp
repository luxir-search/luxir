// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "PostingsReader.h"

#include <cstring>
#include <filesystem>
#include <format>
#include <memory>
#include <stdexcept>
#include <utility>
#include "FieldReader.h"
#include "FieldMetadataMemory.h"
#include "IntColReader.h"
#include "Postings.h"
#include "luxir/util/MemPool.h"

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


struct PostingsReader::FieldPage {
  std::atomic<Field*> slots[FIELDS_PER_PAGE]{};

  ~FieldPage() {
    for (auto& slot : slots) delete slot.load(std::memory_order_relaxed);
  }
};

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
  auto fields = inputStreams[0];
  fields.seek(segInfoOffset - sizeof(int32_t));
  auto count = fields.readInt();
  fieldOffsetsLoc = segInfoOffset - sizeof(int32_t) - (int64_t)count * sizeof(uint32_t);
  fieldOffsets = (const uint32_t*)fields.ptr(fieldOffsetsLoc);
  fieldPages = std::make_unique<std::atomic<FieldPage*>[]>(
      ((size_t)count + FIELDS_PER_PAGE - 1) / FIELDS_PER_PAGE);
  nFields = count;
  if (nFields > 0) {
    fieldMetadataMemory.directories.fetch_add(1, std::memory_order_relaxed);
    fieldMetadataMemory.directoryBytes.fetch_add(
        ((size_t)nFields + FIELDS_PER_PAGE - 1) / FIELDS_PER_PAGE * sizeof(std::atomic<FieldPage*>),
        std::memory_order_relaxed);
  }
  return true;
}

std::shared_ptr<PostingsReader> PostingsReader::create(Directory& dir, uint64_t segId, bool missingFileOK, bool expectSynced) {
  // Try to create the PostingsReader - use private constructor
  auto reader = std::shared_ptr<PostingsReader>(new PostingsReader());
  if (!reader->initializeFromFiles(dir, segId, missingFileOK, expectSynced)) {
    return nullptr;  // Missing files and missingFileOK=true
  }
  return reader;
}

PostingsReader::PostingsReader(Directory& dir, uint64_t segId) {
  // missingFileOK=false throws on a missing file rather than returning false
  initializeFromFiles(dir, segId, false);
}

PostingsReader::~PostingsReader() {
  auto stats = fieldCacheStats();
  auto directoryBytes = ((size_t)nFields + FIELDS_PER_PAGE - 1) / FIELDS_PER_PAGE * sizeof(std::atomic<FieldPage*>);
  fieldMetadataMemory.directories.fetch_sub(nFields > 0, std::memory_order_relaxed);
  fieldMetadataMemory.directoryBytes.fetch_sub(directoryBytes, std::memory_order_relaxed);
  fieldMetadataMemory.pages.fetch_sub(stats.pages, std::memory_order_relaxed);
  fieldMetadataMemory.pageBytes.fetch_sub(stats.pages * sizeof(FieldPage), std::memory_order_relaxed);
  fieldMetadataMemory.entries.fetch_sub(stats.entries, std::memory_order_relaxed);
  fieldMetadataMemory.entryBytes.fetch_sub(stats.entries * sizeof(Field), std::memory_order_relaxed);
  for (size_t i = 0; i < ((size_t)nFields + FIELDS_PER_PAGE - 1) / FIELDS_PER_PAGE; ++i) {
    delete fieldPages[i].load(std::memory_order_relaxed);
  }
}

int32_t PostingsReader::findOrdinal(std::string_view name) const {
  const auto& input = inputStreams[0];
  auto* end = fieldOffsets + nFields;
  auto* found = std::lower_bound(fieldOffsets, end, name, [&](uint32_t offset, std::string_view key) {
    return input.readPackedTerm(fieldOffsetsLoc - offset) < key;
  });
  return found != end && input.readPackedTerm(fieldOffsetsLoc - *found) == name
      ? (int32_t)(found - fieldOffsets) : -1;
}

const PostingsReader::Field* PostingsReader::fieldInfo(std::string_view field) {
  return fieldInfo(findOrdinal(field));
}

const PostingsReader::Field* PostingsReader::fieldInfo(int32_t ordinal) {
  if (ordinal < 0 || ordinal >= nFields) return nullptr;
  auto& pageSlot = fieldPages[(size_t)ordinal / FIELDS_PER_PAGE];
  auto* page = pageSlot.load(std::memory_order_acquire);
  if (page == nullptr) {
    auto fresh = std::make_unique<FieldPage>();
    if (pageSlot.compare_exchange_strong(page, fresh.get(),
                                        std::memory_order_release,
                                        std::memory_order_acquire)) {
      page = fresh.release();
      fieldMetadataMemory.pages.fetch_add(1, std::memory_order_relaxed);
      fieldMetadataMemory.pageBytes.fetch_add(sizeof(FieldPage), std::memory_order_relaxed);
    }
  }
  auto& slot = page->slots[(size_t)ordinal % FIELDS_PER_PAGE];
  auto* entry = slot.load(std::memory_order_acquire);
  if (entry == nullptr) {
    auto fresh = std::make_unique<Field>();
    FieldReader fields(*this, ordinal);
    fields.readFieldInfo(*fresh);
    if (FieldType::sortsByIntColumn(fresh->type) && fresh->columnLoc.offset() > 0) {
      IntColReader column(*this, *fresh);
      fresh->columnBounds = Field::ColumnBounds{column.getMin(), column.getMax()};
      if (column.leafBounded()) fresh->leafMeta = column.leafMeta();
    }
    if (slot.compare_exchange_strong(entry, fresh.get(),
                                    std::memory_order_release,
                                    std::memory_order_acquire)) {
      entry = fresh.release();
      fieldMetadataMemory.entries.fetch_add(1, std::memory_order_relaxed);
      fieldMetadataMemory.entryBytes.fetch_add(sizeof(Field), std::memory_order_relaxed);
    }
  }
  return entry;
}

PostingsReader::FieldCacheStats PostingsReader::fieldCacheStats() const noexcept {
  static_assert(sizeof(FieldPage) == 64 * sizeof(std::atomic<Field*>));
  FieldCacheStats stats;
  size_t pageCount = ((size_t)nFields + FIELDS_PER_PAGE - 1) / FIELDS_PER_PAGE;
  stats.bytes = pageCount * sizeof(std::atomic<FieldPage*>);
  for (size_t i = 0; i < pageCount; ++i) {
    auto* page = fieldPages[i].load(std::memory_order_acquire);
    if (!page) continue;
    stats.pages++;
    stats.bytes += sizeof(FieldPage);
    for (auto& slot : page->slots) {
      if (slot.load(std::memory_order_relaxed)) {
        stats.entries++;
        stats.bytes += sizeof(Field);
      }
    }
  }
  return stats;
}

} // namespace luxir
