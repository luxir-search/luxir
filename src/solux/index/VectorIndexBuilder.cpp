#include "VectorIndexBuilder.h"

#include <faiss/IndexFlat.h>
#include <faiss/IndexIVFPQ.h>
#include <faiss/MetricType.h>
#include <faiss/index_io.h>
#include <faiss/impl/io.h>
#include <faiss/utils/distances.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_set>

#include "solux/reader/FieldReader.h"
#include "solux/reader/VectorAuxReader.h"
#include "solux/reader/VectorReader.h"
#include "solux/store/InputStream.h"
#include "solux/store/OutputStream.h"
#include "solux/util/MemPool.h"
#include "solux/util/log.h"

namespace solux {

namespace {

constexpr int64_t MIN_TRAINING_POINTS_PER_PQ_CENTROID = 39;

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

int32_t chooseIvfNList(int64_t ntotal) {
  if (VectorIndexBuilder::ivfPqNList > 0) {
    return (int32_t)std::min<int64_t>(VectorIndexBuilder::ivfPqNList, ntotal);
  }
  int64_t derived = (int64_t)std::sqrt((double)ntotal);
  derived = std::clamp<int64_t>(derived, 1, 4096);
  return (int32_t)std::min<int64_t>(derived, ntotal);
}

int32_t choosePqM(int32_t dims) {
  if (VectorIndexBuilder::ivfPqM > 0) {
    if (dims % VectorIndexBuilder::ivfPqM != 0) {
      throw std::runtime_error(fmt::format(
        "VectorIndexBuilder: IVF+PQ M={} does not divide dims={}",
        VectorIndexBuilder::ivfPqM, dims));
    }
    return VectorIndexBuilder::ivfPqM;
  }

  int32_t maxM = std::min<int32_t>(32, dims);
  for (int32_t m = maxM; m >= 1; m--) {
    if (dims % m == 0 && dims / m >= 4) return m;
  }
  for (int32_t m = maxM; m >= 1; m--) {
    if (dims % m == 0) {
      if (m == 1) {
        LOG_WARN("VectorIndexBuilder: IVF+PQ dims={} has no divisor > 1; using M=1", dims);
      }
      return m;
    }
  }
  throw std::runtime_error("VectorIndexBuilder: vector dims must be positive");
}

int32_t effectivePqBits() {
  return std::clamp(VectorIndexBuilder::ivfPqBits, 1, 8);
}

int32_t effectiveNProbe(int32_t nlist) {
  int32_t configured = VectorIndexBuilder::ivfPqDefaultNProbe;
  if (configured <= 0) configured = std::max<int32_t>(1, (int32_t)std::sqrt((double)nlist));
  return std::clamp(configured, 1, nlist);
}

bool shouldNormalizeForFaiss(const VectorFieldType& ft) {
  return ft.metric_ == VectorFieldType::METRIC_COSINE && !ft.normalized_ && !ft.normalizeOnWrite_;
}

std::string makeVectorMeta(int32_t dims, const VectorFieldType& ft,
                           int32_t engine, int32_t nlist = 0,
                           int32_t nprobe = 0, int32_t pqM = 0,
                           int32_t pqBits = 0) {
  VectorAuxMeta meta;
  meta.dims = dims;
  meta.metric = (int32_t)ft.metric_;
  meta.cosineNormalizeColumnOnRescore = shouldNormalizeForFaiss(ft) ? 1 : 0;
  meta.engine = engine;
  meta.nlist = nlist;
  meta.defaultBreadth = nprobe;
  meta.pqM = pqM;
  meta.pqBits = pqBits;
  return meta.toBytes();
}

void addVectorsToIndex(faiss::Index& index, VectorReader& vr, int32_t dims,
                       bool normalizeForFaiss, std::vector<float>& scratch,
                       size_t chunkBytes) {
  int64_t numVals = vr.numVectors();
  if (numVals <= 0) return;
  const float* base = (const float*)vr.vectorAtRank(0).data();
  size_t bytesPerVec = (size_t)dims * sizeof(float);
  size_t chunkVecs = chunkBytes == 0 ? (size_t)numVals
                                     : std::max((size_t)1, chunkBytes / bytesPerVec);

  if (!normalizeForFaiss) {
    for (int64_t off = 0; off < numVals; off += (int64_t)chunkVecs) {
      int64_t n = std::min((int64_t)chunkVecs, numVals - off);
      index.add(n, base + off * dims);
    }
    return;
  }

  size_t chunkFloats = chunkVecs * (size_t)dims;
  if (scratch.size() < chunkFloats) scratch.resize(chunkFloats);
  for (int64_t off = 0; off < numVals; off += (int64_t)chunkVecs) {
    int64_t n = std::min((int64_t)chunkVecs, numVals - off);
    std::memcpy(scratch.data(), base + off * dims, (size_t)n * bytesPerVec);
    faiss::fvec_renorm_L2((size_t)dims, (size_t)n, scratch.data());
    index.add(n, scratch.data());
  }
}

void appendTrainingVectors(std::vector<float>& sample, int64_t sampleCap,
                           int64_t& nextSampleOrd,
                           int64_t ntotal,
                           int64_t segmentGlobalStart,
                           VectorReader& vr,
                           int32_t dims) {
  int64_t numVals = vr.numVectors();
  int64_t segmentGlobalEnd = segmentGlobalStart + numVals;
  while (nextSampleOrd < sampleCap) {
    int64_t targetGlobal = (nextSampleOrd * ntotal) / sampleCap;
    if (targetGlobal >= segmentGlobalEnd) break;
    if (targetGlobal >= segmentGlobalStart) {
      auto vec = vr.vectorAtRank(targetGlobal - segmentGlobalStart);
      size_t old = sample.size();
      sample.resize(old + (size_t)dims);
      std::memcpy(sample.data() + old, vec.data(), (size_t)dims * sizeof(float));
    }
    nextSampleOrd++;
  }
}

template <typename Fn>
bool forEachVectorSegment(std::span<const VectorIndexBuilder::SegInput> segments,
                          std::string_view fieldName,
                          int32_t& dims,
                          Fn fn) {
  for (const auto& seg : segments) {
    auto& pr = *seg.postingsReader;
    MemPool pool;
    FieldReader fr(pool, pr);
    if (!fr.seek(fieldName)) continue;

    SegFieldInfo fi;
    fr.readFieldInfo(fi);
    if (fi.type != FieldType::VECTOR) continue;

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

    if (!fn(seg, vr)) return false;
  }
  return true;
}

} // namespace

// Default 1 MiB.  Tests can override via VectorIndexBuilder::renormChunkBytes.
size_t VectorIndexBuilder::renormChunkBytes = 1 * 1024 * 1024;
bool VectorIndexBuilder::buildFaissFlatAuxIndexes = false;
bool VectorIndexBuilder::buildFaissIvfPqAuxIndexes = true;
int32_t VectorIndexBuilder::ivfPqNList = 0;
int32_t VectorIndexBuilder::ivfPqM = 0;
int32_t VectorIndexBuilder::ivfPqBits = 8;
int32_t VectorIndexBuilder::ivfPqDefaultNProbe = 0;
int64_t VectorIndexBuilder::ivfPqMinTrainingVectors =
    MIN_TRAINING_POINTS_PER_PQ_CENTROID * 256;
size_t VectorIndexBuilder::ivfPqTrainingSampleBytes = 64 * 1024 * 1024;
size_t VectorIndexBuilder::ivfPqAddChunkBytes = 16 * 1024 * 1024;

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
    if (!vft->knnSearchable()) continue;
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
  if (!buildFaissFlatAuxIndexes && !buildFaissIvfPqAuxIndexes) {
    LOG_TRACE("VectorIndexBuilder: vector aux build disabled; flat kNN uses column scan");
    return result;
  }

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
  if (buildFaissFlatAuxIndexes) {
    return buildFlatField(fieldName, ft, outFilesToSync);
  }
  return buildIvfPqField(fieldName, ft, outFilesToSync);
}

std::optional<proto::AuxIndexInfo>
VectorIndexBuilder::buildFlatField(std::string_view fieldName,
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
    if (shouldNormalizeForFaiss(ft)) {
      // Cosine via FAISS IP requires unit-norm vectors.  When the write path
      // did not already normalize the column, copy in fixed-size chunks,
      // normalize each chunk, then add.
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
      // Either non-cosine, already normalized on write, or user asserts
      // vectors are already unit-norm.
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
  // opaque_meta stores fixed-width engine metadata.  ntotal is available from
  // the FAISS index itself.
  info.set_opaque_meta(makeVectorMeta(dims, ft, VectorAuxMeta::ENGINE_FLAT));

  LOG_TRACE("VectorIndexBuilder: built {} ntotal={} dims={} metric={} file={}",
           info.name(), index->ntotal, dims, (int)ft.metric_, faissFile);

  return info;
}

std::optional<proto::AuxIndexInfo>
VectorIndexBuilder::buildIvfPqField(std::string_view fieldName,
                                    const VectorFieldType& ft,
                                    std::vector<std::string>& outFilesToSync) {
  int32_t dims = ft.dims_;
  faiss::MetricType metric = toFaissMetric(ft.metric_);
  int64_t ntotal = 0;

  forEachVectorSegment(segments_, fieldName, dims,
      [&](const VectorIndexBuilder::SegInput&, VectorReader& vr) {
    ntotal += vr.numVectors();
    return true;
  });

  if (ntotal == 0) {
    LOG_INFO("VectorIndexBuilder: field {} has no vectors; skipping", fieldName);
    return std::nullopt;
  }

  int32_t nlist = chooseIvfNList(ntotal);
  int32_t pqM = choosePqM(dims);
  int32_t pqBits = effectivePqBits();
  int32_t ksub = 1 << pqBits;
  int64_t pqTrainingFloor = MIN_TRAINING_POINTS_PER_PQ_CENTROID * (int64_t)ksub;
  int64_t requiredTraining = std::max<int64_t>(
      std::max<int64_t>(ivfPqMinTrainingVectors, nlist), pqTrainingFloor);
  if (ntotal < requiredTraining) {
    LOG_TRACE("VectorIndexBuilder: field {} has {} vectors, below IVF+PQ training floor {}; "
              "using flat-over-column fallback",
              fieldName, ntotal, requiredTraining);
    return std::nullopt;
  }

  size_t bytesPerVec = (size_t)dims * sizeof(float);
  int64_t byteCap = bytesPerVec == 0 ? 0 : (int64_t)(ivfPqTrainingSampleBytes / bytesPerVec);
  int64_t sampleCap = std::min<int64_t>(
      ntotal, std::max<int64_t>(requiredTraining, std::max<int64_t>(1, byteCap)));
  std::vector<float> training;
  training.reserve((size_t)sampleCap * (size_t)dims);

  int64_t globalStart = 0;
  int64_t nextSampleOrd = 0;
  forEachVectorSegment(segments_, fieldName, dims,
      [&](const VectorIndexBuilder::SegInput&, VectorReader& vr) {
    appendTrainingVectors(training, sampleCap, nextSampleOrd, ntotal,
                          globalStart, vr, dims);
    globalStart += vr.numVectors();
    return nextSampleOrd < sampleCap;
  });
  int64_t ntrain = (int64_t)(training.size() / (size_t)dims);
  if (ntrain < requiredTraining) {
    LOG_TRACE("VectorIndexBuilder: field {} collected only {} training vectors; "
              "using flat-over-column fallback",
              fieldName, ntrain);
    return std::nullopt;
  }
  if (shouldNormalizeForFaiss(ft)) {
    faiss::fvec_renorm_L2((size_t)dims, (size_t)ntrain, training.data());
  }

  auto quantizer = std::make_unique<faiss::IndexFlat>(dims, metric);
  auto index = std::make_unique<faiss::IndexIVFPQ>(
      quantizer.get(), (size_t)dims, (size_t)nlist, (size_t)pqM,
      (size_t)pqBits, metric);
  quantizer.release();
  index->own_fields = true;
  int32_t nprobe = effectiveNProbe(nlist);
  index->nprobe = (size_t)nprobe;

  LOG_TRACE("VectorIndexBuilder: training IVF+PQ {} ntotal={} ntrain={} dims={} "
            "nlist={} M={} bits={} nprobe={}",
            fieldName, ntotal, ntrain, dims, nlist, pqM, pqBits, nprobe);
  index->train(ntrain, training.data());
  training.clear();
  training.shrink_to_fit();

  std::vector<float> scratch;
  forEachVectorSegment(segments_, fieldName, dims,
      [&](const VectorIndexBuilder::SegInput&, VectorReader& vr) {
    if (vr.numVectors() > 0) {
      addVectorsToIndex(*index, vr, dims, shouldNormalizeForFaiss(ft),
                        scratch, ivfPqAddChunkBytes);
    }
    return true;
  });

  if (index->ntotal != ntotal) {
    throw std::runtime_error(fmt::format(
      "VectorIndexBuilder: IVF+PQ ntotal mismatch for field {} (expected {}, built {})",
      fieldName, ntotal, index->ntotal));
  }

  std::string auxName(NAME_PREFIX);
  auxName.append(fieldName);
  std::string faissFile = Postings::getAuxIndexFileName(auxName, indexGen_, 0);

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
  info.set_built_core_gen(coreGen_);
  info.add_files(faissFile);
  info.set_opaque_meta(makeVectorMeta(
      dims, ft, VectorAuxMeta::ENGINE_IVFPQ, nlist, nprobe, pqM, pqBits));

  LOG_TRACE("VectorIndexBuilder: built IVF+PQ {} ntotal={} dims={} metric={} "
            "nlist={} M={} bits={} file={}",
            info.name(), index->ntotal, dims, (int)ft.metric_,
            nlist, pqM, pqBits, faissFile);

  return info;
}

} // namespace solux
