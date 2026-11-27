// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <atomic>
#include <memory>
#include <memory_resource>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "luxir/api/luxir_types.hpp"
#include "luxir/index/StoredValues.h"
#include "luxir/source/SourceFormat.h"
#include "luxir/source/SourceMap.h"
#include "luxir/store/Directory.h"
#include "luxir/util/Once.h"

namespace luxir {

// Reads the documents a segment kept as submitted.  Opening a segment takes a
// handle on its source map and on each source file the manifest lists for it,
// sized from the manifest, and reads nothing: the map is read on the first use
// that needs it, and a file's header and footer are parsed (or, when the footer
// does not verify, the file is scanned) on the first fetch that needs that
// file, so neither opening a snapshot nor a search pays for them.  Taking the
// handles when the snapshot opens, rather than on the first fetch, is what
// keeps a reader valid after a merge retires the files, and keeps it from
// holding the directory.  Thread safe once built: any number of threads may
// fetch, each with its own Cursor.
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

  // Opens the map and the source files through `dir`, which need not outlive
  // the reader.  `files` are the segment's files as its manifest entry lists
  // them (other files are ignored): the map and the source files among them
  // are opened with their recorded sizes, and the map is read on first use.
  // Without `files` the map is read now and the files it names are opened
  // unsized.  Anything missing or damaged only makes the docs it covers
  // UNREADABLE.
  SourceReader(Directory& dir, std::string mapName, int32_t maxDoc,
               std::span<const FileDescriptor> files = {});

  const std::string& name() const { return mapName; }
  // The map, reading it on the first call; null if it is unreadable.
  const SourceMap* sourceMap() const;

  // Whether the map loaded (its files may still be missing); reads it on the first call.
  bool mapLoaded() const { return sourceMap() != nullptr; }

  // False once the map or a file is known to be missing or unreadable, which
  // may be a transient failure, so a reopen builds a new reader.  A reader
  // whose map was not read yet is healthy while every handle opened.
  bool healthy() const { return opened && !failed.load(std::memory_order_relaxed); }

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

  // Reads the map and matches each file it names to an open handle, opening
  // the ones not opened yet.
  void load() const;

  const std::string mapName;
  const int32_t maxDoc;
  mutable std::shared_ptr<InputFile> mapInput;  // released once the map is read
  bool opened = true;                   // every handle taken at construction opened
  mutable std::atomic<bool> failed{false};  // load() found the map or a file missing or unreadable
  // Source files opened at construction, by name, until load() claims them.
  mutable std::vector<std::pair<std::string, std::shared_ptr<InputFile>>> pending;
  mutable OnceFlag loaded;
  mutable std::optional<SourceMap> map;
  mutable std::vector<std::unique_ptr<OpenFile>> files;  // one per map file; OnceFlag pins them in place
};

}  // namespace luxir
