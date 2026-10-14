// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "SidecarReader.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <memory_resource>
#include <tuple>
#include <xxhash.h>

#include "Postings.h"
#include "luxir/api/index_files.h"
#include "luxir/api/padded_input.h"
#include "luxir/util/log.h"

namespace luxir {

namespace sidecar {

uint32_t tierOf(Kind kind) {
  switch (kind) {
    case Kind::FILE_HEADER:
    case Kind::SEGMENT_INFO:
    case Kind::FIELD_DIRECTORY:
    case Kind::TERM_INDEX:
    case Kind::TERM_TRIE:
    case Kind::TERM_RANGE_TABLE:
    case Kind::POINTS_INDEX:
    case Kind::LIVE_DOCS:
    case Kind::IVF_HEADER:
    case Kind::IVF_LIST_DIRECTORY:
    case Kind::ORDMAP_DIRECTORY:
    case Kind::TRAILER:
      return 0;
    case Kind::POSITIONS:
    case Kind::STORED_FIELDS:
    case Kind::VECTOR_VALUES:
      return 2;
    default:
      return 1;
  }
}

std::string segmentFileName(uint64_t segId) {
  return Postings::getIndexFileNamePrefix(segId) + "__M";
}

std::string snapshotFileName(uint64_t indexGen) {
  return std::string(Postings::PREFIX_FNAME) + "_sidecar_" + Postings::getSortableString(indexGen);
}

std::span<const char> payload(std::string_view file) {
  if (file.size() < 2 * sizeof(uint64_t)) return {};
  size_t length = file.size() - 2 * sizeof(uint64_t);
  uint64_t recorded, digest;
  std::memcpy(&recorded, file.data() + length, sizeof(recorded));
  std::memcpy(&digest, file.data() + length + sizeof(recorded), sizeof(digest));
  if (recorded != length || digest != XXH3_64bits(file.data(), length)) return {};
  return {file.data(), length};
}

} // namespace sidecar

std::optional<SidecarReader> SidecarReader::parse(std::string_view bytes, std::string_view name) {
  auto body = sidecar::payload(bytes);
  if (body.data() == nullptr) {
    LOG_WARN("Ignoring sidecar {}: trailer or digest mismatch", name);
    return std::nullopt;
  }
  std::pmr::monotonic_buffer_resource arena;
  api::Sidecar wire;
  if (!api::decode(wire, api::copyToPaddedInput(std::as_bytes(body), arena), arena)) {
    LOG_WARN("Ignoring sidecar {}: undecodable", name);
    return std::nullopt;
  }
  if (wire.format_version != sidecar::FORMAT_VERSION || wire.chunk_size == 0) {
    LOG_WARN("Ignoring sidecar {}: format {} chunk size {}", name, wire.format_version, wire.chunk_size);
    return std::nullopt;
  }
  SidecarReader reader;
  reader.chunkSize = wire.chunk_size;
  reader.files.reserve(wire.files.size());
  for (const auto& file : wire.files) {
    auto& out = reader.files.emplace_back();
    out.name = file.name;
    out.size = file.size;
    out.xxh3 = file.xxh3;
    out.chunks.assign(file.chunk_xxh3.begin(), file.chunk_xxh3.end());
    if (out.chunks.size() != (file.size + wire.chunk_size - 1) / wire.chunk_size) {
      LOG_WARN("Ignoring sidecar {}: {} has {} chunk hashes for {} bytes", name, out.name, out.chunks.size(), out.size);
      return std::nullopt;
    }
    uint64_t end = 0;
    for (const auto& range : file.ranges) {
      if (range.offset != end || range.length > file.size - range.offset
          || range.field > wire.fields.size()) {
        LOG_WARN("Ignoring sidecar {}: invalid range in {}", name, out.name);
        return std::nullopt;
      }
      end = range.offset + range.length;
      out.ranges.push_back({out.name, range.offset, range.length, range.kind, range.tier,
                            range.field ? std::string(wire.fields[range.field - 1]) : std::string()});
    }
    if (end != file.size) {
      LOG_WARN("Ignoring sidecar {}: ranges of {} do not cover the file", name, out.name);
      return std::nullopt;
    }
  }
  return reader;
}

std::optional<SidecarReader> SidecarReader::open(Directory& dir, const FileDescriptor& descriptor) {
  std::shared_ptr<InputFile> file;
  try { file = dir.openFile(descriptor.name); }
  catch (const std::exception& e) {
    LOG_WARN("Ignoring sidecar {}: {}", descriptor.name, e.what());
    return std::nullopt;
  }
  if (!file) {
    LOG_WARN("Ignoring sidecar {}: missing", descriptor.name);
    return std::nullopt;
  }
  auto bytes = file->read();
  if (bytes.size() != descriptor.size || XXH3_64bits(bytes.data(), bytes.size()) != descriptor.xxh3) {
    LOG_WARN("Ignoring sidecar {}: differs from its manifest descriptor", descriptor.name);
    return std::nullopt;
  }
  return parse(bytes, descriptor.name);
}

std::vector<FileDescriptor> SidecarReader::sidecarsOf(const api::IndexInfo& info) {
  std::vector<FileDescriptor> result;
  if (info.sidecar) result.push_back({std::string(info.sidecar->name), info.sidecar->size, info.sidecar->xxh3});
  for (const auto& segment : info.segments) {
    auto name = sidecar::segmentFileName(segment.seg_id);
    for (const auto& file : segment.files) {
      if (file.name == name) result.push_back({name, file.size, file.xxh3});
    }
  }
  return result;
}

std::vector<SidecarReader::Range> SidecarReader::snapshotRanges(Directory& dir, const api::IndexInfo& info) {
  std::map<std::string_view, const FileDescriptor*> listed;
  auto files = filesOf(info);
  for (const auto& file : files) listed.emplace(file.name, &file);
  std::vector<Range> result;
  for (const auto& descriptor : sidecarsOf(info)) {
    auto reader = open(dir, descriptor);
    if (!reader) continue;
    for (auto& file : reader->files) {
      auto it = listed.find(file.name);
      if (it == listed.end() || it->second->size != file.size || it->second->xxh3 != file.xxh3) {
        LOG_WARN("Ignoring sidecar {} entry {}: not in the manifest", descriptor.name, file.name);
        continue;
      }
      std::ranges::move(file.ranges, std::back_inserter(result));
    }
  }
  std::ranges::stable_sort(result, [](const Range& a, const Range& b) {
    return std::tie(a.tier, a.file, a.offset) < std::tie(b.tier, b.file, b.offset);
  });
  return result;
}

std::vector<FileDescriptor> SidecarReader::transferOrder(Directory& dir, const api::IndexInfo& info,
                                                         std::span<const FileDescriptor> files) {
  // Rank: sidecars as -1, else the lowest tier the file holds.
  std::map<std::string, int, std::less<>> rank;
  for (const auto& range : snapshotRanges(dir, info)) {
    auto [it, added] = rank.try_emplace(range.file, (int)range.tier);
    if (!added) it->second = std::min(it->second, (int)range.tier);
  }
  for (const auto& file : sidecarsOf(info)) rank[file.name] = -1;
  auto key = [&](const FileDescriptor& file) {
    auto it = rank.find(file.name);
    return std::pair(it == rank.end() ? 1 : it->second, file.size);
  };
  std::vector<FileDescriptor> result(files.begin(), files.end());
  std::ranges::stable_sort(result, [&](const FileDescriptor& a, const FileDescriptor& b) {
    return key(a) < key(b);
  });
  return result;
}

const SidecarReader::File* SidecarReader::find(std::string_view name) const {
  for (const auto& file : files) {
    if (file.name == name) return &file;
  }
  return nullptr;
}

bool SidecarReader::verifyChunk(const File& file, uint64_t index, std::span<const char> bytes) const {
  if (index >= file.chunks.size()) return false;
  uint64_t expected = std::min(chunkSize, file.size - index * chunkSize);
  return bytes.size() == expected && XXH3_64bits(bytes.data(), bytes.size()) == file.chunks[index];
}

} // namespace luxir
