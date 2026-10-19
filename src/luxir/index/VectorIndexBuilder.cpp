// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#include "VectorIndexBuilder.h"

#include <faiss/IndexFlat.h>
#include <faiss/IndexIVFPQ.h>
#include <faiss/IndexIVFRaBitQ.h>
#include <faiss/IndexPreTransform.h>
#include <faiss/MetricType.h>
#include <faiss/VectorTransform.h>
#include <faiss/index_io.h>
#include <faiss/impl/io.h>
#include <faiss/utils/distances.h>

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <unordered_set>

#include "luxir/util/Signal.h"
#include "luxir/reader/FieldReader.h"
#include "luxir/reader/VectorAuxReader.h"
#include "luxir/reader/VectorReader.h"
#include "luxir/store/InputStream.h"
#include "luxir/store/OutputStream.h"
#include "luxir/util/MemPool.h"
#include "luxir/util/log.h"

namespace luxir {

namespace {

constexpr int64_t MIN_TRAINING_POINTS_PER_CENTROID = 39;

// Adapter from faiss::IOWriter to luxir::OutputStream.
class FaissOutAdapter : public faiss::IOWriter {
public:
  OutputStream& out;
  explicit FaissOutAdapter(OutputStream& o) : out(o) {}
  size_t operator()(const void* ptr, size_t size, size_t nitems) override {
    out.write(ptr, size * nitems);
    return nitems;
  }
};

// Swaps an empty set of inverted lists into an IVF index for the duration of
// faiss::write_index, so the serialized blob carries only the index "header"
// (params + coarse quantizer + PQ codebooks); the list payloads are written
// separately in Luxir's own layout (VectorAuxListsFooter) and served
// zero-copy from the mmapped file by MmapInvertedLists at read time.  RAII:
// the real lists pointer is restored even if write_index throws (the index
// must never own the stack-resident empty lists).
struct EmptyInvlistsSwap {
  faiss::IndexIVF& ivf;
  faiss::InvertedLists* saved;
  faiss::ArrayInvertedLists empty;

  explicit EmptyInvlistsSwap(faiss::IndexIVF& ivf)
    : ivf(ivf), saved(ivf.invlists), empty(ivf.nlist, ivf.code_size) {
    ivf.invlists = &empty;
  }
  ~EmptyInvlistsSwap() { ivf.invlists = saved; }
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

int32_t chooseIvfNList(int32_t configured, int64_t ntotal) {
  if (configured > 0) return (int32_t)std::min<int64_t>(configured, ntotal);
  return defaultIvfNList(ntotal);
}

int32_t choosePqM(int32_t configured, int32_t dims, faiss::MetricType metric) {
  if (configured > 0) {
    // The schema validates authored settings; keep this check for
    // programmatically constructed field types too.
    if (dims % configured != 0) {
      throw std::runtime_error(fmt::format(
        "VectorIndexBuilder: IVF+PQ M={} does not divide dims={}", configured, dims));
    }
    return configured;
  }

  // IP (including cosine) builds lookup tables per query, so grow code bytes
  // with dims. L2 + by_residual recomputes nlist * M * 256 floats at open
  // and keeps them resident at 8 bits; retain its cap to bound that table.
  int32_t cap = metric == faiss::METRIC_INNER_PRODUCT ? std::max(32, dims / 8) : 32;
  int32_t maxM = std::min(cap, dims);
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

bool shouldNormalizeForFaiss(const VectorFieldType& ft) {
  return ft.metric_ == VectorFieldType::METRIC_COSINE && !ft.normalized_ && !ft.normalizeOnWrite_;
}

// Shared by the owned training sample and borrowed column vectors. Rotation
// always writes to separate storage: FAISS's matrix multiply cannot alias.
template <typename Consume>
void transformVectorChunks(const float* base, int64_t numVals, int32_t dims,
                            bool normalizeForFaiss, const faiss::VectorTransform* rotation,
                            size_t chunkBytes, Consume consume) {
  if (numVals <= 0) return;
  size_t bytesPerVec = (size_t)dims * sizeof(float);
  size_t chunkVecs = std::min((size_t)numVals, std::max((size_t)1, chunkBytes / bytesPerVec));
  size_t chunkFloats = chunkVecs * (size_t)dims;
  std::vector<float> scratch, rotated;
  if (normalizeForFaiss) scratch.resize(chunkFloats);
  if (rotation != nullptr) rotated.resize(chunkFloats);

  for (int64_t off = 0; off < numVals; off += (int64_t)chunkVecs) {
    int64_t n = std::min((int64_t)chunkVecs, numVals - off);
    const float* x = base + off * dims;
    if (normalizeForFaiss) {
      std::memcpy(scratch.data(), x, (size_t)n * bytesPerVec);
      faiss::fvec_renorm_L2((size_t)dims, (size_t)n, scratch.data());
      x = scratch.data();
    }
    if (rotation != nullptr) {
      rotation->apply_noalloc(n, x, rotated.data());
      x = rotated.data();
    }
    consume(off, n, x);
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
    auto* info = pr.fieldInfo(fieldName);
    if (!info) continue;
    const auto& fi = *info;
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

bool VectorIndexBuilder::buildFaissAuxIndexes = true;
// PQ trains 2^bits centroids per sub-quantizer: FAISS wants 39 points per
// centroid, so 8-bit PQ needs ~10k vectors before its codebooks mean much.
int64_t VectorIndexBuilder::ivfPqMinTrainingVectors =
    MIN_TRAINING_POINTS_PER_CENTROID * 256;
// RaBitQ trains no codebook, only the IVF centroids (nlist = sqrt(N) by
// default, so 39 points per centroid holds from N ~ 1.5k).  Training is not
// what binds: below ~8k vectors the exact column scan measured faster than an
// IVF+RaBitQ probe plus rescore at 128 and 768 dims, and it is exact (see
// docs/design/vector-search.md).
int64_t VectorIndexBuilder::ivfRaBitQMinTrainingVectors = 8192;
int64_t VectorIndexBuilder::ivfBuildThresholdScanCost = 1'000'000;
std::atomic<int64_t> VectorIndexBuilder::ivfBuildCountForTests{0};
std::atomic<int64_t> VectorIndexBuilder::ivfMergeBuildCountForTests{0};
size_t VectorIndexBuilder::ivfTrainingSampleBytes = 64 * 1024 * 1024;
size_t VectorIndexBuilder::ivfAddChunkBytes = 16 * 1024 * 1024;

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
    FieldReader fr(*seg.postingsReader);
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

std::vector<AuxInfo>
VectorIndexBuilder::build(const std::vector<std::string>& selectors,
                          const boost::unordered_flat_set<std::string>& skipNames,
                          std::vector<std::string>& outFilesToSync,
                          BuildSite buildSite) {
  std::vector<AuxInfo> result;
  if (selectors.empty()) return result;
  if (!buildFaissAuxIndexes) {
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
    auto info = buildIvfField(fieldName, *vft, outFilesToSync, buildSite);
    if (info) {
      result.emplace_back(std::move(*info));
    }
  }
  return result;
}

std::vector<std::string>
VectorIndexBuilder::matchingOverlayNames(const std::vector<std::string>& selectors) {
  std::vector<std::string> out;
  if (selectors.empty()) return out;

  auto eligible = collectEligibleFields();
  out.reserve(eligible.size());
  for (const auto& [fieldName, vft] : eligible) {
    unused(vft);
    std::string auxName(NAME_PREFIX);
    auxName.append(fieldName);
    if (selectorMatches(selectors, auxName)) {
      out.push_back(std::move(auxName));
    }
  }
  return out;
}

std::optional<AuxInfo>
VectorIndexBuilder::buildIvfField(std::string_view fieldName,
                                  const VectorFieldType& ft,
                                  std::vector<std::string>& outFilesToSync,
                                  BuildSite buildSite) {
  // Test hook: a listener may throw (or block) to exercise vector
  // build-failure paths.  Args: the field name (std::string_view*) and the
  // build site, so a listener can target a specific field and/or site.
  Signal::emit("vectorBuildField", (void*)&fieldName, (void*)(intptr_t)buildSite);
  const VectorAnnConfig& ann = ft.ann();
  int32_t dims = ft.dims_;
  faiss::MetricType metric = toFaissMetric(ft.metric_);
  bool rabitq = ann.codec == VectorAnnConfig::RABITQ;
  const char* codecName = rabitq ? "IVF+RaBitQ" : "IVF+PQ";
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

  int32_t nlist = chooseIvfNList(ann.nlist, ntotal);
  // Training floor.  RaBitQ trains only the IVF centroids; PQ also trains
  // 2^bits centroids per sub-quantizer, which sets its (higher) floor.
  int32_t pqM = 0;
  int32_t pqBits = 0;
  int64_t requiredTraining;
  if (rabitq) {
    requiredTraining = std::max<int64_t>(ivfRaBitQMinTrainingVectors, nlist);
  } else {
    pqBits = std::clamp(ann.pqBits, 1, 8);
    int64_t pqTrainingFloor = MIN_TRAINING_POINTS_PER_CENTROID * ((int64_t)1 << pqBits);
    requiredTraining = std::max<int64_t>(
        std::max<int64_t>(ivfPqMinTrainingVectors, nlist), pqTrainingFloor);
  }
  if (ntotal < requiredTraining) {
    LOG_TRACE("VectorIndexBuilder: field {} has {} vectors, below {} training floor {}; "
              "using flat-over-column fallback",
              fieldName, ntotal, codecName, requiredTraining);
    return std::nullopt;
  }
  int64_t scanCost = ntotal * (int64_t)dims;
  if (scanCost < ivfBuildThresholdScanCost) {
    LOG_TRACE("VectorIndexBuilder: field {} scan cost {} below IVF build threshold {}; "
              "using flat-over-column fallback",
              fieldName, scanCost, ivfBuildThresholdScanCost);
    return std::nullopt;
  }
  if (!rabitq) pqM = choosePqM(ann.pqM, dims, metric);

  size_t bytesPerVec = (size_t)dims * sizeof(float);
  int64_t byteCap = bytesPerVec == 0 ? 0 : (int64_t)(ivfTrainingSampleBytes / bytesPerVec);
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

  // RaBitQ's error bound assumes the codes are taken in a randomly rotated
  // space; FAISS leaves the rotation to the caller ("It is assumed that the
  // Random Matrix Rotation is performed externally", RaBitQuantizer.h).  The
  // rotation is orthonormal, so L2, inner product and cosine (unit vectors
  // stay unit) are preserved and centroid distances stay comparable across
  // segments of either codec.
  std::unique_ptr<faiss::RandomRotationMatrix> rotation;
  auto quantizer = std::make_unique<faiss::IndexFlat>(dims, metric);
  std::unique_ptr<faiss::IndexIVF> index;
  if (rabitq) {
    rotation = std::make_unique<faiss::RandomRotationMatrix>(dims, dims);
    rotation->init(RABITQ_ROTATION_SEED);
    auto rq = std::make_unique<faiss::IndexIVFRaBitQ>(
        quantizer.get(), (size_t)dims, (size_t)nlist, metric,
        /*own_invlists=*/true, (uint8_t)std::clamp(ann.rabitqBits, 1, 9));
    rq->qb = (uint8_t)std::clamp(ann.rabitqQueryBits, 0, 8);
    index = std::move(rq);
  } else {
    auto pq = std::make_unique<faiss::IndexIVFPQ>(
        quantizer.get(), (size_t)dims, (size_t)nlist, (size_t)pqM,
        (size_t)pqBits, metric);
    // See min_points_per_centroid below.
    pq->pq.cp.min_points_per_centroid = 1;
    index = std::move(pq);
  }
  quantizer.release();
  index->own_fields = true;

  if (rotation != nullptr) {
    transformVectorChunks(training.data(), ntrain, dims, false, rotation.get(),
        ivfAddChunkBytes, [&](int64_t off, int64_t n, const float* x) {
      std::memcpy(training.data() + off * dims, x, (size_t)n * bytesPerVec);
    });
  }

  LOG_TRACE("VectorIndexBuilder: training {} {} ntotal={} ntrain={} dims={} "
            "nlist={} M={} pqBits={} rabitqBits={}",
            codecName, fieldName, ntotal, ntrain, dims, nlist, pqM, pqBits,
            rabitq ? ann.rabitqBits : 0);
  if (buildSite == BuildSite::MERGE) {
    ivfMergeBuildCountForTests.fetch_add(1, std::memory_order_relaxed);
  } else {
    ivfBuildCountForTests.fetch_add(1, std::memory_order_relaxed);
  }
  // Luxir gates IVF building on its own training-size policy (the
  // *MinTrainingVectors floors), so FAISS's separate under-training
  // heuristic is redundant.  It is the one message FAISS prints
  // unconditionally to stderr during train(); min_points_per_centroid only
  // controls that warning (not the clustering itself), so drop it to 1 on
  // the coarse quantizer (and the PQ, above) to keep that policy decision ours.
  index->cp.min_points_per_centroid = 1;
  index->train(ntrain, training.data());
  training.clear();
  training.shrink_to_fit();

  forEachVectorSegment(segments_, fieldName, dims,
      [&](const VectorIndexBuilder::SegInput&, VectorReader& vr) {
    if (vr.numVectors() > 0) {
      const float* base = (const float*)vr.vectorAtRank(0).data();
      transformVectorChunks(base, vr.numVectors(), dims, shouldNormalizeForFaiss(ft), rotation.get(),
          ivfAddChunkBytes, [&](int64_t, int64_t n, const float* x) {
        index->add(n, x);
      });
    }
    return true;
  });

  if (index->ntotal != ntotal) {
    throw std::runtime_error(fmt::format(
      "VectorIndexBuilder: {} ntotal mismatch for field {} (expected {}, built {})",
      codecName, fieldName, ntotal, index->ntotal));
  }

  std::string auxName(NAME_PREFIX);
  auxName.append(fieldName);
  // Per-segment overlay naming: production builds always run with exactly one
  // segment (the milestone-1 minimum slice).  A future group-spanning index
  // (n:m segment:index) would use index-level aux naming instead.
  assert(segments_.size() == 1);
  std::string faissFile = Postings::getSegmentOverlayFileName(
      segments_.front().segId, auxName, overlayGen_, 0);
  outFilesToSync.push_back(faissFile);

  FileDescriptor descriptor;
  {
    auto file = dir_.createFile(faissFile);
    OutputStream os;
    os.setFile(&*file);
    FaissOutAdapter adapter(os);
    int64_t faissHeaderBytes;
    {
      EmptyInvlistsSwap swap(*index);
      if (rotation != nullptr) {
        // Serialized as the standard FAISS wrapper (own_fields false: the
        // stack object borrows both); the reader takes it apart again.
        faiss::IndexPreTransform wrapped(rotation.get(), index.get());
        faiss::write_index(&wrapped, &adapter);
      } else {
        faiss::write_index(index.get(), &adapter);
      }
      faissHeaderBytes = (int64_t)os.size();
    }

    // List payloads in the layout VectorAuxListsFooter documents: sizes, ids,
    // codes, footer.  The ids section must land 8-aligned in the file so the
    // reader's in-memory view can be indexed as idx_t directly.
    os.align(8);
    const faiss::InvertedLists& lists = *index->invlists;
    for (size_t i = 0; i < lists.nlist; i++) {
      os.writeLong((int64_t)lists.list_size(i));
    }
    for (size_t i = 0; i < lists.nlist; i++) {
      size_t sz = lists.list_size(i);
      if (sz == 0) continue;
      faiss::InvertedLists::ScopedIds ids(&lists, i);
      os.write(ids.get(), sz * sizeof(faiss::idx_t));
    }
    for (size_t i = 0; i < lists.nlist; i++) {
      size_t sz = lists.list_size(i);
      if (sz == 0) continue;
      faiss::InvertedLists::ScopedCodes codes(&lists, i);
      os.write(codes.get(), sz * lists.code_size);
    }
    os.align(8);
    VectorAuxListsFooter footer;
    footer.faissHeaderBytes = faissHeaderBytes;
    footer.nlist = (int64_t)lists.nlist;
    footer.codeSize = (int64_t)lists.code_size;
    footer.ntotal = ntotal;
    footer.version = VectorAuxListsFooter::VERSION;
    footer.magic = VectorAuxListsFooter::MAGIC;
    os.write(&footer, sizeof(footer));
    os.close();
    dir_.finishFile(*file);
    descriptor = file->descriptor();
  }

  AuxInfo info;
  info.kind = std::string(KIND);
  info.field = std::string(fieldName);
  info.name = std::move(auxName);
  info.gen = overlayGen_;
  info.files.push_back(std::move(descriptor));
  {
    VectorAuxMeta meta;
    meta.dims = dims;
    meta.metric = (int32_t)ft.metric_;
    meta.cosineNormalizeColumnOnRescore = shouldNormalizeForFaiss(ft) ? 1 : 0;
    meta.engine = rabitq ? VectorAuxMeta::ENGINE_IVFRABITQ : VectorAuxMeta::ENGINE_IVFPQ;
    meta.nlist = nlist;
    meta.pqM = pqM;
    meta.pqBits = pqBits;
    meta.rabitqBits = rabitq ? std::clamp(ann.rabitqBits, 1, 9) : 0;
    std::string bytes = meta.toBytes();
    info.opaque_meta.assign((const std::byte*)bytes.data(),
                            (const std::byte*)bytes.data() + bytes.size());
  }

  LOG_TRACE("VectorIndexBuilder: built {} {} ntotal={} dims={} metric={} "
            "nlist={} codeSize={} file={}",
            codecName, info.name, index->ntotal, dims, (int)ft.metric_,
            nlist, index->code_size, faissFile);

  return info;
}

} // namespace luxir
