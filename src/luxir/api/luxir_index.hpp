// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

// HAND-WRITTEN concrete classes for the internal luxir_index.proto manifest.
// Members are ordered by descending alignment. Generated metadata binds them
// by member pointer and field number, independent of declaration order.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <span>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <hpp_proto/field_types.hpp>
#include "luxir/api/luxir_types.hpp"

namespace luxir::api {

struct FileDescriptor {
  std::string_view name;
  uint64_t size = 0;
  uint64_t xxh3 = 0;
};

struct SchemaInfo {
  ::hpp_proto::bytes_view source_def;
  map_view<std::string_view, uint64_t> introduced_gen;
};

struct AuxIndexInfo {
  std::string_view kind;
  std::string_view field;
  std::string_view name;
  uint64_t gen = 0;
  uint64_t commit_time = 0;
  std::span<const FileDescriptor> files;
  ::hpp_proto::bytes_view opaque_meta;
  uint64_t built_core_gen = 0;
};

struct SegmentInfo {
  uint64_t next_overlay_gen = 0;
  uint64_t seg_id = 0;
  uint64_t live_gen = 0;
  uint64_t min_version = 0;
  uint64_t max_version = 0;
  uint64_t commit_time = 0;
  uint64_t schema_gen = 0;
  std::span<const AuxIndexInfo> overlays;
  std::span<const FileDescriptor> files;
  int32_t max_doc = 0;
  int32_t live_docs = 0;
};

struct IndexInfo {
  uint64_t last_seg_id = 0;
  std::optional<SchemaInfo> schema;
  std::optional<FileDescriptor> sidecar;
  std::string_view incarnation;
  uint64_t version = 0;
  uint64_t commit_time = 0;
  uint64_t index_gen = 0;
  uint64_t core_gen = 0;
  uint64_t update_version = 0;
  uint64_t schema_gen = 0;
  std::span<const SegmentInfo> segments;
  std::span<const AuxIndexInfo> aux_indexes;
};

enum class StructureKind {
  STRUCTURE_UNKNOWN = 0, FILE_HEADER = 1, SEGMENT_INFO = 2, FIELD_DIRECTORY = 3,
  TERM_INDEX = 4, TERM_TRIE = 5, TERM_RANGE_TABLE = 6, TERM_BLOCKS = 7,
  DOC_POSTINGS = 8, POSITIONS = 9, DOCS_WITH_FIELD = 10, NORMS = 11, COLUMN = 12,
  COLUMN_INDEX = 13, POINTS_LEAVES = 14, POINTS_INDEX = 15, STORED_FIELDS = 16,
  VECTOR_VALUES = 17, LIVE_DOCS = 18, IVF_HEADER = 19, IVF_LIST_DIRECTORY = 20,
  IVF_LISTS = 21, ORDMAP_DIRECTORY = 22, ORDMAP_DATA = 23, TRAILER = 24
};

struct ByteRange {
  uint64_t offset = 0;
  uint64_t length = 0;
  StructureKind kind = StructureKind::STRUCTURE_UNKNOWN;
  uint32_t tier = 0;
  uint32_t field = 0;
};

struct SidecarFile {
  std::string_view name;
  uint64_t size = 0;
  uint64_t xxh3 = 0;
  std::span<const uint64_t> chunk_xxh3;
  std::span<const ByteRange> ranges;
};

struct Sidecar {
  uint64_t chunk_size = 0;
  std::span<const std::string_view> fields;
  std::span<const SidecarFile> files;
  uint32_t format_version = 0;
};

struct OrdMapSegment {
  uint64_t seg_id = 0;
  uint64_t digest = 0;
  int32_t max_doc = 0;
};

struct OrdMapEntry {
  std::string_view field;
  int64_t num_ords = 0;
  uint64_t offset = 0;
  uint64_t length = 0;
  uint64_t xxh3 = 0;
  std::span<const int64_t> top_ords;
  std::span<const int64_t> top_dfs;
  int64_t unlisted_bound = 0;
  int32_t single_segment = 0;
};

struct OrdMapDirectory {
  uint64_t core_gen = 0;
  std::span<const OrdMapSegment> segments;
  std::span<const OrdMapEntry> entries;
  uint32_t format_version = 0;
};

#define LUXIR_TD(M) static_assert(std::is_trivially_destructible_v<M>);
LUXIR_TD(IndexInfo) LUXIR_TD(SegmentInfo) LUXIR_TD(AuxIndexInfo)
LUXIR_TD(SchemaInfo) LUXIR_TD(FileDescriptor) LUXIR_TD(Sidecar) LUXIR_TD(SidecarFile)
LUXIR_TD(ByteRange) LUXIR_TD(OrdMapDirectory) LUXIR_TD(OrdMapSegment) LUXIR_TD(OrdMapEntry)
#undef LUXIR_TD

#define LUXIR_ENTRY(M)                                                                  \
  bool decode(M &, std::span<const std::byte> data, std::pmr::memory_resource &arena);  \
  bool encode(const M &, std::vector<std::byte> &out);                                  \
  bool write_json(const M &, std::string &out);                                         \
  bool read_json(M &, std::string_view json, std::pmr::memory_resource &arena,          \
                 std::string *error = nullptr);                                          \
  bool merge_json(M &, std::string_view json, std::pmr::memory_resource &arena,         \
                  std::string *error = nullptr);
LUXIR_ENTRY(IndexInfo) LUXIR_ENTRY(SegmentInfo) LUXIR_ENTRY(AuxIndexInfo)
LUXIR_ENTRY(SchemaInfo) LUXIR_ENTRY(FileDescriptor) LUXIR_ENTRY(Sidecar)
LUXIR_ENTRY(SidecarFile) LUXIR_ENTRY(ByteRange) LUXIR_ENTRY(OrdMapDirectory)
LUXIR_ENTRY(OrdMapSegment) LUXIR_ENTRY(OrdMapEntry)
#undef LUXIR_ENTRY

} // namespace luxir::api
