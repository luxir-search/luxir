// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "luxir/api/luxir_index.hpp"
#include "luxir/store/Directory.h"

namespace luxir {

// Sidecars (protos/luxir_index.proto, message Sidecar) map each immutable index
// file to labelled byte ranges with a priority tier, and carry fixed-size chunk
// hashes so a reader that fetches part of a file can verify what it fetched.
// They are hints: no query depends on them, and a missing, corrupt or stale
// sidecar is logged and ignored.
namespace sidecar {

using Kind = api::StructureKind;

inline constexpr uint32_t FORMAT_VERSION = 1;

// Verification granularity. A range reader must fetch and hash whole chunks,
// so the chunk bounds read amplification for small random reads (term blocks,
// postings, column blocks are KiB-sized): 1 MiB is a single fast object-store
// GET, while the hash list stays at 8 bytes per MiB (8 MiB per TiB). Readers
// may still coalesce adjacent chunks into one larger request.
inline constexpr uint64_t CHUNK_SIZE = 1ULL << 20;

// 0: needed to open a segment and plan a query (directories, term indexes,
// points inner nodes, live docs, IVF routing). 1: data most queries read
// (term blocks, postings, norms, columns). 2: bulk payloads read for few
// documents (positions, stored fields, full-precision vectors).
uint32_t tierOf(Kind kind);

// "s<seg>__M": beside the segment's data files, swept with its prefix.
std::string segmentFileName(uint64_t segId);
// "s_sidecar_<index_gen>": the snapshot sidecar written for that commit.
std::string snapshotFileName(uint64_t indexGen);

// The payload of a sidecar-style file (payload, length, XXH3-64), or empty
// when the trailer does not match.
std::span<const char> payload(std::string_view file);

} // namespace sidecar

// Read side of the sidecars of one snapshot.
class SidecarReader {
public:
  struct Range {
    std::string file;
    uint64_t offset = 0;
    uint64_t length = 0;
    sidecar::Kind kind = sidecar::Kind::STRUCTURE_UNKNOWN;
    uint32_t tier = 0;
    std::string field;
  };

  struct File {
    std::string name;
    uint64_t size = 0;
    uint64_t xxh3 = 0;
    std::vector<uint64_t> chunks;
    std::vector<Range> ranges;
  };

  uint64_t chunkSize = 0;
  std::vector<File> files;

  // Decodes one sidecar file's bytes; nullopt (logged) when it is corrupt or
  // of an unknown format.
  static std::optional<SidecarReader> parse(std::string_view bytes, std::string_view name);

  // Opens a sidecar the manifest lists; nullopt (logged) when it is missing,
  // differs from `descriptor`, or does not parse.
  static std::optional<SidecarReader> open(Directory& dir, const FileDescriptor& descriptor);

  // The sidecars `info` lists: the snapshot sidecar, then each segment's.
  static std::vector<FileDescriptor> sidecarsOf(const api::IndexInfo& info);

  // Every range of the snapshot's sidecars whose file entry matches the
  // manifest's descriptor, ordered by tier, then file and offset. Missing or
  // invalid sidecars contribute nothing.
  static std::vector<Range> snapshotRanges(Directory& dir, const api::IndexInfo& info);

  // `files` (a snapshot's transfer list) in fetch order: sidecars first, then
  // by the lowest tier a file holds (unknown files as tier 1), then smaller
  // files first. Reads the sidecars from `dir`, so stage those first.
  static std::vector<FileDescriptor> transferOrder(Directory& dir, const api::IndexInfo& info,
                                                   std::span<const FileDescriptor> files);

  const File* find(std::string_view name) const;

  // Whether `bytes` is chunk `index` of `file` (the last chunk may be short).
  bool verifyChunk(const File& file, uint64_t index, std::span<const char> bytes) const;
};

} // namespace luxir
