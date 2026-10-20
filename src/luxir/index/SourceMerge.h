// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "luxir/schema/SourceSettings.h"
#include "luxir/source/SourceMap.h"
#include "luxir/store/Directory.h"
#include "luxir/util/screaming.h"

namespace luxir {

// One segment a merge reads: its source map (null if its docs keep no source),
// its doc count, and the live docs the merge uses (null if none are deleted).
struct SourceMergeInput {
  const SourceMap* map = nullptr;
  int32_t maxDoc = 0;
  const screaming::FixedBitSet* live = nullptr;
};

struct SourceMergeResult {
  SourceMap map;                           // the merged segment's map; empty if no doc has a source
  std::optional<FileDescriptor> written;   // the file holding relocated records, if any
  int64_t lostDocs = 0;                    // docs whose source could not be read and so were left out
  int32_t docs = 0;                        // docs in the merged segment
};

// Builds the merged segment's source map. A merge moves pointers, not bytes: a
// source file that still has at least settings.minLiveRatio of its records live,
// and is at least settings.minFileBytes, is referenced as it is. Any other file
// referenced by the inputs has its live records copied into one new file, named
// by newName(), so garbage and tiny files do not accumulate. Merged docs follow
// the inputs in order, with deleted docs dropped, as the postings merge does.
// A source file that cannot be read costs only its documents.
SourceMergeResult mergeSources(Directory& dir, const SourceSettings& settings,
                               std::span<const SourceMergeInput> inputs,
                               const std::function<std::string()>& newName);

}  // namespace luxir
