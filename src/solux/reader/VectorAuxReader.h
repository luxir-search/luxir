#pragma once

#include <faiss/Index.h>
#include <faiss/IndexIVF.h>
#include <faiss/impl/io.h>
#include <faiss/index_io.h>
#include <faiss/invlists/InvertedLists.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "AuxReader.h"
#include "solux/api/solux_types.hpp"
#include "solux/store/Directory.h"
#include "solux/store/InputStream.h"
#include "solux/util/log.h"
#include "solux/util/solux_util.h"

namespace solux {

/// Fixed-width metadata for one vector aux index entry.  VectorIndexBuilder
/// packs this struct verbatim into AuxIndexInfo.opaque_meta; the reader copies
/// it back out.  Packed so the byte layout is the struct declaration, with no
/// implicit padding.  Native endianness: aux entries are rebuilt from the
/// segments they describe, never shipped between hosts.
SOLUX_PACKED_START
struct VectorAuxMeta {
  int32_t dims = 0;
  int32_t metric = 0;       // raw solux::api::VectorMetric value
  int32_t cosineNormalizeColumnOnRescore = 0;  // bool: raw cosine column, renorm at rescore
  int32_t engine = 0;       // ENGINE_IVFPQ
  int32_t nlist = 0;        // IVF only
  int32_t defaultBreadth = 0;  // IVF only: build-time default nprobe
  int32_t pqM = 0;          // IVF+PQ only
  int32_t pqBits = 0;       // IVF+PQ only

  static constexpr int32_t ENGINE_IVFPQ = 1;

  std::string toBytes() const {
    return std::string((const char*)this, sizeof(VectorAuxMeta));
  }

  /// Decode + validate.  Throws on size mismatch or unknown engine.
  static VectorAuxMeta fromBytes(std::string_view bytes, std::string_view auxName) {
    if (bytes.size() != sizeof(VectorAuxMeta)) {
      throw std::runtime_error(std::format(
        "VectorAuxMeta: aux '{}' has invalid opaque_meta size {} (expected {})",
        auxName, bytes.size(), sizeof(VectorAuxMeta)));
    }
    VectorAuxMeta meta;
    std::memcpy(&meta, bytes.data(), sizeof(VectorAuxMeta));
    if (meta.engine != ENGINE_IVFPQ) {
      // (int32_t) copies: can't bind a reference to a packed field.
      throw std::runtime_error(std::format(
        "VectorAuxMeta: aux '{}' has unsupported vector engine {}",
        auxName, (int32_t)meta.engine));
    }
    return meta;
  }
} SOLUX_PACKED_END;

/// Default IVF coarse-list count for n vectors: the sqrt(n) rule clamped to
/// [1, 4096].  Single source of truth for both the builder's per-segment
/// build-time nlist default (VectorIndexBuilder::chooseIvfNList) and the query
/// side's merge-stable REFERENCE list count, which defines the nprobe scan
/// fraction "as if one IVF index of nlist=sqrt(N)".  Keeping them on one rule
/// (and one 4096 cap) means the search-side reference cannot silently drift
/// from how segments are actually partitioned.
inline int32_t defaultIvfNList(int64_t n) {
  if (n <= 0) return 1;
  int64_t derived = (int64_t)std::sqrt((double)n);
  derived = std::clamp<int64_t>(derived, 1, 4096);
  return (int32_t)std::min<int64_t>(derived, n);
}

static_assert(sizeof(VectorAuxMeta) == 8 * sizeof(int32_t),
              "VectorAuxMeta layout is an on-disk contract");

/// Trailing footer of a vector aux file.  The file layout (written by
/// VectorIndexBuilder, served by VectorAuxReader) keeps the FAISS blob down to
/// the index "header" (params + coarse quantizer + PQ codebooks + an empty
/// set of inverted lists) and stores the list payloads in Solux's own layout
/// so they are served zero-copy from the file's memory view instead of being
/// deserialized into FAISS-owned RAM:
///
///   [0, H)    FAISS index blob (faiss::write_index with lists swapped empty)
///   [H8, S)   per-list sizes, int64[nlist]           (H8 = H padded to 8)
///   [S, I)    ids, int64[ntotal], concatenated in list order (8-aligned)
///   [I, C)    codes, uint8[ntotal * codeSize], same order
///   [.., end) this footer (8-aligned)
///
/// Native endianness, same contract as VectorAuxMeta: aux files are rebuilt
/// from the segments they describe, never shipped between hosts.
SOLUX_PACKED_START
struct VectorAuxListsFooter {
  int64_t faissHeaderBytes = 0;
  int64_t nlist = 0;
  int64_t codeSize = 0;
  int64_t ntotal = 0;
  int32_t version = 0;
  int32_t magic = 0;

  static constexpr int32_t MAGIC = 0x4C495853;  // "SXIL"
  static constexpr int32_t VERSION = 1;

  static constexpr size_t align8(size_t n) noexcept { return (n + 7) & ~(size_t)7; }

  /// Section offsets within the file, derived from the footer fields.
  size_t sizesOffset() const noexcept { return align8((size_t)faissHeaderBytes); }
  size_t idsOffset() const noexcept { return sizesOffset() + (size_t)nlist * 8; }
  size_t codesOffset() const noexcept { return idsOffset() + (size_t)ntotal * 8; }
  size_t expectedFileBytes() const noexcept {
    // Unsigned multiply: ntotal/codeSize are file-controlled (read from the
    // footer before validation), so a corrupt footer could overflow a signed
    // int64 product (UB) inside the very check meant to reject it.
    return align8(codesOffset() + (uint64_t)ntotal * (uint64_t)codeSize) + sizeof(VectorAuxListsFooter);
  }

  /// Decode + validate the footer at the tail of a fileBytes-long file.
  static VectorAuxListsFooter fromFileTail(const char* base, size_t fileBytes,
                                           std::string_view auxName) {
    if (fileBytes < sizeof(VectorAuxListsFooter)) {
      throw std::runtime_error(std::format(
        "VectorAuxReader: aux '{}' file is {} bytes, smaller than the lists footer",
        auxName, fileBytes));
    }
    VectorAuxListsFooter f;
    std::memcpy(&f, base + fileBytes - sizeof(VectorAuxListsFooter), sizeof(f));
    if (f.magic != MAGIC) {
      throw std::runtime_error(std::format(
        "VectorAuxReader: aux '{}' is not in the header+lists layout (bad magic)", auxName));
    }
    if (f.version != VERSION) {
      throw std::runtime_error(std::format(
        "VectorAuxReader: aux '{}' has unsupported lists layout version {}",
        auxName, (int32_t)f.version));
    }
    // Bound each section against the actual file size *before* the offset math
    // so a crafted footer cannot wrap the unsigned sums in expectedFileBytes()
    // to coincidentally equal fileBytes.  Every section must fit within the
    // file; the ntotal*codeSize bound is phrased as a division to avoid
    // overflowing the product itself.  codeSize > 0 is checked first so the
    // division is safe via short-circuit.
    if (f.faissHeaderBytes <= 0 || f.nlist <= 0 || f.codeSize <= 0 || f.ntotal < 0
        || (uint64_t)f.faissHeaderBytes > fileBytes
        || (uint64_t)f.nlist > fileBytes / 8
        || (uint64_t)f.ntotal > fileBytes / 8
        || (uint64_t)f.ntotal > fileBytes / (uint64_t)f.codeSize
        || f.expectedFileBytes() != fileBytes) {
      throw std::runtime_error(std::format(
        "VectorAuxReader: aux '{}' lists footer does not match file size {}",
        auxName, fileBytes));
    }
    return f;
  }
} SOLUX_PACKED_END;

static_assert(sizeof(VectorAuxListsFooter) == 4 * sizeof(int64_t) + 2 * sizeof(int32_t),
              "VectorAuxListsFooter layout is an on-disk contract");

/// Read-only faiss::InvertedLists serving codes + ids directly out of the aux
/// file's memory view (mmap for FSDirectory, owned buffer for RAMDir): FAISS
/// list scans read zero-copy through Directory instead of from a deserialized
/// RAM copy.  The owning VectorAuxReader keeps the backing InputFile alive
/// for the lifetime of the index.  ReadOnlyInvertedLists supplies throwing
/// add/update/resize.
class MmapInvertedLists final : public faiss::ReadOnlyInvertedLists {
  const faiss::idx_t* ids;      // all lists' ids, concatenated in list order
  const uint8_t* codes;         // all lists' codes, same order
  std::vector<size_t> offsets;  // prefix sums of list sizes, nlist+1 entries

public:
  MmapInvertedLists(size_t nlist, size_t codeSize, const int64_t* listSizes,
                    const faiss::idx_t* ids, const uint8_t* codes)
    : faiss::ReadOnlyInvertedLists(nlist, codeSize), ids(ids), codes(codes) {
    offsets.resize(nlist + 1);
    offsets[0] = 0;
    for (size_t i = 0; i < nlist; i++) {
      offsets[i + 1] = offsets[i] + (size_t)listSizes[i];
    }
  }

  size_t totalSize() const noexcept { return offsets.back(); }

  size_t list_size(size_t listNo) const override {
    return offsets[listNo + 1] - offsets[listNo];
  }
  const uint8_t* get_codes(size_t listNo) const override {
    return codes + offsets[listNo] * code_size;
  }
  const faiss::idx_t* get_ids(size_t listNo) const override {
    return ids + offsets[listNo];
  }
};

/// Rank-space liveness for one segment's vector field, cached per liveDocs
/// generation: bit r is set iff the doc owning vector rank r is live, in the
/// LSB-first byte layout faiss::IDSelectorBitmap consumes, plus the exact
/// live vector cardinality so consumers never recount it.  Bits past
/// numVectors in the final byte are zero, so popcount(bits) == liveVectors.
/// `bits` points either into bytes owned by `backing` or directly at the
/// segment's liveDocs words (dense single-valued fields: valueRank == docId,
/// and the little-endian byte view of the words IS this layout); either way
/// `backing` pins the storage for as long as a query holds the bitmap.
struct RankLiveBitmap {
  const uint8_t* bits;
  size_t numBytes;
  int64_t liveVectors;
  std::shared_ptr<const void> backing;
};

/// AuxReader for kind == "vector_faiss".  Holds a deserialized faiss::Index
/// lazily decoded from the single eagerly-opened file referenced by
/// AuxIndexInfo.files(0), plus the VectorAuxMeta decoded out of
/// AuxIndexInfo.opaque_meta.
///
/// Contract (matches VectorIndexBuilder): one IVF+PQ file per segment overlay.
/// Both single- and multi-valued vector fields are supported: every vector is
/// its own segment-local FAISS id, and KnnQuery maps each id back via the
/// valueRank->docId column.  The file is opened at IndexReader open time so a
/// reader can keep serving an unlinked overlay file; faiss::read_index runs on
/// first kNN use and is cached.
class VectorAuxReader : public AuxReader {
  std::string name;
  std::string field;
  std::shared_ptr<InputFile> file;
  mutable std::once_flag loadOnce;
  // Declared before faissIndex so the index is destroyed first; the index
  // never owns the lists (replace_invlists own=false).
  mutable std::unique_ptr<MmapInvertedLists> mmapLists;
  mutable std::unique_ptr<faiss::Index> faissIndex;
  VectorAuxMeta meta;
  // Cached RankLiveBitmap (built by KnnQuery).  Built once per liveDocs
  // generation - in time proportional to the segment's DELETED docs, not its
  // vectors - and shared by every query against this (segment, field): the
  // FAISS eligibility selector for unfiltered queries and the allocator's
  // live-list accounting both read the bits instead of resolving rank ->
  // doc -> liveDocs per scanned vector, and its cardinality is the
  // segment's exact live vector count.  Keyed by liveGen, so a reader
  // version with newer deletes rebuilds.
  mutable std::mutex rankLiveMutex;
  mutable uint64_t rankLiveGen = 0;
  mutable std::shared_ptr<const RankLiveBitmap> rankLive;

public:
  static constexpr std::string_view KIND = "vector_faiss";

  VectorAuxReader(std::string name, std::string field,
                  uint64_t gen, uint64_t builtCoreGen,
                  std::shared_ptr<InputFile> file,
                  const VectorAuxMeta& meta) noexcept
    : AuxReader(gen, builtCoreGen),
      name(std::move(name)), field(std::move(field)),
      file(std::move(file)), meta(meta) {}

  std::string_view getKind() const override { return KIND; }
  std::string_view getName() const override { return name; }
  std::string_view getField() const noexcept { return field; }
  int32_t getDims() const noexcept { return meta.dims; }
  int32_t getMetric() const noexcept { return meta.metric; }
  bool shouldNormalizeColumnOnCosineRescore() const noexcept {
    return meta.cosineNormalizeColumnOnRescore != 0;
  }
  int32_t getEngine() const noexcept { return meta.engine; }
  int32_t getNList() const noexcept { return meta.nlist; }
  int32_t getDefaultBreadth() const noexcept { return meta.defaultBreadth; }
  int32_t getPqM() const noexcept { return meta.pqM; }
  int32_t getPqBits() const noexcept { return meta.pqBits; }
  bool scoresAreExact() const noexcept { return false; }

  // Search-time entry point.  Decode is lazy and once-only; faiss::Index::search
  // is thread-safe for concurrent readers after construction.  Only the FAISS
  // header (params + coarse quantizer + PQ codebooks) is deserialized into
  // RAM; the inverted-list payloads are served zero-copy from the file's
  // memory view via MmapInvertedLists (see VectorAuxListsFooter).
  faiss::Index* getFaissIndex() const {
    std::call_once(loadOnce, [this]() {
      InputStream is = file->getInputStream();
      const char* base = is.ptr();
      size_t fileBytes = (size_t)is.left();
      VectorAuxListsFooter footer =
        VectorAuxListsFooter::fromFileTail(base, fileBytes, name);

      BufferIOReader io(base, (size_t)footer.faissHeaderBytes);
      std::unique_ptr<faiss::Index> idx(faiss::read_index(&io));
      if (!idx) {
        throw std::runtime_error(std::format(
          "VectorAuxReader: read_index returned null for {}", name));
      }
      if (idx->d != meta.dims) {
        throw std::runtime_error(std::format(
          "VectorAuxReader: dims mismatch for aux '{}': index has {}, opaque_meta says {}",
          name, (int)idx->d, (int32_t)meta.dims));
      }
      auto* ivf = dynamic_cast<faiss::IndexIVF*>(idx.get());
      if (ivf == nullptr) {
        throw std::runtime_error(std::format(
          "VectorAuxReader: aux '{}' header is not an IVF index", name));
      }
      if ((int64_t)ivf->nlist != footer.nlist
          || (int64_t)ivf->code_size != footer.codeSize
          || ivf->ntotal != footer.ntotal) {
        throw std::runtime_error(std::format(
          "VectorAuxReader: aux '{}' lists footer (nlist={} codeSize={} ntotal={}) "
          "does not match index header (nlist={} codeSize={} ntotal={})",
          name, (int64_t)footer.nlist, (int64_t)footer.codeSize, (int64_t)footer.ntotal,
          ivf->nlist, ivf->code_size, ivf->ntotal));
      }

      const int64_t* sizes = (const int64_t*)(base + footer.sizesOffset());
      const faiss::idx_t* listIds = (const faiss::idx_t*)(base + footer.idsOffset());
      const uint8_t* listCodes = (const uint8_t*)(base + footer.codesOffset());
      assert(((uintptr_t)sizes % 8) == 0 && ((uintptr_t)listIds % 8) == 0);
      auto lists = std::make_unique<MmapInvertedLists>(
        (size_t)footer.nlist, (size_t)footer.codeSize, sizes, listIds, listCodes);
      if ((int64_t)lists->totalSize() != footer.ntotal) {
        throw std::runtime_error(std::format(
          "VectorAuxReader: aux '{}' list sizes sum to {}, expected ntotal {}",
          name, lists->totalSize(), (int64_t)footer.ntotal));
      }
      // Drops the empty ArrayInvertedLists the header deserialized with; the
      // index does not own the mmap lists (this reader does, alongside the
      // file view they point into).
      ivf->replace_invlists(lists.get(), /*own=*/false);
      mmapLists = std::move(lists);
      faissIndex = std::move(idx);
    });
    return faissIndex.get();
  }

  /// Get-or-build the RankLiveBitmap for the given liveDocs generation.
  /// build() runs at most once per generation (under the lock; concurrent
  /// first callers wait rather than duplicate the walk) and returns a
  /// RankLiveBitmap whose backing keeps the bits valid for as long as any
  /// query holds the shared_ptr.
  template <typename Build>
  std::shared_ptr<const RankLiveBitmap> rankLiveBitmap(uint64_t liveGen,
                                                       Build&& build) const {
    std::lock_guard<std::mutex> lock(rankLiveMutex);
    if (rankLive == nullptr || rankLiveGen != liveGen) {
      rankLive = std::make_shared<const RankLiveBitmap>(build());
      rankLiveGen = liveGen;
    }
    return rankLive;
  }

  /// Open the FAISS index referenced by `info`.  Returns nullptr when the
  /// file is missing and missingFileOK is true (caller should re-parse
  /// IndexInfo and retry).
  static std::shared_ptr<VectorAuxReader> open(Directory& dir,
                                               const solux::api::AuxIndexInfo& info,
                                               bool missingFileOK) {
    // V1 layout: exactly one file per vector aux entry.
    if (info.files.size() != 1) {
      throw std::runtime_error(std::format(
        "VectorAuxReader: expected 1 file, got {} for aux '{}'",
        info.files.size(), info.name));
    }
    std::string_view fname = info.files[0];

    // expectSynced=true: aux files were fsynced before the IndexInfo that
    // references them was published, same contract as segment files.
    auto file = dir.openFile(fname, /*expectSynced=*/true);
    if (file == nullptr) {
      if (missingFileOK) {
        LOG_TRACE("VectorAuxReader::open: file {} missing for aux '{}', will retry",
                  fname, info.name);
        return nullptr;
      }
      throw std::filesystem::filesystem_error(
        std::format("Missing aux index file '{}' for aux '{}'", fname, info.name),
        std::make_error_code(std::errc::no_such_file_or_directory));
    }

    VectorAuxMeta meta = VectorAuxMeta::fromBytes(std::string_view((const char*)info.opaque_meta.data(), info.opaque_meta.size()), info.name);

    // Residency rule: index data must NOT be force-resident.  IVF/IVF+PQ list
    // payloads are served zero-copy from the file's memory view via
    // MmapInvertedLists (see VectorAuxListsFooter); only the small routing
    // model (centroids + PQ codebooks) is deserialized into RAM.
    //
    // Stay-on-disk story for other index types when we add them:
    //   - HNSW: FAISS 1.14.1's IO_FLAG_MMAP_IFC can mmap both the graph
    //     adjacency (hnsw.neighbors) and the vectors (IndexFlat.codes), copying
    //     only ~12 B/vector of per-node metadata (levels/offsets) into RAM.
    //     Caveat: random-access graph traversal still churns its working
    //     set under memory pressure (the durable cost is the access pattern, not
    //     forced residency), so mmap helps HNSW less than it helps the sequential
    //     IVF / IVF+PQ list scans.
    //   - PQ standalone, NSG, NN-Descent: in-memory; same IO_FLAG_MMAP_IFC
    //     route as HNSW when they matter.
    //
    // Likely tiering once past v1: brute-force-over-column for small segments,
    // HNSW for RAM-fit mid-size, IVF + mmap lists for large.
    return std::make_shared<VectorAuxReader>(
      std::string(info.name), std::string(info.field),
      info.gen, info.built_core_gen, std::move(file), meta);
  }

private:
  /// faiss::IOReader backed by a contiguous in-memory buffer.  Our Directory
  /// implementations expose the whole file as a memory view (mmap for FSDir,
  /// owned buffer for RAMDir), so we can hand FAISS a sequential reader
  /// without copying.  faiss::read_index copies what it needs into the
  /// returned Index, so the buffer doesn't need to outlive the call.
  class BufferIOReader : public faiss::IOReader {
    const char* data;
    size_t size;
    size_t pos = 0;

  public:
    BufferIOReader(const char* d, size_t s) : data(d), size(s) {}

    size_t operator()(void* ptr, size_t itemSize, size_t nitems) override {
      if (itemSize == 0) return nitems;
      size_t avail = (size - pos) / itemSize;
      size_t toRead = std::min(nitems, avail);
      std::memcpy(ptr, data + pos, toRead * itemSize);
      pos += toRead * itemSize;
      return toRead;
    }
  };
};

} // namespace solux
