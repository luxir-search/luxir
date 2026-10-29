// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <faiss/Index.h>
#include <faiss/IndexIVF.h>
#include <faiss/impl/ResultHandler.h>
#include <faiss/MetricType.h>
#include <faiss/impl/IDSelector.h>
#include <faiss/impl/DistanceComputer.h>
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
#include "luxir/api/luxir_types.hpp"
#include "luxir/reader/AuxReader.h"
#include "luxir/reader/PostingsReader.h"
#include "luxir/reader/RowReadAhead.h"
#include "luxir/reader/VectorAuxReader.h"
#include "luxir/reader/VectorReader.h"
#include "luxir/query/VectorEngine.h"
#include "luxir/search/DocSet.h"
#include "luxir/search/Collapse.h"
#include "luxir/util/AtomicMerger.h"
#include "luxir/util/heap.h"
#include "luxir/util/log.h"
#include "luxir/util/screaming.h"
#include "luxir/util/thread.h"

namespace luxir {

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
///      SegmentFaissVectorEngine (IVF+RaBitQ or IVF+PQ aux, approximate).  All slots are
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
/// Request knobs (see KnnQuery message in luxir_types.proto for the wire
/// contract): k = selected units to return (docs by default, groups with a
/// resolved collapse key); ivf.nprobe = merge-stable IVF effort (lists
/// to probe as if the field were ONE IVF index with nlist=sqrt(live vectors) -
/// explicit pins the effort, 0 adapts and auto-deepens); ivf.min_scan_fraction
/// = optional floor on the internal scan fraction; refine_candidates = pinned
/// approximate pool size (host-side, engine-agnostic); exact = bypass ANN
/// entirely (column scan contract).
///
/// The active query domain is the COMPLETE eligibility predicate: per the
/// PrepareContext contract it is already live-filtered (liveDocs
/// intersected with any enclosing filter clauses), and a null domain means
/// no deletes and no filters.  prepare() converts it once per segment into a
/// rank-space bitmap (RankBitmap: bit r = vector rank r eligible), so no
/// engine resolves ranks to docs to test eligibility:
///   - null domain: no bitmap; FAISS gets no selector (its per-vector check
///     compiles out) and the column scan visits every rank.
///   - domain == liveDocs: the segment's liveness bitmap, cached on the
///     overlay per liveDocs generation.
///   - filtered: the filter's bitmap, cached on the overlay per (filter key,
///     liveDocs generation) when the domain has a stable name
///     (PrepareContext::domainKey), else built for this query.
/// FAISS always consumes it through faiss::IDSelectorBitmap, and the column
/// scan walks its set bits word by word.  Its popcount is the segment's
/// eligible vector count, which drives the plan: zero-eligible segments get
/// no engine, low-eligible IVF segments are scored exactly from the column
/// (exactCheaperThanIvf), IVF effort is denominated in eligible vectors (the
/// reference index is the eligible set; zero-eligible lists are skipped),
/// and the doc target is capped by the eligible count.
///
/// Multi-valued vector fields are supported via the segment's persisted
/// valueRank->docId column (VectorReader::docForVectorRank).  The exact column
/// engine collapses a doc's vectors while scanning and returns one best vector
/// per doc.  Approximate aux engines return vector candidates instead; the
/// host pools them and the terminal refine collapses.  Above maxKnnCandidates
/// the result is best-effort.
class KnnQuery final : public luxir::Query {
  std::string_view field;
  const VectorFieldType& fieldType;
  std::span<const float> queryVec;
  int32_t k;
  int32_t nprobe;
  int32_t refineCandidates;
  float minScanFraction;
  bool exact;
  const CollapsePlan* groups;

  bool sameGroups(const KnnQuery& other) const {
    if ((groups == nullptr) != (other.groups == nullptr)) return false;
    if (groups == nullptr) return true;
    if (groups->missing != other.groups->missing || groups->fields.size() != other.groups->fields.size()) return false;
    for (size_t i = 0; i < groups->fields.size(); i++) {
      if (groups->fields[i].name != other.groups->fields[i].name
          || groups->fields[i].type != other.groups->fields[i].type) return false;
    }
    return true;
  }

  // Minimum similarity in the metric's own space (see similarityScoreFloor).
  std::optional<float> similarity;

  void validateKnnLogical() const {
    if (!fieldType.knnSearchable()) {
      throw std::runtime_error(std::format(
          "KnnQuery on vector field '{}' without a metric", field));
    }
    // The parser enforces the field's input contract and cuts the query to
    // its stored prefix.
    assert(fieldType.storedDims() == 0 || (int32_t)queryVec.size() == fieldType.storedDims());
    assert(std::ranges::all_of(queryVec, [](float x) { return std::isfinite(x); }));
  }

public:
  /// One result hit within a segment.  docId is the local docRank within the
  /// segment; score is converted to "higher is better" regardless of vector
  /// metric (see scoreFromDist).
  struct Hit {
    int32_t docId;
    float score;
  };

  struct Stats {
    std::atomic<int64_t> lists{0}, vectors{0}, bytes{0}, rounds{0};
  };
  static inline Stats* statsForTests = nullptr;
  // Test/benchmark seam only. Zero selects the measured default; positive
  // values are clamped to R/k so k groups remain reachable in R documents.
  static inline int32_t membersPerGroupForTests = 0;

  static int64_t membersPerGroup(int64_t documents, int64_t k) {
    // Reserve at least 2*k groups when possible. Eight members matched the
    // larger measured cap's recall, and bounds per-key record work even for
    // large refinement budgets at small k. The override permits cap sweeps.
    int64_t maxMembers = std::max((int64_t)1, documents / k);
    return std::min(maxMembers, membersPerGroupForTests > 0
        ? (int64_t)membersPerGroupForTests : std::min((int64_t)8, std::max((int64_t)1, maxMembers / 2)));
  }

  // Maximum engine candidates retained while trying to fill k distinct units.
  // Approximate group cuts retain documents under a per-key cap.
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

  // Dimension-aware default IVF effort.  The nprobe=0 scan fraction
  // (1/sqrt(referenceBreadth), see defaultBudget) is scaled by
  //   clamp(sqrt(dims / defaultAnnBreadthReferenceDims), 1, ANN_BREADTH_MAX_DIM_FACTOR).
  // Coarse partitions localize neighbors worse as dimension grows (distances
  // to centroids concentrate, so a query's true neighbors spill over many
  // more lists), and when the codes rank well inside the probed lists (1-bit
  // RaBitQ at >= 768 dims) recall is bounded by the lists probed, not by the
  // candidate pool: on GIST-960 / Cohere-768 the unscaled default stops at
  // 0.82-0.87 recall@10 whatever refine_candidates is, and ~2.5x the lists
  // reaches ~0.95.  At <= 128 dims (SIFT, Deep) the unscaled default already
  // reaches ~0.96 and is left unchanged.  Depends only on dims, so it stays
  // merge-stable.  0 disables the scaling.  Mutable for tests / benches.
  static inline int32_t defaultAnnBreadthReferenceDims = 128;
  static constexpr double ANN_BREADTH_MAX_DIM_FACTOR = 4.0;
  static double defaultAnnBreadthDimFactor(int32_t dims) {
    if (defaultAnnBreadthReferenceDims <= 0 || dims <= defaultAnnBreadthReferenceDims) return 1.0;
    return std::min(ANN_BREADTH_MAX_DIM_FACTOR,
                    std::sqrt((double)dims / (double)defaultAnnBreadthReferenceDims));
  }

  /// IVF scan fraction for a breadth (reference-index lists; 0 = adaptive
  /// default) against a reference index of referenceBreadth lists, floored
  /// by minScanFraction.  The fraction is of the ELIGIBLE vectors: the
  /// reference index is nlist = sqrt(eligible), so an unfiltered query sees
  /// the merge-stable sqrt(live) reference, and a filter scales the default
  /// effort up by selectivity^-1/4 - the effort an IVF index built over just
  /// the eligible vectors would spend by its own default.
  ///
  /// The nprobe=0 default probes sqrt(nlist) lists of the reference index,
  /// times the dimension factor (defaultAnnBreadthDimFactor): as a fraction
  /// sqrt(referenceBreadth) / referenceBreadth = 1 / sqrt(referenceBreadth)
  /// (= N^-0.25 for referenceBreadth = sqrt(N)).  It depends only on the
  /// eligible count, so it is MERGE-STABLE: repartitioning the same vectors
  /// into more or fewer segments does not move it.  Host auto-deepening grows
  /// breadth from this lean start when too few distinct live docs come back.
  /// A per-segment sum was rejected: sum_s sqrt(N_s) / sqrt(N) =
  /// sqrt(numSegments) >= 1, which clamps to a full scan; and even summing
  /// per-segment defaults (sum_s sqrt(nlist_s) / sqrt(N) = S^0.75 * N^-0.25)
  /// keeps an S-dependent term that drifts on merges.
  static double scanFraction(int32_t breadth, int32_t referenceBreadth, int32_t dims,
                             float minScanFraction) {
    double p = breadth > 0
      ? (double)breadth / (double)std::max(1, referenceBreadth)
      : defaultAnnBreadthDimFactor(dims) / std::sqrt((double)std::max(1, referenceBreadth));
    p = std::max(p, (double)minScanFraction);
    return std::clamp(p, 0.0, 1.0);
  }

  // Exact fallback for filtered IVF segments.  A filtered IVF segment is
  // scored exactly over its eligible rows when that reads no more than
  // exactFallbackCostRatio times what its IVF search would (see
  // exactCheaperThanIvf).  0 disables the fallback.  Mutable for tests.
  // Calibrated on BM_VectorKnnFiltered (200k 64-d vectors, RaBitQ-1, 4
  // segments): per byte a scored row is ~8x cheaper than the IVF path's
  // per-list and per-id work, so exact wins up to ~5% selectivity (ratio
  // ~6 there) and loses from 10% (ratio ~13).  At 1% IVF was 2-5x slower
  // than exact and lost recall on cluster-correlated filters.
  static inline double exactFallbackCostRatio = 8.0;

  /// Whether scoring a filtered segment's eligible rows from the column is
  /// cheaper than its IVF search, in bytes read weighted by
  /// exactFallbackCostRatio:
  ///   exact = eligible * rowBytes
  ///   ivf   = nlist * rowBytes                    (coarse centroid ranking)
  ///         + p * (vectors * idBytes              (probed lists' ids, each
  ///                                                tested against the bitmap)
  ///            + eligible * codeBytes)            (eligible codes scored)
  /// with p the round-1 scan fraction of eligible vectors.  This is the
  /// Lucene / Elasticsearch rule (exact when the filter matches no more
  /// documents than the approximate search would visit) priced per byte, so
  /// a full-precision row counts as many cheap codes.  It ignores the
  /// terminal rescore (paid by IVF only) and later deepen rounds, both of
  /// which only favor exact more, and IVF's per-list and per-id overheads,
  /// which the ratio prices.  Only filtered segments are considered:
  /// unfiltered IVF segments are large by construction (the build threshold)
  /// and keep their merge-stable effort.
  static bool exactCheaperThanIvf(int64_t eligible, int64_t vectors, int32_t nlist,
                                  size_t codeBytes, int32_t dims, double p) {
    if (exactFallbackCostRatio <= 0.0) return false;
    double rowBytes = (double)dims * (double)sizeof(float);
    double ivfBytes = (double)nlist * rowBytes
      + p * ((double)vectors * (double)sizeof(faiss::idx_t) + (double)eligible * (double)codeBytes);
    return (double)eligible * rowBytes <= exactFallbackCostRatio * ivfBytes;
  }

  // Cap on the breadth growth of one deepen round (see projectedBreadth).
  static constexpr double MAX_BREADTH_GROWTH = 8.0;

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
  // Rank-space filter-domain builds (cache misses and uncached filters), and
  // IVF segments a query routed to exact eligible-row scoring.
  static inline std::atomic<int64_t> filterRankBitmapBuildsForTests{0};
  static inline std::atomic<int64_t> exactFallbackSegmentsForTests{0};
  static inline std::atomic<int64_t> seenFoldsForTests{0};
  // Post-fold hits that were already pooled.  The eligible contract makes
  // this impossible for conforming engines; nonzero means selector wiring
  // broke and deepen rounds silently regressed to re-returning the pool.
  static inline std::atomic<int64_t> staleHitsForTests{0};
  static inline std::atomic<int64_t> prepareCallsForTests{0};

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
  /// scores: query and stored vectors share VectorFieldType::inputError's
  /// bounds, which keep distances finite, and finiteScore maps any NaN that
  /// still arises to the worst finite score.
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

  struct NoGroupState {};

  // Policy selection is at plan time. Docs keep the original vector-backed
  // heap; groups use collapse's stable slots and indexed heap. A partition's
  // capped top-N contains each globally selected document lying in it;
  // GroupAdmission states the union-then-recut argument for arbitrary caps.
  template <bool Groups, typename T, auto Better>
  class BoundedHits {
    struct Entry : T { size_t slot = 0; int32_t documentId = -1; };
    // A functor type, not the function pointer: the heap algorithms then bind
    // the order statically and inline it wherever they are instantiated.
    struct Compare {
      bool operator()(const T& a, const T& b) const noexcept { return Better(a, b); }
    };
    struct GroupState {
      GroupAdmission<false> admission;
      int32_t segment = -1;
      ExpandingPQ<Entry, Compare, true> heap;
      GroupState(const CollapsePlan& plan, int64_t limit, size_t perKey) : admission(plan, perKey), heap((size_t)limit) {}
    };
    int64_t limit;
    std::vector<T> docs;
    [[no_unique_address]] std::conditional_t<Groups, std::unique_ptr<GroupState>, NoGroupState> state;
    std::span<const SegV2D> v2d;
  public:
    bool truncated = false;
    BoundedHits(int64_t limit, const CollapsePlan* plan, std::span<const SegV2D> v2d = {}, size_t perKey = 1)
        : limit(limit), v2d(v2d) {
      if constexpr (Groups) state = std::make_unique<GroupState>(*plan, limit, perKey);
      else unused(plan);
    }
    size_t size() const {
      if constexpr (Groups) return state->heap.size();
      else return docs.size();
    }
    bool competitive(const T& hit) requires Groups {
      if ((int64_t)state->heap.size() < limit) return limit > 0;
      truncated = true;
      return Better(hit, state->heap.top());
    }
    const T* floor() const requires Groups {
      return limit > 0 && (int64_t)state->heap.size() == limit ? &state->heap.top() : nullptr;
    }
    int32_t document(const T& hit) const requires Groups {
      if constexpr (std::is_same_v<T, DocHit>) return hit.docId;
      else return v2d[(size_t)hit.segOrd].resolve(hit.valueRank);
    }
    LUXIR_INLINE bool offer(const T& hit) {
      if constexpr (!Groups) {
        offerBounded(docs, hit, limit, Compare{});
        return true;
      } else {
        if (!competitive(hit)) return false;
        int32_t doc = document(hit);
        if (state->segment != hit.segOrd) {
          state->admission.reader.setSegment(hit.segOrd);
          state->segment = hit.segOrd;
        }
        const auto* key = state->admission.reader.read(hit.segOrd, doc);
        return key != nullptr && offerKey(hit, *key, doc);
      }
    }
    bool offerKey(const T& hit, const CollapseKey& key, int32_t doc = -1) requires Groups {
      auto& admission = state->admission;
      auto& heap = state->heap;
      if (!competitive(hit)) return false;
      Entry candidate;
      static_cast<T&>(candidate) = hit;
      candidate.documentId = doc >= 0 ? doc : document(hit);
      return admission.admit(key, heap, candidate,
          [&](const Entry& head) { return Better(hit, head); }, [](size_t, bool) {},
          [&](const Entry& head) { return hit.segOrd == head.segOrd && candidate.documentId == head.documentId; });
    }
    void merge(BoundedHits& other) {
      truncated |= other.truncated;
      if constexpr (Groups) {
        for (const auto& hit : other.state->heap.span()) {
          if (competitive(hit)) offerKey(hit, other.state->admission.keyAt(hit.slot), hit.documentId);
        }
      } else for (const auto& hit : other.docs) offer(hit);
    }
    std::vector<T> take() {
      if constexpr (!Groups) return std::move(docs);
      else {
        std::vector<T> result;
        result.reserve(size());
        for (const auto& hit : state->heap.span()) result.push_back(hit);
        return result;
      }
    }
  };

  KnnQuery(std::string_view field, const VectorFieldType& fieldType,
           std::span<const float> queryVec, int32_t k,
           int32_t nprobe = 0, int32_t refineCandidates = 0,
           float minScanFraction = 0.0f, bool exact = false,
           const CollapsePlan* groups = nullptr,
           std::optional<float> similarity = std::nullopt)
    : Query(QueryKind::KNN), field(field), fieldType(fieldType),
      queryVec(queryVec), k(k),
      nprobe(nprobe), refineCandidates(refineCandidates),
      minScanFraction(minScanFraction), exact(exact), groups(groups),
      similarity(similarity) {}

  bool recoverableAsPredicate() const noexcept override { return false; }

  bool equalsSameKind(const Query& other) const override {
    const auto& rhs = static_cast<const KnnQuery&>(other);
    return sameGroups(rhs) && field == rhs.field && k == rhs.k
        && nprobe == rhs.nprobe
        && refineCandidates == rhs.refineCandidates
        && std::bit_cast<uint32_t>(minScanFraction)
            == std::bit_cast<uint32_t>(rhs.minScanFraction)
        && exact == rhs.exact
        && similarity.has_value() == rhs.similarity.has_value()
        && (!similarity.has_value()
            || std::bit_cast<uint32_t>(*similarity)
                == std::bit_cast<uint32_t>(*rhs.similarity))
        && queryVec.size() == rhs.queryVec.size()
        && fieldType.storedDims() == rhs.fieldType.storedDims()
        && fieldType.metric() == rhs.fieldType.metric()
        && fieldType.ann().rabitqQueryBits == rhs.fieldType.ann().rabitqQueryBits
        && fieldType.normalized() == rhs.fieldType.normalized()
        && fieldType.normalizeOnWrite()
            == rhs.fieldType.normalizeOnWrite()
        && (queryVec.empty()
            || memcmp(queryVec.data(), rhs.queryVec.data(),
                      queryVec.size_bytes()) == 0);
  }

  uint64_t hashImpl() const override {
    uint64_t value = mixHash(Query::hashImpl(), field);
    value = mixHash(value, k);
    value = mixHash(value, nprobe);
    value = mixHash(value, refineCandidates);
    value = mixHash(value, std::bit_cast<uint32_t>(minScanFraction));
    value = mixHash(value, exact);
    value = mixHash(value, groups != nullptr);
    if (groups != nullptr) {
      value = mixHash(value, (int32_t)groups->missing);
      for (const auto& component : groups->fields) {
        value = mixHash(value, std::string_view(component.name));
        value = mixHash(value, (int32_t)component.type);
      }
    }
    if (similarity.has_value()) {
      value = mixHash(value, std::bit_cast<uint32_t>(*similarity));
    }
    value = mixSampledSequence(
        value, std::as_bytes(queryVec),
        [](uint64_t seed, std::byte byte) {
          return mixHash(seed, byte);
        });
    value = mixHash(value, fieldType.storedDims());
    value = mixHash(value, (int32_t) fieldType.metric());
    value = mixHash(value, fieldType.ann().rabitqQueryBits);
    value = mixHash(value, fieldType.normalized());
    return mixHash(value, fieldType.normalizeOnWrite());
  }

  std::string_view getField() const noexcept { return field; }
  const VectorFieldType& getFieldType() const noexcept { return fieldType; }
  std::span<const float> getQueryVec() const noexcept { return queryVec; }
  int32_t getK() const noexcept { return k; }
  int32_t getNProbe() const noexcept { return nprobe; }
  int32_t getRefineCandidates() const noexcept { return refineCandidates; }
  float getMinScanFraction() const noexcept { return minScanFraction; }
  bool getExact() const noexcept { return exact; }
  const std::optional<float>& getSimilarity() const noexcept {
    return similarity;
  }

  /// The similarity threshold as a floor in score space, or -inf when unset.
  /// L2 similarity is a maximum (non-squared) distance d, scored as
  /// 1 / (1 + d^2); IP and COSINE scores are the similarity itself. The float
  /// expression mirrors scoreFromDist's nonnegative branch, so a hit at
  /// exactly distance d keeps its place.
  float similarityScoreFloor() const noexcept {
    if (!similarity.has_value()) {
      return -std::numeric_limits<float>::infinity();
    }
    float value = *similarity;
    if (fieldType.metric() == VectorFieldType::METRIC_L2) {
      return 1.0f / (1.0f + value * value);
    }
    return value;
  }

  void validateLogicalImpl(
      PlanningContext& context, float multiplier = 1.0f) const override {
    unused(context);
    checkedBoostProduct(multiplier, 1.0f);
    validateKnnLogical();
  }

  FilterKeyScope appendFilterKey(FilterKeyBuilder& out,
                                 const FilterKeyContext& ctx) const override {
    out.appendKind(kind);
    unused(ctx);
    out.appendString(field);
    out.appendSize(queryVec.size());
    for (float value : queryVec) out.appendFloat(value);
    out.appendInt32(k);
    out.appendInt32(nprobe);
    out.appendInt32(refineCandidates);
    out.appendFloat(minScanFraction);
    out.appendBool(exact);
    out.appendBool(groups != nullptr);
    if (groups != nullptr) {
      out.appendInt32((int32_t)groups->missing);
      out.appendSize(groups->fields.size());
      for (const auto& component : groups->fields) {
        out.appendString(component.name);
        out.appendInt32((int32_t)component.type);
      }
    }
    // Membership is score dependent, but only through the raw similarity
    // (boost applies after the cut), so the threshold is a semantic key part.
    out.appendBool(similarity.has_value());
    if (similarity.has_value()) out.appendFloat(*similarity);
    // The mutable execution policies are resolved into the semantic key
    // directly. This is intentionally the MVP alternative to maintaining a
    // policy-generation counter: every value that can change candidate
    // selection is visible here and key equality remains self-contained.
    out.appendInt64(std::max((int64_t)1, maxKnnCandidates));
    out.appendInt32(std::max(0, maxKnnBreadth));
    out.appendInt32(defaultAnnRefineCount);
    out.appendInt32(defaultAnnRefineRatio);
    out.appendInt32(defaultAnnBreadthReferenceDims);
    out.appendInt64(std::bit_cast<int64_t>(exactFallbackCostRatio));
    // Test wrappers may alter candidate selection without a keyable semantic
    // contract. Production engines are deterministic for a pinned reader.
    return engineWrapperForTests || groups != nullptr ? FilterKeyScope::UNCACHEABLE
                                 : FilterKeyScope::READER_STABLE;
  }

  Query::Weight* createWeight(Query::Context& context, int32_t flags,
                              float multiplier = 1.0f) override;

  class Weight final : public Query::Weight {
    KnnQuery& query;
    float boost;

  public:
    Weight(Query::Context& context, KnnQuery& query, int32_t flags, float multiplier)
      : Query::Weight(context, query, flags), query(query),
        boost(constantWhenScored(flags, multiplier)) {
      traits |= NEEDS_PREPARE;  // index-level ANN pass
    }

    class KnnPreparedWeight final : public Query::Weight::PreparedWeight {
      std::vector<std::vector<Hit>> perSegHits;

      class Supplier final : public Query::ScorerSupplier {
        std::span<const Hit> hits;

        class Plan final : public Query::ScorerPlan {
          std::span<const Hit> hits;

        protected:
          Query::Scorer* buildScorer(MemPool& targetPool) override {
            return targetPool.make<KnnQuery::Scorer>(hits);
          }

        public:
          Plan(Supplier& supplier,
               const Query::PlanContext& planContext,
               const Query::ScorerShape& shape, int64_t cost,
               std::span<const Hit> hits)
            : Query::ScorerPlan(
                  planContext, shape, cost),
              hits(hits) {}
        };

      public:
        explicit Supplier(std::span<const Hit> hits) : hits(hits) {
          assert(!hits.empty());
        }

        int64_t cost() override { return (int64_t)hits.size(); }

        Query::ScorerShape describeScorer(
            const Query::PlanContext& buildContext) const override {
          unused(buildContext);
          return {
            .matchState = Query::MatchState::NONEMPTY,
            .directKind = Query::DirectScorerKind::OTHER,
            .reportedTwoPhase = Query::ReportedTwoPhase::NO,
            .windowFillClause = Query::ClauseShape::NONE,
            .termDisjunctionClause = Query::ClauseShape::NONE,
            .termConjunctionClause = Query::ClauseShape::NONE,
            .independentTerm =
                Query::IndependentTermAccess::UNSUPPORTED,
            .docsOnly = Query::DocsOnlyAccess::UNSUPPORTED,
            .directDocSet = Query::DirectDocSetAccess::UNSUPPORTED,
          };
        }

        Query::ScorerPlan* resolve(
            MemPool& planPool,
            const Query::PlanContext& planContext) override {
          return planPool.make<Plan>(
              *this, planContext, describeScorer(planContext), cost(), hits);
        }
      };

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

      Query::ScorerSupplier* scorerSupplierImpl(
          MemPool& target, IndexReader::Segment& segment,
          Query::SupplierExecutionMode executionMode) override {
        unused(executionMode);
        if ((size_t)segment.ord >= perSegHits.size()) return nullptr;
        auto& hits = perSegHits[(size_t)segment.ord];
        if (hits.empty()) return nullptr;
        return target.make<Supplier>(
            std::span<const Hit>(hits.data(), hits.size()));
      }

      size_t retainedBytes() const noexcept override {
        size_t bytes = sizeof(*this) + perSegHits.capacity() * sizeof(perSegHits[0]);
        for (const auto& hits : perSegHits) bytes += hits.capacity() * sizeof(Hit);
        return bytes;
      }

      bool outputIsSubsetOfDomain() const noexcept override { return true; }
    };

  private:
    struct PreparedQuery {
      const float* query;
      uint8_t queryBits;
    };

    template <bool Groups> class SegmentFaissVectorEngine;

    template <bool Groups> struct EngineSlot {
      std::unique_ptr<VectorEngine> engine;
      int64_t vectorCount = 0;
      // Vectors the domain admits (exact: the domain bitmap's popcount, or
      // every vector when the segment has no deletes and no filter).
      int64_t eligibleVectorCount = 0;
      SegmentFaissVectorEngine<Groups>* ivfEngine = nullptr;
    };

  public:
    std::unique_ptr<Query::Weight::PreparedWeight> prepare(Query::Weight::PrepareContext& ctx) override {
      return query.groups ? prepareUnit<true>(ctx) : prepareUnit<false>(ctx);
    }

    template <bool Groups>
    std::unique_ptr<Query::Weight::PreparedWeight> prepareUnit(Query::Weight::PrepareContext& ctx) {
      prepareCallsForTests.fetch_add(1, std::memory_order_relaxed);
      IndexReader& reader = ctx.reader;
      auto segInfos = reader.field(query.getField());
      size_t numSegs = reader.segments().size();
      auto emptyResult = [&]() {
        if constexpr (Groups) if (ctx.warnings) ctx.warnings->warnOnce(
            "knn_group_underfill", std::format("field '{}': returned 0 of k={} groups; candidates_exhausted=true", query.field, query.k));
        return std::make_unique<KnnPreparedWeight>(std::vector<std::vector<Hit>>(numSegs));
      };
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
      // Per segment: vectors the domain admits, an upper bound on the docs
      // owning them, whether the domain is a real filter (not just
      // liveness), and its rank-space bitmap (null: every rank eligible).
      std::vector<int64_t> segEligibleCounts(numSegs, 0);
      std::vector<int64_t> segEligibleDocs(numSegs, 0);
      std::vector<char> segFiltered(numSegs, 0);
      std::vector<std::shared_ptr<const RankBitmap>> domainBitsPerSeg(numSegs);
      // vauxPerSeg: overlays that serve this query (none when exact);
      // cachePerSeg: overlays that cache its rank bitmaps (any query).
      std::vector<VectorAuxReader*> vauxPerSeg(numSegs, nullptr);
      std::vector<const VectorAuxReader*> cachePerSeg(numSegs, nullptr);
      std::string auxName("vec.");
      auxName.append(query.getField());
      std::vector<std::shared_ptr<AuxReader>> auxHolders(numSegs);
      for (size_t i = 0; i < numSegs; i++) {
        auxHolders[i] = reader.segments()[i].getAuxReader(auxName);
        if (!auxHolders[i]) continue;
        auto* vaux = dynamic_cast<VectorAuxReader*>(auxHolders[i].get());
        if (!vaux) {
          if (query.getExact()) continue;
          throw std::runtime_error(std::format(
            "KnnQuery: segment overlay '{}' is not a vector index (kind={})",
            auxName, auxHolders[i]->getKind()));
        }
        cachePerSeg[i] = vaux;
        if (!query.getExact()) vauxPerSeg[i] = vaux;
      }

      // Holds the multi-valued segments' valueRank->docId MonoReaders alive through
      // the engine search + result walk below.  MonoReader is
      // just pointers into the segment mmap (valid for the whole query), so copying it
      // out of the scratch VectorReader is safe and cheap.
      std::vector<std::optional<MonoReader>> monoHolders(numSegs);

      int64_t totalDocsWithValue = 0;
      bool anyMultiValued = false;
      int32_t dims = (int32_t)query.getQueryVec().size();
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
        if (segInfos.at(i) != nullptr) {
          // FIXED_SIZE vector column.  numVectors == #docs-with-field for
          // single-valued, or the total vector count for multi-valued.
          VectorReader vr(reader.segments()[i].postingsReader(), *segInfos.at(i));
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
            // Domain -> rank-space bitmap, built here while the scratch
            // column readers it walks are alive.  Cached on the segment's
            // overlay when there is one (also for exact queries, which
            // share it with later approximate ones).
            IndexReader::Segment& seg = reader.segments()[i];
            DocSet* domain = ctx.domainPerSeg.empty() ? nullptr : ctx.domainPerSeg[i];
            // Domain contract (Query.h): a present domain is live-filtered
            // and is the complete eligibility predicate; null means no
            // deletes and no filters.
            assert(domain != nullptr || seg.liveDocs() == nullptr);
            LiveDocs* live = seg.liveDocs();
            const VectorAuxReader* cache = cachePerSeg[i];
            int32_t lists = cache != nullptr ? cache->getNList() : 0;
            uint64_t liveGen = seg.segInfo.live_gen;
            if (domain == nullptr) {
              segEligibleCounts[i] = segCount;
              segEligibleDocs[i] = vr.docsWithValue();
            } else {
              if (live != nullptr && domain == &live->docset()) {
                auto build = [&]() { return buildRankLiveBitmap(seg, vr, segCount, lists); };
                domainBitsPerSeg[i] = cache != nullptr
                  ? cache->rankLiveBitmap(liveGen, build)
                  : std::make_shared<const RankBitmap>(build());
              } else {
                segFiltered[i] = 1;
                auto build = [&]() { return buildDomainRankBitmap(vr, segCount, *domain, lists); };
                domainBitsPerSeg[i] = cache != nullptr && ctx.domainKey != nullptr
                  ? cache->filterRankBitmap(*ctx.domainKey, liveGen, build)
                  : std::make_shared<const RankBitmap>(build());
              }
              const RankBitmap& bits = *domainBitsPerSeg[i];
              segEligibleCounts[i] = bits.count;
              segEligibleDocs[i] = bits.docs >= 0 ? bits.docs
                : std::min<int64_t>(bits.count, live != nullptr ? live->numLive() : bits.count);
            }
          }
        }
      }
      int64_t ntotal = 0;
      for (int64_t n : segVectorCounts) ntotal += n;
      if (ntotal == 0) {
        return emptyResult();
      }
      // Eligible vectors and an upper bound on eligible docs, over all
      // segments: the effort denominator and the doc-target cap.
      int64_t eligibleTotal = 0, eligibleDocsTotal = 0;
      for (size_t i = 0; i < numSegs; i++) {
        eligibleTotal += segEligibleCounts[i];
        eligibleDocsTotal += segEligibleDocs[i];
      }
      if (eligibleTotal == 0) {
        return emptyResult();
      }

      // Optionally normalize the query for COSINE - stored vectors were
      // normalized on write, or normalized by the builder for raw-column mode,
      // so we need a unit query for cosine = IP semantics to hold.  Done into a
      // local copy; the caller's span is unmodified.
      std::vector<float> queryBuf;
      const float* queryPtr = query.getQueryVec().data();
      if (metric == (int32_t)luxir::api::VectorMetric::COSINE) {
        queryBuf.assign(query.getQueryVec().begin(), query.getQueryVec().end());
        faiss::fvec_renorm_L2((size_t)dims, 1, queryBuf.data());
        queryPtr = queryBuf.data();
      }

      std::span<const SegV2D> v2dSpan(v2dPerSeg.data(), v2dPerSeg.size());

      int64_t kDocs = query.getK();
      // No round can pool more vectors than the domain admits.
      int64_t cap = std::min(eligibleTotal, std::max((int64_t)1, maxKnnCandidates));
      // exact is a contract: the maxKnnCandidates host heuristic must not
      // silently truncate the requested k (the explicit knob wins).  The
      // eligible count still bounds it - fewer eligible docs than k is not a
      // violation, the true top-k is simply all of them.
      if (query.getExact()) cap = std::min(eligibleTotal, std::max(cap, kDocs));
      // Round-1 IVF scan fraction, for the exact-fallback cost model.  The
      // composite engine derives the same value from the same inputs.
      double firstScanFraction = scanFraction(query.getNProbe(), defaultIvfNList(eligibleTotal),
                                              dims, query.getMinScanFraction());
      std::vector<EngineSlot<Groups>> slots;
      slots.reserve(numSegs);
      bool approximateEngine = false;
      // RaBitQ overlays index the rotated space: the query is rotated once
      // per distinct rotation (overlays of one dims share one interned
      // matrix) and outlives every engine built below.  Each inner buffer
      // keeps its address when the outer vector grows.
      std::vector<std::pair<const faiss::LinearTransform*, std::vector<float>>> rotatedQueries;
      auto rotatedQuery = [&](const faiss::LinearTransform* rotation) -> const float* {
        for (const auto& [rot, buf] : rotatedQueries) {
          if (rot == rotation) return buf.data();
        }
        auto& entry = rotatedQueries.emplace_back(rotation, std::vector<float>((size_t)dims));
        rotateVector(*rotation, queryPtr, entry.second.data());
        return entry.second.data();
      };
      // RaBitQ query quantization is a search-time knob: the current
      // schema's value applies to every overlay.
      int32_t rabitqQueryBits = query.getFieldType().ann().rabitqQueryBits;
      for (size_t i = 0; i < numSegs; i++) {
        int64_t segCount = segVectorCounts[i];
        if (segCount <= 0 || segInfos.at(i) == nullptr) continue;
        // Nothing eligible: no engine, no share of the IVF budget.
        if (segEligibleCounts[i] <= 0) continue;
        IndexReader::Segment& seg = reader.segments()[i];
        DocSet* domain = ctx.domainPerSeg.empty() ? nullptr : ctx.domainPerSeg[i];
        VectorAuxReader* vaux = vauxPerSeg[i];
        if (vaux != nullptr && segFiltered[i]) {
          // Few eligible rows: score them exactly from the column instead
          // of probing the overlay (exactCheaperThanIvf).
          auto* ivf = dynamic_cast<faiss::IndexIVF*>(vaux->getFaissIndex());
          if (ivf != nullptr && exactCheaperThanIvf(segEligibleCounts[i], segCount, vaux->getNList(),
                                                    ivf->code_size, dims, firstScanFraction)) {
            exactFallbackSegmentsForTests.fetch_add(1, std::memory_order_relaxed);
            normalizeColumnOnCosineRescore =
              normalizeColumnOnCosineRescore || vaux->shouldNormalizeColumnOnCosineRescore();
            vaux = nullptr;
          }
        }
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
          // Both codecs are IVF; each segment is served by its own overlay's
          // meta, so segments built with different codecs mix freely.
          bool isIvf = vaux->getEngine() == VectorAuxMeta::ENGINE_IVFPQ
            || vaux->getEngine() == VectorAuxMeta::ENGINE_IVFRABITQ;
          approximateEngine = approximateEngine || !vaux->scoresAreExact();
          const float* indexQuery = vaux->getRotation() != nullptr
            ? rotatedQuery(vaux->getRotation())
            : queryPtr;
          // domainBitsPerSeg[i] is shared with the engine, NOT moved: the
          // widen loop's foldSeen() seeds the eligible bitmaps from it.
          auto faissEngine = std::make_unique<SegmentFaissVectorEngine<Groups>>(
            *idx, (int32_t)i, domainBitsPerSeg[i],
            metric, vaux->scoresAreExact(), isIvf,
            vaux->getNList(), query.groups, v2dSpan,
            PreparedQuery{indexQuery, (uint8_t)rabitqQueryBits});
          SegmentFaissVectorEngine<Groups>* ivfEngine = isIvf ? faissEngine.get() : nullptr;
          slots.push_back({
            .engine = std::move(faissEngine),
            .vectorCount = segCount,
            .eligibleVectorCount = segEligibleCounts[i],
            .ivfEngine = ivfEngine,
          });
        } else {
          slots.push_back({
            .engine = std::make_unique<SegmentFlatColumnVectorEngine<Groups>>(
              (int32_t)i, seg, segInfos.at(i), v2dPerSeg[i], domain, domainBitsPerSeg[i].get(),
              dims, metric, normalizeColumnOnCosineRescore, query.groups, v2dSpan),
            .vectorCount = segCount,
            .eligibleVectorCount = segEligibleCounts[i],
            .ivfEngine = nullptr,
          });
        }
      }
      if (slots.empty()) {
        return emptyResult();
      }

      int64_t avgMult = (!Groups && approximateEngine && anyMultiValued && totalDocsWithValue > 0)
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
      int64_t kReq = Groups ? std::min(baseReq, cap) : mulClamped(baseReq, avgMult, cap);
      if (kReq <= 0) return emptyResult();
      // Capped by the eligible docs: a filter matching fewer docs than the
      // target stops widening once they are pooled instead of deepening
      // until every list is scanned.
      int64_t targetDocReq = approximateEngine
        ? std::min<int64_t>(eligibleDocsTotal, std::max(kDocs, baseReq))
        : kDocs;

      std::vector<VectorEngineHit> candidateHits;
      std::conditional_t<Groups, std::unique_ptr<BoundedHits<true, VectorEngineHit, betterHit>>, NoGroupState> groupPool;
      std::conditional_t<Groups, std::unique_ptr<BoundedHits<true, VectorEngineHit, betterHit>>, NoGroupState> exactPool;
      int64_t perKey = 1;
      if constexpr (Groups) {
        perKey = approximateEngine ? membersPerGroup(baseReq, kDocs) : 1;
        groupPool = std::make_unique<BoundedHits<true, VectorEngineHit, betterHit>>(std::min(baseReq, cap), query.groups, v2dSpan, (size_t)perKey);
        exactPool = std::make_unique<BoundedHits<true, VectorEngineHit, betterHit>>(std::min(kDocs, cap), query.groups, v2dSpan);
      }
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
      if constexpr (!Groups) seenDocs.reserve(maxDocHits);

      std::unique_ptr<VectorEngine> baseEngine =
        std::make_unique<CompositeVectorEngine<Groups>>(std::move(slots), ntotal, eligibleTotal, kDocs,
                                                metric, ctx.parallel, query.groups, v2dSpan);
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
      // domain bitmap when it has one, else all-ones; pooled ranks
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
          if (domainBitsPerSeg[i] != nullptr) {
            // Seeded from the domain bitmap, so the domain (filter and
            // liveness) rides along in the one bitmap FAISS probes.
            size_t numBytes = ((size_t)n + 7) / 8;
            assert(domainBitsPerSeg[i]->numBytes == numBytes);
            bits.assign(domainBitsPerSeg[i]->bits, domainBitsPerSeg[i]->bits + numBytes);
          } else {
            // No domain (no deletes, no filter): not-pooled only.
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
        // domain seed or at least one pooled rank. An all-ones span would
        // demote a selector-free scan to a per-vector always-true probe.
        for (size_t i = 0; i < numSegs; i++) {
          if (!eligibleStore[i].empty()
              && (domainBitsPerSeg[i] != nullptr || cleared[i])) {
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

      bool candidatesExhausted = true;

      // WIDEN loop: pool candidates until targetDocReq distinct live docs (or
      // a pool/breadth/cap limit).  Two deepen axes, tried in order:
      //   DEPTH   - the engine pool was not exhausted at this candidate count:
      //             grow kReq (more candidates from the same lists / scan).
      //   BREADTH - pool exhausted but more IVF lists remain: grow breadth
      //             (the engine allocates the next globally-best lists).
      // The whole loop runs on ENGINE scores (exact for column slots,
      // approximate for IVF overlays, possibly above 1 for L2): the stopping
      // condition is a distinct-doc COUNT, which approximate scores answer
      // just as well, so exact rescore
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
          .perKey = perKey,
        };
        VectorSearchResult result = engine->search(request);
        if (statsForTests) statsForTests->rounds++;

        candidatesExhausted = result.poolExhausted && result.breadthExhausted;
        size_t appendStart = candidateHits.size();
        bool freshDocuments = false;
        std::vector<char> roundExactSeg;
        if (!result.scoresAreExact && !result.exactSegOrds.empty()) {
          roundExactSeg.assign(numSegs, 0);
          for (int32_t s : result.exactSegOrds) {
            if (s >= 0 && (size_t)s < numSegs) roundExactSeg[(size_t)s] = 1;
          }
        }
        for (const auto& hit : result.hits) {
          if constexpr (!Groups) if (candidateHits.size() >= maxVectorHits) break;
          if (markSeen(hit.segOrd, hit.valueRank)) {
            if constexpr (Groups) {
              freshDocuments = true;
              bool exactHit = result.scoresAreExact || (!roundExactSeg.empty() && roundExactSeg[(size_t)hit.segOrd]);
              if (exactHit) exactPool->offer(hit);
              else groupPool->offer(hit);
            }
            else candidateHits.push_back(hit);
            if (!result.scoresAreExact
                && (roundExactSeg.empty() || !roundExactSeg[(size_t)hit.segOrd])) {
              segApproxInPool[(size_t)hit.segOrd] = 1;
            }
          }
        }
        if constexpr (Groups) {
          candidateHits = groupPool->take();
          auto exactHits = exactPool->take();
          candidateHits.insert(candidateHits.end(), exactHits.begin(), exactHits.end());
        }
        bool appended = Groups ? freshDocuments : candidateHits.size() != appendStart;
        if (appended) {
          candidatePoolSorted = false;
          if (!result.scoresAreExact) candidateScoresExact = false;
        }

        if (!candidatePoolSorted) {
          if constexpr (!Groups) sortCandidatesByScore(candidateHits);
          candidatePoolSorted = true;
        }
        collapseCandidates(candidateHits, v2dSpan, targetDocReq, docHits, seenDocs);

        // Exact witnesses bypass the approximate budget, including its fill
        // test: otherwise overlapping keys could fill R documents across the
        // two channels without reaching k groups under the approximate cap.
        int64_t pooled = (int64_t)docHits.size();
        size_t poolSize = candidateHits.size();
        if constexpr (Groups) if (approximateEngine) {
          pooled = (int64_t)groupPool->size();
          poolSize = groupPool->size();
        }
        if (pooled >= targetDocReq) break;
        if (poolSize >= maxVectorHits) break;

        // DEPTH: document mode projects the pool size needed for targetDocReq
        // docs from observed docs-per-candidate yield, then requests only
        // the INCREMENT over the banked pool: post-fold every engine returns
        // fresh hits on top of it (eligible exclusion), so re-requesting the
        // cumulative depth would overshoot the projection by the pool size,
        // compounding across rounds.  Floored at the doc deficit so a
        // degenerate projection still progresses.  `appended` is the
        // termination backstop: a round that added nothing fresh and still
        // claims an unexhausted pool (a conforming engine cannot - fresh
        // hits or poolExhausted) must not loop at the same shape.
        if (!result.poolExhausted && appended
            && (int64_t)poolSize < cap) {
          if constexpr (Groups) {
            // A deficit-sized fresh cut can spend every slot on keys already
            // at their cap in the pool. A full R cut avoids that starvation:
            // if it fills, union-then-recut can fill R pooled documents too.
            kReq = std::min(baseReq, cap);
          } else {
            int64_t pool = (int64_t)candidateHits.size();
            int64_t want = projectedKReq(pool, targetDocReq, (int64_t)docHits.size(), cap);
            kReq = std::min(cap - pool,
                            std::max(targetDocReq - (int64_t)docHits.size(),
                                     want - pool));
          }
          foldSeen();
          continue;
        }

        // BREADTH: at least the engine's next-list hint, and geometric from
        // the observed yield (pooled docs per breadth scanned so far), so a
        // selective filter reaches the breadth its yield needs in a few
        // rounds instead of one reference list per round.
        if (result.poolExhausted && !result.breadthExhausted && breadth < maxBreadth) {
          int32_t nextBreadth = result.nextBreadth > breadth ? result.nextBreadth : breadth + 1;
          nextBreadth = std::max(nextBreadth,
              projectedBreadth(result.scannedBreadth, pooled, targetDocReq));
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
        rescoreAndCollapseCandidates<Groups>(candidateHits,
                                     reader,
                                     segInfos,
                                     v2dSpan,
                                     std::span<const char>(segApproxInPool.data(), segApproxInPool.size()),
                                     queryPtr, dims, metric, normalizeColumnOnCosineRescore,
                                     kDocs, docHits, ctx.parallel, query.groups);
      } else if constexpr (Groups) {
        // All-exact parts still require a global cap-one group cut. Reuse
        // the exact pool instead of materializing another collection ladder.
        docHits.clear();
        for (const auto& hit : exactPool->take()) docHits.push_back({hit.segOrd, v2dSpan[(size_t)hit.segOrd].resolve(hit.valueRank), hit.score});
        std::sort(docHits.begin(), docHits.end(), betterDoc);
      } else if ((int64_t)docHits.size() > kDocs) {
        collapseCandidates(candidateHits, v2dSpan, kDocs, docHits, seenDocs);
      }

      if constexpr (Groups) {
        candidatesExhausted &= !groupPool->truncated && !exactPool->truncated;
        if ((int64_t)docHits.size() < kDocs && ctx.warnings) {
          ctx.warnings->warnOnce("knn_group_underfill", std::format(
              "field '{}': returned {} of k={} groups; candidates_exhausted={}",
              query.field, docHits.size(), kDocs, candidatesExhausted));
        }
      }

      // The similarity threshold drops hits from the k nearest rather than
      // reaching past them, so fewer than k docs may remain (Elasticsearch
      // knn.similarity semantics). Scores are exact here: the refine pass
      // (or an all-exact pool) produced them.
      if (query.getSimilarity().has_value()) {
        float floor = query.similarityScoreFloor();
        std::erase_if(docHits, [floor](const DocHit& hit) {
          return !(hit.score >= floor);
        });
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

    Query::ScorerSupplier* scorerSupplierImpl(
        MemPool& target, IndexReader::Segment& segment,
        Query::SupplierExecutionMode executionMode) override {
      unused(target, segment, executionMode);
      return nullptr;
    }

  private:
    /// Per-segment adapter over a deserialized FAISS aux index.  FAISS ids are
    /// segment-local valueRanks, mapped to docIds through SegV2D by the
    /// selector and by the host after collapse.
    ///
    /// IVF indexes do not drive themselves: the CompositeVectorEngine's global
    /// allocator calls coarseRun() (full sorted centroid ranking) and
    /// eligibleListSize() to pick lists across ALL segments, then hands the chosen
    /// lists back through searchSelectedLists().  The plain search() override
    /// is the whole-index path for non-IVF FAISS kinds; no such aux kind
    /// exists today (both codecs are IVF), so it is reserved seam
    /// surface, not a live code path.
    template <bool Groups> class SegmentFaissVectorEngine final : public VectorEngine {
      const CollapsePlan* groupPlan;
      std::span<const SegV2D> groupV2d;

      faiss::Index& index;
      faiss::IndexIVF* ivf;
      int32_t segOrd;
      // The domain in rank space (liveness, or a filter with liveness folded
      // in), shared from the overlay's cache or built for this query; null
      // when the segment has no deletes and no filter.  The selector and the
      // allocator's per-list eligible counts both read it.
      std::shared_ptr<const RankBitmap> domainBits;
      int32_t metric;
      bool exactScores;
      bool isIvf;
      int32_t nlist;
      // Owned by prepareUnit(): canonical for PQ, rotated for RaBitQ.
      const PreparedQuery preparedQuery;

      /// The ONLY selector policy: built per scan call from (domainBits,
      /// request.eligible), so round 1 and deepen rounds cannot drift apart.
      /// Always a bitmap, one bit probe per scanned id:
      ///   eligible present -> IDSelectorBitmap over it (it is seeded from
      ///                       domainBits, so the domain rides along)
      ///   domainBits       -> IDSelectorBitmap over it
      ///   neither          -> null (FAISS compiles the per-vector check out)
      /// IDSelectorBitmap rejects ids past its bytes, so a corrupt id from the
      /// zero-copy mmapped lists cannot read past the bitmap.  Per-call
      /// construction is two stores; owns the selector storage, so keep it
      /// alive across the FAISS call (copy/move deleted to enforce that
      /// selPtr never outlives the optional it points into).
      class RoundSelector {
        std::optional<faiss::IDSelectorBitmap> bitmap;
        const faiss::IDSelector* selPtr = nullptr;

      public:
        RoundSelector(const SegmentFaissVectorEngine& eng,
                      const VectorSearchRequest& request) {
          std::span<const uint8_t> elig =
            (size_t)eng.segOrd < request.eligible.size()
              ? request.eligible[(size_t)eng.segOrd]
              : std::span<const uint8_t>{};
          if (!elig.empty()) {
            bitmap.emplace(elig.size(), elig.data());
            selPtr = &*bitmap;
          } else if (eng.domainBits != nullptr) {
            bitmap.emplace(eng.domainBits->numBytes, eng.domainBits->bits);
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

      struct ScanContext {
        RoundSelector selector;
        const float* const query;
        // PQ ignores the RaBitQ fields. sel points into selector, so this
        // context must stay at its construction address throughout the scan.
        faiss::IVFRaBitQSearchParameters params;

        ScanContext(const SegmentFaissVectorEngine& eng,
                    const VectorSearchRequest& request, size_t listCount)
            : selector(eng, request), query(eng.preparedQuery.query) {
          params.sel = selector.get();
          params.nprobe = listCount;
          params.qb = eng.preparedQuery.queryBits;
          // max_codes stays 0: scan every selected list in full.
        }

        ScanContext(const ScanContext&) = delete;
        ScanContext& operator=(const ScanContext&) = delete;
        ScanContext(ScanContext&&) = delete;
        ScanContext& operator=(ScanContext&&) = delete;
      };

      ScanContext prepareScan(const VectorSearchRequest& request, size_t listCount) const {
        return ScanContext(*this, request, listCount);
      }

      struct GroupResultHandler final : faiss::ResultHandler {
        BoundedHits<true, VectorEngineHit, betterHit>& heap;
        int32_t metric, segment;
        GroupResultHandler(decltype(heap) heap, int32_t metric, int32_t segment)
            : heap(heap), metric(metric), segment(segment) { updateThreshold(); }
        void updateThreshold() {
          const auto* floor = heap.floor();
          threshold = metric == (int32_t)api::VectorMetric::L2
              ? std::numeric_limits<float>::infinity() : -std::numeric_limits<float>::infinity();
          if (floor == nullptr) return;
          // FAISS's strict distance test must admit equal exposed scores
          // for our document tie. Invert one score ULP outward, then round
          // the distance outward too (L2's score conversion rounds twice).
          float score = std::nextafter(floor->score, -std::numeric_limits<float>::infinity());
          if (metric == (int32_t)api::VectorMetric::L2) {
            if (score > 0) {
              float distance = score >= 1.0f ? 1.0f - score : 1.0f / score - 1.0f;
              threshold = std::nextafter(distance, std::numeric_limits<float>::infinity());
            }
          } else threshold = score;
        }
        bool add_result(float distance, faiss::idx_t rank) override {
          bool admitted = heap.offer({scoreFromDist(distance, metric), segment, rank});
          if (admitted) updateThreshold();
          return admitted;
        }
      };

    public:
      SegmentFaissVectorEngine(faiss::Index& index,
                               int32_t segOrd,
                               std::shared_ptr<const RankBitmap> domainBits,
                               int32_t metric,
                               bool exactScores,
                               bool isIvf,
                               int32_t nlist, const CollapsePlan* groupPlan,
                               std::span<const SegV2D> groupV2d,
                               PreparedQuery preparedQuery)
        : groupPlan(groupPlan), groupV2d(groupV2d), index(index),
          ivf(isIvf ? dynamic_cast<faiss::IndexIVF*>(&index) : nullptr),
          segOrd(segOrd),
          domainBits(std::move(domainBits)),
          metric(metric),
          exactScores(exactScores),
          isIvf(isIvf),
          nlist(nlist),
          preparedQuery(preparedQuery) {
        assert(preparedQuery.query != nullptr);
        if (isIvf && ivf == nullptr) {
          throw std::runtime_error("KnnQuery: vector aux marked IVF but FAISS index is not IndexIVF");
        }
      }

      int32_t getNList() const noexcept { return nlist; }

      void coarseRun(int32_t dims,
                     std::vector<faiss::idx_t>& listIds,
                     std::vector<float>& centroidDists) const {
        assert(ivf != nullptr);
        assert(dims == index.d);
        unused(dims);
        listIds.assign((size_t)nlist, (faiss::idx_t)-1);
        centroidDists.assign((size_t)nlist, 0.0f);
        // The rotation is orthonormal, so these distances compare directly
        // with other segments' (rotated or not) in the global list ranking.
        ivf->quantizer->search(1, preparedQuery.query, nlist, centroidDists.data(), listIds.data());
      }

      /// Eligible vector count of one IVF list - the unit the global
      /// allocator charges against the scan-fraction budget (cost =
      /// eligibleListSize / eligible N) and the test that skips a list
      /// outright when it is zero.  Uses the ACTUAL list contents so list
      /// imbalance and filter skew price correctly (full selection always
      /// sums to the eligible N).  With a domain bitmap the count is read
      /// from its per-list cache, counted on first use by testing each id's
      /// bit (no rank -> doc resolve).
      int64_t eligibleListSize(faiss::idx_t listId) const {
        assert(ivf != nullptr);
        if (listId < 0 || listId >= nlist) return 0;
        if (domainBits == nullptr) return (int64_t)ivf->invlists->list_size((size_t)listId);
        return domainBits->listCount(*ivf->invlists, listId);
      }

      /// Scan a contiguous run of pre-selected IVF lists.  const and
      /// thread-safe: the composite engine runs several disjoint list-ranges
      /// of one segment as concurrent tasks, so all scratch is local and the
      /// shared state (index, selector bitmap) is read-only.
      /// rangeEligibleVectors is the eligible count of exactly the lists in
      /// listIds.
      VectorSearchResult searchSelectedLists(const VectorSearchRequest& request,
                                             std::span<const faiss::idx_t> listIds,
                                             std::span<const float> centroidDists,
                                             int64_t rangeEligibleVectors) const {
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

        auto scan = prepareScan(request, listIds.size());
        if constexpr (Groups) {
          std::unique_ptr<faiss::InvertedListScanner> scanner(
              ivf->get_InvertedListScanner(false, scan.params.sel, &scan.params));
          scanner->set_query(scan.query);
          BoundedHits<true, VectorEngineHit, betterHit> heap(request.candidates, groupPlan, groupV2d,
              exactScores ? 1 : (size_t)request.perKey);
          GroupResultHandler handler(heap, metric, segOrd);
          for (size_t list = 0; list < listIds.size(); list++) {
            auto id = listIds[list];
            if (id < 0) continue;
            scanner->set_list(id, centroidDists[list]);
            faiss::InvertedLists::ScopedIds ids(ivf->invlists, (size_t)id);
            faiss::InvertedLists::ScopedCodes codes(ivf->invlists, (size_t)id);
            size_t count = ivf->invlists->list_size((size_t)id);
            // IVFPQ applies its selector and threshold in the optimized loop.
            scanner->scan_codes(count, codes.get(), ids.get(), handler);
          }
          if (statsForTests) {
            statsForTests->lists += (int64_t)listIds.size();
            statsForTests->vectors += (int64_t)handler.stats.scan_cnt;
            statsForTests->bytes += (int64_t)handler.stats.scan_cnt * (int64_t)ivf->code_size;
          }
          // Codes pruned inside FAISS cannot have improved this cut, but a
          // deeper round could admit them. Conservatively retain that signal.
          heap.truncated |= heap.size() == (size_t)request.candidates;
          result.hits = heap.take();
          std::sort(result.hits.begin(), result.hits.end(), betterHit);
          result.poolExhausted = !heap.truncated;
          return result;
        }

        std::vector<faiss::idx_t> ids((size_t)request.candidates, (faiss::idx_t)-1);
        std::vector<float> dists((size_t)request.candidates, 0.0f);

        // Local stats sink, discarded: with a null stats argument FAISS does
        // plain non-atomic '+=' on the global faiss::indexIVF_stats, and
        // concurrent chunk tasks of one query would race on it.
        faiss::IndexIVFStats stats;
        ivf->search_preassigned(1, scan.query, request.candidates,
                                listIds.data(), centroidDists.data(),
                                dists.data(), ids.data(), false, &scan.params,
                                &stats);
        if (statsForTests) {
          statsForTests->lists += (int64_t)listIds.size();
          statsForTests->vectors += (int64_t)stats.ndis;
          statsForTests->bytes += (int64_t)stats.ndis * (int64_t)ivf->code_size;
        }

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
          || rangeEligibleVectors <= request.candidates;
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

        if constexpr (Groups) {
          RoundSelector selector(*this, request);
          BoundedHits<true, VectorEngineHit, betterHit> heap(request.candidates, groupPlan, groupV2d,
              exactScores ? 1 : (size_t)request.perKey);
          GroupResultHandler handler(heap, metric, segOrd);
          faiss::SearchParameters params;
          params.sel = selector.get();
          index.search1(request.query, handler, &params);
          heap.truncated |= heap.size() == (size_t)request.candidates;
          VectorSearchResult result;
          result.hits = heap.take();
          std::sort(result.hits.begin(), result.hits.end(), betterHit);
          result.scoresAreExact = exactScores;
          result.poolExhausted = !heap.truncated;
          return result;
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

    template <bool Groups> class SegmentFlatColumnVectorEngine final : public VectorEngine {
      const CollapsePlan* groupPlan;
      std::span<const SegV2D> groupV2d;

      int32_t segOrd;
      IndexReader::Segment& seg;
      const SegFieldInfo* segInfo;
      const SegV2D& v2d;
      // The domain DocSet feeds only the group key walk; eligibility is
      // domainBits, its rank-space form (null when the domain is null: no
      // deletes, no filter).  Owned by prepareUnit(), which outlives this.
      DocSet* domain;
      const RankBitmap* domainBits;
      int32_t dims;
      int32_t metric;
      bool normalizeColumnOnCosineRescore;

    public:
      SegmentFlatColumnVectorEngine(int32_t segOrd,
                                    IndexReader::Segment& seg,
                                    const SegFieldInfo* segInfo,
                                    const SegV2D& v2d,
                                    DocSet* domain,
                                    const RankBitmap* domainBits,
                                    int32_t dims,
                                    int32_t metric,
                                    bool normalizeColumnOnCosineRescore, const CollapsePlan* groupPlan,
                                    std::span<const SegV2D> groupV2d) noexcept
        : groupPlan(groupPlan), groupV2d(groupV2d), segOrd(segOrd),
          seg(seg),
          segInfo(segInfo),
          v2d(v2d),
          domain(domain),
          domainBits(domainBits),
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

        BoundedHits<Groups, VectorEngineHit, betterHit> heap(request.candidates, groupPlan, groupV2d);
        int64_t eligibleDocs = 0;
        int64_t scanned = 0;
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
        assert(domainBits == nullptr || domainBits->numBytes == ((size_t)numVals + 7) / 8);
        const uint8_t* domBytes = domainBits != nullptr ? domainBits->bits : nullptr;
        size_t domNumBytes = domainBits != nullptr ? domainBits->numBytes : 0;
        // 64 ranks of the domain AND the not-pooled bits.
        auto eligibleWord = [&](size_t w) LUXIR_INLINE {
          uint64_t word = domBytes != nullptr ? rankbits::loadWord(domBytes, domNumBytes, w) : ~0ULL;
          if (!elig.empty()) word &= rankbits::loadWord(elig.data(), elig.size(), w);
          return word;
        };

        // Read-ahead (see RowReadAhead); a filtered scan advises only the rows
        // of eligible ranks, one bit test each.
        StrColReader& col = vr.strColReader();
        RowReadAhead::Gate gate;
        RowReadAhead ahead(gate, col.valuesFile(), col.valuesOffset(),
                           (size_t)col.fixedValueSize(), numVals);
        auto rankEligible = [&](int64_t r) {
          return ((eligibleWord((size_t)(r >> 6)) >> (r & 63)) & 1) != 0;
        };
        auto readAhead = [&](int64_t rank) LUXIR_INLINE {
          if (domBytes == nullptr && elig.empty()) ahead.scanAt(rank);
          else ahead.scanAt(rank, rankEligible);
        };

        // Inline run-collapse: value ranks are doc-ordered (the valDoc map is
        // monotonic; sparse select and dense identity likewise), so a doc's
        // vectors form a contiguous run and max-sim collapse falls out of one
        // pass.  Eligibility comes from the rank bitmaps, never a per-row
        // domain lookup: all-zero words are skipped whole and all-ones words
        // score their 64 rows without testing a bit.
        auto scan = [&](auto emit) {
          if (v2d.mono != nullptr) {
            auto emitRun = [&](int32_t docId, int64_t rank, int64_t end) LUXIR_INLINE {
              readAhead(rank);
              if (pooledInRun(rank, end)) return;
              eligibleDocs++;
              emit(docId, [&]() LUXIR_INLINE {
                if constexpr (Groups) scanned += end - rank;
                auto [bestRank, bestScore] = bestInRun(rank, end, [&](int64_t r) {
                  return exactScore(request.query, vr.vectorAtRank(r), dims, metric,
                                    normalizeColumnOnCosineRescore, 0.0f);
                });
                return VectorEngineHit{bestScore, segOrd, bestRank};
              });
            };
            MonoReader::BulkValues valDoc(*v2d.mono);
            if (domBytes == nullptr) {
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
                emitRun(docId, rank, end);
                rank = end;
              }
            } else {
              // The domain is doc level, so an eligible doc's run is all set
              // bits; jump between set bits and walk each run's extent.
              int64_t rank = rankbits::nextSet(domBytes, domNumBytes, numVals, 0);
              while (rank < numVals) {
                int32_t docId = (int32_t)valDoc.valueAt(rank);
                int64_t end = rank + 1;
                while (end < numVals && domainBits->test(end)
                       && (int32_t)valDoc.valueAt(end) == docId) {
                  end++;
                }
                emitRun(docId, rank, end);
                rank = rankbits::nextSet(domBytes, domNumBytes, numVals, end);
              }
            }
          } else {
            // Single-valued (dense identity or sparse select): one-vector
            // runs.  Docs mode never needs the docId; groups resolve it.
            auto visit = [&](int64_t rank) LUXIR_INLINE {
              readAhead(rank);
              eligibleDocs++;
              int32_t docId = Groups ? v2d.resolve(rank) : -1;
              emit(docId, [&]() LUXIR_INLINE {
                if constexpr (Groups) scanned++;
                return VectorEngineHit{exactScore(request.query, vr.vectorAtRank(rank), dims, metric,
                    normalizeColumnOnCosineRescore, 0.0f), segOrd, rank};
              });
            };
            size_t numWords = ((size_t)numVals + 63) / 64;
            for (size_t w = 0; w < numWords; w++) {
              uint64_t word = eligibleWord(w);
              int64_t base = (int64_t)(w * 64);
              if (numVals - base < 64) word &= (1ULL << (numVals - base)) - 1;
              if (word == 0) continue;
              if (word == ~0ULL) {
                for (int64_t rank = base; rank < base + 64; rank++) visit(rank);
                continue;
              }
              do {
                visit(base + std::countr_zero(word));
                word &= word - 1;
              } while (word != 0);
            }
          }
        };
        if constexpr (Groups) {
          // Exact vectors arrive in doc order. Join one bulk key walk to
          // that stream; drop-policy members are rejected before scoring.
          CollapseKeyReader keys(*groupPlan);
          keys.withKeyWalk(segOrd, domain, [&](auto keyAt) {
            scan([&](int32_t doc, auto makeHit) LUXIR_INLINE {
              auto hit = makeHit();
              if (!heap.competitive(hit)) return;
              const auto* key = keyAt(doc);
              if (key != nullptr) heap.offerKey(hit, *key, doc);
            });
          });
        } else scan([&](int32_t doc, auto makeHit) LUXIR_INLINE {
          unused(doc);
          heap.offer(makeHit());
        });

        if (statsForTests) {
          int64_t count = Groups ? scanned : eligibleDocs;
          statsForTests->vectors += count;
          statsForTests->bytes += count * dims * (int64_t)sizeof(float);
        }
        result.hits = heap.take();
        std::sort(result.hits.begin(), result.hits.end(), betterHit);
        if constexpr (Groups) result.poolExhausted = !heap.truncated;
        else result.poolExhausted = eligibleDocs <= request.candidates;
        return result;
      }
    };

    /// One VectorEngine over all per-segment slots.  Owns the cross-segment
    /// policy so individual segments never self-allocate effort:
    ///
    ///   - Budget: request.breadth (nprobe in reference-index units) becomes
    ///     a scan fraction P = breadth / defaultIvfNList(eligibleN), floored
    ///     by min_scan_fraction (scanFraction).  Denominating effort in
    ///     fraction-of-index makes it MERGE-STABLE: a raw per-segment list
    ///     count would scan ~sqrt(2)x more before a merge than after, so a
    ///     tuned query would degrade on the next merge through no data
    ///     change.  eligibleN is the live vectors for an unfiltered query and
    ///     the filter's eligible vectors otherwise.
    ///   - Allocation: every IVF segment contributes its full centroid-sorted
    ///     list ranking; a lazy k-way merge pops the globally best list and
    ///     charges its eligible size / eligibleN until P is spent.  Small
    ///     segments thus get small (or zero) shares instead of equal work, and
    ///     lists without an eligible member are passed over unscanned.
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
    template <bool Groups> class CompositeVectorEngine final : public VectorEngine {
      const CollapsePlan* groupPlan;
      std::span<const SegV2D> groupV2d;

      std::vector<EngineSlot<Groups>> slots;
      int64_t totalVectors;
      int64_t eligibleTotalVectors;
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
        referenceBreadth = defaultIvfNList(eligibleTotalVectors);
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
          if (ivf == nullptr || slots[i].eligibleVectorCount <= 0 || ivf->getNList() <= 0) continue;
          ivf->coarseRun(request.dims, runs[i].listIds, runs[i].centroidDists);
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

      // See scanFraction for the default and its merge stability.
      double budgetForRequest(const VectorSearchRequest& request) const {
        return scanFraction(request.breadth, referenceBreadth, request.dims, request.minScanFraction);
      }

      int32_t breadthForBudget(double p) const {
        if (p <= 0.0) return 1;
        double breadth = std::ceil(p * (double)std::max(1, referenceBreadth));
        if (breadth >= (double)std::numeric_limits<int32_t>::max()) {
          return std::numeric_limits<int32_t>::max();
        }
        return std::max(1, (int32_t)breadth);
      }

      using CoarsePQ = IndexedPQ<CoarseHead, CoarseHeadLess, int32_t>;

      CoarsePQ coarsePQ() {
        return CoarsePQ(std::span<CoarseHead>(heads.data(), heads.size()),
                        std::span<int32_t>(heapIndexes.data(), heapIndexes.size()), heapSize);
      }

      // Advance the top slot's cursor past its head list (replace-top, one
      // sift), dropping the slot once its ranking is spent.
      void popHead(CoarsePQ& pq) {
        int32_t slotOrd = pq.indexOfTop();
        auto& cursor = cursors[(size_t)slotOrd];
        const auto& run = runs[(size_t)slotOrd];
        cursor++;
        while (cursor < run.listIds.size() && run.listIds[cursor] < 0) cursor++;
        if (cursor < run.listIds.size()) {
          pq.top() = makeHead(slotOrd);
          pq.updateTop();
        } else {
          pq.removeTopIndex();
        }
      }

      // Pass over lists with no eligible member: they cost nothing, add
      // nothing, and must not be scanned.  Run before every head is used, so
      // the next-list hint and breadthExhausted see only useful lists.
      void skipEmptyHeads(CoarsePQ& pq) {
        while (pq.size() > 0
               && slots[(size_t)pq.indexOfTop()].ivfEngine->eligibleListSize(pq.top().listId) == 0) {
          popHead(pq);
        }
      }

      // Extend the global allocation until the requested scan fraction is
      // covered.  Lazy k-way merge over all slots' centroid-sorted runs: pop
      // the globally closest list, charge its eligible size / eligibleN,
      // advance that slot's cursor (replace-top, one sift), repeat.
      // Resumable: cursors and allocatedCost persist, so a deepen round
      // allocates only the increment.  The epsilon absorbs float accumulation
      // when a fraction boundary lands exactly on a list edge.
      void allocateToBudget(const VectorSearchRequest& request) {
        ensureCoarseReady(request);
        if (!hasIvfSlots || heapSize <= 0) return;

        double target = budgetForRequest(request);
        CoarsePQ pq = coarsePQ();
        skipEmptyHeads(pq);
        while (pq.size() > 0 && allocatedCost + 1.0e-15 < target) {
          int32_t slotOrd = pq.indexOfTop();
          faiss::idx_t listId = pq.top().listId;
          int64_t eligibleSize = slots[(size_t)slotOrd].ivfEngine->eligibleListSize(listId);

          selectedListIds[(size_t)slotOrd].push_back(listId);
          selectedCentroidDists[(size_t)slotOrd].push_back(
            runs[(size_t)slotOrd].centroidDists[cursors[(size_t)slotOrd]]);
          selectedListLiveSizes[(size_t)slotOrd].push_back(eligibleSize);
          if (eligibleTotalVectors > 0) {
            allocatedCost += (double)eligibleSize / (double)eligibleTotalVectors;
          }
          popHead(pq);
          skipEmptyHeads(pq);
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
        CoarsePQ pq = coarsePQ();
        int32_t slotOrd = pq.indexOfTop();
        int64_t eligibleSize = slots[(size_t)slotOrd].ivfEngine->eligibleListSize(pq.top().listId);
        double nextCost = eligibleTotalVectors > 0
          ? (double)eligibleSize / (double)eligibleTotalVectors
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
        BoundedHits<Groups, VectorEngineHit, betterHit> approxHeap;
        std::vector<VectorEngineHit> exactHits;
        // Pre-cut count of approximate hits offered to the heap; together
        // with partsExhausted it decides the round's poolExhausted.
        int64_t approxReturned = 0;
        bool anyApprox = false;
        bool partsExhausted = true;

        ScanAccumulator(int64_t heapLimit, const CollapsePlan* plan, std::span<const SegV2D> v2d, size_t perKey)
            : heapLimit(heapLimit), approxHeap(heapLimit, plan, v2d, perKey) {}

        static ScanAccumulator* merge(ScanAccumulator* a, ScanAccumulator* b) {
          if (a->approxHeap.size() < b->approxHeap.size()) std::swap(a, b);
          a->approxHeap.merge(b->approxHeap);
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
            acc.approxHeap.offer(hit);
          }
        }
        // Group exact parts already return their local top k groups. More
        // exact depth cannot improve the global exact bank, and must not
        // delay breadth deepening of an exhausted approximate part.
        if constexpr (Groups) {
          if (!part.scoresAreExact) acc.partsExhausted &= part.poolExhausted;
        } else acc.partsExhausted &= part.poolExhausted;
      }

    public:
      CompositeVectorEngine(std::vector<EngineSlot<Groups>>&& slots, int64_t totalVectors,
                            int64_t eligibleTotalVectors, int64_t kDocs, int32_t metric,
                            bool parallel, const CollapsePlan* groupPlan,
                            std::span<const SegV2D> groupV2d) noexcept
        : groupPlan(groupPlan), groupV2d(groupV2d), slots(std::move(slots)),
          totalVectors(totalVectors),
          eligibleTotalVectors(eligibleTotalVectors),
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
        // Scanning only the new tail on breadth rounds keeps total IVF list
        // scanning O(lists). A breadth round is only entered
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
        // What must NOT happen on the depth axis is skipping already-scanned
        // lists - that silently loses recall. EXCLUDING the host's
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
            units.push_back({(int32_t)i, 0, 0, slot.eligibleVectorCount, candidates, false});
          }
        }

        if (!units.empty()) {
          AtomicMerger<ScanAccumulator> merger(
            [&, limit = request.candidates]() { return new ScanAccumulator(limit, groupPlan, groupV2d, request.perKey); },
            [](ScanAccumulator* acc) { delete acc; });
          // Each search() round is a barrier: the host's deepen decision
          // needs the whole merged pool. The caller runs the final unit;
          // TaskGroupRunner cancels and drains siblings if it throws.
          bool spawnTasks = parallel && units.size() > 1;
          if (spawnTasks) {
            parallelScanRoundsForTests.fetch_add(1, std::memory_order_relaxed);
          }
          TaskGroupRunner runner;
          task_group_fanout(spawnTasks ? runner.group() : nullptr, units.size(),
              [&](size_t i) {
            return [this, unit = units[i], &request, &merger]() {
              VectorSearchResult part = scanUnit(unit, request);
              // Guard the accumulator until it is parked in the merger.
              std::unique_ptr<ScanAccumulator> acc(merger.obtain());
              foldPart(*acc, part);
              merger.release(acc.release());
            };
          });
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
            && (Groups ? !acc->approxHeap.truncated : acc->approxReturned <= request.candidates);
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
          merged.hits = acc->approxHeap.take();
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
            merged.scannedBreadth = std::max(request.breadth, breadthForBudget(allocatedCost));
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
  /// Fill `bits` with the all-set pattern for n ranks: ceil(n/8) 0xFF bytes
  /// with the tail bits past n cleared.  Every rank bitmap in this file
  /// keeps that tail invariant, so popcount(bits) == set ranks (see
  /// RankBitmap); build them through here so the mask lives once.
  static void fillAllRanksSet(std::vector<uint8_t>& bits, int64_t n) {
    bits.assign(((size_t)n + 7) / 8, (uint8_t)0xFF);
    if (n & 7) {
      bits.back() = (uint8_t)(0xFFu >> (8 - (n & 7)));
    }
  }

  /// Build the liveness RankBitmap (rank-space liveness + exact live vector
  /// count) for one segment's vector field.  Run once per liveDocs
  /// generation (cached on the VectorAuxReader; per query for a segment
  /// without an overlay), and sized by the segment's
  /// DELETED docs rather than its vectors: start all-live, walk the zero
  /// bits of liveDocs, and clear each deleted doc's rank(s), resolving
  /// doc -> rank through the has-field bitset (advance + rank) and, for
  /// multi-valued, the forward start/end-rank map - the engines' reverse
  /// (valueRank -> docId) map is never consulted.  Dense single-valued
  /// needs no walk at all: valueRank == docId, so the liveDocs words
  /// themselves are the bitmap (borrowed and pinned via backing) and the
  /// segment's cached live count is the cardinality.
  static RankBitmap buildRankLiveBitmap(IndexReader::Segment& seg,
                                        VectorReader& vr,
                                        int64_t numVectors, int32_t lists) {
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
      RankBitmap result;
      result.bits = (const uint8_t*)liveBits.words;
      result.numBytes = numBytes;
      result.count = (int64_t)live.numLive();
      result.docs = result.count;
      result.backing = seg.liveDocsShared();
      if (lists > 0) result.enableListCounts(lists);
      return result;
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
    RankBitmap result;
    result.bits = bits.data();
    result.numBytes = numBytes;
    result.count = numVectors - cleared;
    result.backing = std::move(owned);
    if (lists > 0) result.enableListCounts(lists);
    return result;
  }

  /// Visit the members of a doc set in ascending order.
  template <typename F>
  static void forEachDoc(DocSet& docs, F&& f) {
    if (docs.type == DocSet::BITSET) {
      const auto& bits = ((const BitDocSet&)docs).bits();
      int32_t size = bits.size();
      screaming::FixedBitSet::visitOnes(bits.words,
          (int32_t)screaming::FixedBitSet::sizeInWords(size), [&](int32_t d) {
        if (d < size) f(d);
      });
    } else {
      for (int32_t d : ((const ArrDocSet&)docs).docs()) f(d);
    }
  }

  /// Build the rank-space form of a filtered (live-folded) domain for one
  /// segment's vector field: bit r is set iff the doc owning rank r is in
  /// the domain, plus its eligible vector and doc counts.  Run once per
  /// (filter key, liveDocs generation) when cached on the overlay, else once
  /// per query.  The doc -> rank mapping mirrors buildRankLiveBitmap:
  ///   - dense single-valued: identity; a bitset domain is copied word for
  ///     word, an array domain sets its docs' bits;
  ///   - sparse (has-field bitset): the doc's rank among docs with the
  ///     field.  A domain far smaller than the field drives the walk
  ///     (advance + rank per member); otherwise the field's docs are walked
  ///     in order (rank is the walk position) and probed against the domain;
  ///   - multi-valued: each member's [start, end) value-rank range, set a
  ///     word at a time.
  /// Cost is O(numVectors / 64 + members), not a per-rank lookup.
  static RankBitmap buildDomainRankBitmap(VectorReader& vr, int64_t numVectors, DocSet& domain,
                                          int32_t lists) {
    filterRankBitmapBuildsForTests.fetch_add(1, std::memory_order_relaxed);
    auto owned = rankbits::zeroWords(numVectors);
    uint64_t* words = owned->data();
    size_t numWords = owned->size();
    StrColReader& col = vr.strColReader();
    const bool multi = vr.isMultiValued();
    auto& dr = col.docsReader();
    int64_t docs = 0;
    auto addDoc = [&](int32_t docRank) LUXIR_INLINE {
      docs++;
      if (multi) {
        auto [begin, end] = col.getStartEndValueRank(docRank);
        rankbits::setRange(words, begin, end);
      } else {
        words[(size_t)docRank >> 6] |= 1ULL << (docRank & 63);
      }
    };
    if (!dr.hasBitset()) {
      // Every doc has the field: docRank == docId.
      int32_t fieldDocs = vr.docsWithValue();
      if (!multi && domain.type == DocSet::BITSET) {
        const auto& bits = ((const BitDocSet&)domain).bits();
        size_t copyWords = std::min(numWords, (size_t)screaming::FixedBitSet::sizeInWords(bits.size()));
        std::memcpy(words, bits.words, copyWords * sizeof(uint64_t));
        if (numVectors & 63) words[numWords - 1] &= (1ULL << (numVectors & 63)) - 1;
        docs = rankbits::popcount(words, numWords);
      } else {
        forEachDoc(domain, [&](int32_t d) { if (d < fieldDocs) addDoc(d); });
      }
    } else {
      screaming::BitSet::Iterator fieldIt(dr.bitset());
      if ((int64_t)domain.card() * 8 < (int64_t)vr.docsWithValue()) {
        int32_t fieldDoc = -1;  // last doc-with-field landed on (END once drained)
        forEachDoc(domain, [&](int32_t d) {
          if (fieldDoc < d) fieldDoc = fieldIt.advance(d);
          if (fieldDoc == d) addDoc(fieldIt.rank());
        });
      } else {
        DocSetProbe probe(&domain);
        int32_t rank = 0;
        for (int32_t d = fieldIt.next(); d != screaming::BitSet::END; d = fieldIt.next(), rank++) {
          if (probe.get(d)) addDoc(rank);
        }
      }
    }
    RankBitmap result;
    result.bits = (const uint8_t*)words;
    result.numBytes = ((size_t)numVectors + 7) / 8;
    result.count = multi ? rankbits::popcount(words, numWords) : docs;
    result.docs = docs;
    result.backing = std::move(owned);
    if (lists > 0) result.enableListCounts(lists);
    return result;
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
  template <bool Groups> struct RescoreAccumulator : MergeableData {
    int64_t limit;
    BoundedHits<Groups, DocHit, betterDoc> heap;

    RescoreAccumulator(int64_t limit, const CollapsePlan* plan) : limit(limit), heap(limit, plan) {}

    static RescoreAccumulator* merge(RescoreAccumulator* a, RescoreAccumulator* b) {
      if (a->heap.size() < b->heap.size()) std::swap(a, b);
      a->heap.merge(b->heap);
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
  template <bool Groups = false>
  static void rescoreAndCollapseCandidates(std::vector<VectorEngineHit>& candidates,
                                           IndexReader& reader,
                                           IndexReader::FieldSegments segInfos,
                                           std::span<const SegV2D> v2dPerSeg,
                                           std::span<const char> segApprox,
                                           const float* queryPtr,
                                           int32_t dims,
                                           int32_t metric,
                                           bool normalizeColumnOnCosineRescore,
                                           int64_t kDocs,
                                           std::vector<DocHit>& docHits,
                                           bool parallel, const CollapsePlan* groupPlan = nullptr) {
    docHits.clear();
    if (candidates.empty() || kDocs <= 0) return;

    std::sort(candidates.begin(), candidates.end(),
        [](const VectorEngineHit& a, const VectorEngineHit& b) {
          if (a.segOrd != b.segOrd) return a.segOrd < b.segOrd;
          return a.valueRank < b.valueRank;
        });

    AtomicMerger<RescoreAccumulator<Groups>> merger(
      [kDocs, groupPlan]() { return new RescoreAccumulator<Groups>(kDocs, groupPlan); },
      [](RescoreAccumulator<Groups>* acc) { delete acc; });

    auto rescoreBucket = [&](size_t bucketBegin, size_t bucketEnd, RowReadAhead::Gate& gate) {
      int32_t segOrd = candidates[bucketBegin].segOrd;
      if (segOrd < 0 || (size_t)segOrd >= segInfos.size()) {
        throw std::runtime_error("KnnQuery: vector candidate segment ordinal out of range");
      }
      const SegFieldInfo* info = segInfos.at((size_t)segOrd);
      if (info == nullptr) {
        throw std::runtime_error("KnnQuery: vector candidate segment has no field info");
      }
      const SegV2D& v2d = v2dPerSeg[(size_t)segOrd];
      int64_t rescored = 0;
      bool keepEngineScores = !segApprox.empty() && !segApprox[(size_t)segOrd];
      std::optional<VectorReader> vr;
      if (!keepEngineScores) {
        vr.emplace(reader.segments()[(size_t)segOrd].postingsReader(), *info);
      }

      [[no_unique_address]] std::conditional_t<Groups,
          std::optional<VectorReader::Iterator>, NoGroupState> fullDocs;
      if constexpr (Groups) if (vr && vr->isMultiValued()) fullDocs.emplace(vr->strColReader());

      // Read-ahead of the bucket's rows on a column that is not resident (see
      // RowReadAhead).  A unit is a candidate's row, or for a group witness
      // its document's whole value run (the witness can sit anywhere in it),
      // found by a second iterator in doc order.  Reads that iterator causes
      // count as the batch's cold pages, which only leans toward staying on.
      int64_t units = (int64_t)(bucketEnd - bucketBegin);
      std::optional<RowReadAhead> ahead;
      [[no_unique_address]] std::conditional_t<Groups,
          std::optional<VectorReader::Iterator>, NoGroupState> aheadDocs;
      auto rankAt = [&](int64_t k) { return candidates[bucketBegin + (size_t)k].valueRank; };
      auto queue = [&](int64_t k) {
        int64_t rank = rankAt(k);
        if constexpr (Groups) if (aheadDocs) {
          int32_t doc = v2d.resolve(rank);
          if (aheadDocs->docId() < doc) aheadDocs->advance(doc);
          auto [begin, end] = aheadDocs->valueRange();
          ahead->queueRows(begin, end);
          return;
        }
        ahead->queueRows(rank, rank + 1);
      };
      if (vr) {
        ahead.emplace(gate, vr->strColReader());
        if constexpr (Groups) if (fullDocs) aheadDocs.emplace(vr->strColReader());
        ahead->probe({rankAt(units / 4), rankAt(units / 2 + units / 4)});
      }

      // unique_ptr guards the obtained accumulator against a throw before
      // release (same idiom as FacetOp).
      std::unique_ptr<RescoreAccumulator<Groups>> acc(merger.obtain());
      for (size_t i = bucketBegin; i < bucketEnd;) {
        if (ahead) ahead->sparseAt((int64_t)(i - bucketBegin), units, queue);
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
          if constexpr (Groups) {
            // A PQ witness nominates a document, not one of its values.
            // Restore its full per-document max before group admission.
            int64_t begin = candidates[i].valueRank, end = begin + 1;
            if (fullDocs) {
              if (fullDocs->docId() < docId) fullDocs->advance(docId);
              assert(fullDocs->docId() == docId);
              auto range = fullDocs->valueRange();
              begin = range.first;
              end = range.second;
            }
            bestScore = bestInRun(begin, end, [&](int64_t rank) {
              rescored++;
              return exactScore(queryPtr, vr->vectorAtRank(rank), dims, metric,
                  normalizeColumnOnCosineRescore, 0.0f);
            }).second;
          } else {
            bestScore = bestInRun((int64_t)i, (int64_t)runEnd, [&](int64_t c) {
              return exactScore(queryPtr, vr->vectorAtRank(candidates[(size_t)c].valueRank),
                  dims, metric, normalizeColumnOnCosineRescore, candidates[(size_t)c].score);
            }).second;
          }
        }
        acc->heap.offer(DocHit{segOrd, docId, bestScore});
        i = runEnd;
      }
      if (statsForTests) {
        int64_t count = Groups ? rescored : keepEngineScores ? 0 : (int64_t)(bucketEnd - bucketBegin);
        statsForTests->vectors += count;
        statsForTests->bytes += count * dims * (int64_t)sizeof(float);
      }
      merger.release(acc.release());
    };

    // Cancel and drain published buckets if a caller-side bucket throws.
    int64_t grain = std::max<int64_t>(1, rescoreTaskGrainCandidates);
    std::vector<std::pair<size_t, size_t>> tasks, local;
    RowReadAhead::Gate gate;  // shared by the buckets that run on this thread
    for (size_t b = 0; b < candidates.size();) {
      size_t e = b + 1;
      while (e < candidates.size() && candidates[e].segOrd == candidates[b].segOrd) e++;
      if (!parallel) rescoreBucket(b, e, gate);
      else if ((int64_t)(e - b) >= grain || e == candidates.size()) tasks.emplace_back(b, e);
      else local.emplace_back(b, e);
      b = e;
    }
    TaskGroupRunner runner;
    task_group_fanout(tasks.size() > 1 ? runner.group() : nullptr, tasks.size(), [&](size_t i) {
      auto [b, e] = tasks[i];
      return [&rescoreBucket, b, e]() {
        RowReadAhead::Gate taskGate;
        rescoreBucket(b, e, taskGate);
      };
    });
    for (auto [b, e] : local) rescoreBucket(b, e, gate);
    runner.join();

    RescoreAccumulator<Groups>* acc = merger.getData();
    docHits = acc->heap.take();
    std::sort(docHits.begin(), docHits.end(), betterDoc);
  }

  // Query and stored vectors share inputError's element bound, which keeps
  // every exact and FAISS distance finite.  A NaN score would break the
  // strict-total-order contract (betterHit/betterDoc) that makes the bounded
  // cuts deterministic and std::sort defined, so any NaN that slips past the
  // bound maps to a worst-rank score at the point where scores are produced.  One ULP above lowest(): TopScoreCollector's
  // minCompetitiveVal starts at lowest() and admits only strictly greater
  // scores, and a kNN hit must rank last, not be counted in matches yet
  // vanish from the returned docs.
  static float finiteScore(float score) {
    if (!std::isnan(score)) [[likely]] return score;
    return std::nextafter(std::numeric_limits<float>::lowest(), 0.0f);
  }

  // Keep L2's nonnegative branch and IP in sync with scoreFromDist below.
  // COSINE has a separate stored-vector norm branch for raw-column rescore mode.
  static float exactScore(const float* queryPtr,
                          std::span<const float> vec,
                          int32_t dims,
                          int32_t metric,
                          bool normalizeColumnOnCosineRescore,
                          float fallbackScore) {
    assert(queryPtr != nullptr);
    assert((int32_t)vec.size() == dims);
    switch (metric) {
      case (int32_t)luxir::api::VectorMetric::L2: {
        float dist = faiss::fvec_L2sqr(queryPtr, vec.data(), (size_t)dims);
        return finiteScore(1.0f / (1.0f + dist));
      }
      case (int32_t)luxir::api::VectorMetric::IP:
        return finiteScore(faiss::fvec_inner_product(queryPtr, vec.data(), (size_t)dims));
      case (int32_t)luxir::api::VectorMetric::COSINE: {
        float sum = faiss::fvec_inner_product(queryPtr, vec.data(), (size_t)dims);
        if (!normalizeColumnOnCosineRescore) return finiteScore(sum);

        // Raw-column cosine pays the vector norm at query time.  This is the
        // explicit tradeoff for preserving the originally submitted bytes when
        // normalize_on_write=false; the default cosine path stores unit vectors.
        float normSq = faiss::fvec_norm_L2sqr(vec.data(), (size_t)dims);
        if (!std::isfinite(normSq) || normSq <= VectorFieldType::MIN_COSINE_NORM_SQ) return 0.0f;
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

  // Convert FAISS's per-metric distance to an internal "higher is better" score.
  //   L2:    squared distance d >= 0 maps to 1/(1+d); d < 0 maps to 1-d.
  //   IP:    FAISS returns dot product - already higher-is-better.
  //   COSINE: stored as IP over normalized vectors (write path or builder),
  //           query is normalized at search time - same as IP.
  // NaN distances map to the worst score (see finiteScore).
  static float scoreFromDist(float dist, int32_t metric) {
    switch (metric) {
      case (int32_t)luxir::api::VectorMetric::L2:
        // Preserve ordering among negative RaBitQ estimates. Scores above
        // 1 are internal only: terminal rescore restores exact L2 scores.
        return finiteScore(dist >= 0.0f ? 1.0f / (1.0f + dist) : 1.0f - dist);
      case (int32_t)luxir::api::VectorMetric::IP:
      case (int32_t)luxir::api::VectorMetric::COSINE:
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

  /// Breadth projection for a breadth round, the breadth analog of
  /// projectedKReq: scanned breadth * (target docs / pooled docs) plus the
  /// same slack, growing at least 1x and at most MAX_BREADTH_GROWTH per round
  /// (the cap when nothing is pooled yet).  A pure function of
  /// counts, so serial and parallel runs deepen identically.  0 when the
  /// engine reports no scanned breadth (no IVF slots).
  static int32_t projectedBreadth(int32_t scannedBreadth, int64_t pooled, int64_t target) {
    if (scannedBreadth <= 0) return 0;
    double growth = pooled > 0 ? (double)target / (double)pooled * 1.3 : MAX_BREADTH_GROWTH;
    growth = std::clamp(growth, 1.0, MAX_BREADTH_GROWTH);
    double want = std::ceil((double)scannedBreadth * growth);
    if (want >= (double)std::numeric_limits<int32_t>::max()) return std::numeric_limits<int32_t>::max();
    return (int32_t)want;
  }

};

} // namespace luxir
