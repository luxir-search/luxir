// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "Sidecar.h"

#include <algorithm>
#include <memory_resource>
#include <stdexcept>
#include <xxhash.h>

#include "luxir/api/build.h"
#include "luxir/reader/VectorAuxReader.h"
#include "luxir/search/OrdMapStore.h"

namespace luxir::sidecar {

void Partition::start(uint64_t offset, Kind kind, uint32_t field) {
  starts.push_back({offset, offset, kind, field});
}

void Partition::exact(uint64_t offset, uint64_t length, Kind kind, uint32_t field) {
  if (length > 0) exacts.push_back({offset, offset + length, kind, field});
}

std::vector<Extent> Partition::extents(uint64_t size) const {
  // Ties at one offset go to the lowest tier, then the lowest kind.
  auto better = [](const Mark& a, const Mark& b) {
    return std::pair(tierOf(a.kind), (int)a.kind) < std::pair(tierOf(b.kind), (int)b.kind);
  };
  std::vector<Mark> sortedStarts = starts;
  std::ranges::sort(sortedStarts, [&](const Mark& a, const Mark& b) {
    return a.offset != b.offset ? a.offset < b.offset : better(a, b);
  });
  std::vector<Mark> sortedExacts = exacts;
  std::ranges::sort(sortedExacts, [&](const Mark& a, const Mark& b) {
    return a.offset != b.offset ? a.offset < b.offset : better(a, b);
  });

  std::vector<uint64_t> bounds{0, size};
  for (const auto& mark : sortedStarts) bounds.push_back(std::min(mark.offset, size));
  for (const auto& mark : sortedExacts) {
    bounds.push_back(std::min(mark.offset, size));
    bounds.push_back(std::min(mark.end, size));
  }
  std::ranges::sort(bounds);
  bounds.erase(std::unique(bounds.begin(), bounds.end()), bounds.end());

  std::vector<Extent> result;
  size_t nextStart = 0;
  for (size_t i = 0; i + 1 < bounds.size(); i++) {
    uint64_t begin = bounds[i], end = bounds[i + 1];
    Extent extent{begin, end - begin, Kind::STRUCTURE_UNKNOWN, 0};
    while (nextStart < sortedStarts.size() && sortedStarts[nextStart].offset < begin) nextStart++;
    // The exact extent holding [begin, end): the last one starting at or before begin.
    auto it = std::ranges::upper_bound(sortedExacts, begin, {}, &Mark::offset);
    const Mark* holder = nullptr;
    for (auto scan = it; scan != sortedExacts.begin();) {
      --scan;
      if (scan->end >= end) { holder = &*scan; break; }
      if (scan->offset < begin) break;
    }
    if (holder) {
      extent.kind = holder->kind;
      extent.field = holder->field;
    } else if (nextStart < sortedStarts.size() && sortedStarts[nextStart].offset == begin) {
      extent.kind = sortedStarts[nextStart].kind;
      extent.field = sortedStarts[nextStart].field;
    }
    if (!result.empty() && result.back().kind == extent.kind && result.back().field == extent.field) {
      result.back().length += extent.length;
    } else {
      result.push_back(extent);
    }
  }
  return result;
}

uint32_t Contents::fieldId(std::string_view name) {
  for (size_t i = 0; i < fields.size(); i++) {
    if (fields[i] == name) return (uint32_t)i + 1;
  }
  fields.emplace_back(name);
  return (uint32_t)fields.size();
}

void hash(std::string_view bytes, FileMap& file) {
  file.size = bytes.size();
  file.xxh3 = XXH3_64bits(bytes.data(), bytes.size());
  file.chunks.clear();
  file.chunks.reserve((bytes.size() + CHUNK_SIZE - 1) / CHUNK_SIZE);
  for (uint64_t offset = 0; offset < bytes.size(); offset += CHUNK_SIZE) {
    auto length = std::min<uint64_t>(CHUNK_SIZE, bytes.size() - offset);
    file.chunks.push_back(XXH3_64bits(bytes.data() + offset, length));
  }
}

FileDescriptor write(Directory& dir, const std::string& name, const Contents& contents) {
  std::pmr::monotonic_buffer_resource arena;
  api::Sidecar wire;
  wire.format_version = FORMAT_VERSION;
  wire.chunk_size = CHUNK_SIZE;
  auto* fields = api::build::allocArray(wire.fields, contents.fields.size(), arena);
  for (size_t i = 0; i < contents.fields.size(); i++) fields[i] = contents.fields[i];
  auto* files = api::build::allocArray(wire.files, contents.files.size(), arena);
  for (size_t i = 0; i < contents.files.size(); i++) {
    const auto& file = contents.files[i];
    files[i].name = file.name;
    files[i].size = file.size;
    files[i].xxh3 = file.xxh3;
    files[i].chunk_xxh3 = file.chunks;
    auto* ranges = api::build::allocArray(files[i].ranges, file.extents.size(), arena);
    for (size_t j = 0; j < file.extents.size(); j++) {
      const auto& extent = file.extents[j];
      ranges[j] = {extent.offset, extent.length, extent.kind, tierOf(extent.kind), extent.field};
    }
  }
  std::vector<std::byte> bytes;
  if (!api::encode(wire, bytes)) throw std::runtime_error("Failed to encode sidecar " + name);
  uint64_t trailer[2] = {bytes.size(), XXH3_64bits(bytes.data(), bytes.size())};
  Directory::FileCreateOptions options;
  options.expectedSize = bytes.size() + sizeof(trailer);
  auto out = dir.createFile(name, options);
  OutputStream stream(out.get());
  stream.write(bytes.data(), bytes.size());
  stream.write(trailer, sizeof(trailer));
  stream.close();
  dir.finishFile(*out);
  return out->descriptor();
}

namespace {

std::vector<Extent> vectorAuxExtents(std::string_view bytes, std::string_view name) {
  auto footer = VectorAuxListsFooter::fromFileTail(bytes.data(), bytes.size(), name);
  Partition partition;
  partition.exact(0, footer.sizesOffset(), Kind::IVF_HEADER);
  partition.exact(footer.sizesOffset(), footer.idsOffset() - footer.sizesOffset(), Kind::IVF_LIST_DIRECTORY);
  uint64_t trailer = bytes.size() - sizeof(VectorAuxListsFooter);
  partition.exact(footer.idsOffset(), trailer - footer.idsOffset(), Kind::IVF_LISTS);
  partition.exact(trailer, sizeof(VectorAuxListsFooter), Kind::TRAILER);
  return partition.extents(bytes.size());
}

std::vector<Extent> ordMapExtents(std::string_view bytes) {
  auto directory = OrdMapStore::directoryOffset(bytes);
  if (!directory) throw std::runtime_error("unreadable OrdMap directory");
  Partition partition;
  partition.exact(0, *directory, Kind::ORDMAP_DATA);
  partition.exact(*directory, bytes.size() - *directory, Kind::ORDMAP_DIRECTORY);
  return partition.extents(bytes.size());
}

} // namespace

FileMap describe(Directory& dir, const FileDescriptor& descriptor, std::string_view auxKind) {
  auto input = dir.openFile(descriptor.name);
  if (!input) throw std::runtime_error("Missing file " + descriptor.name);
  auto bytes = input->read();
  FileMap file;
  file.name = descriptor.name;
  hash(bytes, file);
  if (file.size != descriptor.size || file.xxh3 != descriptor.xxh3) {
    throw std::runtime_error("File " + descriptor.name + " does not match its descriptor");
  }
  try {
    if (auxKind == VectorAuxReader::KIND) file.extents = vectorAuxExtents(bytes, descriptor.name);
    else if (auxKind == OrdMapStore::KIND) file.extents = ordMapExtents(bytes);
    else if (auxKind.empty() && descriptor.name.find("__L") != std::string::npos) {
      file.extents = {{0, file.size, Kind::LIVE_DOCS, 0}};
    }
  } catch (const std::exception& e) {
    LOG_WARN("Sidecar: no layout for {}: {}", descriptor.name, e.what());
    file.extents.clear();
  }
  if (file.extents.empty() && file.size > 0) file.extents = {{0, file.size, Kind::STRUCTURE_UNKNOWN, 0}};
  return file;
}

} // namespace luxir::sidecar
