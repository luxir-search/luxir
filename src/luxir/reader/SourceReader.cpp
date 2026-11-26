// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "SourceReader.h"

#include <algorithm>
#include <memory_resource>

#include "luxir/api/luxir_types.hpp"
#include "luxir/api/padded_input.h"
#include "luxir/store/SourceFiles.h"
#include "luxir/util/log.h"

namespace luxir {

SourceReader::SourceReader(Directory& dir, std::string mapName, int32_t maxDoc,
                           std::span<const FileDescriptor> segmentFiles)
    : mapName(std::move(mapName)), maxDoc(maxDoc) {
  auto open = [&](const std::string& name, uint64_t size) -> std::shared_ptr<InputFile> {
    try {
      auto input = dir.openFile(name, {.size = size});
      if (!input) LOG_WARN("source file {} is missing", name);
      if (input && size && input->size() != size) {
        LOG_WARN("source file {} is {} bytes, its manifest records {}", name, input->size(), size);
        return {};
      }
      return input;
    } catch (const std::exception& e) {
      LOG_WARN("source file {} could not be opened: {}", name, e.what());
      return {};
    }
  };
  if (segmentFiles.empty()) {
    // No manifest listing: the map names the files, so it is read now.
    try {
      map = SourceMap::read(dir, this->mapName, maxDoc);
    } catch (const std::exception& e) {
      LOG_WARN("source map unreadable: {}", e.what());
      failed = true;
      return;
    }
    for (const auto& ref : map->files) {
      auto input = open(ref.name, 0);
      opened &= input != nullptr;
      pending.emplace_back(ref.name, std::move(input));
    }
    return;
  }
  bool listed = false;
  for (const auto& file : segmentFiles) {
    if (file.name == this->mapName) {
      listed = true;
      mapInput = open(file.name, file.size);
      opened &= mapInput != nullptr;
    } else if (isSourceDataFile(file.name)) {
      auto input = open(file.name, file.size);
      opened &= input != nullptr;
      pending.emplace_back(file.name, std::move(input));
    }
  }
  if (!listed) {
    LOG_WARN("source map {} is not among its segment's files", this->mapName);
    opened = false;
  }
}

void SourceReader::load() const {
  callOnce(loaded, [&] {
    if (!map) {
      if (mapInput) {
        try {
          map = SourceMap::read(*mapInput, mapName, maxDoc);
        } catch (const std::exception& e) {
          LOG_WARN("source map unreadable: {}", e.what());
        }
        mapInput.reset();
      }
      if (!map) {
        failed = true;
        pending.clear();
        return;
      }
    }
    files.reserve(map->files.size());
    for (const auto& ref : map->files) {
      auto& file = *files.emplace_back(std::make_unique<OpenFile>());
      auto it = std::ranges::find(pending, ref.name, &std::pair<std::string, std::shared_ptr<InputFile>>::first);
      if (it != pending.end()) file.input = it->second;
      else LOG_WARN("source map {} names {}, which its segment does not list", mapName, ref.name);
      if (!file.input) failed = true;
    }
    pending.clear();
  });
}

const SourceMap* SourceReader::sourceMap() const {
  load();
  return map ? &*map : nullptr;
}

const source::FileReader* SourceReader::OpenFile::get(const std::string& name) const {
  callOnce(parsed, [&] {
    if (!input) return;
    std::string error;
    hold = input->acquire({0, input->size()});
    reader = source::FileReader::open(std::string_view(hold.getData(), hold.getLength()), &error);
    if (!reader) {
      LOG_WARN("source file {} is unreadable: {}", name, error);
      hold = {};
    }
  });
  return reader ? &*reader : nullptr;
}

SourceReader::Status SourceReader::fetch(int32_t doc, Cursor& cursor, source::Record& out) const {
  if (!sourceMap()) return Status::UNREADABLE;
  uint32_t fileIndex = 0, ordinal = 0;
  if (!map->locate(doc, cursor.run, fileIndex, ordinal)) return Status::NONE;
  const auto* reader = files[fileIndex]->get(map->files[fileIndex].name);
  if (!reader) return Status::UNREADABLE;
  auto block = reader->blockFor(ordinal, cursor.file == fileIndex ? cursor.block : -1);
  if (block < 0) return Status::UNREADABLE;
  if (cursor.file != fileIndex || cursor.block != block) {
    cursor.file = UINT32_MAX;
    if (!reader->readBlock((size_t)block, cursor.codecs, cursor.decoded)) {
      LOG_WARN("source block {} of {} is damaged", block, map->files[fileIndex].name);
      return Status::UNREADABLE;
    }
    cursor.file = fileIndex;
    cursor.block = block;
  }
  out = cursor.decoded.record(ordinal - cursor.decoded.firstOrdinal);
  return out.flags & source::kFlagMissing ? Status::NONE : Status::OK;
}

bool SourceReader::renderJson(const source::Record& record, std::string& out) {
  if (record.flags & source::kFlagMissing) return false;
  if (!(record.flags & source::kFlagProtoMap)) {
    out.assign(record.body);
    return true;
  }
  std::pmr::monotonic_buffer_resource arena;
  api::Map map;
  auto bytes = std::as_bytes(std::span(record.body.data(), record.body.size()));
  if (!api::decode(map, api::copyToPaddedInput(bytes, arena), arena)) return false;
  out.clear();
  return api::write_json(map, out);
}

bool SourceReader::Fields::parse(const source::Record& record) {
  map = {};
  arena.release();
  if (!source::holdsFieldValues(record.flags)) return false;
  if (record.flags & source::kFlagProtoMap) {
    auto bytes = std::as_bytes(std::span(record.body.data(), record.body.size()));
    return api::decode(map, api::copyToPaddedInput(bytes, arena), arena);
  }
  // As the update path parses a document (the same reader, so the same values).
  return api::read_json(map, record.body, arena);
}

}  // namespace luxir
