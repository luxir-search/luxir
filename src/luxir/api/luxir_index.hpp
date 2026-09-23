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

#define LUXIR_TD(M) static_assert(std::is_trivially_destructible_v<M>);
LUXIR_TD(IndexInfo) LUXIR_TD(SegmentInfo) LUXIR_TD(AuxIndexInfo)
LUXIR_TD(SchemaInfo) LUXIR_TD(FileDescriptor)
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
LUXIR_ENTRY(SchemaInfo) LUXIR_ENTRY(FileDescriptor)
#undef LUXIR_ENTRY

} // namespace luxir::api
