// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "luxir/source/SourceFormat.h"
#include "luxir/source/SourceMap.h"
#include "luxir/store/Directory.h"

namespace luxir {

// Reads the documents a segment kept as submitted.  Opening a segment maps its
// source map and source files and reads nothing from them, so a search never
// pays for them; the bytes are read only by a fetch.  Mapping when the snapshot
// opens, rather than on the first fetch, is what keeps a reader valid after a
// merge retires the files, and keeps it from holding the directory.  Immutable
// once built, so any number of threads may fetch; each brings its own Cursor.
class SourceReader {
public:
  // A reader's block cache.  Consecutive fetches in doc order hit it.
  struct Cursor {
    source::Codecs codecs;
    uint32_t file = UINT32_MAX;
    int64_t block = -1;
    source::DecodedBlock decoded;
  };

  enum class Status {
    OK,
    NONE,        // the doc has no stored source
    UNREADABLE,  // its map, file or block is damaged or missing
  };

  // Opens the map and files through `dir`, which need not outlive the reader.
  // Anything missing or damaged only makes the docs it covers UNREADABLE.
  SourceReader(Directory& dir, std::string mapName, int32_t maxDoc);

  const std::string& name() const { return mapName; }

  // The record of `doc`, valid until the next fetch through `cursor`.
  Status fetch(int32_t doc, Cursor& cursor, source::Record& out) const;

  // JSON text of a fetched record: its bytes as submitted, or, for a document
  // received as a serialized Map, the Map rendered as JSON. False for a placeholder.
  static bool renderJson(const source::Record& record, std::string& out);

private:
  struct OpenFile {
    std::shared_ptr<InputFile> input;
    std::optional<source::FileReader> reader;
  };

  const std::string mapName;
  std::optional<SourceMap> map;
  std::vector<OpenFile> files;
};

}  // namespace luxir
