// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "OrdMapStore.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <format>
#include <memory_resource>
#include <xxhash.h>

#include "OrdMap.h"
#include "luxir/api/build.h"
#include "luxir/api/padded_input.h"
#include "luxir/reader/Postings.h"
#include "luxir/reader/SidecarReader.h"
#include "luxir/util/log.h"

namespace luxir {

uint64_t OrdMapStore::segmentDigest(std::span<const FileDescriptor> files) {
  std::vector<const FileDescriptor*> data;
  for (const auto& file : files) {
    if (file.name.find("__L") == std::string::npos) data.push_back(&file);
  }
  std::ranges::sort(data, {}, &FileDescriptor::name);
  std::string identity;
  for (const auto* file : data) {
    identity.append(file->name).push_back('\0');
    identity.append((const char*)&file->xxh3, sizeof(file->xxh3));
  }
  return XXH3_64bits(identity.data(), identity.size());
}

std::string OrdMapStore::fileName(uint64_t coreGen) {
  return std::string(Postings::PREFIX_FNAME) + "_" + std::string(NAME) + "_"
      + Postings::getSortableString(coreGen) + "_" + Postings::getSortableString(0);
}

FileDescriptor OrdMapStore::write(Directory& dir, const std::string& name, uint64_t coreGen,
                                  std::span<const Segment> segments,
                                  std::span<const std::pair<std::string, std::shared_ptr<OrdMap>>> maps) {
  std::pmr::monotonic_buffer_resource arena;
  api::OrdMapDirectory directory;
  directory.format_version = FORMAT_VERSION;
  directory.core_gen = coreGen;
  auto* segs = api::build::allocArray(directory.segments, segments.size(), arena);
  for (size_t i = 0; i < segments.size(); i++) {
    segs[i] = {segments[i].segId, segments[i].digest, segments[i].maxDoc};
  }
  std::vector<char> bytes;
  size_t count = 0;
  for (const auto& [field, map] : maps) count += map != nullptr;
  auto* entries = api::build::allocArray(directory.entries, count, arena);
  size_t n = 0;
  for (const auto& [field, map] : maps) {
    if (!map) continue;
    auto& entry = entries[n++];
    entry.field = field;
    entry.num_ords = map->numOrds();
    auto payload = map->bytes();
    if (payload.empty()) {
      entry.single_segment = map->firstFullSeg() + 1;
    } else {
      bytes.resize((bytes.size() + 7) & ~(size_t)7);
      entry.offset = bytes.size();
      entry.length = payload.size();
      entry.xxh3 = XXH3_64bits(payload.data(), payload.size());
      bytes.insert(bytes.end(), payload.begin(), payload.end());
    }
    const auto& top = map->topTerms();
    auto* ords = api::build::allocArray(entry.top_ords, top.entries.size(), arena);
    auto* dfs = api::build::allocArray(entry.top_dfs, top.entries.size(), arena);
    for (size_t i = 0; i < top.entries.size(); i++) {
      ords[i] = top.entries[i].ord;
      dfs[i] = top.entries[i].df;
    }
    entry.unlisted_bound = top.unlistedBound;
  }
  std::vector<std::byte> encoded;
  if (!api::encode(directory, encoded)) throw std::runtime_error("Failed to encode OrdMap directory");
  auto start = bytes.size();
  bytes.resize(start + encoded.size());
  std::memcpy(bytes.data() + start, encoded.data(), encoded.size());
  uint64_t trailer[2] = {encoded.size(), XXH3_64bits(encoded.data(), encoded.size())};
  bytes.insert(bytes.end(), (const char*)trailer, (const char*)trailer + sizeof(trailer));

  Directory::FileCreateOptions options;
  options.expectedSize = bytes.size();
  auto out = dir.createFile(name, options);
  OutputStream stream(out.get());
  stream.write(bytes.data(), bytes.size());
  stream.close();
  dir.finishFile(*out);
  return out->descriptor();
}

std::optional<uint64_t> OrdMapStore::directoryOffset(std::string_view bytes) {
  // The directory is the sidecar-style payload of the file's tail.
  if (bytes.size() < 2 * sizeof(uint64_t)) return std::nullopt;
  uint64_t length;
  std::memcpy(&length, bytes.data() + bytes.size() - 2 * sizeof(uint64_t), sizeof(length));
  if (length > bytes.size() - 2 * sizeof(uint64_t)) return std::nullopt;
  uint64_t offset = bytes.size() - 2 * sizeof(uint64_t) - length;
  if (sidecar::payload(bytes.substr(offset)).data() == nullptr) return std::nullopt;
  return offset;
}

OrdMapStore::OrdMapStore(std::string name, uint64_t gen, uint64_t builtCoreGen, std::shared_ptr<InputFile> file)
    : AuxReader(gen, builtCoreGen), name(std::move(name)), file(std::move(file)) {
  parse();
}

void OrdMapStore::parse() {
  auto bytes = file->read();
  auto offset = directoryOffset(bytes);
  if (!offset) {
    LOG_WARN("Ignoring persisted OrdMaps '{}': directory trailer or digest mismatch", name);
    return;
  }
  auto body = sidecar::payload(bytes.substr(*offset));
  std::pmr::monotonic_buffer_resource arena;
  api::OrdMapDirectory directory;
  if (!api::decode(directory, api::copyToPaddedInput(std::as_bytes(body), arena), arena)
      || directory.format_version != FORMAT_VERSION) {
    LOG_WARN("Ignoring persisted OrdMaps '{}': undecodable or unknown format", name);
    return;
  }
  coreGen = directory.core_gen;
  for (const auto& segment : directory.segments) {
    segments.push_back({segment.seg_id, segment.max_doc, segment.digest});
  }
  for (const auto& entry : directory.entries) {
    if (entry.top_ords.size() != entry.top_dfs.size() || entry.offset > *offset
        || entry.length > *offset - entry.offset || entry.single_segment < 0
        || (size_t)entry.single_segment > segments.size()) {
      LOG_WARN("Ignoring persisted OrdMaps '{}': invalid entry for field '{}'", name, entry.field);
      entries.clear();
      return;
    }
    Entry parsed{entry.num_ords, entry.single_segment, entry.offset, entry.length, entry.xxh3, {}};
    for (size_t i = 0; i < entry.top_ords.size(); i++) {
      parsed.topTerms.entries.push_back({entry.top_ords[i], entry.top_dfs[i]});
    }
    parsed.topTerms.unlistedBound = entry.unlisted_bound;
    entries.emplace(std::string(entry.field), std::move(parsed));
  }
  valid = true;
}

std::shared_ptr<OrdMapStore> OrdMapStore::open(Directory& dir, const api::AuxIndexInfo& info, bool missingFileOK) {
  if (info.files.size() != 1) {
    throw std::runtime_error(std::format("OrdMapStore: expected 1 file, got {} for aux '{}'",
                                         info.files.size(), info.name));
  }
  auto file = dir.openFile(info.files[0].name, /*expectSynced=*/true);
  if (!file) {
    if (missingFileOK) return nullptr;
    throw std::filesystem::filesystem_error(
        std::format("Missing aux index file '{}' for aux '{}'", info.files[0].name, info.name),
        std::make_error_code(std::errc::no_such_file_or_directory));
  }
  return std::make_shared<OrdMapStore>(std::string(info.name), info.gen, info.built_core_gen, std::move(file));
}

bool OrdMapStore::matches(uint64_t readerCoreGen, std::span<const Segment> readerSegments) const {
  return valid && coreGen == readerCoreGen && getBuiltCoreGen() == readerCoreGen
      && std::ranges::equal(segments, readerSegments);
}

std::shared_ptr<OrdMap> OrdMapStore::load(std::string_view field) const {
  auto it = entries.find(field);
  if (!valid || it == entries.end()) return nullptr;
  const auto& entry = it->second;
  TopTerms topTerms = entry.topTerms;
  if (entry.singleSegment > 0) {
    return std::make_shared<OrdMap>(entry.numOrds, entry.singleSegment - 1, std::move(topTerms));
  }
  auto bytes = file->read().substr(entry.offset, entry.length);
  if (bytes.size() != entry.length || XXH3_64bits(bytes.data(), bytes.size()) != entry.xxh3) {
    LOG_WARN("Ignoring persisted OrdMap for '{}' in '{}': payload digest mismatch", field, name);
    return nullptr;
  }
  std::unique_ptr<char[]> data(new char[entry.length]);
  std::memcpy(data.get(), bytes.data(), bytes.size());
  auto map = std::make_shared<OrdMap>(std::move(data), 0, (int64_t)entry.length, std::move(topTerms));
  if (map->numOrds() != entry.numOrds) {
    LOG_WARN("Ignoring persisted OrdMap for '{}' in '{}': ord count mismatch", field, name);
    return nullptr;
  }
  return map;
}

} // namespace luxir
