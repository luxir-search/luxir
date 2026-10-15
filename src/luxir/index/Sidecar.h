// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "luxir/reader/SidecarReader.h"

// Write side of sidecars (see luxir/reader/SidecarReader.h). PostingsWriter
// records each segment's layout as it finishes the segment; IndexWriter hashes
// the finished files and writes the segment sidecar, and assembles the
// snapshot sidecar for live docs, overlays and index-level aux files.
namespace luxir::sidecar {

struct Extent {
  uint64_t offset = 0;
  uint64_t length = 0;
  Kind kind = Kind::STRUCTURE_UNKNOWN;
  uint32_t field = 0; // 1-based index into Contents::fields; 0 = not field-scoped
  bool operator==(const Extent&) const = default;
};

struct FileMap {
  std::string name;
  uint64_t size = 0;
  uint64_t xxh3 = 0;
  std::vector<uint64_t> chunks;
  std::vector<Extent> extents; // ascending, disjoint, covering [0, size)
  bool operator==(const FileMap&) const = default;
};

// Labels one file's bytes from what its writer recorded: the offsets where
// structures start, and exact extents for structures whose length it knows.
// Each byte belongs to the exact extent holding it, else to the structure
// starting at the closest preceding boundary when one starts exactly there,
// else to STRUCTURE_UNKNOWN. Ties at one offset go to the lowest tier.
class Partition {
  struct Mark {
    uint64_t offset;
    uint64_t end;
    Kind kind;
    uint32_t field;
  };
  std::vector<Mark> starts;
  std::vector<Mark> exacts;

public:
  void start(uint64_t offset, Kind kind, uint32_t field = 0);
  void exact(uint64_t offset, uint64_t length, Kind kind, uint32_t field = 0);
  std::vector<Extent> extents(uint64_t size) const;
};

struct Contents {
  std::vector<std::string> fields;
  std::vector<FileMap> files;

  // The 1-based field id for `name`, adding it when new.
  uint32_t fieldId(std::string_view name);
  bool operator==(const Contents&) const = default;
};

// Fills `file`'s size, digest and chunk hashes from `bytes`.
void hash(std::string_view bytes, FileMap& file);

// Writes `contents` as the immutable sidecar `name` (not synced) and returns
// its descriptor.
FileDescriptor write(Directory& dir, const std::string& name, const Contents& contents);

// The map of a finished file outside any segment's data: live docs, a
// segment overlay of `auxKind`, or an index-level aux file. Unknown layouts
// become one STRUCTURE_UNKNOWN range. Throws if the file is missing or does
// not match `file`.
FileMap describe(Directory& dir, const FileDescriptor& file, std::string_view auxKind);

} // namespace luxir::sidecar
