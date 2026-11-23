// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <memory>
#include <memory_resource>
#include <optional>
#include <string>
#include <vector>

#include "luxir/api/luxir_types.hpp"
#include "luxir/index/StoredValues.h"
#include "luxir/source/SourceFormat.h"
#include "luxir/source/SourceMap.h"
#include "luxir/store/Directory.h"
#include "luxir/util/Once.h"

namespace luxir {

// Reads the documents a segment kept as submitted.  Opening a segment reads
// its small source map and takes a handle on each source file, and that is
// all: a file's header and footer are parsed (or, when the footer does not
// verify, the file is scanned) on the first fetch that needs it, so a search
// never pays for them.  Taking the handles when the snapshot opens, rather than
// on the first fetch, is what keeps a reader valid after a merge retires the
// files, and keeps it from holding the directory.  Immutable once built, so any
// number of threads may fetch; each brings its own Cursor.
class SourceReader {
public:
  // A reader's position: the run, file and decompressed block of the last
  // fetch.  Consecutive fetches in doc order hit it without a search.
  struct Cursor {
    source::Codecs codecs;
    size_t run = 0;
    uint32_t file = UINT32_MAX;
    int64_t block = -1;
    source::DecodedBlock decoded;
  };

  enum class Status {
    OK,
    NONE,        // the doc has no stored source
    UNREADABLE,  // its map, file or block is damaged or missing
  };

  // Reads the map and opens the files through `dir`, which need not outlive the
  // reader.  Anything missing or damaged only makes the docs it covers UNREADABLE.
  SourceReader(Directory& dir, std::string mapName, int32_t maxDoc);

  const std::string& name() const { return mapName; }
  // The loaded map, or null.
  const SourceMap* sourceMap() const { return map ? &*map : nullptr; }

  // Whether the map loaded (its files may still be missing).
  bool mapLoaded() const { return map.has_value(); }

  // Whether the map loaded and every file it names opened. An unhealthy reader
  // may have hit a transient failure, so a reopen builds a new one.
  bool healthy() const {
    if (!map) return false;
    for (const auto& file : files) if (!file->input) return false;
    return true;
  }

  // The record of `doc`, valid until the next fetch through `cursor`.
  Status fetch(int32_t doc, Cursor& cursor, source::Record& out) const;

  // JSON text of a fetched record: its bytes as submitted, or, for a document
  // received as a serialized Map, the Map rendered as JSON. False for a placeholder.
  static bool renderJson(const source::Record& record, std::string& out);

  // A fetched record parsed again, to read the values of stored fields that
  // were left out of stored fields because the record holds them (a
  // source-backed field; see StoredFieldsWriter::markSourceBacked).
  // Reusable: each parse replaces the previous record.
  class Fields {
  public:
    // False unless the record holds its document's field values
    // (source::holdsFieldValues) and parses.
    bool parse(const source::Record& record);

    // Calls emit(std::span<const std::string_view>) with the stored values of
    // field `name` as ingest stored them (storedValues), from its last key in
    // the document; the views last for the call.  False when the document has
    // no such key.
    template <class Emit>
    bool values(std::string_view name, Emit&& emit) const {
      const api::Val* found = nullptr;
      for (const auto& [key, val] : map.fields) {
        if (key == name) found = &*val;  // last wins, as at ingest
      }
      if (!found) return false;
      try {
        storedValues(*found, name, emit);
      } catch (const std::exception&) {
        return false;  // ingest rejects such a value, so no indexed doc has one
      }
      return true;
    }

  private:
    std::pmr::monotonic_buffer_resource arena;
    api::Map map;
  };

private:
  struct OpenFile {
    std::shared_ptr<InputFile> input;
    mutable HeldRange hold;  // the whole file, while `reader` views it
    mutable OnceFlag parsed;
    mutable std::optional<source::FileReader> reader;

    // The parsed file, parsing it on the first call; null if it is unreadable.
    const source::FileReader* get(const std::string& name) const;
  };

  const std::string mapName;
  std::optional<SourceMap> map;
  std::vector<std::unique_ptr<OpenFile>> files;  // one per map file; OnceFlag pins them in place
};

}  // namespace luxir
