// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "SourceReader.h"

#include <memory_resource>

#include "luxir/api/luxir_types.hpp"
#include "luxir/api/padded_input.h"
#include "luxir/util/log.h"

namespace luxir {

SourceReader::SourceReader(Directory& dir, std::string mapName, int32_t maxDoc) : mapName(std::move(mapName)) {
  try {
    map = SourceMap::read(dir, this->mapName, maxDoc);
  } catch (const std::exception& e) {
    LOG_WARN("source map unreadable: {}", e.what());
    return;
  }
  files.resize(map->files.size());
  for (size_t i = 0; i < files.size(); i++) {
    const auto& name = map->files[i].name;
    try {
      files[i].input = dir.openFile(name);
      if (!files[i].input) {
        LOG_WARN("source file {} is missing", name);
        continue;
      }
      std::string error;
      files[i].reader = source::FileReader::open(files[i].input->read(), &error);
      if (!files[i].reader) LOG_WARN("source file {} is unreadable: {}", name, error);
    } catch (const std::exception& e) {
      LOG_WARN("source file {} could not be opened: {}", name, e.what());
    }
  }
}

SourceReader::Status SourceReader::fetch(int32_t doc, Cursor& cursor, source::Record& out) const {
  if (!map) return Status::UNREADABLE;
  uint32_t fileIndex = 0, ordinal = 0;
  if (!map->locate(doc, fileIndex, ordinal)) return Status::NONE;
  const auto& file = files[fileIndex];
  if (!file.reader) return Status::UNREADABLE;
  const auto& reader = *file.reader;
  auto block = reader.blockFor(ordinal);
  if (block < 0) return Status::UNREADABLE;
  if (cursor.file != fileIndex || cursor.block != block) {
    cursor.file = UINT32_MAX;
    if (!reader.readBlock((size_t)block, cursor.codecs, cursor.decoded)) {
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

}  // namespace luxir
