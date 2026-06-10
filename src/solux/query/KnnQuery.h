#pragma once

#include <faiss/Index.h>
#include <faiss/IndexIVF.h>
#include <faiss/MetricType.h>
#include <faiss/impl/IDSelector.h>
#include <faiss/utils/distances.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <format>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <boost/unordered/unordered_flat_set.hpp>

#include "Query.h"
#include "protos/solux_types.pb.h"
#include "solux/reader/AuxReader.h"
#include "solux/reader/PostingsReader.h"
#include "solux/reader/VectorAuxReader.h"
#include "solux/reader/VectorReader.h"
#include "solux/query/VectorEngine.h"
#include "solux/search/DocSet.h"
#include "solux/util/AtomicMerger.h"
#include "solux/util/heap.h"
#include "solux/util/log.h"
#include "solux/util/screaming.h"
#include "solux/util/thread.h"

namespace solux {

/// kNN query against a vector field.
///
/// Execution model: kNN is globally bounded, so search materializes once during
/// the query preparation phase (Weight::prepare), then scores replay from
/// per-segment hit arrays.  prepare() runs four stages:
///
///   1. Resolve per segment: field info, valueRank->docId resolver (SegV2D),
///      vector counts, and the "vec.<field>" overlay aux reader if present.
///   2. Build one engine slot per segment behind the VectorEngine seam:
///      SegmentFlatColumnVectorEngine (exact brute-force over the mmapped
///      column - the default and the below-IVF-threshold fallback) or
///      SegmentFaissVectorEngine (IVF+PQ aux, approximate).  All slots are
///      wrapped in one CompositeVectorEngine, which owns global IVF list
///      allocation across segments (see its class comment).
///   3. WIDEN: the host loop below re-enters engine->search() until enough
///      distinct live docs are pooled, deepening along two axes (candidate
///      depth, then IVF breadth).  All widening runs on engine scores; exact
///      rescore never influences the loop.  Each search() round scans its
///      (segment, list-range) work units on parallel TBB tasks when the
///      request is parallel (PrepareContext.parallel), merging through an
///      AtomicMerger accumulator; the round itself is a barrier, so the
///      deepen decisions stay sequential.
///   4. REFINE once: a single terminal pass rescores the pooled candidates
///      from the full-precision column (one ordered sparse pass per segment,
///      parallel across segments), collapses multi-valued hits to docs, and
///      cuts the global top-k.  Hits are then bucketed per segment in docId
///      order for the segment-parallel TopDocsReq pipeline.
///
/// Request knobs (see KnnQuery message in solux_types.proto for the wire
/// contract): k = docs to return; nprobe = merge-stable IVF effort (lists to
/// probe as if the field were ONE IVF index with nlist=sqrt(live vectors) -
/// explicit pins the effort, 0 adapts and auto-deepens); min_scan_fraction =
/// optional floor on the internal scan fraction; refine_candidates = pinned
/// approximate pool size; exact = bypass ANN entirely (column scan contract).
///
/// The active query domain (liveDocs intersected with any enclosing filter
/// clauses) is pushed into the engine.  The column engine checks it directly
/// while scanning docs; FAISS aux engines use a segment-local IDSelector.
///
/// Multi-valued vector fields are supported via the segment's persisted
/// valueRank->docId column (VectorReader::docForVectorRank).  The exact column
/// engine collapses a doc's vectors while scanning and returns one best vector
/// per doc.  Approximate aux engines return vector candidates instead; the
/// host pools them and the terminal refine collapses.  Above maxKnnCandidates
/// the result is best-effort.
class KnnQuery final : public solux::Query {
  std::string_view field;
  const VectorFieldType& fieldType;
  std::span<const float> queryVec;
  int32_t k;
  int32_t nprobe;
  int32_t refineCandidates;
  float minScanFraction;
  bool exact;

public:
  /// One result hit within a segment.  docId is the local docRank within the
  /// segment; score is converted to "higher is better" regardless of vector
  /// metric (see scoreFromDist).
  struct Hit {
    int32_t docId;
    float score;
  };

  // Maximum engine candidates retained while trying to fill k distinct docs.
  // Exact column flat returns doc-collapsed candidates; aux engines may return
  // vector candidates.  Mutable so tests can shrink it for cap coverage.
  static inline int64_t maxKnnCandidates = 1000000;

  // Maximum recall breadth a non-flat engine can request while deepening.
  // Flat ignores breadth; this is a host safety cap for future engines.
  static inline int32_t maxKnnBreadth = 1000000;

  // Parallel-scan task sizing.  An IVF segment's selected lists are chunked
  // into contiguous (segment, list-range) scan tasks of at least this many
  // live vectors, so one task amortizes its FAISS scanner setup and never
  // degenerates into a per-tiny-list task (a PQ list scan is only a few
  // microseconds at default sizing).  Per-list live sizes are already priced
  // by the allocator, so chunking is exact, not estimated.  Chunk boundaries
  // are identical in serial and parallel mode - the knob changes scheduling,
  // never results.  Mutable for tests.
  static inline int64_t scanTaskGrainVectors = 8192;

  // Terminal-rescore task sizing: a segment's candidate bucket runs as its
  // own task only at or above this many pooled candidates; smaller buckets
  // fold inline on the calling thread.  Mutable for tests.
  static inline int64_t rescoreTaskGrainCandidates = 256;

  // Approximate ANN engines over-fetch vector candidates before exact
  // full-precision rescore.  Default sizing is affine with a DECAYING ratio:
  //   kReq = count + k * max(REFINE_MIN_RATIO, log2(REFINE_UNITY_K / k))
  // (same shape as Solr's distributed-faceting overrequest, count +
  // ratio*limit, with the ratio shrinking as k grows).  Rationale: PQ
  // mis-ranking displaces a true neighbor by an absolute number of impostors
  // that does not scale with k, so small k needs the fixed headroom - while
  // large k is shotgun retrieval whose near-tied tail nobody can rank
  // meaningfully (and no benchmark measures recall that deep), so the
  // multiplier decays to REFINE_MIN_RATIO by k = REFINE_UNITY_K instead of
  // inflating the rescore pool and the engine's result heap.  An explicit
  // request refine_candidates pins the pool size directly (clamped up to k -
  // an absolute count like nprobe, not a multiplier).  defaultAnnRefineRatio:
  // 0 = the adaptive decay; > 0 pins a fixed default ratio (tests use this
  // to emulate a pure multiplier).  Mutable for tests / benches.
  static inline int32_t defaultAnnRefineCount = 128;
  static inline int32_t defaultAnnRefineRatio = 0;
  static constexpr double REFINE_UNITY_K = 1000.0;
  static constexpr double REFINE_MIN_RATIO = 1.1;

  // Test seam: when set, prepare() wraps the production flat engine and runs
  // the host deepen loop against the wrapper instead.  Lets tests drive the
  // loop with non-flat engine behaviors (breadth rounds, approximate scores)
  // that no production engine exhibits yet.  Always null in production.
  static inline std::function<std::unique_ptr<VectorEngine>(VectorEngine& flat, int64_t ntotal)>
      engineWrapperForTests;

  // Test observability: counts widen rounds that actually spawned parallel
  // scan tasks.  Lets tests assert that parallel mode propagates to nested
  // query positions (e.g. kNN inside a boolean clause), which is otherwise
  // invisible because results are identical either way.
  static inline std::atomic<int64_t> parallelScanRoundsForTests{0};

  /// Resolves a segment-local value rank to its owning docId.  Exactly one mode is
  /// active per segment:
  ///   - mono   : multi-valued reverse map (valueRank -> docId), read off the column.
  ///   - sel    : sparse single-valued (some docs lack the field) -> docId is the
  ///              valueRank-th set bit of the has-field bitset (select()).  No
  ///              per-vector array; the Selector caches only a small per-bucket
  ///              prefix and reuses the column's existing bitset.
  ///   - neither: dense single-valued, valueRank == docId (identity).
  /// The MonoReader / Selector must outlive every resolve() call (both live for the
  /// duration of the engine search + result walk inside Weight::prepare).
  class SegV2D {
  public:
    MonoReader* mono = nullptr;
    const screaming::BitSet::Selector* sel = nullptr;
    int32_t resolve(int64_t valueRank) const {
      if (mono) return (int32_t)mono->valueAt(valueRank);
      if (sel) return sel->select((int32_t)valueRank);
      return (int32_t)valueRank;
    }
  };

  struct DocHit {
    int32_t segOrd;
    int32_t docId;
    float score;
  };

  struct VectorKey {
    int64_t valueRank;
    int32_t segOrd;
  };

  struct VectorKeyHash {
    size_t operator()(const VectorKey& key) const noexcept {
      uint64_t x = (uint64_t)(uint32_t)key.segOrd;
      uint64_t y = (uint64_t)key.valueRank;
      y ^= y >> 33;
      y *= 0xff51afd7ed558ccdULL;
      y ^= y >> 33;
      y *= 0xc4ceb9fe1a85ec53ULL;
      y ^= y >> 33;
      return (size_t)(y ^ (x * 0x9e3779b97f4a7c15ULL));
    }
  };

  struct VectorKeyEqual {
    bool operator()(const VectorKey& a, const VectorKey& b) const noexcept {
      return a.segOrd == b.segOrd && a.valueRank == b.valueRank;
    }
  };

  /// Strict total order on engine hits: score desc, then (segOrd, valueRank)
  /// asc.  Being total makes every bounded top-N cut deterministic regardless
  /// of scan or merge order - the property the parallel scan accumulator
  /// relies on for parallel == serial results.  Totality requires non-NaN
  /// scores: the query vector is validated finite up front, and finiteScore
  /// maps any NaN arising from stored vectors to the worst finite score.
  static bool betterHit(const VectorEngineHit& a, const VectorEngineHit& b) noexcept {
    if (a.score != b.score) return a.score > b.score;
    if (a.segOrd != b.segOrd) return a.segOrd < b.segOrd;
    return a.valueRank < b.valueRank;
  }

  /// Strict total order on collapsed doc hits (same shape as betterHit).
  static bool betterDoc(const DocHit& a, const DocHit& b) noexcept {
    if (a.score != b.score) return a.score > b.score;
    if (a.segOrd != b.segOrd) return a.segOrd < b.segOrd;
    return a.docId < b.docId;
  }

  /// Offer an item to a bounded keep-the-best heap in a plain vector
  /// (std heap discipline with comp = better, so heap.front() is the WORST
  /// retained item).  Vector storage instead of std::priority_queue so
  /// merges and the final drain read the elements in place rather than
  /// destructively popping, and a full-heap accept is one replace-top sift
  /// (update_heap_top) instead of pop+push.
  template <typename T, typename Better>
  static void offerBounded(std::vector<T>& heap, const T& item, int64_t limit,
                           Better&& better) {
    if ((int64_t)heap.size() < limit) {
      heap.push_back(item);
      std::push_heap(heap.begin(), heap.end(), better);
      return;
    }
    if (!heap.empty() && better(item, heap.front())) {
      heap.front() = item;
      update_heap_top(heap.begin(), heap.end(), better);
    }
  }

  KnnQuery(std::string_view field, const VectorFieldType& fieldType,
           std::span<const float> queryVec, int32_t k,
           int32_t nprobe = 0, int32_t refineCandidates = 0,
           float minScanFraction = 0.0f, bool exact = false)
    : field(field), fieldType(fieldType), queryVec(queryVec), k(k),
      nprobe(nprobe), refineCandidates(refineCandidates),
      minScanFraction(minScanFraction), exact(exact) {}

  std::string_view getField() const noexcept { return field; }
  const VectorFieldType& getFieldType() const noexcept { return fieldType; }
  std::span<const float> getQueryVec() const noexcept { return queryVec; }
  int32_t getK() const noexcept { return k; }
  int32_t getNProbe() const noexcept { return nprobe; }
  int32_t getRefineCandidates() const noexcept { return refineCandidates; }
  float getMinScanFraction() const noexcept { return minScanFraction; }
  bool getExact() const noexcept { return exact; }

  Query::Weight* createWeight(Query::Context& context) override {
    return context.pool.make<KnnQuery::Weight>(context, *this);
  }

  class Weight final : public Query::Weight {
    KnnQuery& query;

  public:
    Weight(Query::Context& context, KnnQuery& query)
      : Query::Weight(context), query(query) {
      if (!query.getFieldType().knnSearchable()) {
        throw std::runtime_error(std::format(
          "KnnQuery on vector field '{}' without a metric", query.getField()));
      }
      if (query.getFieldType().dims() > 0 &&
          (int32_t)query.getQueryVec().size() != query.getFieldType().dims()) {
        throw std::runtime_error(std::format(
          "KnnQuery: query vector dims {} do not match schema dims {} for field '{}'",
          query.getQueryVec().size(), query.getFieldType().dims(), query.getField()));
      }
      // NaN scores would break the strict-total-order contract every bounded
      // candidate cut relies on (see betterHit); reject bad queries loudly
      // instead of ranking them arbitrarily.
      for (float v : query.getQueryVec()) {
        if (!std::isfinite(v)) {
          throw std::runtime_error(std::format(
            "KnnQuery: query vector for field '{}' must contain only finite values",
            query.getField()));
        }
      }
    }

    bool needsPrepare() const noexcept override { return true; }

    class KnnPreparedWeight final : public Query::Weight::PreparedWeight {
      std::vector<std::vector<Hit>> perSegHits;

    public:
      explicit KnnPreparedWeight(std::vector<std::vector<Hit>>&& perSegHits)
        : perSegHits(std::move(perSegHits)) {}

      Query::Scorer* createScorer(MemPool& target, IndexReader::Segment& segment) override {
        if ((size_t)segment.ord >= perSegHits.size()) return nullptr;
        auto& hits = perSegHits[(size_t)segment.ord];
        if (hits.empty()) return nullptr;
        return target.make<KnnQuery::Scorer>(
          std::span<const Hit>(hits.data(), hits.size()));
      }

      bool outputIsSubsetOfDomain() const noexcept override { return true; }
    };

  private:
    class SegmentFaissVectorEngine;

    struct EngineSlot {
      std::unique_ptr<VectorEngine> engine;
      int64_t vectorCount = 0;
      int64_t liveVectorCount = 0;
      SegmentFaissVectorEngine* ivfEngine = nullptr;
    };

  public:
    std::unique_ptr<Query::Weight::PreparedWeight> prepare(Query::Weight::PrepareContext& ctx) override {
      IndexReader& reader = ctx.reader;
      size_t numSegs = reader.segments().size();
      std::vector<std::vector<Hit>> perSegHits(numSegs);

      // prepare() runs in the parallel phase, possibly deep in a work-stealing
      // stack; use the thread-local pool (inline buffer in TLS, not on this
      // frame) instead of a stack-resident MemPool.  Per-thread, so still off
      // the shared request pool.  Sparse selectors / field-info live in this
      // pool and are consumed within prepare(), before the guard rewinds.
      auto poolGuard = MemPool::threadLocalPoolGuard();
      MemPool& scratch = poolGuard.pool();

      // Within a segment, FAISS ids are segment-local value ranks (0 ..
      // numVectors).  Each segment gets a SegV2D resolver mapping valueRank to
      // docId: identity for dense single-valued, a bitset select() lookup for
      // sparse single-valued, and the column's reverse map for multi-valued.
      std::vector<SegV2D> v2dPerSeg(numSegs);
      std::vector<int64_t> segVectorCounts(numSegs, 0);
      std::vector<int64_t> segLiveVectorCounts(numSegs, 0);
      std::vector<VectorAuxReader*> vauxPerSeg(numSegs, nullptr);
      std::string auxName("vec.");
      auxName.append(query.getField());
      if (!query.getExact()) {
        for (size_t i = 0; i < numSegs; i++) {
          auto aux = reader.segments()[i].getAuxReader(auxName);
          if (!aux) continue;
          auto* vaux = dynamic_cast<VectorAuxReader*>(aux.get());
          if (!vaux) {
            throw std::runtime_error(std::format(
              "KnnQuery: segment overlay '{}' is not a vector index (kind={})",
              auxName, aux->getKind()));
          }
          vauxPerSeg[i] = vaux;
        }
      }

      // Holds the multi-valued segments' valueRank->docId MonoReaders alive through
      // the engine search + result walk below.  MonoReader is
      // just pointers into the segment mmap (valid for the whole query), so copying it
      // out of the scratch VectorReader is safe and cheap.
      std::vector<std::optional<MonoReader>> monoHolders(numSegs);
      std::vector<SegFieldInfo> segInfoStorage(numSegs);
      std::vector<SegFieldInfo*> segInfos(numSegs, nullptr);
      for (size_t i = 0; i < numSegs; i++) {
        FieldReader fieldReader(scratch, reader.segments()[i].postingsReader());
        if (fieldReader.seek(query.getField())) {
          fieldReader.readFieldInfo(segInfoStorage[i]);
          segInfos[i] = &segInfoStorage[i];
        }
      }

      int64_t totalDocsWithValue = 0;
      bool anyMultiValued = false;
      int32_t dims = query.getFieldType().dims() > 0
        ? query.getFieldType().dims()
        : (int32_t)query.getQueryVec().size();
      if (dims <= 0) {
        throw std::runtime_error(std::format(
          "KnnQuery: query vector for field '{}' must have positive dims", query.getField()));
      }
      int32_t metric = (int32_t)query.getFieldType().metric();
      bool normalizeColumnOnCosineRescore =
        query.getFieldType().metric() == VectorFieldType::METRIC_COSINE
        && !query.getFieldType().normalized()
        && !query.getFieldType().normalizeOnWrite();
      for (size_t i = 0; i < numSegs; i++) {
        int64_t segCount = 0;
        if (segInfos[i] != nullptr) {
          // FIXED_SIZE vector column.  numVectors == #docs-with-field for
          // single-valued, or the total vector count for multi-valued.
          VectorReader vr(reader.segments()[i].postingsReader(), *segInfos[i]);
          segCount = vr.numVectors();
          if (segCount > 0) {
            if (vr.dims() != dims) {
              throw std::runtime_error(std::format(
                "KnnQuery: query vector dims {} do not match segment dims {} for field '{}'",
                dims, vr.dims(), query.getField()));
            }
            totalDocsWithValue += vr.docsWithValue();
            segVectorCounts[i] = segCount;
            if (vr.isMultiValued()) {
              anyMultiValued = true;
              // valueRank -> docId comes straight off the column's reverse map.
              MonoReader* mr = vr.strColReader().getValDocReader();
              if (mr == nullptr) {
                throw std::runtime_error(std::format(
                  "KnnQuery: multi-valued vector field '{}' is missing its valueRank->docId "
                  "map (segment predates the map); reindex required", query.getField()));
              }
              monoHolders[i].emplace(*mr);
              v2dPerSeg[i].mono = &*monoHolders[i];
            } else {
              // Single-valued: dense => identity (valueRank == docId).  Sparse (some
              // docs lack the field) => resolve via select() on the has-field bitset:
              // docId is the valueRank-th set bit.  No per-vector array; the Selector
              // caches only a small per-bucket prefix (nBuckets+1 ints) and reuses the
              // column's bitset.
              auto& dr = vr.strColReader().docsReader();
              if (dr.hasBitset()) {
                v2dPerSeg[i].sel = makeValueDocSelector(scratch, dr.bitset());
              }
            }
            segLiveVectorCounts[i] = liveVectorUpperBound(reader.segments()[i], vr);
          }
        }
      }
      int64_t ntotal = 0;
      for (int64_t n : segVectorCounts) ntotal += n;
      if (ntotal == 0) {
        return std::make_unique<KnnPreparedWeight>(std::move(perSegHits));
      }
      int64_t liveNTotal = 0;
      for (int64_t n : segLiveVectorCounts) liveNTotal += n;
      if (liveNTotal == 0) {
        return std::make_unique<KnnPreparedWeight>(std::move(perSegHits));
      }

      // Optionally normalize the query for COSINE - stored vectors were
      // normalized on write, or normalized by the builder for raw-column mode,
      // so we need a unit query for cosine = IP semantics to hold.  Done into a
      // local copy; the caller's span is unmodified.
      std::vector<float> queryBuf;
      const float* queryPtr = query.getQueryVec().data();
      if (metric == proto::VectorParams::COSINE) {
        queryBuf.assign(query.getQueryVec().begin(), query.getQueryVec().end());
        faiss::fvec_renorm_L2((size_t)dims, 1, queryBuf.data());
        queryPtr = queryBuf.data();
      }

      std::span<const SegV2D> v2dSpan(v2dPerSeg.data(), v2dPerSeg.size());

      int64_t kDocs = query.getK();
      int64_t cap = std::min(ntotal, std::max((int64_t)1, maxKnnCandidates));
      // exact is a contract: the maxKnnCandidates host heuristic must not
      // silently truncate the requested k (the explicit knob wins).  ntotal
      // still bounds it - fewer stored docs than k is not a violation, the
      // true top-k is simply all of them.
      if (query.getExact()) cap = std::min(ntotal, std::max(cap, kDocs));
      std::vector<EngineSlot> slots;
      slots.reserve(numSegs);
      bool approximateEngine = false;
      for (size_t i = 0; i < numSegs; i++) {
        int64_t segCount = segVectorCounts[i];
        if (segCount <= 0 || segInfos[i] == nullptr) continue;
        VectorAuxReader* vaux = vauxPerSeg[i];
        if (vaux != nullptr) {
          if (vaux->getDims() != dims) {
            throw std::runtime_error(std::format(
              "KnnQuery: query vector dims {} do not match segment index dims {} for field '{}'",
              dims, vaux->getDims(), query.getField()));
          }
          if (vaux->getMetric() != metric) {
            throw std::runtime_error(std::format(
              "KnnQuery: segment index metric {} does not match field metric {} for field '{}'",
              vaux->getMetric(), metric, query.getField()));
          }
          normalizeColumnOnCosineRescore =
            normalizeColumnOnCosineRescore || vaux->shouldNormalizeColumnOnCosineRescore();
          faiss::Index* idx = vaux->getFaissIndex();
          if (idx == nullptr || idx->ntotal == 0) continue;
          if (idx->ntotal != segCount) {
            throw std::runtime_error(std::format(
              "KnnQuery: segment FAISS ntotal={} != segment vector count={} for field '{}'",
              idx->ntotal, segCount, query.getField()));
          }
          bool isIvf = vaux->getEngine() == VectorAuxMeta::ENGINE_IVFPQ;
          approximateEngine = approximateEngine || !vaux->scoresAreExact();
          auto faissEngine = std::make_unique<SegmentFaissVectorEngine>(
            *idx, (int32_t)i, reader.segments()[i], v2dPerSeg[i],
            ctx.domainPerSeg.empty() ? nullptr : ctx.domainPerSeg[i],
            metric, vaux->scoresAreExact(), isIvf,
            vaux->getNList());
          SegmentFaissVectorEngine* ivfEngine = isIvf ? faissEngine.get() : nullptr;
          slots.push_back({
            .engine = std::move(faissEngine),
            .vectorCount = segCount,
            .liveVectorCount = segLiveVectorCounts[i],
            .ivfEngine = ivfEngine,
          });
        } else {
          slots.push_back({
            .engine = std::make_unique<SegmentFlatColumnVectorEngine>(
              (int32_t)i, reader.segments()[i], segInfos[i], v2dPerSeg[i],
              ctx.domainPerSeg.empty() ? nullptr : ctx.domainPerSeg[i],
              dims, metric, normalizeColumnOnCosineRescore),
            .vectorCount = segCount,
            .liveVectorCount = segLiveVectorCounts[i],
            .ivfEngine = nullptr,
          });
        }
      }
      if (slots.empty()) {
        return std::make_unique<KnnPreparedWeight>(std::move(perSegHits));
      }

      int64_t avgMult = (approximateEngine && anyMultiValued && totalDocsWithValue > 0)
        ? ceilDivClamped(ntotal, totalDocsWithValue, cap)
        : 1;
      int64_t baseReq = kDocs;
      if (approximateEngine) {
        // Explicit refine_candidates pins the candidate pool size (an
        // absolute count, clamped up to k); the absent-knob default is
        // affine with a k-decaying ratio (see defaultAnnRefineCount above).
        if (query.getRefineCandidates() > 0) {
          baseReq = std::min(cap, std::max(kDocs, (int64_t)query.getRefineCandidates()));
        } else {
          double ratio = defaultAnnRefineRatio > 0
            ? (double)defaultAnnRefineRatio
            : std::max(REFINE_MIN_RATIO, std::log2(REFINE_UNITY_K / (double)kDocs));
          baseReq = std::min(cap, (int64_t)defaultAnnRefineCount
                                    + (int64_t)((double)kDocs * ratio));
        }
        baseReq = std::max(baseReq, kDocs);
      }
      int64_t kReq = mulClamped(baseReq, avgMult, cap);
      if (kReq <= 0) return std::make_unique<KnnPreparedWeight>(std::move(perSegHits));
      int64_t targetDocReq = approximateEngine
        ? std::min<int64_t>(liveNTotal, std::max(kDocs, baseReq))
        : kDocs;

      std::vector<VectorEngineHit> candidateHits;
      boost::unordered_flat_set<VectorKey, VectorKeyHash, VectorKeyEqual> seenVectors;
      std::vector<DocHit> docHits;
      boost::unordered_flat_set<uint64_t> seenDocs;
      // Per-segment exactness of the pooled candidates: a segment is marked
      // once any round contributes a possibly-approximate hit from it; the
      // terminal rescore re-reads the column only for marked segments.
      std::vector<char> segApproxInPool(numSegs, 0);
      size_t maxDocHits = (size_t)std::min<int64_t>(targetDocReq, cap);
      size_t maxVectorHits = (size_t)cap;
      candidateHits.reserve((size_t)std::min(kReq, cap));
      seenVectors.reserve((size_t)std::min(kReq, cap));
      docHits.reserve(maxDocHits);
      seenDocs.reserve(maxDocHits);

      std::unique_ptr<VectorEngine> baseEngine =
        std::make_unique<CompositeVectorEngine>(std::move(slots), ntotal, liveNTotal, kDocs,
                                                metric, ctx.parallel);
      VectorEngine* engine = baseEngine.get();
      std::unique_ptr<VectorEngine> wrapperEngine;
      if (engineWrapperForTests) {
        wrapperEngine = engineWrapperForTests(*baseEngine, ntotal);
        engine = wrapperEngine.get();
      }

      // breadth carries the request nprobe in REFERENCE-index units (lists of
      // a hypothetical single nlist=sqrt(N) IVF index; the composite engine
      // converts it to a scan fraction).  Explicit nprobe pins the effort cap
      // (breadth == maxBreadth, no deepening past it); 0 starts at the
      // engine's adaptive default and may deepen up to maxKnnBreadth.
      int32_t breadth = query.getNProbe() > 0 ? query.getNProbe() : 0;
      int32_t maxBreadth = query.getNProbe() > 0
        ? query.getNProbe()
        : std::max(0, maxKnnBreadth);
      bool candidatePoolSorted = true;
      bool candidateScoresExact = true;
      // WIDEN loop: pool candidates until targetDocReq distinct live docs (or
      // a pool/breadth/cap limit).  Two deepen axes, tried in order:
      //   DEPTH   - the engine pool was not exhausted at this candidate count:
      //             grow kReq (more candidates from the same lists / scan).
      //   BREADTH - pool exhausted but more IVF lists remain: grow breadth
      //             (the engine allocates the next globally-best lists).
      // The whole loop runs on ENGINE scores (exact for column slots,
      // approximate for IVF+PQ): the stopping condition is a distinct-doc
      // COUNT, which approximate scores answer just as well, so exact rescore
      // is deferred to one terminal pass after the loop.  Deliberately not
      // overlapped with rescore: interleaving would re-read column vectors
      // for segments that later rounds add candidates to, trading throughput
      // under load for latency.  seenVectors dedups across rounds because
      // depth rounds re-return prior hits (FAISS has no resumable cursor) and
      // flat slots re-run on breadth rounds.
      for (;;) {
        VectorSearchRequest request{
          .query = queryPtr,
          .dims = dims,
          .candidates = kReq,
          .breadth = breadth,
          .minScanFraction = query.getMinScanFraction(),
        };
        VectorSearchResult result = engine->search(request);

        size_t appendStart = candidateHits.size();
        std::vector<char> roundExactSeg;
        if (!result.scoresAreExact && !result.exactSegOrds.empty()) {
          roundExactSeg.assign(numSegs, 0);
          for (int32_t s : result.exactSegOrds) {
            if (s >= 0 && (size_t)s < numSegs) roundExactSeg[(size_t)s] = 1;
          }
        }
        for (const auto& hit : result.hits) {
          if (candidateHits.size() >= maxVectorHits) break;
          VectorKey key{hit.segOrd, hit.valueRank};
          if (seenVectors.insert(key).second) {
            candidateHits.push_back(hit);
            if (!result.scoresAreExact
                && (roundExactSeg.empty() || !roundExactSeg[(size_t)hit.segOrd])) {
              segApproxInPool[(size_t)hit.segOrd] = 1;
            }
          }
        }
        bool appended = candidateHits.size() != appendStart;
        if (appended) {
          candidatePoolSorted = false;
          if (!result.scoresAreExact) candidateScoresExact = false;
        }

        if (!candidatePoolSorted) {
          sortCandidatesByScore(candidateHits);
          candidatePoolSorted = true;
        }
        collapseCandidates(candidateHits, v2dSpan, targetDocReq, docHits, seenDocs);

        if ((int64_t)docHits.size() >= targetDocReq) break;
        if (candidateHits.size() >= maxVectorHits) break;

        if (!result.poolExhausted && kReq < cap) {
          int64_t want = projectedKReq(kReq, targetDocReq, (int64_t)docHits.size(), cap);
          kReq = std::min(cap, std::max(kReq + 1, want));
          continue;
        }

        if (result.poolExhausted && !result.breadthExhausted && breadth < maxBreadth) {
          int32_t nextBreadth = result.nextBreadth > breadth ? result.nextBreadth : breadth + 1;
          breadth = std::min(nextBreadth, maxBreadth);
          continue;
        }

        break;
      }

      // REFINE: one terminal exact pass over the whole pooled candidate set.
      // Running it once (instead of per widen round) means each segment's
      // sorted sparse column read happens exactly once, and no incremental
      // "already rescored" bookkeeping is needed.  The full pool (not just
      // targetDocReq docs) feeds the rescore, so exact scores can promote any
      // pooled vector into the final top-k.  All-exact pools skip it; their
      // loop-time collapse used targetDocReq, so re-cut to kDocs if larger.
      if (!candidateScoresExact && !candidateHits.empty()) {
        rescoreAndCollapseCandidates(candidateHits,
                                     reader,
                                     std::span<SegFieldInfo* const>(segInfos.data(), segInfos.size()),
                                     v2dSpan,
                                     std::span<const char>(segApproxInPool.data(), segApproxInPool.size()),
                                     queryPtr, dims, metric, normalizeColumnOnCosineRescore,
                                     kDocs, docHits, ctx.parallel);
      } else if ((int64_t)docHits.size() > kDocs) {
        collapseCandidates(candidateHits, v2dSpan, kDocs, docHits, seenDocs);
      }

      if ((int64_t)docHits.size() < kDocs && cap < ntotal) {
        LOG_DEBUG("KnnQuery: {} of k={} docs for field '{}' at candidate cap {}",
                  docHits.size(), kDocs, query.getField(), cap);
      }

      // Materialize the per-segment hit arrays, sorted by docId.
      std::sort(docHits.begin(), docHits.end(), [](const DocHit& a, const DocHit& b) {
        return (a.segOrd == b.segOrd) ? (a.docId < b.docId) : (a.segOrd < b.segOrd);
      });
      for (size_t r = 0; r < docHits.size();) {
        int32_t ord = docHits[r].segOrd;
        size_t j = r + 1;
        while (j < docHits.size() && docHits[j].segOrd == ord) j++;
        auto& out = perSegHits[(size_t)ord];
        out.reserve(j - r);
        for (size_t i = r; i < j; i++) {
          out.push_back({docHits[i].docId, docHits[i].score});
        }
        r = j;
      }

      return std::make_unique<KnnPreparedWeight>(std::move(perSegHits));
    }

    Query::Scorer* createScorer(MemPool& target, IndexReader::Segment& segment) override {
      unused(target, segment);
      return nullptr;
    }

  private:
    /// Per-segment adapter over a deserialized FAISS aux index.  FAISS ids are
    /// segment-local valueRanks, mapped to docIds through SegV2D by the
    /// selector and by the host after collapse.
    ///
    /// IVF indexes do not drive themselves: the CompositeVectorEngine's global
    /// allocator calls coarseRun() (full sorted centroid ranking) and
    /// liveListSize() to pick lists across ALL segments, then hands the chosen
    /// lists back through searchSelectedLists().  The plain search() override
    /// is the whole-index path for non-IVF FAISS kinds; no such aux kind
    /// exists today (ENGINE_IVFPQ is the only one), so it is reserved seam
    /// surface, not a live code path.
    class SegmentFaissVectorEngine final : public VectorEngine {
      class LocalDomainSelector final : public faiss::IDSelector {
        int32_t segOrd;
        IndexReader::Segment& seg;
        const SegV2D& v2d;
        DocSet* domain;

      public:
        LocalDomainSelector(int32_t segOrd, IndexReader::Segment& seg,
                            const SegV2D& v2d, DocSet* domain) noexcept
          : segOrd(segOrd), seg(seg), v2d(v2d), domain(domain) {}

        bool is_member(faiss::idx_t id) const final {
          if (id < 0) return false;
          int32_t docId = v2d.resolve((int64_t)id);
          auto* live = seg.liveDocs();
          if (live != nullptr && !live->bitset().get(docId)) return false;
          if (domain != nullptr && !domain->get(docId)) return false;
          unused(segOrd);
          return true;
        }

        bool live_member(faiss::idx_t id) const {
          if (id < 0) return false;
          int32_t docId = v2d.resolve((int64_t)id);
          auto* live = seg.liveDocs();
          return live == nullptr || live->bitset().get(docId);
        }

        bool hasDeletes() const noexcept {
          return seg.liveDocs() != nullptr;
        }
      };

      faiss::Index& index;
      faiss::IndexIVF* ivf;
      int32_t segOrd;
      LocalDomainSelector selector;
      int32_t metric;
      bool exactScores;
      bool isIvf;
      int32_t nlist;

    public:
      SegmentFaissVectorEngine(faiss::Index& index,
                               int32_t segOrd,
                               IndexReader::Segment& seg,
                               const SegV2D& v2d,
                               DocSet* domain,
                               int32_t metric,
                               bool exactScores,
                               bool isIvf,
                               int32_t nlist)
        : index(index),
          ivf(isIvf ? dynamic_cast<faiss::IndexIVF*>(&index) : nullptr),
          segOrd(segOrd),
          selector(segOrd, seg, v2d, domain),
          metric(metric),
          exactScores(exactScores),
          isIvf(isIvf),
          nlist(nlist) {
        if (isIvf && ivf == nullptr) {
          throw std::runtime_error("KnnQuery: vector aux marked IVF but FAISS index is not IndexIVF");
        }
      }

      int32_t getNList() const noexcept { return nlist; }

      void coarseRun(const float* query, int32_t dims,
                     std::vector<faiss::idx_t>& listIds,
                     std::vector<float>& centroidDists) const {
        assert(ivf != nullptr);
        assert(query != nullptr);
        assert(dims == index.d);
        unused(dims);
        listIds.assign((size_t)nlist, (faiss::idx_t)-1);
        centroidDists.assign((size_t)nlist, 0.0f);
        ivf->quantizer->search(1, query, nlist, centroidDists.data(), listIds.data());
      }

      /// Live vector count of one IVF list - the unit the global allocator
      /// charges against the scan-fraction budget (cost = liveListSize / N).
      /// Uses the ACTUAL invlist length so list imbalance prices correctly
      /// (full selection always sums to ~N).  Under deletes this walks the
      /// list's ids once to count live members - acceptable because it is
      /// only called for lists the allocator actually selects (or peeks for
      /// nextBreadth), never all of them; liveness only (not the query
      /// domain), matching the live-N denominator.
      int64_t liveListSize(faiss::idx_t listId) const {
        assert(ivf != nullptr);
        if (listId < 0 || listId >= nlist) return 0;
        size_t rawSize = ivf->invlists->list_size((size_t)listId);
        if (rawSize == 0) return 0;
        if (!selector.hasDeletes()) return (int64_t)rawSize;

        int64_t liveSize = 0;
        const faiss::idx_t* listIds = ivf->invlists->get_ids((size_t)listId);
        for (size_t i = 0; i < rawSize; i++) {
          if (selector.live_member(listIds[i])) liveSize++;
        }
        ivf->invlists->release_ids((size_t)listId, listIds);
        return liveSize;
      }

      /// Scan a contiguous run of pre-selected IVF lists.  const and
      /// thread-safe: the composite engine runs several disjoint list-ranges
      /// of one segment as concurrent tasks, so all scratch is local and the
      /// shared state (index, selector, v2d) is read-only.  rangeLiveVectors
      /// is the live count of exactly the lists in listIds.
      VectorSearchResult searchSelectedLists(const VectorSearchRequest& request,
                                             std::span<const faiss::idx_t> listIds,
                                             std::span<const float> centroidDists,
                                             int64_t rangeLiveVectors) const {
        assert(ivf != nullptr);
        assert(request.query != nullptr);
        assert(request.dims == index.d);

        VectorSearchResult result;
        result.scoresAreExact = exactScores;
        result.breadthExhausted = true;
        if (request.candidates <= 0 || listIds.empty()) {
          result.poolExhausted = true;
          return result;
        }

        std::vector<faiss::idx_t> ids((size_t)request.candidates, (faiss::idx_t)-1);
        std::vector<float> dists((size_t)request.candidates, 0.0f);

        faiss::SearchParametersIVF ivfParams;
        // FAISS declares sel as a mutable pointer but only calls const
        // members on it during search.
        ivfParams.sel = const_cast<LocalDomainSelector*>(&selector);
        // max_codes remains 0: selected IVF lists are scanned in full.
        ivfParams.nprobe = listIds.size();
        // Local stats sink, discarded: with a null stats argument FAISS does
        // plain non-atomic '+=' on the global faiss::indexIVF_stats, and
        // concurrent chunk tasks of one query would race on it.
        faiss::IndexIVFStats stats;
        ivf->search_preassigned(1, request.query, request.candidates,
                                listIds.data(), centroidDists.data(),
                                dists.data(), ids.data(), false, &ivfParams,
                                &stats);

        result.hits.reserve((size_t)request.candidates);
        int64_t liveHits = 0;
        for (int64_t i = 0; i < request.candidates; i++) {
          faiss::idx_t fid = ids[(size_t)i];
          if (fid < 0) continue;
          liveHits++;
          result.hits.push_back({
            .score = scoreFromDist(dists[(size_t)i], metric),
            .segOrd = segOrd,
            .valueRank = (int64_t)fid,
          });
        }

        // Drained if FAISS could not fill the heap (fewer selector-passing
        // members than requested) or if every live vector in the scanned
        // lists already fit.  Range-local semantics; the composite's
        // accumulator derives the ROUND's poolExhausted from per-chunk
        // counters (foldPart), so this flag only describes this list range.
        result.poolExhausted = liveHits < request.candidates
          || rangeLiveVectors <= request.candidates;
        return result;
      }

      VectorSearchResult search(const VectorSearchRequest& request) override {
        // Flat (non-IVF) FAISS path only.  IVF aux indexes are always routed
        // through CompositeVectorEngine::searchSelectedLists() after global list
        // allocation, so they never reach this method - there is no per-segment
        // IVF breadth fallback here (that would be a second, divergent deepen
        // policy competing with the host allocator).
        assert(!isIvf);
        assert(request.query != nullptr);
        assert(request.dims == index.d);
        if (request.candidates <= 0) {
          VectorSearchResult empty;
          empty.scoresAreExact = exactScores;
          empty.poolExhausted = true;
          empty.breadthExhausted = true;
          return empty;
        }

        std::vector<faiss::idx_t> ids((size_t)request.candidates, (faiss::idx_t)-1);
        std::vector<float> dists((size_t)request.candidates, 0.0f);

        faiss::SearchParameters flatParams;
        flatParams.sel = &selector;
        index.search(1, request.query, request.candidates, dists.data(), ids.data(), &flatParams);

        VectorSearchResult result;
        result.hits.reserve((size_t)request.candidates);
        int64_t liveHits = 0;
        for (int64_t i = 0; i < request.candidates; i++) {
          faiss::idx_t fid = ids[(size_t)i];
          if (fid < 0) continue;  // FAISS pad - no more live results.
          liveHits++;
          result.hits.push_back({
            .score = scoreFromDist(dists[(size_t)i], metric),
            .segOrd = segOrd,
            .valueRank = (int64_t)fid,
          });
        }

        result.scoresAreExact = exactScores;
        result.poolExhausted = liveHits < request.candidates || request.candidates >= index.ntotal;
        result.breadthExhausted = true;
        return result;
      }
    };

    class SegmentFlatColumnVectorEngine final : public VectorEngine {
      int32_t segOrd;
      IndexReader::Segment& seg;
      SegFieldInfo* segInfo;
      const SegV2D& v2d;
      DocSet* domain;
      int32_t dims;
      int32_t metric;
      bool normalizeColumnOnCosineRescore;

    public:
      SegmentFlatColumnVectorEngine(int32_t segOrd,
                                    IndexReader::Segment& seg,
                                    SegFieldInfo* segInfo,
                                    const SegV2D& v2d,
                                    DocSet* domain,
                                    int32_t dims,
                                    int32_t metric,
                                    bool normalizeColumnOnCosineRescore) noexcept
        : segOrd(segOrd),
          seg(seg),
          segInfo(segInfo),
          v2d(v2d),
          domain(domain),
          dims(dims),
          metric(metric),
          normalizeColumnOnCosineRescore(normalizeColumnOnCosineRescore) {}

      VectorSearchResult search(const VectorSearchRequest& request) override {
        assert(request.query != nullptr);
        assert(request.dims == dims);

        VectorSearchResult result;
        result.scoresAreExact = true;
        result.breadthExhausted = true;
        if (request.candidates <= 0) {
          result.poolExhausted = true;
          return result;
        }

        std::vector<VectorEngineHit> heap;
        int64_t eligibleDocs = 0;
        if (segInfo == nullptr) {
          result.poolExhausted = true;
          return result;
        }
        VectorReader vr(seg.postingsReader(), *segInfo);
        int64_t numVals = vr.numVectors();
        if (numVals == 0) {
          result.poolExhausted = true;
          return result;
        }
        if (vr.dims() != dims) {
          throw std::runtime_error(std::format(
            "KnnQuery: query vector dims {} do not match segment dims {}",
            dims, vr.dims()));
        }

        // Inline run-collapse: value ranks are doc-ordered (the valDoc map is
        // monotonic; sparse select and dense identity likewise), so a doc's
        // vectors form a contiguous run and max-sim collapse falls out of one
        // pass.  liveDocs/domain are applied as a per-run predicate.
        if (v2d.mono != nullptr) {
          MonoReader::BulkValues valDoc(*v2d.mono);
          int64_t idx = valDoc.next();
          int64_t rank = 0;
          while (rank < numVals) {
            int32_t docId = (int32_t)valDoc.value();
            int64_t end = rank;
            do {
              end++;
              idx = valDoc.next();
            } while (end < numVals && (int32_t)valDoc.value() == docId);
            unused(idx);

            if (docEligible(docId)) {
              eligibleDocs++;
              auto [bestRank, bestScore] = bestInRun(rank, end, [&](int64_t r) {
                return exactScore(request.query, vr.vectorAtRank(r), dims, metric,
                                  normalizeColumnOnCosineRescore, 0.0f);
              });
              offerBounded(heap,
                           VectorEngineHit{
                             .score = bestScore,
                             .segOrd = segOrd,
                             .valueRank = bestRank,
                           },
                           request.candidates, betterHit);
            }
            rank = end;
          }
        } else {
          // Single-valued (dense identity or sparse select): runs are length
          // 1 by construction.
          for (int64_t rank = 0; rank < numVals; rank++) {
            int32_t docId = v2d.resolve(rank);
            if (!docEligible(docId)) continue;
            eligibleDocs++;
            offerBounded(heap,
                         VectorEngineHit{
                           .score = exactScore(request.query, vr.vectorAtRank(rank), dims, metric,
                                               normalizeColumnOnCosineRescore, 0.0f),
                           .segOrd = segOrd,
                           .valueRank = rank,
                         },
                         request.candidates, betterHit);
          }
        }

        result.hits = std::move(heap);
        std::sort(result.hits.begin(), result.hits.end(), betterHit);
        result.poolExhausted = eligibleDocs <= request.candidates;
        return result;
      }

    private:
      bool docEligible(int32_t docId) const {
        auto* live = seg.liveDocs();
        if (live != nullptr && !live->bitset().get(docId)) return false;
        if (domain != nullptr && !domain->get(docId)) return false;
        return true;
      }
    };

    /// One VectorEngine over all per-segment slots.  Owns the cross-segment
    /// policy so individual segments never self-allocate effort:
    ///
    ///   - Budget: request.breadth (nprobe in reference-index units) becomes
    ///     a scan fraction P = breadth / defaultIvfNList(liveN), floored by
    ///     min_scan_fraction.  Denominating effort in fraction-of-index makes
    ///     it MERGE-STABLE: a raw per-segment list count would scan ~sqrt(2)x
    ///     more before a merge than after, so a tuned query would degrade on
    ///     the next merge through no data change.
    ///   - Allocation: every IVF segment contributes its full centroid-sorted
    ///     list ranking; a lazy k-way merge pops the globally best list and
    ///     charges its live size / liveN until P is spent.  Small segments
    ///     thus get small (or zero) shares instead of equal work.
    ///   - Deepening: the merge heap and per-slot cursors persist across
    ///     search() calls; a breadth round just keeps popping where the last
    ///     round stopped.  Flat (column / non-IVF) slots ignore breadth.
    ///   - Scheduling: each round's work is a flat worklist of (segment,
    ///     list-range) scan units - IVF tails chunked by live-vector cost to
    ///     scanTaskGrainVectors, whole slots for non-IVF engines - run on TBB
    ///     tasks when the request is parallel and folded through an
    ///     AtomicMerger accumulator.  The unit is deliberately NOT the
    ///     segment: affinity-driven allocation concentrates budget in one
    ///     segment, so per-segment tasks would serialize exactly the hot
    ///     case.  Chunk boundaries are computed identically in serial and
    ///     parallel mode, so results are bit-identical either way.
    ///
    /// SINGLE-SHOT lifecycle: coarseReady, cursors, watermarks and
    /// lastScannedCandidates mutate monotonically across search() rounds of
    /// ONE query.  prepare() builds a fresh instance per query; reusing one
    /// across queries would silently serve the old query's coarse ranking.
    class CompositeVectorEngine final : public VectorEngine {
      std::vector<EngineSlot> slots;
      int64_t totalVectors;
      int64_t liveTotalVectors;
      int64_t kDocs;
      int32_t metric;
      bool parallel;

      struct CoarseRun {
        std::vector<faiss::idx_t> listIds;
        std::vector<float> centroidDists;
      };

      struct CoarseHead {
        float score = 0.0f;
        int32_t slotOrd = -1;
        faiss::idx_t listId = -1;
      };

      struct CoarseHeadLess {
        bool operator()(const CoarseHead& a, const CoarseHead& b) const noexcept {
          if (a.score != b.score) return a.score < b.score;
          if (a.slotOrd != b.slotOrd) return a.slotOrd > b.slotOrd;
          return a.listId > b.listId;
        }
      };

      // Coarse-selection state, persisted across deepen rounds (this IS the
      // resumable breadth state).  Per slot: runs holds the full sorted
      // (listId, centroidDist) ranking from coarseRun(); cursors[i] is the
      // k-way-merge position into it.  heads/heapIndexes/heapSize back the
      // IndexedPQ of each slot's current head, keyed by centroid score -
      // updateTop() (replace-top) advances a slot in one sift, the same heap
      // discipline the doc collectors use.  selected* accumulate the
      // allocation in global pop order; selectedCentroidDists and
      // selectedListLiveSizes stay 1:1 with selectedListIds because
      // search_preassigned consumes the first pair positionally and the
      // scan-task chunker prices ranges off the third.
      std::vector<CoarseRun> runs;
      std::vector<size_t> cursors;
      std::vector<CoarseHead> heads;
      std::vector<int32_t> heapIndexes;
      int32_t heapSize = 0;
      std::vector<std::vector<faiss::idx_t>> selectedListIds;
      std::vector<std::vector<float>> selectedCentroidDists;
      std::vector<std::vector<int64_t>> selectedListLiveSizes;
      // Per-slot watermarks: how many selected lists have already been
      // scanned into the host pool, so a pure-breadth deepen round scans only
      // the freshly allocated tail instead of re-scanning every list.
      // lastScannedCandidates detects a depth (candidate-count) change, which
      // DOES force a full re-scan (FAISS has no resumable list cursor).
      std::vector<size_t> scannedListCount;
      int64_t lastScannedCandidates = -1;
      double allocatedCost = 0.0;
      int32_t referenceBreadth = 1;
      bool coarseReady = false;
      bool hasIvfSlots = false;

      // Candidate count for a FLAT (exact) slot: its proportional share of the
      // request, floored at kDocs so a small segment can still supply all k
      // docs by itself.  Deliberately smaller than the IVF slots' full kReq:
      // the over-fetch headroom in kReq exists so approximate scores can be
      // reordered by the exact rescore, and flat scores are already exact -
      // a flat slot's top-kDocs are final, no reorder headroom needed.
      int64_t candidatesForSlot(int64_t requested, int64_t n) const {
        if (requested <= 0 || n <= 0) return 0;
        long double proportional = ((long double)requested * (long double)n)
          / (long double)std::max<int64_t>(1, totalVectors);
        int64_t bySize = (int64_t)std::ceil(proportional);
        int64_t floor = std::min(kDocs, n);
        return std::min(n, std::max(bySize, floor));
      }

      CoarseHead makeHead(int32_t slotOrd) const {
        const auto& run = runs[(size_t)slotOrd];
        size_t cursor = cursors[(size_t)slotOrd];
        faiss::idx_t listId = run.listIds[cursor];
        float dist = run.centroidDists[cursor];
        return {
          .score = scoreFromDist(dist, metric),
          .slotOrd = slotOrd,
          .listId = listId,
        };
      }

      void ensureCoarseReady(const VectorSearchRequest& request) {
        if (coarseReady) return;
        coarseReady = true;
        referenceBreadth = defaultIvfNList(liveTotalVectors);
        runs.resize(slots.size());
        cursors.assign(slots.size(), 0);
        heads.resize(slots.size());
        selectedListIds.resize(slots.size());
        selectedCentroidDists.resize(slots.size());
        selectedListLiveSizes.resize(slots.size());
        scannedListCount.assign(slots.size(), 0);
        heapIndexes.clear();
        heapIndexes.reserve(slots.size());

        for (size_t i = 0; i < slots.size(); i++) {
          auto* ivf = slots[i].ivfEngine;
          if (ivf == nullptr || slots[i].liveVectorCount <= 0 || ivf->getNList() <= 0) continue;
          ivf->coarseRun(request.query, request.dims, runs[i].listIds, runs[i].centroidDists);
          while (cursors[i] < runs[i].listIds.size() && runs[i].listIds[cursors[i]] < 0) {
            cursors[i]++;
          }
          if (cursors[i] >= runs[i].listIds.size()) continue;
          heads[i] = makeHead((int32_t)i);
          heapIndexes.push_back((int32_t)i);
          hasIvfSlots = true;
        }
        heapSize = (int32_t)heapIndexes.size();
      }

      double defaultBudget() const {
        // nprobe=0 default: probe as many lists as a SINGLE IVF index of
        // nlist=referenceBreadth would by its own default, i.e. sqrt(nlist).  As
        // a scan fraction that is sqrt(referenceBreadth) / referenceBreadth =
        // 1 / sqrt(referenceBreadth) (= N^-0.25 for referenceBreadth=sqrt(N)).
        // This depends only on the total live vector count, so it is
        // MERGE-STABLE: repartitioning the same vectors into more or fewer
        // segments does not move it.  Host auto-deepening grows breadth from
        // this lean start when too few distinct live docs come back.
        //
        // A per-segment sum was rejected: sum_s sqrt(N_s) / sqrt(N) =
        // sqrt(numSegments) >= 1, which clamps to a full scan; and even summing
        // per-segment defaults (sum_s sqrt(nlist_s) / sqrt(N) = S^0.75 * N^-0.25)
        // keeps an S-dependent term that drifts on merges.
        return 1.0 / std::sqrt((double)std::max(1, referenceBreadth));
      }

      double budgetForRequest(const VectorSearchRequest& request) const {
        double p = request.breadth > 0
          ? (double)request.breadth / (double)std::max(1, referenceBreadth)
          : defaultBudget();
        p = std::max(p, (double)request.minScanFraction);
        return std::clamp(p, 0.0, 1.0);
      }

      int32_t breadthForBudget(double p) const {
        if (p <= 0.0) return 1;
        double breadth = std::ceil(p * (double)std::max(1, referenceBreadth));
        if (breadth >= (double)std::numeric_limits<int32_t>::max()) {
          return std::numeric_limits<int32_t>::max();
        }
        return std::max(1, (int32_t)breadth);
      }

      // Extend the global allocation until the requested scan fraction is
      // covered.  Lazy k-way merge over all slots' centroid-sorted runs: pop
      // the globally closest list, charge its live size / liveN, advance that
      // slot's cursor (replace-top, one sift), repeat.  Resumable: cursors
      // and allocatedCost persist, so a deepen round allocates only the
      // increment.  The epsilon absorbs float accumulation when a fraction
      // boundary lands exactly on a list edge.
      void allocateToBudget(const VectorSearchRequest& request) {
        ensureCoarseReady(request);
        if (!hasIvfSlots || heapSize <= 0) return;

        double target = budgetForRequest(request);
        if (allocatedCost + 1.0e-15 >= target) return;

        IndexedPQ<CoarseHead, CoarseHeadLess, int32_t> pq(
          std::span<CoarseHead>(heads.data(), heads.size()),
          std::span<int32_t>(heapIndexes.data(), heapIndexes.size()),
          heapSize);
        while (pq.size() > 0 && allocatedCost + 1.0e-15 < target) {
          int32_t slotOrd = pq.indexOfTop();
          auto& head = pq.top();
          faiss::idx_t listId = head.listId;
          int64_t liveSize = slots[(size_t)slotOrd].ivfEngine->liveListSize(listId);

          selectedListIds[(size_t)slotOrd].push_back(listId);
          selectedCentroidDists[(size_t)slotOrd].push_back(
            runs[(size_t)slotOrd].centroidDists[cursors[(size_t)slotOrd]]);
          selectedListLiveSizes[(size_t)slotOrd].push_back(liveSize);
          if (liveTotalVectors > 0) {
            allocatedCost += (double)liveSize / (double)liveTotalVectors;
          }

          cursors[(size_t)slotOrd]++;
          while (cursors[(size_t)slotOrd] < runs[(size_t)slotOrd].listIds.size()
                 && runs[(size_t)slotOrd].listIds[cursors[(size_t)slotOrd]] < 0) {
            cursors[(size_t)slotOrd]++;
          }
          if (cursors[(size_t)slotOrd] < runs[(size_t)slotOrd].listIds.size()) {
            head = makeHead(slotOrd);
            pq.updateTop();
          } else {
            pq.removeTopIndex();
          }
        }
        heapSize = (int32_t)pq.size();
      }

      // Hint for the host's next breadth round: the breadth value (in
      // reference-index units) whose budget covers the allocation so far PLUS
      // the next globally-best unallocated list.  Peeked off the persisted
      // heap without popping; minStep and the +1 floor guarantee the hint
      // makes progress even for tiny or empty lists.
      int32_t nextBreadth(const VectorSearchRequest& request) {
        if (!hasIvfSlots || heapSize <= 0) return 0;
        IndexedPQ<CoarseHead, CoarseHeadLess, int32_t> pq(
          std::span<CoarseHead>(heads.data(), heads.size()),
          std::span<int32_t>(heapIndexes.data(), heapIndexes.size()),
          heapSize);
        int32_t slotOrd = pq.indexOfTop();
        int64_t liveSize = slots[(size_t)slotOrd].ivfEngine->liveListSize(pq.top().listId);
        double nextCost = liveTotalVectors > 0
          ? (double)liveSize / (double)liveTotalVectors
          : 0.0;
        double minStep = 1.0 / (double)std::max(1, referenceBreadth);
        int32_t next = breadthForBudget(std::min(1.0, allocatedCost + std::max(nextCost, minStep)));
        return std::max(next, request.breadth + 1);
      }

      /// One scan task: a contiguous range of one IVF slot's selected lists
      /// (chunked to at least scanTaskGrainVectors live vectors), or a whole
      /// non-IVF slot (listBegin == listEnd == 0).
      struct ScanUnit {
        int32_t slotOrd;
        size_t listBegin;
        size_t listEnd;
        int64_t live;
        int64_t candidates;
        bool ivfChunk;
      };

      /// One round's scan-merge state.  Each task folds its sub-result here
      /// via AtomicMerger obtain/release, so live accumulators are bounded by
      /// actual concurrency and a serial round degenerates to one
      /// accumulator, one pass.  Approximate hits pass through a bounded
      /// top-(request.candidates) heap - the same global cut a single
      /// shard-level IVF index would apply (recall-equal to the uncut union:
      /// for disjoint lists, top-k of the union is contained in the union of
      /// per-range top-k).  Exact-claimed hits BYPASS the cut: an exact score
      /// evicted by an inflated approximate score could never be restored by
      /// the terminal rescore, breaking the mixed-composition ground-truth
      /// contract.
      struct ScanAccumulator : MergeableData {
        int64_t heapLimit;
        // Bounded keep-best heap over approximate hits (vector-backed, see
        // offerBounded) so merge() and the final drain read it in place.
        std::vector<VectorEngineHit> approxHeap;
        std::vector<VectorEngineHit> exactHits;
        // Pre-cut count of approximate hits offered to the heap; together
        // with partsExhausted it decides the round's poolExhausted.
        int64_t approxReturned = 0;
        bool anyApprox = false;
        bool partsExhausted = true;

        explicit ScanAccumulator(int64_t heapLimit) : heapLimit(heapLimit) {}

        static ScanAccumulator* merge(ScanAccumulator* a, ScanAccumulator* b) {
          if (a->approxHeap.size() < b->approxHeap.size()) std::swap(a, b);
          for (const auto& hit : b->approxHeap) {
            offerBounded(a->approxHeap, hit, a->heapLimit, betterHit);
          }
          a->exactHits.insert(a->exactHits.end(), b->exactHits.begin(), b->exactHits.end());
          a->approxReturned += b->approxReturned;
          a->anyApprox = a->anyApprox || b->anyApprox;
          a->partsExhausted = a->partsExhausted && b->partsExhausted;
          return a;
        }
      };

      VectorSearchResult scanUnit(const ScanUnit& unit, const VectorSearchRequest& request) const {
        const auto& slot = slots[(size_t)unit.slotOrd];
        VectorSearchRequest subReq = request;
        subReq.candidates = unit.candidates;
        if (!unit.ivfChunk) {
          return slot.engine->search(subReq);
        }
        // search_preassigned requires centroid distances aligned 1:1 with
        // list ids; the arrays are only coupled by adjacent push_backs in
        // allocateToBudget.
        assert(selectedListIds[(size_t)unit.slotOrd].size()
               == selectedCentroidDists[(size_t)unit.slotOrd].size());
        return slot.ivfEngine->searchSelectedLists(
          subReq,
          std::span<const faiss::idx_t>(
            selectedListIds[(size_t)unit.slotOrd].data() + unit.listBegin,
            unit.listEnd - unit.listBegin),
          std::span<const float>(
            selectedCentroidDists[(size_t)unit.slotOrd].data() + unit.listBegin,
            unit.listEnd - unit.listBegin),
          unit.live);
      }

      /// Hits route by result-claimed exactness (exactness is a property of
      /// RESULTS, not engine construction): exact-claimed hits bypass the
      /// bounded cut, approximate hits go through it.  Exhaustion is one
      /// identity for every unit shape: AND of each part's own poolExhausted.
      /// A list-range chunk's flag (searchSelectedLists) is false exactly
      /// when it filled its heap with live vectors to spare - the per-chunk
      /// "more depth could help" signal; hits dropped by the MERGED cut are
      /// accounted separately via approxReturned in search().
      static void foldPart(ScanAccumulator& acc, const VectorSearchResult& part) {
        if (part.scoresAreExact) {
          acc.exactHits.insert(acc.exactHits.end(), part.hits.begin(), part.hits.end());
        } else {
          acc.anyApprox = true;
          acc.approxReturned += (int64_t)part.hits.size();
          for (const auto& hit : part.hits) {
            offerBounded(acc.approxHeap, hit, acc.heapLimit, betterHit);
          }
        }
        acc.partsExhausted = acc.partsExhausted && part.poolExhausted;
      }

    public:
      CompositeVectorEngine(std::vector<EngineSlot>&& slots, int64_t totalVectors,
                            int64_t liveTotalVectors, int64_t kDocs, int32_t metric,
                            bool parallel) noexcept
        : slots(std::move(slots)),
          totalVectors(totalVectors),
          liveTotalVectors(liveTotalVectors),
          kDocs(kDocs),
          metric(metric),
          parallel(parallel) {}

      VectorSearchResult search(const VectorSearchRequest& request) override {
        VectorSearchResult merged;
        merged.scoresAreExact = true;
        merged.poolExhausted = true;
        merged.breadthExhausted = true;
        if (request.candidates <= 0) return merged;

        allocateToBudget(request);

        // The host re-enters search() to deepen along one of two axes per round:
        //   DEPTH  - request.candidates grew: need more candidates from the SAME
        //            lists.  FAISS has no resumable list cursor, so re-scan all
        //            selected lists at the new depth.
        //   BREADTH - allocateToBudget appended new lists at the same candidate
        //            depth: only the freshly allocated tail needs scanning, since
        //            prior lists' hits are already unioned into the host pool.
        // Scanning only the new tail on a breadth round turns the old O(rounds x
        // lists) IVF re-scan into O(lists) total.  A breadth round is only entered
        // after a poolExhausted=true round (host loop), so every prior list is
        // already drained at this candidate depth - omitting them from this
        // round's exhaustion accounting is correct.  Flat (below-threshold)
        // engines are breadth-independent and small, so they are simply re-run;
        // the host dedups their unchanged hits via seenVectors, and re-running
        // keeps search() a pure function of its arguments for the flat-only
        // path (test oracles rely on that; the IVF tail-scan is incremental
        // only because the host unions).
        //
        // RECALL SAFETY: IVF lists are disjoint, and for disjoint sets A, B
        // the global top-k of (A union B) is contained in top-k(A) union
        // top-k(B).  So the host's union of per-round top-k subsets is a
        // SUPERSET of what one global pass over all selected lists would
        // return - PROVIDED every list was scanned at the final candidate
        // count, which is exactly what the depthGrew full re-scan maintains.
        // Making the DEPTH axis incremental would break that invariant and
        // silently lose recall, beyond merely fighting the cursorless API.
        // The same lemma is why per-chunk scanning below loses nothing: every
        // chunk scans at the full round candidate count.
        bool depthGrew = request.candidates != lastScannedCandidates;
        lastScannedCandidates = request.candidates;

        // Build the round's flat worklist: IVF tails chunked by live-vector
        // cost (the allocator already priced every selected list), whole
        // slots for non-IVF engines.  An under-grain remainder folds into the
        // previous chunk rather than becoming a trivial task.  Boundaries do
        // not depend on the execution mode.
        std::vector<ScanUnit> units;
        for (size_t i = 0; i < slots.size(); i++) {
          auto& slot = slots[i];
          if (slot.ivfEngine != nullptr) {
            size_t total = selectedListIds[i].size();
            size_t start = depthGrew ? 0 : scannedListCount[i];
            scannedListCount[i] = total;
            if (start >= total) continue;
            int64_t candidates = std::min(request.candidates, slot.vectorCount);
            if (candidates <= 0) continue;
            // Per-chunk candidates are capped by the chunk's live count: a
            // chunk can never return more hits than it has live vectors, and
            // the uncapped slot-wide count would size every chunk's FAISS
            // heap and ids/dists scratch at the full request depth -
            // multiplied by the number of concurrent chunks on large-k
            // queries.
            int64_t grain = std::max<int64_t>(1, scanTaskGrainVectors);
            size_t firstUnit = units.size();
            size_t chunkBegin = start;
            int64_t chunkLive = 0;
            for (size_t l = start; l < total; l++) {
              chunkLive += selectedListLiveSizes[i][l];
              if (chunkLive >= grain) {
                units.push_back({(int32_t)i, chunkBegin, l + 1, chunkLive,
                                 std::min(candidates, chunkLive), true});
                chunkBegin = l + 1;
                chunkLive = 0;
              }
            }
            if (chunkBegin < total) {
              if (units.size() > firstUnit) {
                units.back().listEnd = total;
                units.back().live += chunkLive;
                units.back().candidates = std::min(candidates, units.back().live);
              } else {
                units.push_back({(int32_t)i, chunkBegin, total, chunkLive,
                                 std::min(candidates, chunkLive), true});
              }
            }
          } else {
            int64_t candidates = candidatesForSlot(request.candidates, slot.vectorCount);
            if (candidates <= 0) continue;
            units.push_back({(int32_t)i, 0, 0, slot.liveVectorCount, candidates, false});
          }
        }

        if (!units.empty()) {
          AtomicMerger<ScanAccumulator> merger(
            [limit = request.candidates]() { return new ScanAccumulator(limit); },
            [](ScanAccumulator* acc) { delete acc; });
          // Each search() round is a barrier: the host's deepen decision
          // needs the whole merged pool.  TaskGroupRunner runs every unit
          // inline when not spawning (serial requests take the same code
          // path and produce the same result), and its destructor owns the
          // unwind: if an inline unit throws, it cancels and drains the
          // spawned tasks without double-throwing.
          bool spawnTasks = parallel && units.size() > 1;
          if (spawnTasks) {
            parallelScanRoundsForTests.fetch_add(1, std::memory_order_relaxed);
          }
          TaskGroupRunner runner;
          for (const auto& unit : units) {
            runner.run(spawnTasks, [this, &unit, &request, &merger]() {
              VectorSearchResult part = scanUnit(unit, request);
              // unique_ptr guards the obtained accumulator: a throw between
              // obtain and release would otherwise orphan it (the merger
              // only frees what is parked in its slot).
              std::unique_ptr<ScanAccumulator> acc(merger.obtain());
              foldPart(*acc, part);
              merger.release(acc.release());
            });
          }
          runner.join();

          ScanAccumulator* acc = merger.getData();
          merged.scoresAreExact = !acc->anyApprox;
          // "More depth on the same lists cannot add hits": every part
          // reported itself drained AND the merged approximate cut dropped
          // nothing.  The second term can hold poolExhausted=false when all
          // parts drained individually but their union exceeded the cut; the
          // host then takes depth rounds (full re-scans) to pull the cut
          // hits through the bounded heap before breadth can advance - a
          // bounded latency cost that is semantically required, since only
          // a deeper round can surface those hits.
          merged.poolExhausted = acc->partsExhausted
            && acc->approxReturned <= request.candidates;
          // Sub-results are per-segment, so exactness IS segment-attributable
          // here: report the exact parts' segments (derived from the hits
          // themselves) so the host's terminal rescore (run when this merge
          // is approximate overall) can keep their scores instead of
          // re-reading their columns.
          if (!merged.scoresAreExact && !acc->exactHits.empty()) {
            std::vector<int32_t> segs;
            segs.reserve(acc->exactHits.size());
            for (const auto& hit : acc->exactHits) segs.push_back(hit.segOrd);
            std::sort(segs.begin(), segs.end());
            segs.erase(std::unique(segs.begin(), segs.end()), segs.end());
            merged.exactSegOrds = std::move(segs);
          }
          merged.hits = std::move(acc->approxHeap);
          merged.hits.insert(merged.hits.end(),
                             acc->exactHits.begin(), acc->exactHits.end());
          // betterHit is a strict total order, so a plain sort is
          // deterministic no matter what order tasks folded in.
          std::sort(merged.hits.begin(), merged.hits.end(), betterHit);
        }

        if (hasIvfSlots) {
          merged.breadthExhausted = heapSize <= 0;
          if (!merged.breadthExhausted) {
            merged.nextBreadth = nextBreadth(request);
          }
        }
        return merged;
      }
    };
  };

  class Scorer final : public Query::Scorer {
    std::span<const Hit> hits;
    int32_t cur = -1;

  public:
    explicit Scorer(std::span<const Hit> hits) noexcept : hits(hits) {}

    int32_t next() override {
      cur++;
      return cur < (int32_t)hits.size() ? hits[cur].docId : PostingsReader::END;
    }

    int32_t advance(int32_t docid) override {
      if (cur >= 0 && cur < (int32_t)hits.size() && hits[cur].docId >= docid) {
        return hits[cur].docId;
      }
      int32_t doc;
      while ((doc = next()) < docid) {}
      return doc;
    }

    bool advanceExact(int32_t docid) override {
      return advance(docid) == docid;
    }

    int32_t docId() override {
      if (cur < 0) return -1;
      return cur < (int32_t)hits.size() ? hits[cur].docId : PostingsReader::END;
    }

    float score() override {
      assert(cur >= 0 && cur < (int32_t)hits.size());
      return hits[cur].score;
    }
  };

private:
  static constexpr double MIN_COSINE_NORM_SQ = 1.0e-30;

  /// O(1) UPPER BOUND on a segment's live vector count, used as the budget
  /// denominator (referenceBreadth, list-cost normalization) and the
  /// targetDocReq cap.  An exact count under deletes required walking the
  /// field (every rank for sparse single-valued, the whole column iterator
  /// for multi-valued) on EVERY query - O(N_seg) bookkeeping to budget a
  /// search that examines ~N^0.75 vectors at the default.  An upper bound is
  /// the safe direction to relax: overestimating only makes the scan
  /// fraction leaner and targetDocReq higher, and host deepening compensates
  /// (the same stance as cost accounting ignoring the domain filter).
  /// UNDERestimating is not safe: targetDocReq = min(liveTotal, ...) would
  /// stop the widen loop before K achievable docs.  Exact for no-deletes and
  /// for dense single-valued (identity mapping => live count == numLive).
  static int64_t liveVectorUpperBound(IndexReader::Segment& segment,
                                      VectorReader& vr) {
    LiveDocs* live = segment.liveDocs();
    if (live == nullptr) return vr.numVectors();
    if (!vr.isMultiValued()) {
      // Each live doc holds at most one vector.
      return std::min<int64_t>(vr.numVectors(), live->numLive());
    }
    return vr.numVectors();
  }

  /// Max-sim collapse of one doc's run of doc-ordered vector positions: scores
  /// each position in [begin, end) via scoreAt and returns (bestPos, bestScore).
  /// Ties keep the lowest position, so the dense engine scan (positions are
  /// valueRanks) and the sparse host rescore (positions are candidate indexes)
  /// rank a doc's equal-scoring vectors identically.
  template <typename ScoreAt>
  static std::pair<int64_t, float> bestInRun(int64_t begin, int64_t end, ScoreAt&& scoreAt) {
    assert(begin < end);
    int64_t bestPos = begin;
    float bestScore = scoreAt(begin);
    for (int64_t pos = begin + 1; pos < end; pos++) {
      float score = scoreAt(pos);
      if (score > bestScore) {
        bestScore = score;
        bestPos = pos;
      }
    }
    return {bestPos, bestScore};
  }

  /// Terminal-rescore merge state: per-segment bucket tasks offer their
  /// doc-collapsed exact hits into a bounded top-kDocs heap.  betterDoc is a
  /// strict total order, so the retained set is independent of task order.
  struct RescoreAccumulator : MergeableData {
    int64_t limit;
    std::vector<DocHit> heap;

    explicit RescoreAccumulator(int64_t limit) : limit(limit) {}

    static RescoreAccumulator* merge(RescoreAccumulator* a, RescoreAccumulator* b) {
      if (a->heap.size() < b->heap.size()) std::swap(a, b);
      for (const auto& hit : b->heap) {
        offerBounded(a->heap, hit, a->limit, betterDoc);
      }
      return a;
    }
  };

  /// Terminal refine: collapse the pooled candidates to the top kDocs docs,
  /// exact-rescoring approximate segments' vectors from the column.
  /// Candidates are sorted by (segOrd, valueRank) first so the column is read
  /// in ONE ordered sparse pass per segment (sequential mmap access instead
  /// of score-order random access), and because valueRank order groups a
  /// doc's vectors contiguously, the multi-valued max-sim collapse (bestInRun,
  /// shared with the flat column engine's full scan) happens inline during
  /// that same pass.  Collapsing before the score sort is legal precisely
  /// because the surviving scores are exact - the engine's approximate
  /// ranking no longer matters.  Candidates from segments NOT marked in
  /// segApprox already carry exact scores: they keep them (no column read)
  /// but still run-collapse, since a flat FAISS aux returns per-vector hits.
  /// An empty segApprox rescores everything.
  ///
  /// Each segment's bucket is an independent column pass, so buckets run as
  /// parallel tasks (runs never span segments).  Parallelism is bounded by
  /// the segments the pool landed in - few, under affinity - which is
  /// acceptable because rescore is cheap relative to the scan; buckets under
  /// rescoreTaskGrainCandidates fold inline on the calling thread.
  static void rescoreAndCollapseCandidates(std::vector<VectorEngineHit>& candidates,
                                           IndexReader& reader,
                                           std::span<SegFieldInfo* const> segInfos,
                                           std::span<const SegV2D> v2dPerSeg,
                                           std::span<const char> segApprox,
                                           const float* queryPtr,
                                           int32_t dims,
                                           int32_t metric,
                                           bool normalizeColumnOnCosineRescore,
                                           int64_t kDocs,
                                           std::vector<DocHit>& docHits,
                                           bool parallel) {
    docHits.clear();
    if (candidates.empty() || kDocs <= 0) return;

    std::sort(candidates.begin(), candidates.end(),
        [](const VectorEngineHit& a, const VectorEngineHit& b) {
          if (a.segOrd != b.segOrd) return a.segOrd < b.segOrd;
          return a.valueRank < b.valueRank;
        });

    AtomicMerger<RescoreAccumulator> merger(
      [kDocs]() { return new RescoreAccumulator(kDocs); },
      [](RescoreAccumulator* acc) { delete acc; });

    auto rescoreBucket = [&](size_t bucketBegin, size_t bucketEnd) {
      int32_t segOrd = candidates[bucketBegin].segOrd;
      if (segOrd < 0 || (size_t)segOrd >= segInfos.size()) {
        throw std::runtime_error("KnnQuery: vector candidate segment ordinal out of range");
      }
      SegFieldInfo* info = segInfos[(size_t)segOrd];
      if (info == nullptr) {
        throw std::runtime_error("KnnQuery: vector candidate segment has no field info");
      }
      const SegV2D& v2d = v2dPerSeg[(size_t)segOrd];
      bool keepEngineScores = !segApprox.empty() && !segApprox[(size_t)segOrd];
      std::optional<VectorReader> vr;
      if (!keepEngineScores) {
        vr.emplace(reader.segments()[(size_t)segOrd].postingsReader(), *info);
      }

      // unique_ptr guards the obtained accumulator against a throw before
      // release (same idiom as StatsOp / FacetOp).
      std::unique_ptr<RescoreAccumulator> acc(merger.obtain());
      for (size_t i = bucketBegin; i < bucketEnd;) {
        int32_t docId = v2d.resolve(candidates[i].valueRank);
        size_t runEnd = i + 1;
        while (runEnd < bucketEnd && v2d.resolve(candidates[runEnd].valueRank) == docId) {
          runEnd++;
        }

        float bestScore;
        if (keepEngineScores) {
          bestScore = bestInRun((int64_t)i, (int64_t)runEnd, [&](int64_t c) {
            return candidates[(size_t)c].score;
          }).second;
        } else {
          bestScore = bestInRun((int64_t)i, (int64_t)runEnd, [&](int64_t c) {
            return exactScore(queryPtr, vr->vectorAtRank(candidates[(size_t)c].valueRank),
                              dims, metric, normalizeColumnOnCosineRescore,
                              candidates[(size_t)c].score);
          }).second;
        }
        offerBounded(acc->heap, DocHit{segOrd, docId, bestScore}, kDocs, betterDoc);
        i = runEnd;
      }
      merger.release(acc.release());
    };

    // TaskGroupRunner owns the unwind path: if an inline (under-grain)
    // bucket throws while spawned buckets are in flight, its destructor
    // cancels and drains them instead of letting ~task_group double-throw.
    TaskGroupRunner runner;
    int64_t grain = std::max<int64_t>(1, rescoreTaskGrainCandidates);
    for (size_t b = 0; b < candidates.size();) {
      int32_t segOrd = candidates[b].segOrd;
      size_t e = b + 1;
      while (e < candidates.size() && candidates[e].segOrd == segOrd) e++;
      // A bucket spawns only above the grain, and never when it is the whole
      // pool (a single bucket gains nothing from a worker hand-off).
      bool spawn = parallel
        && (int64_t)(e - b) >= grain
        && !(b == 0 && e == candidates.size());
      runner.run(spawn, [&rescoreBucket, b, e]() { rescoreBucket(b, e); });
      b = e;
    }
    runner.join();

    RescoreAccumulator* acc = merger.getData();
    docHits = std::move(acc->heap);
    std::sort(docHits.begin(), docHits.end(), betterDoc);
  }

  // The query vector is validated finite at Weight construction, but stored
  // vectors are not, so a distance can still come back NaN (a stored NaN, or
  // inf - inf accumulation).  NaN scores would break the strict-total-order
  // contract (betterHit/betterDoc) that makes the bounded cuts deterministic
  // and std::sort defined, so map NaN to a worst-rank score at the point
  // where scores are produced.  One ULP above lowest(): TopScoreCollector's
  // minCompetitiveVal starts at lowest() and admits only strictly greater
  // scores, and a kNN hit must rank last, not be counted in matches yet
  // vanish from the returned docs.
  static float finiteScore(float score) {
    if (!std::isnan(score)) [[likely]] return score;
    return std::nextafter(std::numeric_limits<float>::lowest(), 0.0f);
  }

  // Keep L2/IP conversions in sync with scoreFromDist below.  COSINE has a
  // separate stored-vector norm branch for raw-column rescore mode.
  static float exactScore(const float* queryPtr,
                          std::span<const float> vec,
                          int32_t dims,
                          int32_t metric,
                          bool normalizeColumnOnCosineRescore,
                          float fallbackScore) {
    assert(queryPtr != nullptr);
    assert((int32_t)vec.size() == dims);
    switch (metric) {
      case proto::VectorParams::L2: {
        float dist = faiss::fvec_L2sqr(queryPtr, vec.data(), (size_t)dims);
        return finiteScore(1.0f / (1.0f + dist));
      }
      case proto::VectorParams::IP:
        return finiteScore(faiss::fvec_inner_product(queryPtr, vec.data(), (size_t)dims));
      case proto::VectorParams::COSINE: {
        float sum = faiss::fvec_inner_product(queryPtr, vec.data(), (size_t)dims);
        if (!normalizeColumnOnCosineRescore) return finiteScore(sum);

        // Raw-column cosine pays the vector norm at query time.  This is the
        // explicit tradeoff for preserving the originally submitted bytes when
        // normalize_on_write=false; the default cosine path stores unit vectors.
        float normSq = faiss::fvec_norm_L2sqr(vec.data(), (size_t)dims);
        if (!std::isfinite(normSq) || normSq <= MIN_COSINE_NORM_SQ) return 0.0f;
        return finiteScore(sum / std::sqrt(normSq));
      }
      default:
        return fallbackScore;
    }
  }

  static void sortCandidatesByScore(std::vector<VectorEngineHit>& candidates) {
    std::stable_sort(candidates.begin(), candidates.end(),
        [](const VectorEngineHit& a, const VectorEngineHit& b) {
          return a.score > b.score;
        });
  }

  static void collapseCandidates(const std::vector<VectorEngineHit>& candidates,
                                 std::span<const SegV2D> v2dPerSeg,
                                 int64_t kDocs,
                                 std::vector<DocHit>& docHits,
                                 boost::unordered_flat_set<uint64_t>& seenDocs) {
    docHits.clear();
    seenDocs.clear();
    for (const auto& hit : candidates) {
      int32_t docId = v2dPerSeg[(size_t)hit.segOrd].resolve(hit.valueRank);
      uint64_t key = packSegDoc(hit.segOrd, docId);
      if (seenDocs.insert(key).second) {
        docHits.push_back({hit.segOrd, docId, hit.score});
        if ((int64_t)docHits.size() == kDocs) break;
      }
    }
  }

  // Convert FAISS's per-metric distance to a "higher is better" score.
  //   L2:    FAISS returns squared distance - invert to 1/(1+d).
  //   IP:    FAISS returns dot product - already higher-is-better.
  //   COSINE: stored as IP over normalized vectors (write path or builder),
  //           query is normalized at search time - same as IP.
  // NaN distances map to the worst score (see finiteScore).
  static float scoreFromDist(float dist, int32_t metric) {
    switch (metric) {
      case proto::VectorParams::L2:
        return finiteScore(1.0f / (1.0f + dist));
      case proto::VectorParams::IP:
      case proto::VectorParams::COSINE:
      default:
        return finiteScore(dist);
    }
  }

  static uint64_t packSegDoc(int32_t segOrd, int32_t docId) {
    return ((uint64_t)(uint32_t)segOrd << 32) | (uint32_t)docId;
  }

  static int64_t ceilDivClamped(int64_t num, int64_t den, int64_t cap) {
    if (den <= 0) return cap;
    if (num <= 0) return 0;
    int64_t q = num / den;
    if (num % den != 0) q++;
    return std::min(q, cap);
  }

  static int64_t mulClamped(int64_t a, int64_t b, int64_t cap) {
    if (a <= 0 || b <= 0) return 0;
    if (a > cap / b) return cap;
    return std::min(a * b, cap);
  }

  static int64_t projectedKReq(int64_t kReq, int64_t kDocs, int64_t docsSeen, int64_t cap) {
    int64_t denom = std::max((int64_t)1, docsSeen);
    // Heuristic projection: kReq * (target docs / observed docs) plus slack.
    double want = std::ceil((double)kReq * (double)kDocs / (double)denom * 1.3);
    if (want >= (double)cap) return cap;
    return (int64_t)want;
  }

};

} // namespace solux
