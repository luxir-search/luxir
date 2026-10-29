// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "luxir/schema/SourceSettings.h"
#include "luxir/source/SourceMap.h"
#include "luxir/store/Directory.h"

namespace luxir {

// Where doc `doc` of merge input `input` lands in the merged segment, or -1
// when it is dropped: the merge's own mapping (SegmentMerger::mapDocId), so
// the source layout follows whatever order the merge produces.
using SourceDocMap = std::function<int32_t(size_t input, int32_t doc)>;

// Moves records between files on behalf of the layout below, which knows
// files, ordinals and counts but nothing about a record.  One instance serves
// one merge and writes at most one output file.
class SourceRelocation {
public:
  virtual ~SourceRelocation() = default;

  // Whether ordinals [from, from + count) of `file` can be read.  Asked for
  // every range of a file before any of it is copied; one false answer keeps
  // the file referenced as it is.
  virtual bool verify(const SourceFileRef& file, uint32_t from, uint32_t count) = 0;

  // Appends those records to the output file, opening it on first use.
  // `firstOut` receives the output ordinal of the first one.  Throws if a
  // verified range cannot be read after all.
  virtual void copy(const SourceFileRef& file, uint32_t from, uint32_t count, uint32_t& firstOut) = 0;

  // The output file's name, once copy() has opened it.
  virtual const std::string& outputName() const = 0;

  // Finishes the output file, if one was opened: its reference and descriptor.
  struct Output {
    SourceFileRef ref;
    FileDescriptor descriptor;
  };
  virtual std::optional<Output> finish() = 0;
};

struct SourceMergeResult {
  SourceMap map;                           // the merged segment's map; empty if no doc has a source
  std::optional<FileDescriptor> written;   // the file holding relocated records, if any
  std::vector<std::string> unreadable;     // files kept by reference because they could not be verified
};

// Builds the merged segment's source map.  A merge moves pointers, not bytes:
// a file that still has at least settings.minLiveRatio of its records live, is
// at least settings.minFileBytes, and holds at least 1/settings.maxFiles of the
// output's live bytes is referenced as it is.  Any other file referenced by the
// inputs has its live records copied into one new file through `relocation`,
// so garbage and small files do not accumulate; a segment therefore references
// at most maxFiles + 1 files.  A file the relocation cannot verify stays
// referenced instead, so an unreadable file never costs more than it already
// has.  Documents land where docMap puts them.
SourceMergeResult mergeSources(const SourceSettings& settings, std::span<const SourceMap* const> inputs,
                               const SourceDocMap& docMap, SourceRelocation& relocation);

// The relocation for the source file format: verifies block checksums, copies
// a full block as it is and repacks the records of a partial one (a flush's
// last block) so small blocks fold together.  newName() names the output file
// when it is first needed.
std::unique_ptr<SourceRelocation> sourceFormatRelocation(Directory& dir, const SourceSettings& settings,
                                                         std::function<std::string()> newName);

}  // namespace luxir
