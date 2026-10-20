// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "luxir/store/Directory.h"

namespace luxir {

// Where a segment's documents keep their source: runs of doc IDs that map onto
// consecutive ordinals of one source file.  A flushed segment has one run over
// one file.  A merge keeps doc order and drops deleted docs, so its map is the
// inputs' runs, cut at the dropped docs, over the files the inputs referenced
// (plus a file the merge wrote for the ones it relocated).  Doc IDs without a
// run have no source.
//
// Stored as "s<seg>__S<gen>", a Luxir-internal file (not the frozen format of
// the source files it names):
//   LUXIR header | u64 format=1 | u32 n_files | u32 n_runs
//   | n_files * (u32 len, name, u32 docs, u64 bytes)
//   | n_runs * (u32 doc_start, count, file, ordinal)
//   | u64 xxh3 of everything before
struct SourceRun {
  int32_t docStart = 0;
  int32_t count = 0;
  uint32_t file = 0;     // index into SourceMap::files
  uint32_t ordinal = 0;  // ordinal in that file of the run's first doc
};

// A source file a map refers to, with what a merge needs to decide whether to
// keep referencing it without opening it.
struct SourceFileRef {
  std::string name;    // "v<seq>"
  uint32_t docs = 0;   // records in the whole file, live or not
  uint64_t bytes = 0;  // file size
};

class SourceMap {
public:
  std::vector<SourceFileRef> files;  // in first-use order
  std::vector<SourceRun> runs;       // ascending docStart, non-overlapping

  // Index of the file named `ref.name` in files, adding it when new.
  uint32_t fileIndex(const SourceFileRef& ref);

  // Appends a run.  Extends the previous run when it continues in the same file.
  void addRun(int32_t docStart, int32_t count, uint32_t file, uint32_t ordinal);

  // The file and ordinal holding doc's source; false if it has none.
  bool locate(int32_t doc, uint32_t& file, uint32_t& ordinal) const;

  // Docs covered by a run.
  int64_t coveredDocs() const;

  // Writes the map as `name`, appending the name to `names` and its descriptor to `descriptors`.
  void write(Directory& dir, const std::string& name, std::vector<std::string>* names,
             std::vector<FileDescriptor>* descriptors) const;

  // Reads and verifies `name`; throws std::runtime_error when it is damaged or inconsistent.
  static SourceMap read(Directory& dir, const std::string& name, int32_t maxDoc);
};

}  // namespace luxir
