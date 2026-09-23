// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

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

constexpr int64_t MIN_TRAINING_POINTS_PER_PQ_CENTROID = 39;

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

int32_t chooseIvfNList(int64_t ntotal) {
  if (VectorIndexBuilder::ivfPqNList > 0) {
    return (int32_t)std::min<int64_t>(VectorIndexBuilder::ivfPqNList, ntotal);
  }
  return defaultIvfNList(ntotal);
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
    FieldReader fr(pr);
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
bool VectorIndexBuilder::buildFaissIvfPqAuxIndexes = true;
int32_t VectorIndexBuilder::ivfPqNList = 0;
int32_t VectorIndexBuilder::ivfPqM = 0;
int32_t VectorIndexBuilder::ivfPqBits = 8;
int32_t VectorIndexBuilder::ivfPqDefaultNProbe = 0;
int64_t VectorIndexBuilder::ivfPqMinTrainingVectors =
    MIN_TRAINING_POINTS_PER_PQ_CENTROID * 256;
int64_t VectorIndexBuilder::ivfPqBuildThresholdScanCost = 1'000'000;
std::atomic<int64_t> VectorIndexBuilder::ivfPqBuildCountForTests{0};
std::atomic<int64_t> VectorIndexBuilder::ivfPqMergeBuildCountForTests{0};
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
  if (!buildFaissIvfPqAuxIndexes) {
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
    auto info = buildField(fieldName, *vft, outFilesToSync, buildSite);
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
VectorIndexBuilder::buildField(std::string_view fieldName,
                               const VectorFieldType& ft,
                               std::vector<std::string>& outFilesToSync,
                               BuildSite buildSite) {
  // Test hook: a listener may throw (or block) to exercise vector
  // build-failure paths.  Args: the field name (std::string_view*) and the
  // build site, so a listener can target a specific field and/or site.
  Signal::emit("vectorBuildField", (void*)&fieldName, (void*)(intptr_t)buildSite);
  return buildIvfPqField(fieldName, ft, outFilesToSync, buildSite);
}

std::optional<AuxInfo>
VectorIndexBuilder::buildIvfPqField(std::string_view fieldName,
                                    const VectorFieldType& ft,
                                    std::vector<std::string>& outFilesToSync,
                                    BuildSite buildSite) {
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
  int64_t scanCost = ntotal * (int64_t)dims;
  if (scanCost < ivfPqBuildThresholdScanCost) {
    LOG_TRACE("VectorIndexBuilder: field {} scan cost {} below IVF+PQ build threshold {}; "
              "using flat-over-column fallback",
              fieldName, scanCost, ivfPqBuildThresholdScanCost);
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
  if (buildSite == BuildSite::MERGE) {
    ivfPqMergeBuildCountForTests.fetch_add(1, std::memory_order_relaxed);
  } else {
    ivfPqBuildCountForTests.fetch_add(1, std::memory_order_relaxed);
  }
  // Luxir gates IVF/PQ building on its own training-size policy
  // (ivfPqMinTrainingVectors), so FAISS's separate under-training heuristic is
  // redundant.  It is the one message FAISS prints unconditionally to stderr
  // during train(); min_points_per_centroid only controls that warning (not the
  // clustering itself), so drop it to 1 on both the coarse quantizer and the PQ
  // to keep that policy decision ours.
  index->cp.min_points_per_centroid = 1;
  index->pq.cp.min_points_per_centroid = 1;
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
      faiss::write_index(index.get(), &adapter);
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
    std::string meta = makeVectorMeta(
        dims, ft, VectorAuxMeta::ENGINE_IVFPQ, nlist, nprobe, pqM, pqBits);
    info.opaque_meta.assign((const std::byte*)meta.data(),
                            (const std::byte*)meta.data() + meta.size());
  }

  LOG_TRACE("VectorIndexBuilder: built IVF+PQ {} ntotal={} dims={} metric={} "
            "nlist={} M={} bits={} file={}",
            info.name, index->ntotal, dims, (int)ft.metric_,
            nlist, pqM, pqBits, faissFile);

  return info;
}

} // namespace luxir
