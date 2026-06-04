#pragma once

#include <faiss/Index.h>
#include <faiss/impl/io.h>
#include <faiss/index_io.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#include "AuxReader.h"
#include "protos/solux_types.pb.h"
#include "solux/store/Directory.h"
#include "solux/store/InputStream.h"
#include "solux/util/log.h"

namespace solux {

/// AuxReader for kind == "vector_faiss".  Holds a deserialized faiss::Index
/// loaded from the single file referenced by AuxIndexInfo.files(0), plus the
/// dims and metric values cached out of AuxIndexInfo.opaque_meta.
///
/// V1 contract (matches VectorIndexBuilder): one file per entry, IndexFlat
/// {L2,IP,COSINE}.  Both single- and multi-valued vector fields are supported:
/// every vector is its own FAISS id, and KnnQuery maps each id back to its
/// owning doc via the segment's valueRank->docId column.  The FAISS index is
/// fully deserialized into process memory, so the source file does not need to
/// outlive the reader.
class VectorAuxReader : public AuxReader {
  std::string name;
  std::string field;
  std::unique_ptr<faiss::Index> faissIndex;
  int32_t dims;

  // Raw int from proto::VectorParams::Metric (recorded in opaque_meta at build).
  int32_t metric;

public:
  static constexpr std::string_view KIND = "vector_faiss";

  VectorAuxReader(std::string name, std::string field,
                  uint64_t gen, uint64_t builtCoreGen,
                  std::unique_ptr<faiss::Index> idx,
                  int32_t dims, int32_t metric) noexcept
    : AuxReader(gen, builtCoreGen),
      name(std::move(name)), field(std::move(field)),
      faissIndex(std::move(idx)), dims(dims), metric(metric) {}

  std::string_view getKind() const override { return KIND; }
  std::string_view getName() const override { return name; }
  std::string_view getField() const noexcept { return field; }
  int32_t getDims() const noexcept { return dims; }
  int32_t getMetric() const noexcept { return metric; }

  // Search-time entry point.  faiss::Index::search is thread-safe for
  // concurrent readers.
  faiss::Index* getFaissIndex() const noexcept { return faissIndex.get(); }

  /// Open the FAISS index referenced by `info`.  Returns nullptr when the
  /// file is missing and missingFileOK is true (caller should re-parse
  /// IndexInfo and retry).
  static std::shared_ptr<VectorAuxReader> open(Directory& dir,
                                               const proto::AuxIndexInfo& info,
                                               bool missingFileOK) {
    // V1 layout: exactly one file per vector aux entry.
    if (info.files_size() != 1) {
      throw std::runtime_error(std::format(
        "VectorAuxReader: expected 1 file, got {} for aux '{}'",
        info.files_size(), info.name()));
    }
    std::string_view fname = info.files(0);

    // expectSynced=true: aux files were fsynced before the IndexInfo that
    // references them was published, same contract as segment files.
    auto file = dir.openFile(fname, /*expectSynced=*/true);
    if (file == nullptr) {
      if (missingFileOK) {
        LOG_TRACE("VectorAuxReader::open: file {} missing for aux '{}', will retry",
                  fname, info.name());
        return nullptr;
      }
      throw std::filesystem::filesystem_error(
        std::format("Missing aux index file '{}' for aux '{}'", fname, info.name()),
        std::make_error_code(std::errc::no_such_file_or_directory));
    }

    // Decode opaque_meta produced by VectorIndexBuilder: int32 dims, int32 metric.
    // Older entries without opaque_meta still work - we fall back to idx->d for
    // dims and report metric=0 (unknown).
    int32_t dimsMeta = 0;
    int32_t metricMeta = 0;
    if (info.opaque_meta().size() >= 2 * sizeof(int32_t)) {
      std::memcpy(&dimsMeta, info.opaque_meta().data(), sizeof(int32_t));
      std::memcpy(&metricMeta, info.opaque_meta().data() + sizeof(int32_t), sizeof(int32_t));
    }

    auto bytes = file->read();
    BufferIOReader io(bytes.data(), bytes.size());
    // TODO: zero-copy IndexFlat path.  IndexFlat's on-disk bytes are the same
    // float vectors we already store in the column; read_index unconditionally
    // copies them into IndexFlatCodes::codes (its owned std::vector<uint8_t>),
    // so we pay 1x extra RAM per flat aux index.  When/if we keep flat as a
    // production path (e.g. small-segment optimization, where building HNSW
    // doesn't pay off), skip writing the FAISS file in the builder and run
    // brute-force kNN at search time directly over the mmap'd vector column
    // via faiss::knn_L2sqr / knn_inner_product.  No deserialize, no copy,
    // same SIMD distance kernels.
    //
    // Stay-on-disk story for other index types when we add them:
    //   - HNSW: FAISS 1.14.1's IO_FLAG_MMAP_IFC can mmap both the graph
    //     adjacency (hnsw.neighbors) and the vectors (IndexFlat.codes), copying
    //     only ~12 B/vector of per-node metadata (levels/offsets) into RAM, so
    //     the deserialize-into-RAM copy IS avoidable.  The old "HNSW can't mmap"
    //     belief came from the legacy IO_FLAG_MMAP, which only mapped IVF on-disk
    //     lists.  Caveat: random-access graph traversal still churns its working
    //     set under memory pressure (the durable cost is the access pattern, not
    //     forced residency), so mmap helps HNSW less than it helps the sequential
    //     IVF / IVF+PQ list scans.
    //   - IVF* (IVFFlat, IVFPQ, ...): FAISS supports OnDiskInvertedLists +
    //     IO_FLAG_MMAP, which keeps the bulk inverted-list data on disk and
    //     only loads centroids/metadata.  Two frictions vs our stack: (1) the
    //     mmap path is wired through read_index(const char* fname, ...), not
    //     IOReader, so it bypasses Directory; (2) builder must write the
    //     sidecar .ivfdata layout.  Cleanest long-term fix is a custom
    //     faiss::InvertedLists subclass backed by a Solux InputFile - we own
    //     the on-disk layout, IO_FLAG_MMAP becomes irrelevant, and FAISS
    //     reads through Directory regardless of impl.  Non-trivial but
    //     unblocks IVF cleanly.
    //   - PQ standalone, NSG, NN-Descent: in-memory, same as HNSW.
    //
    // Likely tiering once past v1: brute-force-over-column for small segments,
    // HNSW for RAM-fit mid-size, IVF + custom InvertedLists for large.
    std::unique_ptr<faiss::Index> idx(faiss::read_index(&io));
    if (!idx) {
      throw std::runtime_error(std::format(
        "VectorAuxReader: read_index returned null for {}", fname));
    }
    if (dimsMeta != 0 && idx->d != dimsMeta) {
      throw std::runtime_error(std::format(
        "VectorAuxReader: dims mismatch for aux '{}': index has {}, opaque_meta says {}",
        info.name(), (int)idx->d, dimsMeta));
    }

    int32_t dims = dimsMeta != 0 ? dimsMeta : (int32_t)idx->d;
    return std::make_shared<VectorAuxReader>(
      std::string(info.name()), std::string(info.field()),
      info.gen(), info.built_core_gen(),
      std::move(idx), dims, metricMeta);
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
