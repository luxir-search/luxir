#pragma once

#include <faiss/Index.h>
#include <faiss/IndexIVF.h>
#include <faiss/MetricType.h>
#include <faiss/impl/IDSelector.h>
#include <faiss/utils/distances.h>

#include <algorithm>
#include <atomic>
#include <bit>
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
#include "solux/api/solux_types.hpp"
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
/// The active query domain is pushed into the engine as the COMPLETE
/// eligibility predicate: per the PrepareContext contract it is already
/// live-filtered (liveDocs intersected with any enclosing filter clauses),
/// and a null domain means no deletes and no filters.  The column engine
/// checks it directly while scanning docs.  FAISS aux engines pick the
/// cheapest selector per segment: none at all when the domain is null (the
/// scanner's per-vector check compiles out), a cached rank-space liveness
/// bitmap when the domain is exactly liveDocs (built once per liveDocs
/// generation, shared across queries), or a resolve-based IDSelector for
/// per-query filtered domains.
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

  // Test observability: rank-space liveness bitmap builds (cache misses).
  // Lets tests assert the per-liveGen cache reuses across queries and
  // rebuilds when new deletes commit.
  static inline std::atomic<int64_t> rankLiveBitmapBuildsForTests{0};
  static inline std::atomic<int64_t> seenFoldsForTests{0};
  // Post-fold hits that were already pooled.  The eligible contract makes
  // this impossible for conforming engines; nonzero means selector wiring
  // broke and deepen rounds silently regressed to re-returning the pool.
  static inline std::atomic<int64_t> staleHitsForTests{0};

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

  /// Dedup key for a (segOrd, valueRank) hit: segOrd in the top 20 bits,
  /// valueRank in the low 44 (1M segments, 17.6T vectors per segment).
  /// Debug-assert ONLY - no other code enforces these bounds, they are
  /// physics-scale (2^44 ranks implies a tens-of-TB segment file).  A wrap
  /// would silently alias keys and drop distinct hits from the pool.
  static constexpr int PACK_RANK_BITS = 44;
  static uint64_t packSegRank(int32_t segOrd, int64_t valueRank) {
    assert(segOrd >= 0 && segOrd < (1 << (64 - PACK_RANK_BITS)));
    assert(valueRank >= 0 && valueRank < ((int64_t)1 << PACK_RANK_BITS));
    return ((uint64_t)(uint32_t)segOrd << PACK_RANK_BITS) | (uint64_t)valueRank;
  }

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

  Query::Weight* createWeight(Query::Context& context, int32_t flags,
                              float multiplier = 1.0f) override {
    return context.pool.make<KnnQuery::Weight>(context, *this, flags, multiplier);
  }

  class Weight final : public Query::Weight {
    KnnQuery& query;
    float boost;

  public:
    Weight(Query::Context& context, KnnQuery& query, int32_t flags, float multiplier)
      : Query::Weight(context, flags), query(query),
        boost(constantWhenScored(flags, multiplier)) {
      traits |= NEEDS_PREPARE;  // index-level ANN pass
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
      std::vector<std::shared_ptr<const RankLiveBitmap>> rankLivePerSeg(numSegs);
      std::vector<SegFieldInfo> segInfoStorage(numSegs);
      std::vector<SegFieldInfo*> segInfos(numSegs, nullptr);
      for (size_t i = 0; i < numSegs; i++) {
        FieldReader fieldReader(reader.segments()[i].postingsReader());
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
            IndexReader::Segment& seg = reader.segments()[i];
            if (vauxPerSeg[i] != nullptr && seg.liveDocs() != nullptr) {
              // Rank-space liveness bitmap, cached on the aux reader per
              // liveDocs generation; fetched here so a (rare) rebuild runs
              // while the scratch column readers it walks are alive.
              // liveListSize accounting always reads it, for an unfiltered
              // query (domain == the liveDocs docset) it doubles as the
              // whole FAISS eligibility selector, and it carries this
              // segment's EXACT live vector count.
              rankLivePerSeg[i] = vauxPerSeg[i]->rankLiveBitmap(
                seg.segInfo.live_gen, [&]() {
                  return buildRankLiveBitmap(seg, vr, segCount);
                });
              segLiveVectorCounts[i] = rankLivePerSeg[i]->liveVectors;
            } else {
              segLiveVectorCounts[i] = liveVectorUpperBound(seg, vr);
            }
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
      if (metric == (int32_t)solux::api::VectorMetric::COSINE) {
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
        IndexReader::Segment& seg = reader.segments()[i];
        DocSet* domain = ctx.domainPerSeg.empty() ? nullptr : ctx.domainPerSeg[i];
        // Domain contract (Query.h): a present domain is live-filtered and
        // is the complete eligibility predicate; null means no deletes and
        // no filters.  The engines rely on it - they never re-check
        // liveDocs.
        assert(domain != nullptr || seg.liveDocs() == nullptr);
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
          // rankLivePerSeg[i] is shared with the engine, NOT moved: the widen
          // loop's foldSeen() seeds the eligible bitmaps from it.
          auto faissEngine = std::make_unique<SegmentFaissVectorEngine>(
            *idx, (int32_t)i, seg, v2dPerSeg[i], domain,
            rankLivePerSeg[i],
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
              (int32_t)i, seg, segInfos[i], v2dPerSeg[i], domain,
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
      boost::unordered_flat_set<uint64_t> seenVectors;
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

      // Cross-round dedup runs in two phases.  Round 1 (the overwhelmingly
      // common only round) uses the small seenVectors hash set and passes no
      // eligible bitmaps.  The FIRST deepen of either axis folds the seen set
      // into per-segment rank-space "eligible" bitmaps (init: the segment's
      // cached rank-live bitmap when it has one, else all-ones; pooled ranks
      // cleared), which then serve double duty: the host's dedup structure
      // (O(1) bit test instead of a hash probe, and bounded at numVectors/8
      // bytes where the hash set would grow toward the candidate cap), and
      // the engines' NOT-pooled exclusion filter via request.eligible - so
      // every post-fold round spends its whole result heap on fresh hits
      // (the resumable cursor FAISS does not provide), and kReq switches
      // from cumulative pool depth to a per-round fresh budget.  A segment
      // with no liveness seed gets its span only once something of its is
      // pooled (markSeen activates it), so untouched no-delete segments
      // keep FAISS's compiled-out no-selector fast path.  Fold cost is
      // O(ntotal/8 + seen), paid only by queries that deepen.
      std::vector<std::vector<uint8_t>> eligibleStore;
      std::vector<std::span<const uint8_t>> eligibleSpans;
      bool seenFolded = false;
      auto foldSeen = [&]() {
        if (seenFolded) return;
        seenFolded = true;
        seenFoldsForTests.fetch_add(1, std::memory_order_relaxed);
        eligibleStore.resize(numSegs);
        eligibleSpans.assign(numSegs, {});
        std::vector<char> cleared(numSegs, 0);
        for (size_t i = 0; i < numSegs; i++) {
          int64_t n = segVectorCounts[i];
          if (n <= 0) continue;
          auto& bits = eligibleStore[i];
          if (rankLivePerSeg[i] != nullptr) {
            size_t numBytes = ((size_t)n + 7) / 8;
            assert(rankLivePerSeg[i]->numBytes == numBytes);
            bits.assign(rankLivePerSeg[i]->bits, rankLivePerSeg[i]->bits + numBytes);
          } else {
            // No cached liveness (no deletes, or a flat slot): not-pooled
            // only.  Flat engines filter the domain themselves, so for them
            // the bit means just "not pooled yet" - same dedup semantics.
            fillAllRanksSet(bits, n);
          }
        }
        for (uint64_t key : seenVectors) {
          size_t seg = (size_t)(key >> PACK_RANK_BITS);
          uint64_t r = key & (((uint64_t)1 << PACK_RANK_BITS) - 1);
          eligibleStore[seg][(size_t)(r >> 3)] &= (uint8_t)~(1u << (r & 7));
          cleared[seg] = 1;
        }
        // Hand engines a span only where it can exclude something: a
        // liveness seed or at least one pooled rank.  An all-ones span
        // would only demote a previously selector-less scan to a per-vector
        // always-true probe.
        for (size_t i = 0; i < numSegs; i++) {
          if (!eligibleStore[i].empty()
              && (rankLivePerSeg[i] != nullptr || cleared[i])) {
            eligibleSpans[i] = eligibleStore[i];
          }
        }
        seenVectors = boost::unordered_flat_set<uint64_t>{};
      };
      // Mark a hit seen; true if it was fresh.  Post-fold every engine
      // returns only fresh hits (eligible is binding), so a stale hit here
      // means a broken engine/selector wiring - counted for tests.  The
      // first pooled rank of a span-less segment activates its span for the
      // next round.
      auto markSeen = [&](int32_t segOrd, int64_t valueRank) -> bool {
        if (!seenFolded) {
          return seenVectors.insert(packSegRank(segOrd, valueRank)).second;
        }
        auto& bits = eligibleStore[(size_t)segOrd];
        size_t byteIdx = (size_t)(valueRank >> 3);
        uint8_t mask = (uint8_t)(1u << (valueRank & 7));
        bool fresh = (bits[byteIdx] & mask) != 0;
        bits[byteIdx] &= (uint8_t)~mask;
        if (!fresh) {
          staleHitsForTests.fetch_add(1, std::memory_order_relaxed);
        } else if (eligibleSpans[(size_t)segOrd].empty()) {
          eligibleSpans[(size_t)segOrd] = bits;
        }
        return fresh;
      };

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
      // under load for latency.
      for (;;) {
        VectorSearchRequest request{
          .query = queryPtr,
          .dims = dims,
          .candidates = kReq,
          .breadth = breadth,
          .minScanFraction = query.getMinScanFraction(),
          .eligible = std::span<const std::span<const uint8_t>>(eligibleSpans),
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
          if (markSeen(hit.segOrd, hit.valueRank)) {
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

        // DEPTH: project the POOL size that should collapse to targetDocReq
        // docs from the observed docs-per-candidate yield, then request only
        // the INCREMENT over the banked pool: post-fold every engine returns
        // fresh hits on top of it (eligible exclusion), so re-requesting the
        // cumulative depth would overshoot the projection by the pool size,
        // compounding across rounds.  Floored at the doc deficit so a
        // degenerate projection still progresses.  `appended` is the
        // termination backstop: a round that added nothing fresh and still
        // claims an unexhausted pool (a conforming engine cannot - fresh
        // hits or poolExhausted) must not loop at the same shape.
        if (!result.poolExhausted && appended
            && (int64_t)candidateHits.size() < cap) {
          int64_t pool = (int64_t)candidateHits.size();
          int64_t want = projectedKReq(pool, targetDocReq, (int64_t)docHits.size(), cap);
          kReq = std::min(cap - pool,
                          std::max(targetDocReq - (int64_t)docHits.size(),
                                   want - pool));
          foldSeen();
          continue;
        }

        if (result.poolExhausted && !result.breadthExhausted && breadth < maxBreadth) {
          int32_t nextBreadth = result.nextBreadth > breadth ? result.nextBreadth : breadth + 1;
          breadth = std::min(nextBreadth, maxBreadth);
          foldSeen();
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

      // Boost is applied only after the raw-similarity top-k cut, so it can
      // change scores (including zeroing them) but never the retained docs.
      if (boost != 1.0f) {
        for (auto& hit : docHits) hit.score *= boost;
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
      /// Resolve-based eligibility selector over the live-filtered query
      /// domain (the PrepareContext contract: a present domain already
      /// includes liveDocs, so there is no separate liveness check).  FAISS
      /// consults the selector per scanned vector, before the distance, so
      /// this is the slow path - it is only used for per-query (filtered)
      /// domains; the unfiltered-with-deletes case uses the cached
      /// rank-space bitmap via faiss::IDSelectorBitmap instead, and the
      /// no-deletes no-filter case passes no selector at all (the scanner's
      /// per-vector check compiles out).
      class LocalDomainSelector final : public faiss::IDSelector {
        const SegV2D& v2d;
        DocSet* domain;
        // Optional rank-space eligibility bits (request.eligible): tested
        // before the resolve so already-pooled ranks cost one bit probe, no
        // doc resolution.  Out-of-range ids are rejected, mirroring
        // faiss::IDSelectorBitmap's byte-bound guard, so a corrupt id from
        // the zero-copy mmapped lists cannot read past the bitmap.  Empty
        // when the host has not folded yet.
        std::span<const uint8_t> eligible;

      public:
        LocalDomainSelector(const SegV2D& v2d, DocSet* domain,
                            std::span<const uint8_t> eligible = {}) noexcept
          : v2d(v2d), domain(domain), eligible(eligible) {}

        bool is_member(faiss::idx_t id) const final {
          if (id < 0) return false;
          uint64_t r = (uint64_t)id;
          if (!eligible.empty()) {
            if ((r >> 3) >= eligible.size()) return false;
            if (((eligible[r >> 3] >> (r & 7)) & 1) == 0) return false;
          }
          return domain->get(v2d.resolve((int64_t)id));
        }
      };

      faiss::Index& index;
      faiss::IndexIVF* ivf;
      int32_t segOrd;
      const SegV2D& v2d;
      LiveDocs* liveDocs;
      // Rank-space liveness bitmap, present iff the segment has deletes
      // (shared from the VectorAuxReader's per-liveGen cache).  liveListSize
      // always reads it; RoundSelector hands it to FAISS as the eligibility
      // bitmap when the domain is exactly liveDocs (unfiltered query).
      std::shared_ptr<const RankLiveBitmap> rankLive;
      // The query domain per the live-filtered contract; domainIsLive marks
      // the unfiltered case (domain == the liveDocs docset) where rank-space
      // bitmaps can stand in for the resolve-based check.
      DocSet* domain;
      bool domainIsLive;
      int32_t metric;
      bool exactScores;
      bool isIvf;
      int32_t nlist;

      /// The ONLY selector policy: built per scan call from (domain,
      /// domainIsLive, rankLive, request.eligible), so round 1 and deepen
      /// rounds cannot drift apart.  Cheapest selector wins:
      ///   no domain, no eligible      -> null (FAISS compiles the per-vector
      ///                                  check out)
      ///   domain == liveDocs          -> IDSelectorBitmap over rankLive, or
      ///                                  over eligible once present (it is
      ///                                  seeded from rankLive, so liveness
      ///                                  rides along - one bit probe)
      ///   filtered domain             -> LocalDomainSelector, composing the
      ///                                  eligible bit test (when present)
      ///                                  with the resolve-based check
      /// Per-call construction is two stores; owns the selector storage, so
      /// keep it alive across the FAISS call (copy/move deleted to enforce
      /// that selPtr never outlives the optionals it points into).
      class RoundSelector {
        std::optional<faiss::IDSelectorBitmap> bitmap;
        std::optional<LocalDomainSelector> domainSel;
        const faiss::IDSelector* selPtr = nullptr;

      public:
        RoundSelector(const SegmentFaissVectorEngine& eng,
                      const VectorSearchRequest& request) {
          std::span<const uint8_t> elig =
            (size_t)eng.segOrd < request.eligible.size()
              ? request.eligible[(size_t)eng.segOrd]
              : std::span<const uint8_t>{};
          if (eng.domain != nullptr && !eng.domainIsLive) {
            domainSel.emplace(eng.v2d, eng.domain, elig);
            selPtr = &*domainSel;
          } else if (!elig.empty()) {
            bitmap.emplace(elig.size(), elig.data());
            selPtr = &*bitmap;
          } else if (eng.domainIsLive) {
            bitmap.emplace(eng.rankLive->numBytes, eng.rankLive->bits);
            selPtr = &*bitmap;
          }
        }

        RoundSelector(const RoundSelector&) = delete;
        RoundSelector& operator=(const RoundSelector&) = delete;

        // FAISS declares sel as a mutable pointer but only calls const
        // members on it during search.
        faiss::IDSelector* get() const {
          return const_cast<faiss::IDSelector*>(selPtr);
        }
      };

    public:
      SegmentFaissVectorEngine(faiss::Index& index,
                               int32_t segOrd,
                               IndexReader::Segment& seg,
                               const SegV2D& v2d,
                               DocSet* domain,
                               std::shared_ptr<const RankLiveBitmap> rankLive,
                               int32_t metric,
                               bool exactScores,
                               bool isIvf,
                               int32_t nlist)
        : index(index),
          ivf(isIvf ? dynamic_cast<faiss::IndexIVF*>(&index) : nullptr),
          segOrd(segOrd),
          v2d(v2d),
          liveDocs(seg.liveDocs()),
          rankLive(std::move(rankLive)),
          domain(domain),
          domainIsLive(domain != nullptr && liveDocs != nullptr
                       && domain == &liveDocs->docset()),
          metric(metric),
          exactScores(exactScores),
          isIvf(isIvf),
          nlist(nlist) {
        if (isIvf && ivf == nullptr) {
          throw std::runtime_error("KnnQuery: vector aux marked IVF but FAISS index is not IndexIVF");
        }
        assert((liveDocs != nullptr) == (this->rankLive != nullptr));
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
      /// (full selection always sums to ~N).  Under deletes this reads the
      /// cached rank-space liveness bitmap per id (no rank -> doc resolve);
      /// liveness only (not the query domain), matching the live-N
      /// denominator.
      int64_t liveListSize(faiss::idx_t listId) const {
        assert(ivf != nullptr);
        if (listId < 0 || listId >= nlist) return 0;
        size_t rawSize = ivf->invlists->list_size((size_t)listId);
        if (rawSize == 0) return 0;
        if (liveDocs == nullptr) return (int64_t)rawSize;

        const uint8_t* bits = rankLive->bits;
        int64_t liveSize = 0;
        const faiss::idx_t* listIds = ivf->invlists->get_ids((size_t)listId);
        for (size_t i = 0; i < rawSize; i++) {
          uint64_t r = (uint64_t)listIds[i];
          liveSize += (bits[r >> 3] >> (r & 7)) & 1;
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

        // Single per-round selector policy (see RoundSelector); null when
        // the segment needs no filtering this round, so the scanner
        // compiles the per-vector check out.
        RoundSelector roundSel(*this, request);
        faiss::SearchParametersIVF ivfParams;
        ivfParams.sel = roundSel.get();
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

        RoundSelector roundSel(*this, request);
        faiss::SearchParameters flatParams;
        flatParams.sel = roundSel.get();
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

        // Pooled-rank exclusion (request.eligible), honored at DOC level:
        // this engine's scores are exact and it emits each doc's max-sim
        // vector, so a cleared bit anywhere in a doc's run means the doc's
        // best vector is already in the host pool and nothing else in the
        // run can ever improve the doc - skip it whole.  (A segment is
        // served by exactly one slot, so cleared ranks here are exactly
        // this engine's prior emissions.)  Skipped docs do not count toward
        // eligibleDocs: poolExhausted means "a deeper request adds nothing",
        // and pooled docs never will.
        std::span<const uint8_t> elig =
          (size_t)segOrd < request.eligible.size()
            ? request.eligible[(size_t)segOrd]
            : std::span<const uint8_t>{};
        auto pooledInRun = [&elig](int64_t begin, int64_t end) {
          if (elig.empty()) return false;
          for (int64_t r = begin; r < end; r++) {
            if (((elig[(size_t)(r >> 3)] >> (r & 7)) & 1) == 0) return true;
          }
          return false;
        };

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

            if (docEligible(docId) && !pooledInRun(rank, end)) {
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
            if (pooledInRun(rank, rank + 1)) continue;
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
      // The domain, when present, is live-filtered and is the complete
      // eligibility predicate (PrepareContext contract); null means the
      // segment has no deletes and no filters.
      bool docEligible(int32_t docId) const {
        return domain == nullptr || domain->get(docId);
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
    /// lastScannedBreadth mutate across search() rounds of
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
      // lastScannedBreadth detects a depth round (the host grows exactly one
      // axis per round, so an UNCHANGED breadth means depth), which DOES
      // force a full re-scan - candidate counts cannot stand in for this
      // since post-fold they are per-round fresh budgets and may repeat or
      // shrink between rounds.
      std::vector<size_t> scannedListCount;
      int32_t lastScannedBreadth = -1;
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
        //   DEPTH  - breadth unchanged: need more candidates from the SAME
        //            lists.  FAISS has no resumable list cursor, so re-scan all
        //            selected lists; request.eligible excludes everything the
        //            host already pooled, so the re-scan returns only fresh
        //            hits and the round's candidate count is a FRESH budget,
        //            not a cumulative depth (it may repeat or shrink, which is
        //            why breadth - the host grows exactly one axis per round -
        //            is the depth-round signal, not the candidate count).
        //   BREADTH - allocateToBudget appended new lists: only the freshly
        //            allocated tail needs scanning, since prior lists' hits
        //            are already unioned into the host pool.
        // Scanning only the new tail on a breadth round turns the old O(rounds x
        // lists) IVF re-scan into O(lists) total.  A breadth round is only entered
        // after a poolExhausted=true round (host loop), so every prior list is
        // already drained at this depth - omitting them from this round's
        // exhaustion accounting is correct.  Flat (below-threshold) engines are
        // breadth-independent and small, so they are simply re-run; they honor
        // request.eligible like every engine (a doc whose pooled best vector is
        // excluded can never improve an exact score, so the whole doc is
        // skipped), which keeps search() a pure function of its arguments (the
        // bitmaps are arguments; test oracles rely on the purity).
        //
        // RECALL SAFETY: IVF lists are disjoint, and for disjoint sets A, B
        // the global top-k of (A union B) is contained in top-k(A) union
        // top-k(B).  So the host's union of per-round top-k subsets is a
        // SUPERSET of what one global pass over all selected lists would
        // return - PROVIDED every list was scanned at the final candidate
        // count, which is exactly what the depth-round full re-scan maintains.
        // What must NOT happen on the depth axis is skipping previously
        // scanned lists - that silently loses recall.  EXCLUDING the host's
        // already-POOLED ranks (request.eligible) is safe, and is also what
        // makes the smaller fresh-budget heaps sound: if x is in
        // top-(pooled+k)(P) and x is not pooled, fewer than k members of
        // (P minus pooled) beat x, so x is in top-k(P minus pooled) - the
        // banked pool plus a k-deep fresh scan covers a (pooled+k)-deep
        // cumulative scan exactly.  (Pooled is what the host KEPT -
        // merge-cut hits were never pooled, stay eligible, and come back on
        // the next round.)  The same lemma is why per-chunk scanning below
        // loses nothing: every chunk scans at the full round candidate count.
        bool depthGrew = request.breadth == lastScannedBreadth;
        lastScannedBreadth = request.breadth;

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
    float maxScore = 0.0f;

  public:
    explicit Scorer(std::span<const Hit> hits) noexcept : hits(hits) {
      for (const Hit& hit : hits) maxScore = std::max(maxScore, hit.score);
    }

    int32_t next() override {
      cur++;
      return cur < (int32_t)hits.size() ? hits[cur].docId : PostingsReader::END;
    }

    int32_t advance(int32_t docid) override {
      assert(docId() < docid);  // strict Scorer contract; callers guard
      int32_t doc;
      while ((doc = next()) < docid) {}
      return doc;
    }

    int32_t docId() override {
      if (cur < 0) return -1;
      return cur < (int32_t)hits.size() ? hits[cur].docId : PostingsReader::END;
    }

    float score() override {
      assert(cur >= 0 && cur < (int32_t)hits.size());
      return hits[cur].score;
    }

    float getMaxScore(int32_t upTo) override {
      unused(upTo);
      return maxScore;
    }

    float getMaxScoreForSetup(int32_t upTo) override {
      unused(upTo);
      return maxScore;
    }
  };

private:
  static constexpr double MIN_COSINE_NORM_SQ = 1.0e-30;

  /// Fill `bits` with the all-set pattern for n ranks: ceil(n/8) 0xFF bytes
  /// with the tail bits past n cleared.  Every rank bitmap in this file
  /// keeps that tail invariant, so popcount(bits) == set ranks (see
  /// RankLiveBitmap); build them through here so the mask lives once.
  static void fillAllRanksSet(std::vector<uint8_t>& bits, int64_t n) {
    bits.assign(((size_t)n + 7) / 8, (uint8_t)0xFF);
    if (n & 7) {
      bits.back() = (uint8_t)(0xFFu >> (8 - (n & 7)));
    }
  }

  /// Build the RankLiveBitmap (rank-space liveness + exact live vector
  /// count) for one segment's vector field.  Run once per liveDocs
  /// generation (cached on the VectorAuxReader), and sized by the segment's
  /// DELETED docs rather than its vectors: start all-live, walk the zero
  /// bits of liveDocs, and clear each deleted doc's rank(s), resolving
  /// doc -> rank through the has-field bitset (advance + rank) and, for
  /// multi-valued, the forward start/end-rank map - the engines' reverse
  /// (valueRank -> docId) map is never consulted.  Dense single-valued
  /// needs no walk at all: valueRank == docId, so the liveDocs words
  /// themselves are the bitmap (borrowed and pinned via backing) and the
  /// segment's cached live count is the cardinality.
  static RankLiveBitmap buildRankLiveBitmap(IndexReader::Segment& seg,
                                            VectorReader& vr,
                                            int64_t numVectors) {
    rankLiveBitmapBuildsForTests.fetch_add(1, std::memory_order_relaxed);
    LiveDocs& live = *seg.liveDocs();
    const screaming::FixedBitSet& liveBits = live.bitset();
    size_t numBytes = ((size_t)numVectors + 7) / 8;
    StrColReader& col = vr.strColReader();
    const bool multi = vr.isMultiValued();
    auto& dr = col.docsReader();
    if (!multi && !dr.hasBitset()) {
      static_assert(std::endian::native == std::endian::little,
                    "liveDocs words double as the LSB-first byte bitmap");
      assert((int64_t)live.size() == numVectors);
      return {(const uint8_t*)liveBits.words, numBytes, (int64_t)live.numLive(),
              seg.liveDocsShared()};
    }

    auto owned = std::make_shared<std::vector<uint8_t>>();
    std::vector<uint8_t>& bits = *owned;
    fillAllRanksSet(bits, numVectors);
    int64_t cleared = 0;
    auto clearRanks = [&bits, &cleared](int64_t begin, int64_t end) {
      for (int64_t r = begin; r < end; r++) {
        bits[(size_t)(r >> 3)] &= (uint8_t)~(1u << (r & 7));
      }
      cleared += end - begin;
    };

    const int32_t maxDoc = live.size();
    const int32_t numWords = (int32_t)screaming::FixedBitSet::sizeInWords(maxDoc);
    if (dr.hasBitset()) {
      screaming::BitSet::Iterator fieldDocs(dr.bitset());
      int32_t fieldDoc = -1;  // last doc-with-field landed on (END once drained)
      screaming::FixedBitSet::visitZeroes(liveBits.words, numWords, [&](int32_t d) {
        if (d >= maxDoc) return;  // zero tail bits of the last word are not docs
        if (fieldDoc < d) fieldDoc = fieldDocs.advance(d);
        if (fieldDoc != d) return;  // deleted doc holds no vectors
        int32_t docRank = fieldDocs.rank();
        if (multi) {
          auto [begin, end] = col.getStartEndValueRank(docRank);
          clearRanks(begin, end);
        } else {
          clearRanks(docRank, docRank + 1);
        }
      });
    } else {
      // Every doc has the field, so docRank == docId.  Multi-valued only:
      // the dense single-valued case borrowed liveDocs above.
      screaming::FixedBitSet::visitZeroes(liveBits.words, numWords, [&](int32_t d) {
        if (d >= maxDoc) return;
        auto [begin, end] = col.getStartEndValueRank(d);
        clearRanks(begin, end);
      });
    }
    const uint8_t* data = bits.data();
    return {data, numBytes, numVectors - cleared, std::move(owned)};
  }

  /// O(1) UPPER BOUND on a segment's live vector count, used as the budget
  /// denominator (referenceBreadth, list-cost normalization) and the
  /// targetDocReq cap for segments with no cached exact count: flat
  /// (aux-less) segments under deletes.  FAISS segments under deletes use
  /// the exact cardinality the cached RankLiveBitmap carries instead - the
  /// count is amortized into the per-liveGen bitmap build, where computing
  /// it here would re-walk the field on EVERY query, O(N_seg) bookkeeping
  /// to budget a search that examines ~N^0.75 vectors at the default.  An
  /// upper bound is the safe direction to relax: overestimating only makes
  /// the scan fraction leaner and targetDocReq higher, and host deepening
  /// compensates (the same stance as cost accounting ignoring the domain
  /// filter).  UNDERestimating is not safe: targetDocReq = min(liveTotal,
  /// ...) would stop the widen loop before K achievable docs.  Exact for
  /// no-deletes and for dense single-valued (identity mapping => live count
  /// == numLive).
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
      case (int32_t)solux::api::VectorMetric::L2: {
        float dist = faiss::fvec_L2sqr(queryPtr, vec.data(), (size_t)dims);
        return finiteScore(1.0f / (1.0f + dist));
      }
      case (int32_t)solux::api::VectorMetric::IP:
        return finiteScore(faiss::fvec_inner_product(queryPtr, vec.data(), (size_t)dims));
      case (int32_t)solux::api::VectorMetric::COSINE: {
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
      case (int32_t)solux::api::VectorMetric::L2:
        return finiteScore(1.0f / (1.0f + dist));
      case (int32_t)solux::api::VectorMetric::IP:
      case (int32_t)solux::api::VectorMetric::COSINE:
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
