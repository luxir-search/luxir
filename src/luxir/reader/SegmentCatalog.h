// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CatalogRange.h"
#include "FieldRecordCodec.h"
#include "Space.h"

namespace luxir {
namespace catalog_io { class Errors; }

// The catalog occupies the end of a segment's first file.
// Field tables appear in path order. Each consists of the space's records in
// name order followed by their offsets and one offset for the end.
// The directory lists the file numbers, version runs and each row space's
// path, doc count, field table and ownership. The last eight bytes contain
// the directory length. The manifest's CatalogRange locates the whole region.

// Offset of a field record from the start of its table's records.
using FieldOffset = uint32_t;

// One row space's field records. A field's ordinal is its position in name order.
// Borrows the catalog bytes.
class FieldTable {
  std::span<const char> records;  // Encoded records, back to back.
  std::span<const char> offsets;  // One offset per record and one for the end.

  FieldOffset offset(int32_t ordinal) const noexcept;

public:
  FieldTable(std::span<const char> records, std::span<const char> offsets, const catalog_io::Errors& errors);
  int32_t size() const noexcept { return (int32_t)(offsets.size() / sizeof(FieldOffset) - 1); }
  std::span<const char> record(int32_t ordinal) const;
  PackedTerm name(int32_t ordinal) const noexcept;
  // Ordinal of the named field, or -1.
  int32_t find(std::string_view name) const noexcept;
};

// A row space of the segment: the root documents, or the rows of one nested path.
struct SpaceDescriptor : RowSpace {
  FieldTable fields;  // The fields for this space, in name order.
};

// Where the segment's version runs are (see VersionRuns.h).
struct VersionRunsDescriptor {
  field_parts::Slice slice;  // Blocks, directory and trailer, null when absent.
  uint64_t trailerOffset = 0;    // Offset of the trailer within the slice.
  uint32_t count = 0;            // Number of version runs.
};

// A segment's decoded directory: its row spaces and their field tables. Field records
// themselves are decoded on request (fieldHeader, field). The directory and field tables
// borrow the supplied catalog span.
class SegmentCatalog {
  std::vector<SpaceDescriptor> spaces;             // Indexed by SpaceOrdinal, root first.
  std::vector<uint64_t> dataSizes;  // Addressable bytes by file number, zero for an absent file.
  VersionRunsDescriptor versions;  // Location and size of the request version runs.

public:
  // The first step of an open: enough of the directory to learn which files the
  // segment has, so decode() can validate the directory against the file inventory.
  class DirectoryHead {
    friend class SegmentCatalog;
    uint64_t offset;                 // Offset of the catalog in file 0.
    int32_t maxDoc;                  // Doc count of the root space.
    std::span<const char> tail;      // Catalog bytes.
    std::span<const char> entries;   // Raw row space entries from the directory.
    uint64_t directoryStart;         // Offset of the directory within the catalog.
    std::vector<uint32_t> numbers;   // File numbers the segment uses.

    DirectoryHead() = default;

  public:
    uint64_t catalogOffset() const noexcept { return offset; }
    std::span<const uint32_t> fileNumbers() const noexcept { return numbers; }
  };

private:
  static DirectoryHead readDirectoryRegion(std::span<const char> region, uint64_t offset,
                                           const catalog_io::Errors& errors);

public:
  // Discover the catalog from the trailer of file 0.
  static DirectoryHead readDirectory(std::span<const char> file, SegmentReadContext context = SegmentReadContext());
  static DirectoryHead readDirectory(std::span<const char> file, CatalogRange range, SegmentReadContext context);
  // Open the exact catalog bytes, which reach the end of file 0.
  static DirectoryHead readDirectory(std::span<const char> catalog, uint64_t offset, uint64_t fileSize,
                                     SegmentReadContext context = SegmentReadContext());
  static SegmentCatalog decode(const DirectoryHead& head, field_parts::FileSizes files,
                               SegmentReadContext context = SegmentReadContext());
  std::span<const SpaceDescriptor> rowSpaces() const noexcept { return spaces; }
  const VersionRunsDescriptor& versionRuns() const noexcept { return versions; }
  std::optional<SpaceOrdinal> findSpace(std::string_view path) const noexcept;
  // Decodes a field header or record on request. PostingsReader owns the cache.
  FieldRecordHeader fieldHeader(SpaceOrdinal space, int32_t ordinal, const SegmentCore* core = nullptr) const;
  FieldRecordPtr field(SpaceOrdinal space, int32_t ordinal, const SegmentCore* core = nullptr) const;
};

// Writes the catalog at the end of file 0. Call beginSpace() for each row space in path
// order (root first), addField() for each of its fields in name order, then finish().
class SegmentCatalogWriter {
  OutputStream& output;                // Writes to file 0.
  RAMFile spaceEntries;            // Buffers directory entries until finish().
  OutputStream directory;  // Writes the buffered directory entries.
  const RowSpace* root = nullptr;  // The caller keeps row spaces alive through finish().
  const RowSpace* current = nullptr;      // The space whose fields are being added.
  uint64_t recordsStart = 0;           // File offset of the current table's first record.
  uint32_t numSpaces = 0;  // Number of row space entries.
  std::vector<FieldOffset> offsets;    // Offsets of the current table's records.

  void addOffset();
  void endSpace();

public:
  explicit SegmentCatalogWriter(OutputStream& output) : output(output), spaceEntries(""), directory(&spaceEntries) {}
  void beginSpace(const RowSpace& space);
  void addField(const IndexFieldInfo& fieldInfo);
  void finish(std::span<const uint32_t> fileNumbers, const VersionRunsDescriptor& versions);
};

} // namespace luxir
