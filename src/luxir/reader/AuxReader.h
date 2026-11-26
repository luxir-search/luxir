// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace luxir {

class TermMembership;

// What the first use of a segment overlay does when the overlay's bytes are
// invalid. Segment overlays open on first use, never with the segment, from
// files the snapshot opened, and a kind's open reports invalid bytes as
// SegmentCorruptError. Any other failure (a failed read, running out of
// memory) fails that use under either policy, and the next use retries.
enum class OverlayPolicy {
  // Queries need what the overlay holds: invalid bytes fail every use.
  REQUIRED,
  // An accelerator with an exact fallback: an overlay whose bytes are invalid
  // reads as absent, as on a segment that never had one, and the segment
  // stays in service.
  ADVISORY,
};

/// Read-side handle for one entry in IndexInfo.aux_indexes.  Subclasses own
/// whatever the underlying aux kind needs to answer queries (e.g. a
/// deserialized faiss::Index for kind "vector_faiss").
///
/// Aux files are deleted by IndexWriter only after a new IndexInfo is durable,
/// so a reader that races with a commit may see an aux file referenced by the
/// IndexInfo it just parsed but already unlinked from the directory.  Each
/// subclass's open() returns nullptr on missing-file when missingFileOK=true
/// so IndexReader can re-parse IndexInfo (the retry will see the new commit's
/// aux list).
///
/// Unknown kinds are tolerated.  IndexReader dispatches by `getKind()` and
/// silently skips entries it doesn't recognize, so older binaries can read
/// indexes that contain aux kinds added later.
class AuxReader {
  uint64_t gen;
  uint64_t builtCoreGen;

public:
  virtual ~AuxReader() = default;

  // Identity from the AuxIndexInfo entry.  `getName()` is unique within an index.
  virtual std::string_view getKind() const = 0;
  virtual std::string_view getName() const = 0;
  uint64_t getGen() const noexcept { return gen; }
  uint64_t getBuiltCoreGen() const noexcept { return builtCoreGen; }

  // Non-null when this overlay can prove terms absent from its segment.
  virtual const TermMembership* termMembership() const noexcept { return nullptr; }

protected:
  AuxReader(uint64_t gen, uint64_t builtCoreGen) noexcept
    : gen(gen), builtCoreGen(builtCoreGen) {}
};

} // namespace luxir
