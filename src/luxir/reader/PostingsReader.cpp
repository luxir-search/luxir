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

// Open-time reads go through ranges: a lazy file fetches only these bytes.
void validateSegmentFileHeader(InputFile& file, std::string_view fileName) {
  std::string_view header = Postings::LUXIR_HEADER;
  if (file.size() < header.size()) {
    throw std::runtime_error(std::format(
        "Segment file '{}' has invalid Luxir magic; expected '{}'",
        fileName, header));
  }
  InputStream is = file.rangeStream(0, header.size());
  if (std::memcmp(is.ptr(), header.data(), header.size()) != 0) {
    throw std::runtime_error(std::format(
        "Segment file '{}' has invalid Luxir magic; expected '{}'",
        fileName, header));
  }
}

[[noreturn]] void invalidSegment(std::string_view fileName, std::string_view what) {
  throw std::runtime_error(std::format("Segment file '{}' is corrupt: {}", fileName, what));
}

} // namespace

// Metadata layout: see StoredFieldsWriter.
StoredFieldsMetadata::StoredFieldsMetadata(PostingsReader& reader, const SegFieldInfo& field) {
  auto input = reader.getInputStream(field.columnLoc.filenum());
  const char* ptr = input.ptr((int64_t)field.columnLoc.offset() + field.columnMetaOff);
  const char* end = input.ptr(input.size());
  maxChunkBytes = (int64_t)InputStream::readVlong(ptr, end);
  numDirtyChunks = (int32_t)InputStream::readVint(ptr, end);
  uint8_t codec = (uint8_t)*ptr++;
  if (codec != StoredChunkFormat::LZ4 && codec != StoredChunkFormat::ZSTD) {
    throw std::runtime_error("stored fields: unknown codec " + std::to_string(codec));
  }
  format.codec = (StoredChunkFormat::Codec)codec;
  format.chunkTarget = InputStream::readVint(ptr, end);
  format.maxDocsPerChunk = InputStream::readVint(ptr, end);
  uint32_t dictSize = InputStream::readVint(ptr, end);
  if (dictSize > (size_t)(end - ptr)) {
    throw std::runtime_error("stored fields: dictionary exceeds file end");
  }
  dictionary = std::string_view(ptr, dictSize);
  ptr += dictSize;
  uint32_t numFields = InputStream::readVint(ptr, end);
  fieldNames.reserve(numFields);
  for (uint32_t i = 0; i < numFields; i++) {
    // The bytes live in the segment mapping for the PostingsReader's lifetime.
    PackedTerm term(const_cast<char*>(ptr));
    fieldNames.emplace_back(term.data(), term.size());
    ptr += term.memorySize();
  }
  if (!dictionary.empty()) {
    if (format.codec != StoredChunkFormat::ZSTD) {
      throw std::runtime_error("stored fields: dictionary requires ZSTD");
    }
    ddict.emplace(dictionary);
  }
}


struct PostingsReader::FieldPage {
  std::atomic<Field*> slots[FIELDS_PER_PAGE]{};

  ~FieldPage() {
    for (auto& slot : slots) delete slot.load(std::memory_order_relaxed);
  }
};

InputStream PostingsReader::materializeStream(uint32_t fnum) {
  std::lock_guard lock(streamMutex);
  StreamSlot& slot = streams[fnum];
  if (!slot.ready.load(std::memory_order_relaxed)) {
    // May fetch the whole file and throw; the slot stays unready then and the
    // next caller retries.
    slot.stream = files[fnum]->getInputStream();
    slot.ready.store(true, std::memory_order_release);
  }
  return slot.stream;
}

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

  // File 0 ends with: ... field metadata, offsets table, field count, segment
  // info, segment info size. See PostingsWriter::writeSegmentInfo.
  InputFile& first = *files[0];
  const int64_t firstSize = (int64_t)first.size();
  const int64_t headerSize = (int64_t)Postings::LUXIR_HEADER.size();
  if (firstSize < headerSize + (int64_t)sizeof(int32_t)) invalidSegment(segInfoFile, "no segment info");
  auto segInfoSize = first.rangeStream((uint64_t)firstSize - sizeof(int32_t), sizeof(int32_t)).readInt();
  if (segInfoSize < 0 || segInfoSize > firstSize - headerSize - (int64_t)sizeof(int32_t)) {
    invalidSegment(segInfoFile, "invalid segment info size");
  }
  segInfoOffset = firstSize - sizeof(int32_t) - segInfoSize;
  InputStream firstIS = first.rangeStream((uint64_t)segInfoOffset, (uint64_t)segInfoSize);
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

  auto versionsLoc = seg_location::read(firstIS);
  uint32_t versionCount = firstIS.readVint();
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
    InputStream headerIS = file.rangeStream(offset, sizeof(VersionRunsHeader));
    const char* fileBase = headerIS.ptr() - offset;
    VersionRunsHeader header;
    std::memcpy(&header, headerIS.ptr(), sizeof(header));
    if (header.bytes > offset + sizeof(VersionRunsHeader)) {
      throw std::runtime_error("Invalid request-version run coverage or encoding");
    }
    file.rangeStream(offset + sizeof(VersionRunsHeader) - header.bytes, header.bytes);
    versions = VersionRuns(fileBase, fileSize, offset, versionCount, maxdoc);
  } else if (maxdoc != 0 || versionCount != 0 || !versionsLoc.isNull()) {
    throw std::runtime_error("Segment has invalid request birth versions");
  }

  // Field count, offsets table, then the catalog they point into.
  if (segInfoOffset < headerSize + (int64_t)sizeof(int32_t)) invalidSegment(segInfoFile, "no field index");
  auto count = first.rangeStream((uint64_t)segInfoOffset - sizeof(int32_t), sizeof(int32_t)).readInt();
  if (count < 0 || (int64_t)count * (int64_t)sizeof(uint32_t) > segInfoOffset - headerSize - (int64_t)sizeof(int32_t)) {
    invalidSegment(segInfoFile, "invalid field count");
  }
  fieldOffsetsLoc = segInfoOffset - sizeof(int32_t) - (int64_t)count * sizeof(uint32_t);
  fieldOffsets = (const uint32_t*)first.rangeStream((uint64_t)fieldOffsetsLoc, (uint64_t)count * sizeof(uint32_t)).ptr();
  catalogStart = fieldOffsetsLoc;
  for (int32_t i = 0; i < count; i++) {
    // Field entries precede the table; the largest offset is the first entry.
    if ((int64_t)fieldOffsets[i] > fieldOffsetsLoc - headerSize) invalidSegment(segInfoFile, "invalid field offset");
    catalogStart = std::min(catalogStart, fieldOffsetsLoc - (int64_t)fieldOffsets[i]);
  }
  catalog = first.rangeStream((uint64_t)catalogStart, (uint64_t)(firstSize - catalogStart));
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
  fieldMetadataMemory.entryBytes.fetch_sub(stats.entryBytes, std::memory_order_relaxed);
  for (size_t i = 0; i < ((size_t)nFields + FIELDS_PER_PAGE - 1) / FIELDS_PER_PAGE; ++i) {
    delete fieldPages[i].load(std::memory_order_relaxed);
  }
}

int32_t PostingsReader::findOrdinal(std::string_view name) const {
  const auto& input = catalog;
  const int64_t tableLoc = fieldOffsetsLoc - catalogStart;
  auto* end = fieldOffsets + nFields;
  auto* found = std::lower_bound(fieldOffsets, end, name, [&](uint32_t offset, std::string_view key) {
    return input.readPackedTerm(tableLoc - offset) < key;
  });
  return found != end && input.readPackedTerm(tableLoc - *found) == name
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
    if (fresh->type == FieldType::BIN && (fresh->flags & FieldType::STORED) != 0) {
      fresh->stored = std::make_unique<const StoredFieldsMetadata>(*this, *fresh);
    }
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
      fieldMetadataMemory.entryBytes.fetch_add(entry->allocatedBytes(), std::memory_order_relaxed);
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
      if (auto* entry = slot.load(std::memory_order_acquire)) {
        stats.entries++;
        stats.entryBytes += entry->allocatedBytes();
      }
    }
  }
  stats.bytes += stats.entryBytes;
  return stats;
}

} // namespace luxir
