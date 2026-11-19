// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "PostingsReader.h"
#include "SegmentCore.h"

#include <memory>
#include <stdexcept>
#include <utility>
#include "FieldReader.h"
#include "FieldMetadataMemory.h"
#include "IntColReader.h"
#include "Postings.h"
#include "luxir/index/SegmentSummary.h"

namespace luxir {

// Metadata layout: see StoredFieldsWriter.
StoredFieldsMetadata::StoredFieldsMetadata(PostingsReader& reader, const SegFieldInfo& field) {
  auto input = reader.core().getInputStream(field.columnLoc.filenum());
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

void PostingsReader::initialize(int32_t maxDoc, int64_t fieldOffsetsLoc, const uint32_t* fieldOffsets, int32_t count) {
  maxdoc = maxDoc;
  this->fieldOffsetsLoc = fieldOffsetsLoc;
  this->fieldOffsets = fieldOffsets;
  fieldPages = std::make_unique<std::atomic<FieldPage*>[]>(
      ((size_t)count + FIELDS_PER_PAGE - 1) / FIELDS_PER_PAGE);
  nFields = count;
  if (nFields > 0) {
    fieldMetadataMemory.directories.fetch_add(1, std::memory_order_relaxed);
    fieldMetadataMemory.directoryBytes.fetch_add(
        ((size_t)nFields + FIELDS_PER_PAGE - 1) / FIELDS_PER_PAGE * sizeof(std::atomic<FieldPage*>),
        std::memory_order_relaxed);
  }
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
  const auto input = core().getInputStream(0);
  auto* end = fieldOffsets + nFields;
  // Offsets were checked at open; a name must also end before the offsets table.
  auto nameAt = [&](uint32_t offset) {
    auto term = input.readPackedTerm(fieldOffsetsLoc - offset);
    if (term.memorySize() > offset) throw std::runtime_error("Segment field name extends past the field metadata");
    return term;
  };
  auto* found = std::lower_bound(fieldOffsets, end, name, [&](uint32_t offset, std::string_view key) {
    return nameAt(offset) < key;
  });
  return found != end && nameAt(*found) == name
      ? (int32_t)(found - fieldOffsets) : -1;
}

std::optional<PostingsReader::Field::ColumnBounds> PostingsReader::summaryColumnBounds(const Field& field) const {
  if (!summary) return std::nullopt;
  const api::FieldSummary* record = summary->field((std::string_view)field.fieldname);
  if (!record || record->type != (uint32_t)field.type
      || (record->summary_flags & SegmentSummary::HAS_NUMERIC) == 0) {
    return std::nullopt;
  }
  return Field::ColumnBounds{record->num_min, record->num_max};
}

const NumColumnFormat::LeafMeta* PostingsReader::Field::numericLeafMeta() const {
  if (!FieldType::isNumericColumn(type) || columnLoc.offset() == 0) return nullptr;
  std::call_once(leafMetaOnce, [this] {
    IntColReader column(*reader, *this);
    if (column.leafBounded()) leafMeta = column.leafMeta();
  });
  return leafMeta ? &*leafMeta : nullptr;
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
    core().checkFieldLocations(*fresh);
    if (FieldType::storage(fresh->type) == FieldType::Storage::BIN && (fresh->flags & FieldType::STORED) != 0) {
      fresh->stored = std::make_unique<const StoredFieldsMetadata>(*this, *fresh);
    }
    fresh->reader = this;
    if (FieldType::isNumericColumn(fresh->type) && fresh->columnLoc.offset() > 0) {
      // The summary's bounds are the column writer's own (IntColWriter).
      fresh->columnBounds = summaryColumnBounds(*fresh);
      if (!fresh->columnBounds) {
        IntColReader column(*this, *fresh);
        fresh->columnBounds = Field::ColumnBounds{column.getMin(), column.getMax()};
      }
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
