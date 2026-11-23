// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace luxir {

// What the first use of a segment overlay does when it cannot open it.
// Segment overlays open on first use, never with the segment, from files the
// snapshot opened. A failed read (an ApiError such as FileIOException, a
// system error, or running out of memory) fails that use under either policy,
// and the next use retries.
enum class OverlayPolicy {
  // Invalid overlay bytes fail every use with SegmentCorruptError.
  ON_DEMAND,
  // An accelerator: invalid overlay bytes leave an UnavailableAuxReader that
  // answers nothing, and the segment stays in service.
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

protected:
  AuxReader(uint64_t gen, uint64_t builtCoreGen) noexcept
    : gen(gen), builtCoreGen(builtCoreGen) {}
};

/// Stands in for an advisory overlay that could not be opened: it answers
/// nothing, and whatever consults the overlay does without it.
class UnavailableAuxReader final : public AuxReader {
  std::string kind;
  std::string name;

public:
  UnavailableAuxReader(std::string_view kind, std::string_view name, uint64_t gen, uint64_t builtCoreGen)
      : AuxReader(gen, builtCoreGen), kind(kind), name(name) {}
  std::string_view getKind() const override { return kind; }
  std::string_view getName() const override { return name; }
};

} // namespace luxir
