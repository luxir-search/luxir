// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "PostingsReader.h"
#include "SegmentCore.h"
#include "ReaderErrors.h"

#include <algorithm>
#include <memory>
#include <new>
#include <stdexcept>
#include <utility>
#include "FieldMetadataMemory.h"
#include "Postings.h"

namespace luxir {

StoredFieldsMetadata::StoredFieldsMetadata(PostingsReader& reader, const DecodedFieldRecord& fieldInfo) {
  const auto& stored = *fieldInfo.stored();
  auto bytes = reader.core().slice(stored.names());
  InputStream input(bytes.data(), bytes.data() + bytes.size());
  auto invalid = [&](std::string_view reason) {
    corruptSegment(reader.core().segmentName(), reason, std::string_view(fieldInfo.name), stored.location.filenum());
  };
  auto count = input.tryVint();
  if (!count) invalid(count.error());
  if (*count > (uint64_t)input.left()) invalid("stored fields: member count exceeds names bytes");
  names.reserve(*count);
  for (uint32_t i = 0; i < *count; i++) {
    auto term = input.tryPackedTerm();
    if (term.isNull()) invalid("stored fields: truncated member name");
    names.push_back(term);
  }
  backed.assign(names.size(), false);
  auto numBacked = input.tryVint();
  if (!numBacked) invalid(numBacked.error());
  for (uint64_t i = 0; i < *numBacked; i++) {
    auto id = input.tryVint();
    if (!id) invalid(id.error());
    if (*id >= names.size()) invalid("stored fields: source-backed field id out of range");
    backed[*id] = true;
  }
  if (input.left() != 0) invalid("stored fields: trailing name bytes");
}

namespace {

void accountOpened(size_t bytes) {
  fieldMetadataMemory.openedEntries.fetch_add(1, std::memory_order_relaxed);
  fieldMetadataMemory.openedEntryBytes.fetch_add(bytes, std::memory_order_relaxed);
}

// Publishes a fully built object. On failure the caller retains its allocation.
template <class T, class P>
bool publishOnce(std::atomic<T*>& slot, T*& entry, P& fresh) {
  if (!slot.compare_exchange_strong(entry, fresh.get(), std::memory_order_release,
                                     std::memory_order_acquire)) return false;
  entry = fresh.release();
  return true;
}

template <class T, class Open, class Published>
T* acquire(std::atomic<T*>& slot, Open&& open, Published&& published) {
  auto* entry = slot.load(std::memory_order_acquire);
  if (!entry) {
    auto fresh = open();
    if (publishOnce(slot, entry, fresh)) published(*entry);
  }
  return entry;
}

void addOpenedStats(PostingsReader::FieldCacheStats& stats, size_t bytes) {
  stats.openedEntries++;
  stats.openedEntryBytes += bytes;
}

} // namespace

// One opened resource, linked into the reader's list for this row space.
struct PostingsReader::StoredResource {
  StoredResource* next = nullptr;  // Next published resource, immutable after insertion.
  int32_t ordinal;  // Resource's position in the field table.
  StoredFieldsMetadata metadata;  // Opened names and optional decompressor dictionary.

  StoredResource(PostingsReader& reader, const DecodedFieldRecord& fieldInfo)
      : ordinal(fieldInfo.ordinal), metadata(reader, fieldInfo) {}

  size_t allocatedBytes() const {
    return sizeof(*this) - sizeof(metadata) + metadata.allocatedBytes();
  }
};

// Page header followed by exactly one record slot per field covered by the page.
struct PostingsReader::FieldPage {
  using Slot = std::atomic<const DecodedFieldRecord*>;
  std::atomic<OpenedField*> opened = nullptr;  // Same field count as the record slots.

  const Slot* slots() const noexcept { return reinterpret_cast<const Slot*>(this + 1); }
  Slot* slots() noexcept { return reinterpret_cast<Slot*>(this + 1); }

  static size_t bytes(size_t count) noexcept { return sizeof(FieldPage) + count * sizeof(Slot); }

  static std::unique_ptr<FieldPage> create(size_t count) {
    static_assert(alignof(FieldPage) >= alignof(Slot));
    auto* page = ::new (::operator new(bytes(count))) FieldPage;
    ::new (page->slots()) Slot[count]();
    return std::unique_ptr<FieldPage>(page);
  }

  static void operator delete(void* ptr) noexcept { ::operator delete(ptr); }

  void destroyEntries(size_t count) noexcept {
    std::unique_ptr<OpenedField[]> openedSlots(opened.load(std::memory_order_relaxed));
    for (size_t i = 0; i < count; ++i) {
      auto& slot = slots()[i];
      if (openedSlots) delete openedSlots[i].load(std::memory_order_relaxed);
      delete slot.load(std::memory_order_relaxed);
    }
  }
};

void PostingsReader::initialize(int32_t maxDoc, int32_t count) {
  maxdoc = maxDoc;
  if (count > 0) {
    fieldPages = std::make_unique<std::atomic<FieldPage*>[]>(numFieldPages(count));
    nFields = count;
    fieldMetadataMemory.pagePointerArrays.fetch_add(1, std::memory_order_relaxed);
    fieldMetadataMemory.pagePointerBytes.fetch_add(
        numFieldPages(nFields) * sizeof(std::atomic<FieldPage*>),
        std::memory_order_relaxed);
  }
}

PostingsReader::~PostingsReader() {
  auto stats = fieldCacheStats();
  auto pagePointerBytes = numFieldPages(nFields) * sizeof(std::atomic<FieldPage*>);
  fieldMetadataMemory.pagePointerArrays.fetch_sub(nFields > 0, std::memory_order_relaxed);
  fieldMetadataMemory.pagePointerBytes.fetch_sub(pagePointerBytes, std::memory_order_relaxed);
  fieldMetadataMemory.pages.fetch_sub(stats.pages, std::memory_order_relaxed);
  fieldMetadataMemory.pageBytes.fetch_sub(stats.pageBytes, std::memory_order_relaxed);
  fieldMetadataMemory.entries.fetch_sub(stats.entries, std::memory_order_relaxed);
  fieldMetadataMemory.entryBytes.fetch_sub(stats.entryBytes, std::memory_order_relaxed);
  fieldMetadataMemory.openedPages.fetch_sub(stats.openedPages, std::memory_order_relaxed);
  fieldMetadataMemory.openedPageBytes.fetch_sub(stats.openedPageBytes, std::memory_order_relaxed);
  fieldMetadataMemory.openedEntries.fetch_sub(stats.openedEntries, std::memory_order_relaxed);
  fieldMetadataMemory.openedEntryBytes.fetch_sub(stats.openedEntryBytes, std::memory_order_relaxed);
  auto* resource = storedResources.load(std::memory_order_relaxed);
  while (resource) {
    auto* next = resource->next;
    delete resource;
    resource = next;
  }
  for (size_t i = 0; i < numFieldPages(nFields); ++i) {
    std::unique_ptr<FieldPage> page(fieldPages[i].load(std::memory_order_relaxed));
    if (page) page->destroyEntries(fieldPageSize(i));
  }
}

int32_t PostingsReader::findOrdinal(std::string_view name) const {
  return core().catalog.rowSpaces()[(size_t)space].fields.find(name);
}

PackedTerm PostingsReader::fieldName(int32_t ordinal) const noexcept {
  return core().catalog.rowSpaces()[(size_t)space].fields.name(ordinal);
}

FieldRecordHeader PostingsReader::fieldHeader(int32_t ordinal) const {
  return core().catalog.fieldHeader(space, ordinal, &core());
}

FieldRecordPtr PostingsReader::decodeField(int32_t ordinal) const {
  return core().catalog.field(space, ordinal, &core());
}

const DecodedFieldRecord* PostingsReader::fieldInfo(std::string_view field) {
  return fieldInfo(findOrdinal(field));
}

size_t PostingsReader::fieldPageSize(size_t pageIndex) const noexcept {
  return std::min(FIELDS_PER_PAGE, (size_t)nFields - pageIndex * FIELDS_PER_PAGE);
}

PostingsReader::FieldPage& PostingsReader::fieldPage(size_t pageIndex) {
  return *acquire(fieldPages[pageIndex], [&] {
    return FieldPage::create(fieldPageSize(pageIndex));
  }, [&](const FieldPage&) {
    fieldMetadataMemory.pages.fetch_add(1, std::memory_order_relaxed);
    fieldMetadataMemory.pageBytes.fetch_add(FieldPage::bytes(fieldPageSize(pageIndex)), std::memory_order_relaxed);
  });
}

const DecodedFieldRecord* PostingsReader::fieldInfo(int32_t ordinal) {
  if (ordinal < 0 || ordinal >= nFields) return nullptr;
  auto& page = fieldPage((size_t)ordinal / FIELDS_PER_PAGE);
  auto& slot = page.slots()[(size_t)ordinal % FIELDS_PER_PAGE];
  return acquire(slot, [&] { return decodeField(ordinal); }, [](const DecodedFieldRecord& entry) {
    fieldMetadataMemory.entries.fetch_add(1, std::memory_order_relaxed);
    fieldMetadataMemory.entryBytes.fetch_add(entry.allocatedBytes(), std::memory_order_relaxed);
  });
}

PostingsReader::OpenedField& PostingsReader::openedField(const DecodedFieldRecord& fieldInfo) {
  auto ordinal = fieldInfo.ordinal;
  // Publish the canonical record even for an open requested through decodeField().
  const auto* canonical = this->fieldInfo(ordinal);
  if (!canonical) throw std::out_of_range("opened field ordinal");
  if (canonical->type != fieldInfo.type) {
    throw std::invalid_argument("opened field type differs from its record");
  }
  auto pageIndex = (size_t)ordinal / FIELDS_PER_PAGE;
  auto& page = fieldPage(pageIndex);
  auto* opened = acquire(page.opened, [&] {
    return std::make_unique<OpenedField[]>(fieldPageSize(pageIndex));
  }, [&](const OpenedField&) {
    fieldMetadataMemory.openedPages.fetch_add(1, std::memory_order_relaxed);
    fieldMetadataMemory.openedPageBytes.fetch_add(fieldPageSize(pageIndex) * sizeof(OpenedField), std::memory_order_relaxed);
  });
  return opened[(size_t)ordinal % FIELDS_PER_PAGE];
}

NumColumnFormat::LeafMeta readNumericLeafMeta(std::span<const char> index, int64_t numValues,
                                              SegmentReadContext context) {
  auto metadata = NumColumnFormat::LeafMeta::decode(index, numValues);
  if (!metadata) context.invalid(metadata.error());
  return *metadata;
}

const NumColumnFormat::LeafMeta* PostingsReader::openNumericLeafMeta(const DecodedFieldRecord& fieldInfo) {
  const auto* number = fieldInfo.numberColumn();
  if (!number || !FieldType::isNumericColumn(fieldInfo.type)
      || fieldInfo.docsWithField != maxDoc() || fieldInfo.endValueRanks()) return nullptr;
  return acquire(openedField(fieldInfo), [&] {
    auto index = number->index();
    auto metadata = readNumericLeafMeta(core().slice(index), fieldInfo.numValues,
        segmentReadContext(core(), fieldInfo, index.location.filenum()));
    return std::make_unique<NumColumnFormat::LeafMeta>(metadata);
  }, [](const NumColumnFormat::LeafMeta& entry) { accountOpened(sizeof(entry)); });
}

const StoredFieldsMetadata* PostingsReader::openStoredMetadata(const DecodedFieldRecord& fieldInfo) {
  if (!fieldInfo.stored()) return nullptr;
  auto* head = storedResources.load(std::memory_order_acquire);
  std::unique_ptr<StoredResource> fresh;
  do {
    for (auto* resource = head; resource; resource = resource->next) {
      if (resource->ordinal == fieldInfo.ordinal) return &resource->metadata;
    }
    if (!fresh) {
      const auto* canonical = this->fieldInfo(fieldInfo.ordinal);
      if (!canonical) throw std::out_of_range("opened field ordinal");
      if (!canonical->stored()) throw std::invalid_argument("opened field type differs from its record");
      fresh = std::make_unique<StoredResource>(*this, *canonical);
    }
    fresh->next = head;
  } while (!publishOnce(storedResources, head, fresh));
  accountOpened(head->allocatedBytes());
  return &head->metadata;
}

const StoredDecompressionDictionary* PostingsReader::openStoredDictionary(const field_parts::Stored& stored,
                                                                           const StoredFieldsMetadata& metadata) {
  auto dictionary = stored.dictionary();
  if (!dictionary) return nullptr;
  return acquire(metadata.decoder, [&] {
    return std::make_unique<StoredDecompressionDictionary>(std::string_view(core().slice(dictionary)));
  }, [](const StoredDecompressionDictionary& entry) { accountOpened(entry.allocatedBytes()); });
}

PostingsReader::FieldCacheStats PostingsReader::fieldCacheStats() const noexcept {
  FieldCacheStats stats;
  size_t pageCount = numFieldPages(nFields);
  stats.bytes = pageCount * sizeof(std::atomic<FieldPage*>);
  for (size_t i = 0; i < pageCount; ++i) {
    const auto* page = fieldPages[i].load(std::memory_order_acquire);
    if (!page) continue;
    auto count = fieldPageSize(i);
    stats.pages++;
    stats.pageBytes += FieldPage::bytes(count);
    auto* opened = page->opened.load(std::memory_order_acquire);
    if (opened) {
      stats.openedPages++;
      stats.openedPageBytes += count * sizeof(OpenedField);
    }
    for (size_t j = 0; j < count; ++j) {
      auto& slot = page->slots()[j];
      if (opened) {
        if (auto* entry = opened[j].load(std::memory_order_acquire)) addOpenedStats(stats, sizeof(*entry));
      }
      if (auto* entry = slot.load(std::memory_order_acquire)) {
        stats.entries++;
        stats.entryBytes += entry->allocatedBytes();
      }
    }
  }
  for (auto* resource = storedResources.load(std::memory_order_acquire); resource; resource = resource->next) {
    addOpenedStats(stats, resource->allocatedBytes());
    if (auto* decoder = resource->metadata.decoder.load(std::memory_order_acquire)) {
      addOpenedStats(stats, decoder->allocatedBytes());
    }
  }
  stats.bytes += stats.pageBytes + stats.entryBytes + stats.openedPageBytes + stats.openedEntryBytes;
  return stats;
}

} // namespace luxir
