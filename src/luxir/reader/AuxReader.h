#pragma once

#include <cstdint>
#include <memory>
#include <string_view>

namespace luxir {

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

} // namespace luxir
