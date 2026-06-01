#include "VectorIndexBuilder.h"

#include <faiss/IndexFlat.h>
#include <faiss/MetricType.h>
#include <faiss/index_io.h>
#include <faiss/impl/io.h>
#include <faiss/utils/distances.h>

#include <algorithm>
#include <stdexcept>
#include <unordered_set>

#include "solux/reader/FieldReader.h"
#include "solux/reader/VectorReader.h"
#include "solux/store/InputStream.h"
#include "solux/store/OutputStream.h"
#include "solux/util/MemPool.h"
#include "solux/util/log.h"

namespace solux {

namespace {

// Adapter from faiss::IOWriter to solux::OutputStream.
class FaissOutAdapter : public faiss::IOWriter {
public:
  OutputStream& out;
  explicit FaissOutAdapter(OutputStream& o) : out(o) {}
  size_t operator()(const void* ptr, size_t size, size_t nitems) override {
    out.write(ptr, size * nitems);
    return nitems;
  }
};

faiss::MetricType toFaissMetric(VectorFieldType::Metric m) {
  switch (m) {
    case VectorFieldType::METRIC_L2: return faiss::METRIC_L2;
    case VectorFieldType::METRIC_IP: return faiss::METRIC_INNER_PRODUCT;
    // Cosine = IP on L2-normalized vectors.  buildField copies + normalizes
    // each segment's vectors before calling index->add().
    case VectorFieldType::METRIC_COSINE: return faiss::METRIC_INNER_PRODUCT;
    default:
      throw std::runtime_error("VectorIndexBuilder: unsupported metric");
  }
}

} // namespace

// Default 1 MiB.  Tests can override via VectorIndexBuilder::renormChunkBytes.
size_t VectorIndexBuilder::renormChunkBytes = 1 * 1024 * 1024;

bool VectorIndexBuilder::selectorMatches(const std::vector<std::string>& selectors,
                                         std::string_view name) {
  for (const auto& s : selectors) {
    if (s == "*") return true;
    if (s == name) return true;
  }
  return false;
}

std::vector<std::pair<std::string, const VectorFieldType*>>
VectorIndexBuilder::collectEligibleFields() {
  // Collect field names typed VECTOR from every segment.
  std::unordered_set<std::string> seen;
  for (auto& seg : segments_) {
    MemPool pool;
    FieldReader fr(pool, *seg.postingsReader);
    while (fr.readNextField()) {
      SegFieldInfo fi;
      fr.readFieldInfo(fi);
      if (fi.type == FieldType::VECTOR) {
        seen.emplace((std::string_view)fr.name());
      }
    }
  }

  std::vector<std::pair<std::string, const VectorFieldType*>> out;
  out.reserve(seen.size());
  for (const auto& name : seen) {
    auto* ft = schema_.getFieldTypePtr(name);
    if (ft == nullptr || ft->type() != FieldType::VECTOR) continue;
    auto* vft = (const VectorFieldType*)ft;
    if (!vft->buildsAnnIndex()) continue;
    // Multi-valued vectors are indexed too: every value is added to FAISS, and the
    // query layer maps each FAISS id back to its owning doc via the valueRank->docId
    // column (StrColReader::getValDocReader).
    out.emplace_back(name, vft);
  }
  // Sort for stable AuxIndexInfo ordering across rebuilds (eases diffs/tests).
  std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  return out;
}

std::vector<proto::AuxIndexInfo>
VectorIndexBuilder::build(const std::vector<std::string>& selectors,
                          const boost::unordered_flat_set<std::string>& skipNames,
                          std::vector<std::string>& outFilesToSync) {
  std::vector<proto::AuxIndexInfo> result;
  if (selectors.empty()) return result;

  auto eligible = collectEligibleFields();
  for (const auto& [fieldName, vft] : eligible) {
    std::string auxName(NAME_PREFIX);
    auxName.append(fieldName);
    if (!selectorMatches(selectors, auxName)) continue;
    if (skipNames.contains(auxName)) {
      // Caller has a still-valid carried entry; rebuild would be wasted work.
      LOG_TRACE("VectorIndexBuilder: {} already up-to-date; skipping rebuild", auxName);
      continue;
    }

    LOG_TRACE("VectorIndexBuilder: building {} (metric={}, dims_pinned={})",
             auxName, (int)vft->metric_, vft->dims_);
    auto info = buildField(fieldName, *vft, outFilesToSync);
    if (info) {
      result.emplace_back(std::move(*info));
    }
  }
  return result;
}

std::optional<proto::AuxIndexInfo>
VectorIndexBuilder::buildField(std::string_view fieldName,
                               const VectorFieldType& ft,
                               std::vector<std::string>& outFilesToSync) {
  // dims=0 means "infer from first segment with values".  All segments must
  // agree on dims; mismatch throws.
  int32_t dims = ft.dims_;
  faiss::MetricType metric = toFaissMetric(ft.metric_);
  // Defer FAISS index construction until we know dims (might come from a
  // later segment if the schema didn't pin it).
  std::unique_ptr<faiss::IndexFlat> index;

  // Chunked working buffer for the COSINE renormalization path.  Allocated
  // lazily on first use, reused across segments.  Bounds working memory at
  // ~renormChunkBytes regardless of segment size.
  std::vector<float> renormBuf;

  for (auto& seg : segments_) {
    auto& pr = *seg.postingsReader;
    MemPool pool;
    FieldReader fr(pool, pr);
    if (!fr.seek(fieldName)) continue;

    SegFieldInfo fi;
    fr.readFieldInfo(fi);
    if (fi.type != FieldType::VECTOR) continue;

    // Multi-valued is handled transparently: numVectors() counts all values across
    // docs and the column stores them contiguously, so the add() calls below index
    // every vector regardless of valued-ness.
    VectorReader vr(pr, fi);
    int32_t segDims = vr.dims();
    if (segDims <= 0) continue;
    if (dims == 0) {
      dims = segDims;
    } else if (dims != segDims) {
      throw std::runtime_error(fmt::format(
        "VectorIndexBuilder: dims mismatch in field {} (expected {}, segment {} has {})",
        fieldName, dims, seg.segId, segDims));
    }

    int64_t numVals = vr.numVectors();
    if (numVals == 0) continue;

    if (!index) {
      // IndexFlat keeps the entire vector array in process memory
      // (IndexFlatCodes::codes is just a std::vector<uint8_t>).  Peak RAM
      // during build is roughly 2x the vector data: once in the source column
      // (mmap or RAM), once in FAISS.  write_index streams its output, so the
      // serialization step doesn't add a third copy.
      // For collections that don't fit in RAM, switch to an IndexIVF* variant
      // with OnDiskInvertedLists, or HNSW/PQ for compressed in-memory storage.
      index = std::make_unique<faiss::IndexFlat>(dims, metric);
    }

    // Vector storage is contiguous (fixed-size column), so for the no-renorm
    // path we can hand FAISS the whole block at once.  No live filtering:
    // deleted-doc vectors stay in the index until the next rebuild - query
    // layer filters.
    const float* base = (const float*)vr.vectorAtRank(0).data();
    if ((ft.metric_ == VectorFieldType::METRIC_COSINE) && !ft.normalized_) {
      // Cosine via FAISS IP requires unit-norm vectors.  Copy in fixed-size
      // chunks, normalize each chunk, then add.
      size_t bytesPerVec = (size_t)dims * sizeof(float);
      size_t chunkVecs = std::max((size_t)1, renormChunkBytes / bytesPerVec);
      size_t chunkFloats = chunkVecs * (size_t)dims;
      if (renormBuf.size() < chunkFloats) renormBuf.resize(chunkFloats);
      for (int64_t off = 0; off < numVals; off += (int64_t)chunkVecs) {
        int64_t n = std::min((int64_t)chunkVecs, numVals - off);
        std::memcpy(renormBuf.data(), base + off * dims,
                    (size_t)n * bytesPerVec);
        faiss::fvec_renorm_L2((size_t)dims, (size_t)n, renormBuf.data());
        index->add(n, renormBuf.data());
      }
    } else {
      // Either non-cosine, or user asserts vectors are already unit-norm.
      index->add(numVals, base);
    }
  }

  // No segment had any values - nothing to build.  Skip emitting an AuxIndexInfo.
  if (!index) {
    LOG_INFO("VectorIndexBuilder: field {} has no vectors; skipping", fieldName);
    return std::nullopt;
  }

  // Compose name and file.
  std::string auxName(NAME_PREFIX);
  auxName.append(fieldName);
  std::string faissFile = Postings::getAuxIndexFileName(auxName, indexGen_, 0);

  // Write FAISS bytes via our Directory.
  {
    auto file = dir_.createFile(faissFile);
    OutputStream os;
    os.setFile(&*file);
    FaissOutAdapter adapter(os);
    faiss::write_index(index.get(), &adapter);
    os.close();
    dir_.finishFile(*file);
  }

  outFilesToSync.push_back(faissFile);

  proto::AuxIndexInfo info;
  info.set_kind(std::string(KIND));
  info.set_field(std::string(fieldName));
  info.set_name(std::move(auxName));
  info.set_gen(indexGen_);
  // Record the segment composition we built against; carried in IndexInfo so
  // future commits can invalidate this entry when segments merge/split.
  info.set_built_core_gen(coreGen_);
  info.add_files(faissFile);
  // opaque_meta: pack {dims, metric} for cheap read-side checks.  ntotal is
  // available from the FAISS index itself.  Layout: int32 dims, int32 metric.
  std::string meta;
  meta.resize(sizeof(int32_t) * 2);
  std::memcpy(meta.data(), &dims, sizeof(int32_t));
  int32_t metricInt = (int32_t)ft.metric_;
  std::memcpy(meta.data() + sizeof(int32_t), &metricInt, sizeof(int32_t));
  info.set_opaque_meta(std::move(meta));

  LOG_TRACE("VectorIndexBuilder: built {} ntotal={} dims={} metric={} file={}",
           info.name(), index->ntotal, dims, (int)ft.metric_, faissFile);

  return info;
}

} // namespace solux
