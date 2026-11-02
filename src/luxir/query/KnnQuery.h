// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <faiss/Index.h>
#include <faiss/IndexIVF.h>
#include <faiss/impl/ResultHandler.h>
#include <faiss/MetricType.h>
#include <faiss/impl/IDSelector.h>
#include <faiss/impl/DistanceComputer.h>
#include <faiss/impl/RaBitQUtils.h>
#include <faiss/utils/distances.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <boost/unordered/unordered_flat_set.hpp>

#include "Query.h"
#include "KnnIntervalCollector.h"
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
///      SegmentIvfScanner (IVF+RaBitQ or IVF+PQ aux, approximate).  All slots are
///      wrapped in one CompositeVectorEngine, which owns global IVF list
///      allocation across segments (see its class comment).
///   3. WIDEN: satisfy the bounded channel's document budget and reach k
///      distinct units. DEPTH uses a fresh bounded cut; BREADTH carries
///      interval records and witnesses into newly selected lists. Shared
///      floors span chunks and segments, with a barrier between rounds.
///   4. REFINE: read distinct-unit seeds and mandatory bounded candidates,
///      then the remaining competitive interval values. Both read sets are
///      ordered by segment/value rank and reuse exact scores. Collapse to
///      exact unit representatives and materialize per-segment hit arrays.
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
/// The active query domain is pushed into the engine as the COMPLETE
/// eligibility predicate: per the PrepareContext contract it is already
/// live-filtered (liveDocs intersected with any enclosing filter clauses),
/// and a null domain means no deletes and no filters.  The column engine
/// checks it directly while scanning docs.  FAISS aux engines pick the
/// cheapest selector per segment: none when the domain is null, a cached
/// rank-space liveness
/// bitmap when the domain is exactly liveDocs (built once per liveDocs
/// generation, shared across queries), or a resolve-based IDSelector for
/// per-query filtered domains.
///
/// Multi-valued vector fields are supported via the segment's persisted
/// valueRank->docId column (VectorReader::docForVectorRank).  The exact column
/// engine collapses a doc's vectors while scanning and returns one best vector
/// per doc.  Approximate aux engines return vector candidates instead; the
/// host pools them and the terminal refine collapses. Bounded selection is
/// best-effort at maxKnnCandidates; intervals use the request-memory ceiling.
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
    // Selector-passing codes, including those rejected by the 1-bit gate.
    std::atomic<int64_t> scanned{0}, extraBitEvaluations{0}, gateRejections{0};
    std::atomic<int64_t> retainedPeak{0}, retainedTerminal{0}, seedReads{0}, remainingReads{0};
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
  // are identical in serial and parallel mode. Shared confidence pruning
  // can change work and rare approximate misses with scheduling. Mutable for tests.
  static inline int64_t scanTaskGrainVectors = 8192;

  static constexpr float RABITQ_GATE_EPSILON = 1.9f;
  static constexpr float RABITQ_SELECTION_EPSILON = 1.9f;

  // Terminal-rescore task sizing: a segment's candidate bucket runs as its
  // own task only at or above this many pooled candidates; smaller buckets
  // fold inline on the calling thread.  Mutable for tests.
  static inline int64_t rescoreTaskGrainCandidates = 256;

  // Cover the vectors whose estimate error can swap them with the k-th
  // neighbor; that band holds sublinearly more vectors as k grows. Constants
  // measured on 1024-d IP, PQ M=128 x 8 bits: within 0.001 recall of the
  // scanned-list ceiling for k=1..1000 at default effort and nprobe=256.
  // An explicit refine_candidates replaces this default (clamped up to k).
  // Fitted on one corpus. Larger estimate error relative to neighbor gaps
  // (960-d L2 descriptors, 768/1536-d text with real queries) leaves recall
  // that larger counts recover. RaBitQ interval selection does not use this.
  static int64_t boundedDefaultCandidates(int64_t k, int64_t cap) {
    assert(k > 0 && cap >= 0);
    if (k >= cap) return cap;
    double headroom = 24.0 + std::ceil(15.0 * std::pow((double)k, 0.8));
    // Check before conversion/addition, including when cap is INT64_MAX.
    if (headroom >= (double)(cap - k)) return cap;
    return k + (int64_t)headroom;
  }

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
  /// of arrival order for a given candidate set. Confidence pruning can
  /// change that set with scheduling. Totality requires non-NaN
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
    const T* floor() const {
      if (limit <= 0 || (int64_t)size() != limit) return nullptr;
      if constexpr (Groups) return &state->heap.top();
      else return &docs.front();
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

  template <bool Groups> class UnitKeyReader {
    std::conditional_t<Groups, std::optional<CollapseKeyReader>, NoGroupState> reader;
    uint64_t docKey;
    int32_t segment = -1;
  public:
    explicit UnitKeyReader(const CollapsePlan* plan) {
      if constexpr (Groups) reader.emplace(*plan);
    }
    auto read(int32_t segOrd, int32_t doc) {
      if constexpr (Groups) {
        if (segment != segOrd) { reader->setSegment(segOrd); segment = segOrd; }
        return reader->read(segOrd, doc);
      } else {
        docKey = packSegDoc(segOrd, doc);
        return &docKey;
      }
    }
  };

  template <bool Groups>
  class BoundedScanCut {
  public:
    static constexpr size_t BATCH_SIZE = 64;
    struct GroupKeys {
      std::array<CollapseKey, BATCH_SIZE> keys;
      std::array<int32_t, BATCH_SIZE> docs;
    };
    struct Batch {
      std::array<VectorEngineHit, BATCH_SIZE> hits;
      [[no_unique_address]] std::conditional_t<Groups, GroupKeys, NoGroupState> groups;
      size_t size = 0;
    };

  private:
    int64_t limit;
    BoundedHits<Groups, VectorEngineHit, betterHit> approximate;
    std::vector<VectorEngineHit> exact;
    std::mutex mutex;
    std::atomic<float> publishedFloor{-std::numeric_limits<float>::infinity()};
    int64_t approximateVisited = 0;
    bool anyApproximate = false;
    bool partsExhausted = true;

    void publish() {
      if (const auto* floor = approximate.floor()) {
        // The cut never changes within an epoch; serialized heap updates
        // only raise its floor. Readers need no access to the heap itself.
        publishedFloor.store(floor->score, std::memory_order_relaxed);
      }
    }

  public:
    BoundedScanCut(int64_t limit, const CollapsePlan* plan,
                   std::span<const SegV2D> v2d, size_t perKey)
        : limit(limit), approximate(limit, plan, v2d, perKey) {
      assert(limit > 0);
    }

    float floorScore() const { return publishedFloor.load(std::memory_order_relaxed); }
    size_t batchSize() const { return (size_t)std::min(limit, (int64_t)BATCH_SIZE); }

    void offer(Batch& batch) {
      std::lock_guard lock(mutex);
      for (size_t i = 0; i < batch.size; i++) {
        if constexpr (Groups) {
          approximate.offerKey(batch.hits[i], batch.groups.keys[i], batch.groups.docs[i]);
        } else approximate.offer(batch.hits[i]);
      }
      publish();
      batch.size = 0;
    }

    void finishScan(int64_t scanned) {
      std::lock_guard lock(mutex);
      approximateVisited += scanned;
      anyApproximate = true;
    }

    void foldPart(const VectorSearchResult& part) {
      std::lock_guard lock(mutex);
      if (part.scoresAreExact) {
        exact.insert(exact.end(), part.hits.begin(), part.hits.end());
      } else {
        anyApproximate = true;
        approximateVisited += (int64_t)part.hits.size();
        for (const auto& hit : part.hits) approximate.offer(hit);
        publish();
      }
      if constexpr (Groups) {
        if (!part.scoresAreExact) partsExhausted &= (part.boundedSelection == VectorSelection::COMPLETE);
      } else partsExhausted &= (part.boundedSelection == VectorSelection::COMPLETE);
    }

    // Called after the task barrier. Exact hits bypass the approximate cut.
    VectorSearchResult take() {
      VectorSearchResult result;
      result.scoresAreExact = !anyApproximate;
      result.boundedSelection = partsExhausted && (Groups
          ? !approximate.truncated && approximate.size() < (size_t)limit
          : approximateVisited <= limit) ? VectorSelection::COMPLETE : VectorSelection::MORE_DEPTH;
      if (!result.scoresAreExact) {
        for (const auto& hit : exact) result.exactSegOrds.push_back(hit.segOrd);
        std::sort(result.exactSegOrds.begin(), result.exactSegOrds.end());
        result.exactSegOrds.erase(std::unique(result.exactSegOrds.begin(), result.exactSegOrds.end()),
                                  result.exactSegOrds.end());
      }
      result.hits = approximate.take();
      result.hits.insert(result.hits.end(), exact.begin(), exact.end());
      std::sort(result.hits.begin(), result.hits.end(), betterHit);
      return result;
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
    out.appendInt32(defaultAnnBreadthReferenceDims);
    // Test wrappers may alter candidate selection without a keyable semantic
    // contract.
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

    template <bool Groups> class SegmentIvfScanner;

    template <bool Groups> struct EngineSlot {
      std::unique_ptr<VectorEngine> engine;
      int64_t vectorCount = 0;
      int64_t liveVectorCount = 0;
      std::unique_ptr<SegmentIvfScanner<Groups>> ivfEngine;
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

      int64_t boundedDocsWithValue = 0, boundedVectors = 0, boundedLiveVectors = 0;
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
            bool bounded = vauxPerSeg[i] != nullptr
                && (!vauxPerSeg[i]->isRaBitQ() || query.getRefineCandidates() > 0);
            if (bounded) { boundedDocsWithValue += vr.docsWithValue(); boundedVectors += segCount; }
            segVectorCounts[i] = segCount;
            if (vr.isMultiValued()) {
              anyMultiValued |= bounded;
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
            if (bounded) boundedLiveVectors += segLiveVectorCounts[i];
          }
        }
      }
      int64_t ntotal = 0;
      for (int64_t n : segVectorCounts) ntotal += n;
      if (ntotal == 0) {
        return emptyResult();
      }
      int64_t liveNTotal = 0;
      for (int64_t n : segLiveVectorCounts) liveNTotal += n;
      if (liveNTotal == 0) {
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
      bool boundedEngine = boundedVectors > 0;
      KnnIntervalCollector<Groups> intervals(kDocs, context.planningContext().memoryTracker, query.getField());
      std::vector<EngineSlot<Groups>> slots;
      slots.reserve(numSegs);
      bool intervalEngine = false;
      // RaBitQ overlays index the rotated space: the query is rotated once
      // per distinct rotation (overlays of one dims share one interned
      // transform) and outlives every engine built below.  Each inner buffer
      // keeps its address when the outer vector grows.
      std::vector<std::pair<const VectorRotation*, std::vector<float>>> rotatedQueries;
      auto rotatedQuery = [&](const VectorRotation* rotation) -> const float* {
        for (const auto& [rot, buf] : rotatedQueries) {
          if (rot == rotation) return buf.data();
        }
        auto& entry = rotatedQueries.emplace_back(rotation, std::vector<float>((size_t)dims));
        rotation->apply(queryPtr, entry.second.data());
        return entry.second.data();
      };
      // RaBitQ query quantization is a search-time knob: the current
      // schema's value applies to every overlay.
      int32_t rabitqQueryBits = query.getFieldType().ann().rabitqQueryBits;
      for (size_t i = 0; i < numSegs; i++) {
        int64_t segCount = segVectorCounts[i];
        if (segCount <= 0 || segInfos.at(i) == nullptr) continue;
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
          // Both codecs are IVF; each segment is served by its own overlay's
          // meta, so segments built with different codecs mix freely.
          auto* ivf = dynamic_cast<faiss::IndexIVF*>(idx);
          if (ivf == nullptr) throw std::runtime_error("KnnQuery: vector aux is not an IVF index");
          bool interval = vaux->isRaBitQ() && query.getRefineCandidates() == 0;
          intervalEngine |= interval;
          const float* indexQuery = vaux->getRotation() != nullptr
            ? rotatedQuery(vaux->getRotation())
            : queryPtr;
          slots.push_back({
            .engine = nullptr,
            .vectorCount = segCount,
            .liveVectorCount = segLiveVectorCounts[i],
            .ivfEngine = std::make_unique<SegmentIvfScanner<Groups>>(
                *ivf, (int32_t)i, seg, v2dPerSeg[i], domain, rankLivePerSeg[i],
                metric, query.groups, PreparedQuery{indexQuery, (uint8_t)rabitqQueryBits},
                vaux->getAmplitudes(), interval ? &intervals : nullptr),
          });
        } else {
          slots.push_back({
            .engine = std::make_unique<SegmentFlatColumnVectorEngine<Groups>>(
              (int32_t)i, seg, segInfos.at(i), v2dPerSeg[i], domain,
              dims, metric, normalizeColumnOnCosineRescore, query.groups, v2dSpan),
            .vectorCount = segCount,
            .liveVectorCount = segLiveVectorCounts[i],
            .ivfEngine = nullptr,
          });
        }
      }
      if (slots.empty()) {
        return emptyResult();
      }

      // The bounded policy retains its existing work cap. Interval retention
      // has only the request's opt-in byte ceiling, regardless of input size.
      int64_t cap = query.getExact() || (intervalEngine && !boundedEngine) ? ntotal
          : std::min(ntotal, std::max((int64_t)1, maxKnnCandidates));
      int64_t selectedLimit = intervalEngine || query.getExact() ? kDocs : std::min(kDocs, cap);

      int64_t avgMult = (!Groups && boundedEngine && anyMultiValued && boundedDocsWithValue > 0)
        ? ceilDivClamped(boundedVectors, boundedDocsWithValue, cap)
        : 1;
      int64_t baseReq = kDocs;
      if (boundedEngine) {
        if (query.getRefineCandidates() > 0) {
          baseReq = std::min(cap, std::max(kDocs, (int64_t)query.getRefineCandidates()));
        } else {
          baseReq = boundedDefaultCandidates(kDocs, cap);
        }
        baseReq = std::max(baseReq, kDocs);
      }
      int64_t kReq = Groups ? std::min(baseReq, cap) : mulClamped(baseReq, avgMult, cap);
      if (kReq <= 0) return emptyResult();
      int64_t targetDocReq = boundedEngine
        ? std::min<int64_t>(boundedLiveVectors, std::max(kDocs, baseReq))
        : 0;

      std::vector<VectorEngineHit> candidateHits, exactCandidates;
      std::conditional_t<Groups, std::unique_ptr<BoundedHits<true, VectorEngineHit, betterHit>>, NoGroupState> groupPool;
      int64_t perKey = 1;
      if constexpr (Groups) {
        perKey = boundedEngine ? membersPerGroup(baseReq, kDocs) : 1;
        groupPool = std::make_unique<BoundedHits<true, VectorEngineHit, betterHit>>(std::min(baseReq, cap), query.groups, v2dSpan, (size_t)perKey);
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
        std::make_unique<CompositeVectorEngine<Groups>>(std::move(slots), liveNTotal, kDocs,
                                                metric, ctx.parallel, query.groups, v2dSpan, intervals);
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
        // liveness seed or at least one pooled rank. An all-ones span would
        // demote a selector-free scan to a per-vector always-true probe.
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

      bool candidatesExhausted = true;

      // Bounded DEPTH and IVF BREADTH are independent obligations. Interval
      // state persists across breadth rounds and never asks for more depth.
      // Exact reads remain terminal, after all scanning has finished.
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

        candidatesExhausted = (result.boundedSelection == VectorSelection::COMPLETE) && result.breadthExhausted;
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
          bool exactHit = result.scoresAreExact || (!roundExactSeg.empty() && roundExactSeg[(size_t)hit.segOrd]);
          if (exactHit) {
            exactCandidates.push_back(hit);
            continue;
          }
          if constexpr (!Groups) if (candidateHits.size() >= maxVectorHits) break;
          if (markSeen(hit.segOrd, hit.valueRank)) {
            if constexpr (Groups) {
              freshDocuments = true;
              groupPool->offer(hit);
            } else candidateHits.push_back(hit);
            segApproxInPool[(size_t)hit.segOrd] = 1;
          }
        }
        if constexpr (Groups) candidateHits = groupPool->take();
        bool appended = Groups ? freshDocuments : candidateHits.size() != appendStart;
        if (appended) {
          candidatePoolSorted = false;
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
        if constexpr (Groups) if (boundedEngine) {
          pooled = (int64_t)groupPool->size();
          poolSize = groupPool->size();
        }
        bool boundedDone = !boundedEngine || pooled >= targetDocReq || poolSize >= maxVectorHits;
        intervals.resetBoundedUnits();
        UnitKeyReader<Groups> keyReader(query.groups);
        for (const auto& hit : candidateHits) {
          int32_t doc = v2dSpan[(size_t)hit.segOrd].resolve(hit.valueRank);
          const auto* key = keyReader.read(hit.segOrd, doc);
          if (key) intervals.observeBounded(*key);
        }
        intervals.compact();
        bool unitsFilled = intervals.unitCount() >= (size_t)kDocs;
        if (boundedDone && unitsFilled) break;
        if (!intervalEngine && poolSize >= maxVectorHits) break;

        // DEPTH: document mode projects the pool size needed for targetDocReq
        // docs from observed docs-per-candidate yield, then requests only
        // the INCREMENT over the banked pool: post-fold every engine returns
        // fresh hits on top of it (eligible exclusion), so re-requesting the
        // cumulative depth would overshoot the projection by the pool size,
        // compounding across rounds.  Floored at the doc deficit so a
        // degenerate projection still progresses.  `appended` is the
        // termination backstop: a round that added nothing fresh and still
        // claims an unexhausted pool (a conforming engine cannot - fresh
        // hits or bounded completion) must not loop at the same shape.
        if (!boundedDone && result.boundedSelection == VectorSelection::MORE_DEPTH && appended
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

        if ((boundedDone || result.boundedSelection == VectorSelection::COMPLETE) && !result.breadthExhausted && breadth < maxBreadth) {
          int32_t nextBreadth = result.nextBreadth > breadth ? result.nextBreadth : breadth + 1;
          breadth = std::min(nextBreadth, maxBreadth);
          if (boundedEngine) foldSeen();
          continue;
        }

        break;
      }

      // Both terminal stages read in segment/value-rank order. Bounded
      // candidates are mandatory seed reads; interval records bypass their cut.
      candidateHits.insert(candidateHits.end(), exactCandidates.begin(), exactCandidates.end());
      if (statsForTests && intervalEngine) {
        statsForTests->retainedPeak += (int64_t)intervals.peakRetained();
        statsForTests->retainedTerminal += (int64_t)intervals.retained().size();
      }
      rescoreAndCollapseCandidates<Groups>(candidateHits, intervals, reader, segInfos, v2dSpan,
          segApproxInPool, queryPtr, dims, metric, normalizeColumnOnCosineRescore,
          selectedLimit, docHits, ctx.parallel, query.groups);

      if constexpr (Groups) {
        candidatesExhausted &= !groupPool->truncated && selectedLimit >= kDocs;
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
    // The composite allocates lists across segments; each scanner owns only
    // immutable segment/query inputs. Distance computers and key readers are
    // task-local, while the bounded cut spans every chunk and segment.
    template <bool Groups> class SegmentIvfScanner {
      const CollapsePlan* groupPlan;
      RaBitQAmplitudes amplitudes;
      KnnIntervalCollector<Groups>* intervals;

      /// Resolve-based eligibility selector over the live-filtered query
      /// domain (the PrepareContext contract: a present domain already
      /// includes liveDocs, so there is no separate liveness check).  FAISS
      /// consults the selector per scanned vector, before the distance, so
      /// this is the slow path - it is only used for per-query (filtered)
      /// domains; the unfiltered-with-deletes case uses the cached
      /// rank-space bitmap via faiss::IDSelectorBitmap instead, and the
      /// no-deletes no-filter case passes no selector.
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

      faiss::IndexIVF& ivf;
      faiss::IndexIVFRaBitQ* rabitq;
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
      // Owned by prepareUnit(): canonical for PQ, rotated for RaBitQ.
      const PreparedQuery preparedQuery;

      /// The ONLY selector policy: built per scan call from (domain,
      /// domainIsLive, rankLive, request.eligible), so round 1 and deepen
      /// rounds cannot drift apart.  Cheapest selector wins:
      ///   no domain, no eligible      -> null
      ///   domain == liveDocs          -> IDSelectorBitmap over rankLive, or
      ///                                  over eligible once present (it is
      ///                                  seeded from rankLive, so liveness
      ///                                  rides along - one bit probe)
      ///   filtered domain             -> LocalDomainSelector, composing the
      ///                                  eligible bit test (when present)
      ///                                  with the resolve-based check
      /// Per-call construction is two stores; owns the selector storage, so
      /// keep it alive throughout the scan so selPtr never outlives the
      /// optionals it points into.
      class RoundSelector {
        std::optional<faiss::IDSelectorBitmap> bitmap;
        std::optional<LocalDomainSelector> domainSel;
        const faiss::IDSelector* selPtr = nullptr;

      public:
        RoundSelector(const SegmentIvfScanner& eng,
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

        const faiss::IDSelector* get() const { return selPtr; }
      };

      class BoundedResultHandler final : public faiss::ResultHandler {
        BoundedScanCut<Groups>& cut;
        typename BoundedScanCut<Groups>::Batch batch;
        int32_t metric, segment;
        float lastFloor = NAN;
        const SegV2D& v2d;
        struct GroupState {
          CollapseKeyReader reader;
          BoundedHits<true, VectorEngineHit, betterHit> local;
          GroupState(const CollapsePlan& plan, int32_t segment, int64_t limit, size_t perKey)
              : reader(plan), local(limit, &plan, {}, perKey) { reader.setSegment(segment); }
        };
        [[no_unique_address]] std::conditional_t<Groups,
            std::unique_ptr<GroupState>, NoGroupState> groups;

      public:
        BoundedResultHandler(BoundedScanCut<Groups>& cut, int32_t metric, int32_t segment,
                             const SegV2D& v2d, const CollapsePlan* plan,
                             int64_t localLimit, size_t perKey)
            : cut(cut), metric(metric), segment(segment), v2d(v2d) {
          if constexpr (Groups) {
            groups = std::make_unique<GroupState>(*plan, segment, localLimit, perKey);
          }
          updateThreshold();
        }
        void updateThreshold() {
          float score = cut.floorScore();
          if constexpr (Groups) {
            if (const auto* local = groups->local.floor()) score = std::max(score, local->score);
          }
          if (score != lastFloor) {
            threshold = distanceThreshold(score, metric);
            lastFloor = score;
          }
        }
        void flush() {
          if (batch.size != 0) cut.offer(batch);
          updateThreshold();
        }
        bool add_result(float distance, faiss::idx_t rank) override {
          float score = scoreFromDist(distance, metric);
          if (score < cut.floorScore()) return false;
          if constexpr (Groups) {
            int32_t doc = v2d.resolve(rank);
            const auto* key = groups->reader.read(segment, doc);
            if (key == nullptr) return false;
            // A capped local top-R contains every possible global winner.
            // Reject hot-group members before copying keys or locking the cut.
            if (!groups->local.offerKey({score, segment, rank}, *key, doc)) return false;
            batch.groups.keys[batch.size] = *key;
            batch.groups.docs[batch.size] = doc;
          }
          batch.hits[batch.size++] = {score, segment, rank};
          if (batch.size == cut.batchSize()) flush();
          else updateThreshold();
          return true;
        }
      };

      class IntervalResultHandler {
        KnnIntervalCollector<Groups>& collector;
        typename KnnIntervalCollector<Groups>::Batch batch;
        int32_t metric, segment;
        float lastFloor = NAN;
        const SegV2D& v2d;
        UnitKeyReader<Groups> reader;
      public:
        float threshold;
        IntervalResultHandler(KnnIntervalCollector<Groups>& collector, int32_t metric, int32_t segment,
                              const SegV2D& v2d, const CollapsePlan* plan)
            : collector(collector), batch(collector.memoryResource()), metric(metric), segment(segment),
              v2d(v2d), reader(plan) {
          batch.reserve(64);
          updateThreshold();
        }
        float floorScore() const { return collector.floorScore(); }
        void updateThreshold() {
          float score = floorScore();
          if (score != lastFloor) {
            threshold = distanceThreshold(score, metric);
            lastFloor = score;
          }
        }
        void flush() {
          if (!batch.empty()) collector.offer(batch);
          updateThreshold();
        }
        void offer(float score, float lower, float upper, int64_t rank) {
          if (upper < floorScore()) return;
          int32_t doc = v2d.resolve(rank);
          const auto* key = reader.read(segment, doc);
          if (key == nullptr) return;
          batch.emplace_back(VectorEngineHit{score, segment, rank}, lower, upper, *key,
                             collector.memoryResource());
          if (batch.size() == 64) flush();
        }
      };

    public:
      SegmentIvfScanner(faiss::IndexIVF& ivf, int32_t segOrd,
                        IndexReader::Segment& seg, const SegV2D& v2d, DocSet* domain,
                        std::shared_ptr<const RankLiveBitmap> rankLive, int32_t metric,
                        const CollapsePlan* groupPlan, PreparedQuery preparedQuery,
                        RaBitQAmplitudes amplitudes, KnnIntervalCollector<Groups>* intervals)
          : groupPlan(groupPlan), amplitudes(amplitudes), intervals(intervals), ivf(ivf), rabitq(dynamic_cast<faiss::IndexIVFRaBitQ*>(&ivf)),
            segOrd(segOrd), v2d(v2d), liveDocs(seg.liveDocs()), rankLive(std::move(rankLive)),
            domain(domain), domainIsLive(domain != nullptr && liveDocs != nullptr
                                        && domain == &liveDocs->docset()),
            metric(metric), preparedQuery(preparedQuery) {
        assert(preparedQuery.query != nullptr);
        assert((liveDocs != nullptr) == (this->rankLive != nullptr));
      }

      bool usesIntervals() const { return intervals != nullptr; }
      int32_t getNList() const noexcept { return (int32_t)ivf.nlist; }

      void coarseRun(int32_t dims,
                     std::vector<faiss::idx_t>& listIds,
                     std::vector<float>& centroidDists) const {
        assert(dims == ivf.d);
        unused(dims);
        listIds.assign(ivf.nlist, (faiss::idx_t)-1);
        centroidDists.assign(ivf.nlist, 0.0f);
        // The rotation is orthonormal, so these distances compare directly
        // with other segments' (rotated or not) in the global list ranking.
        ivf.quantizer->search(1, preparedQuery.query, (faiss::idx_t)ivf.nlist, centroidDists.data(), listIds.data());
      }

      /// Live vector count of one IVF list - the unit the global allocator
      /// charges against the scan-fraction budget (cost = liveListSize / N).
      /// Uses the ACTUAL invlist length so list imbalance prices correctly
      /// (full selection always sums to ~N).  Under deletes this reads the
      /// cached rank-space liveness bitmap per id (no rank -> doc resolve);
      /// liveness only (not the query domain), matching the live-N
      /// denominator.
      int64_t liveListSize(faiss::idx_t listId) const {
        if (listId < 0 || (size_t)listId >= ivf.nlist) return 0;
        size_t rawSize = ivf.invlists->list_size((size_t)listId);
        if (rawSize == 0) return 0;
        if (liveDocs == nullptr) return (int64_t)rawSize;

        const uint8_t* bits = rankLive->bits;
        int64_t liveSize = 0;
        const faiss::idx_t* listIds = ivf.invlists->get_ids((size_t)listId);
        for (size_t i = 0; i < rawSize; i++) {
          uint64_t r = (uint64_t)listIds[i];
          liveSize += (bits[r >> 3] >> (r & 7)) & 1;
        }
        ivf.invlists->release_ids((size_t)listId, listIds);
        return liveSize;
      }

      void searchSelectedLists(const VectorSearchRequest& request,
                               std::span<const faiss::idx_t> listIds,
                               std::span<const float> centroidDists,
                               int64_t rangeLiveVectors, BoundedScanCut<Groups>& cut) const {
        if (intervals) scanLists<true>(request, listIds, centroidDists, rangeLiveVectors, cut);
        else scanLists<false>(request, listIds, centroidDists, rangeLiveVectors, cut);
      }

    private:
      template <bool Interval>
      void scanLists(const VectorSearchRequest& request,
                               std::span<const faiss::idx_t> listIds,
                               std::span<const float> centroidDists,
                               int64_t rangeLiveVectors, BoundedScanCut<Groups>& cut) const {
        assert(request.query != nullptr && request.dims == ivf.d);
        RoundSelector selector(*this, request);
        const auto* sel = selector.get();
        auto handler = [&]() {
          if constexpr (Interval) return IntervalResultHandler(*intervals, metric, segOrd, v2d, groupPlan);
          else return BoundedResultHandler(cut, metric, segOrd, v2d, groupPlan,
              std::min(request.candidates, rangeLiveVectors), request.perKey);
        }();
        std::unique_ptr<faiss::InvertedListScanner> pqScanner;
        std::vector<float> centroid;
        if (rabitq != nullptr) centroid.resize((size_t)ivf.d);
        else {
          pqScanner.reset(ivf.get_InvertedListScanner(false, sel));
          pqScanner->set_query(preparedQuery.query);
        }
        int64_t scanned = 0, extra = 0, rejected = 0;
        bool multiBit = rabitq != nullptr && rabitq->rabitq.nb_bits > 1;
        bool keepMax = metric != (int32_t)api::VectorMetric::L2;
        for (size_t list = 0; list < listIds.size(); list++) {
          auto id = listIds[list];
          if (id < 0) continue;
          faiss::InvertedLists::ScopedIds ids(ivf.invlists, (size_t)id);
          faiss::InvertedLists::ScopedCodes codes(ivf.invlists, (size_t)id);
          size_t count = ivf.invlists->list_size((size_t)id);
          std::unique_ptr<faiss::FlatCodesDistanceComputer> distanceComputer;
          faiss::RaBitQDistanceComputer* dc = nullptr;
          if (rabitq != nullptr) {
            ivf.quantizer->reconstruct(id, centroid.data());
            distanceComputer.reset(rabitq->rabitq.get_distance_computer(
                preparedQuery.queryBits, centroid.data()));
            distanceComputer->set_query(preparedQuery.query);
            dc = dynamic_cast<faiss::RaBitQDistanceComputer*>(distanceComputer.get());
            assert(dc != nullptr);
          } else pqScanner->set_list(id, centroidDists[list]);
          const float* listAmplitudes = nullptr;
          if constexpr (Interval) listAmplitudes = amplitudes.list((size_t)id).data();
          double gateScale = dc ? (double)dc->g_error * (RABITQ_GATE_EPSILON / 1.9f) : 0;
          double selectionScale = dc ? (double)dc->g_error * RABITQ_SELECTION_EPSILON : 0;

          // Keep the PQ kernel, refreshing the shared floor between blocks
          // even when no result reaches the handler. RaBitQ uses the same
          // handler after its separate 1-bit and extra-bit stages.
          for (size_t begin = 0; begin < count; begin += 256) {
            size_t end = std::min(count, begin + 256);
            handler.updateThreshold();
            if (dc == nullptr) {
              if constexpr (!Interval) pqScanner->scan_codes(end - begin, codes.get() + begin * ivf.code_size,
                                    ids.get() + begin, handler);
            } else for (size_t i = begin; i < end; i++) {
              if (sel != nullptr && !sel->is_member(ids[i])) continue;
              const uint8_t* code = codes.get() + i * ivf.code_size;
              scanned++;
              float distance = dc->distance_to_code_1bit(code);
              if (multiBit) {
                // f_error is beside the sign factors, already touched by the
                // 1-bit kernel. It includes FAISS's 1.9 and the L2 factor.
                float fError;
                std::memcpy(&fError, code + (ivf.d + 7) / 8 + 2 * sizeof(float), sizeof(fError));
                double error = fError * gateScale;
                double optimistic = keepMax ? (double)distance + error : (double)distance - error;
                bool reject = keepMax ? optimistic <= handler.threshold : optimistic >= handler.threshold;
                if (reject) { rejected++; continue; }
                distance = dc->distance_to_code_full(code);
                extra++;
              }
              if constexpr (Interval) {
                double error = listAmplitudes[i] * selectionScale;
                double optimistic = keepMax ? (double)distance + error : (double)distance - error;
                if (keepMax ? optimistic <= handler.threshold : optimistic >= handler.threshold) continue;
                auto [lower, upper] = scoreInterval(distance, error, metric);
                handler.offer(scoreFromDist(distance, metric), lower, upper, ids[i]);
              } else if (keepMax ? distance > handler.threshold : distance < handler.threshold) {
                handler.add_result(distance, ids[i]);
              }
            }
          }
          handler.flush();
        }
        if constexpr (!Interval) {
          if (rabitq == nullptr) scanned = (int64_t)handler.stats.scan_cnt;
          cut.finishScan(scanned);
        }
        if (statsForTests) {
          int64_t distances = multiBit ? extra : scanned;
          statsForTests->lists += (int64_t)listIds.size();
          statsForTests->vectors += distances;
          statsForTests->bytes += distances * (int64_t)ivf.code_size;
          statsForTests->scanned += scanned;
          statsForTests->extraBitEvaluations += extra;
          statsForTests->gateRejections += rejected;
        }
      }
    };

    template <bool Groups> class SegmentFlatColumnVectorEngine final : public VectorEngine {
      const CollapsePlan* groupPlan;
      std::span<const SegV2D> groupV2d;

      int32_t segOrd;
      IndexReader::Segment& seg;
      const SegFieldInfo* segInfo;
      const SegV2D& v2d;
      DocSet* domain;
      int32_t dims;
      int32_t metric;
      bool normalizeColumnOnCosineRescore;

    public:
      SegmentFlatColumnVectorEngine(int32_t segOrd,
                                    IndexReader::Segment& seg,
                                    const SegFieldInfo* segInfo,
                                    const SegV2D& v2d,
                                    DocSet* domain,
                                    int32_t dims,
                                    int32_t metric,
                                    bool normalizeColumnOnCosineRescore, const CollapsePlan* groupPlan,
                                    std::span<const SegV2D> groupV2d) noexcept
        : groupPlan(groupPlan), groupV2d(groupV2d), segOrd(segOrd),
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
          result.boundedSelection = VectorSelection::COMPLETE;
          return result;
        }

        BoundedHits<Groups, VectorEngineHit, betterHit> heap(request.candidates, groupPlan, groupV2d);
        int64_t eligibleDocs = 0;
        int64_t scanned = 0;
        if (segInfo == nullptr) {
          result.boundedSelection = VectorSelection::COMPLETE;
          return result;
        }
        VectorReader vr(seg.postingsReader(), *segInfo);
        int64_t numVals = vr.numVectors();
        if (numVals == 0) {
          result.boundedSelection = VectorSelection::COMPLETE;
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
        // this engine's prior emissions.) Skipped docs do not count toward
        // eligibleDocs, the exact column work count.
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

        // Read-ahead (see RowReadAhead); a filtered scan advises only the rows
        // of eligible docs, resolved through its own valDoc cursor (windows
        // ascend).  Reads that cursor causes count as the window's cold pages,
        // which only leans toward staying on.
        StrColReader& col = vr.strColReader();
        RowReadAhead::Gate gate;
        RowReadAhead ahead(gate, col.valuesFile(), col.valuesOffset(),
                           (size_t)col.fixedValueSize(), numVals);
        std::optional<MonoReader::BulkValues> aheadValDoc;
        if (domain != nullptr && v2d.mono != nullptr) aheadValDoc.emplace(*v2d.mono);
        int32_t aheadDoc = -1;
        bool aheadEligible = false;
        auto eligibleRank = [&](int64_t r) {
          int32_t doc = aheadValDoc ? (int32_t)aheadValDoc->valueAt(r) : v2d.resolve(r);
          if (doc != aheadDoc) {
            aheadDoc = doc;
            aheadEligible = docEligible(doc);
          }
          return aheadEligible;
        };
        auto readAhead = [&](int64_t rank) LUXIR_INLINE {
          if (domain == nullptr) ahead.scanAt(rank);
          else ahead.scanAt(rank, eligibleRank);
        };

        // Inline run-collapse: value ranks are doc-ordered (the valDoc map is
        // monotonic; sparse select and dense identity likewise), so a doc's
        // vectors form a contiguous run and max-sim collapse falls out of one
        // pass.  liveDocs/domain are applied as a per-run predicate.
        auto scan = [&](auto emit) {
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
              readAhead(rank);
              if (docEligible(docId) && !pooledInRun(rank, end)) {
                eligibleDocs++;
                emit(docId, [&]() LUXIR_INLINE {
                  if constexpr (Groups) scanned += end - rank;
                  auto [bestRank, bestScore] = bestInRun(rank, end, [&](int64_t r) {
                    return exactScore(request.query, vr.vectorAtRank(r), dims, metric,
                                      normalizeColumnOnCosineRescore, 0.0f);
                  });
                  return VectorEngineHit{bestScore, segOrd, bestRank};
                });
              }
              rank = end;
            }
          } else {
            // Single-valued (dense identity or sparse select): one-vector runs.
            for (int64_t rank = 0; rank < numVals; rank++) {
              readAhead(rank);
              if (pooledInRun(rank, rank + 1)) continue;
              int32_t docId = v2d.resolve(rank);
              if (!docEligible(docId)) continue;
              eligibleDocs++;
              emit(docId, [&]() LUXIR_INLINE {
                if constexpr (Groups) scanned++;
                return VectorEngineHit{exactScore(request.query, vr.vectorAtRank(rank), dims, metric,
                    normalizeColumnOnCosineRescore, 0.0f), segOrd, rank};
              });
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
        result.boundedSelection = VectorSelection::COMPLETE;
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
    ///     tasks when the request is parallel, sharing one bounded cut.
    ///     The unit is deliberately NOT the segment: affinity-driven allocation concentrates budget in one
    ///     segment, so per-segment tasks would serialize exactly the hot
    ///     case. Chunk boundaries do not depend on execution mode, but the
    ///     order of confidence pruning can affect rare approximate misses.
    ///
    /// SINGLE-SHOT lifecycle: coarseReady, cursors, watermarks and
    /// lastScannedBreadth mutate across search() rounds of
    /// ONE query.  prepare() builds a fresh instance per query; reusing one
    /// across queries would silently serve the old query's coarse ranking.
    template <bool Groups> class CompositeVectorEngine final : public VectorEngine {
      KnnIntervalCollector<Groups>& intervals;
      const CollapsePlan* groupPlan;
      std::span<const SegV2D> groupV2d;

      std::vector<EngineSlot<Groups>> slots;
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
      // scanners consume the first pair positionally and the scan-task
      // chunker prices ranges off the third.
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
      bool coarseScanned = false;

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
          auto* ivf = slots[i].ivfEngine.get();
          if (ivf == nullptr || slots[i].liveVectorCount <= 0 || ivf->getNList() <= 0) continue;
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

      double defaultBudget(int32_t dims) const {
        // nprobe=0 default: probe as many lists as a SINGLE IVF index of
        // nlist=referenceBreadth would by its own default, i.e. sqrt(nlist),
        // times the dimension factor (defaultAnnBreadthDimFactor).  As
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
        return defaultAnnBreadthDimFactor(dims)
          / std::sqrt((double)std::max(1, referenceBreadth));
      }

      double budgetForRequest(const VectorSearchRequest& request) const {
        double p = request.breadth > 0
          ? (double)request.breadth / (double)std::max(1, referenceBreadth)
          : defaultBudget(request.dims);
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
      };

      void scanUnit(const ScanUnit& unit, const VectorSearchRequest& request,
                    BoundedScanCut<Groups>& cut) const {
        const auto& slot = slots[(size_t)unit.slotOrd];
        if (slot.ivfEngine == nullptr) {
          VectorSearchRequest subReq = request;
          subReq.candidates = std::min(kDocs, slot.vectorCount);
          auto part = slot.engine->search(subReq);
          if (part.scoresAreExact) {
            UnitKeyReader<Groups> keyReader(groupPlan);
            for (const auto& hit : part.hits) {
              int32_t doc = groupV2d[(size_t)hit.segOrd].resolve(hit.valueRank);
              const auto* key = keyReader.read(hit.segOrd, doc);
              if (key) intervals.offerExact(hit, *key);
            }
          }
          cut.foldPart(part);
          return;
        }
        assert(selectedListIds[(size_t)unit.slotOrd].size()
               == selectedCentroidDists[(size_t)unit.slotOrd].size());
        slot.ivfEngine->searchSelectedLists(
          request,
          std::span<const faiss::idx_t>(
            selectedListIds[(size_t)unit.slotOrd].data() + unit.listBegin,
            unit.listEnd - unit.listBegin),
          std::span<const float>(
            selectedCentroidDists[(size_t)unit.slotOrd].data() + unit.listBegin,
            unit.listEnd - unit.listBegin), unit.live, cut);
      }

    public:
      CompositeVectorEngine(std::vector<EngineSlot<Groups>>&& slots,
                            int64_t liveTotalVectors, int64_t kDocs, int32_t metric,
                            bool parallel, const CollapsePlan* groupPlan,
                            std::span<const SegV2D> groupV2d, KnnIntervalCollector<Groups>& intervals) noexcept
        : intervals(intervals), groupPlan(groupPlan), groupV2d(groupV2d), slots(std::move(slots)),
          liveTotalVectors(liveTotalVectors),
          kDocs(kDocs),
          metric(metric),
          parallel(parallel) {}

      VectorSearchResult search(const VectorSearchRequest& request) override {
        VectorSearchResult merged;
        merged.scoresAreExact = true;
        merged.boundedSelection = VectorSelection::COMPLETE;
        merged.breadthExhausted = true;
        if (request.candidates <= 0) return merged;

        allocateToBudget(request);

        // DEPTH rescans only bounded slots, excluding banked identities and
        // starting a fresh cut. BREADTH visits new list tails for both policies.
        // Exact slots run once; their top-k witnesses survive every round.
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
            size_t start = depthGrew && !slot.ivfEngine->usesIntervals() ? 0 : scannedListCount[i];
            scannedListCount[i] = total;
            if (start >= total) continue;
            int64_t grain = std::max<int64_t>(1, scanTaskGrainVectors);
            size_t firstUnit = units.size();
            size_t chunkBegin = start;
            int64_t chunkLive = 0;
            for (size_t l = start; l < total; l++) {
              chunkLive += selectedListLiveSizes[i][l];
              if (chunkLive >= grain) {
                units.push_back({(int32_t)i, chunkBegin, l + 1, chunkLive});
                chunkBegin = l + 1;
                chunkLive = 0;
              }
            }
            if (chunkBegin < total) {
              if (units.size() > firstUnit) {
                units.back().listEnd = total;
                units.back().live += chunkLive;
              } else {
                units.push_back({(int32_t)i, chunkBegin, total, chunkLive});
              }
            }
          } else if (!coarseScanned) {
            units.push_back({(int32_t)i, 0, 0, slot.liveVectorCount});
          }
        }

        if (!units.empty()) {
          // A new cut is a fresh epoch, including DEPTH rounds that exclude
          // the banked hits. Carrying their previous floor would starve it.
          BoundedScanCut<Groups> cut(request.candidates, groupPlan, groupV2d, request.perKey);
          bool spawnTasks = parallel && units.size() > 1;
          if (spawnTasks) parallelScanRoundsForTests.fetch_add(1, std::memory_order_relaxed);
          TaskGroupRunner runner;
          task_group_fanout(spawnTasks ? runner.group() : nullptr, units.size(), [&](size_t i) {
            return [this, unit = units[i], &request, &cut]() { scanUnit(unit, request, cut); };
          });
          runner.join();
          merged = cut.take();
        }

        coarseScanned = true;
        intervals.compact();
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

  // Terminal selection has two disjoint read sets, both sorted by segment and
  // value rank. Exact slots reuse their scores; bounded group nominations read
  // the whole document. Interval records never acquire that bounded expansion.
  template <bool Groups = false>
  static void rescoreAndCollapseCandidates(std::vector<VectorEngineHit>& candidates,
                                           KnnIntervalCollector<Groups>& collector,
                                           IndexReader& reader,
                                           IndexReader::FieldSegments segInfos,
                                           std::span<const SegV2D> v2dPerSeg,
                                           std::span<const char> segApprox,
                                           const float* queryPtr, int32_t dims, int32_t metric,
                                           bool normalizeColumnOnCosineRescore,
                                           int64_t kDocs, std::vector<DocHit>& docHits,
                                           bool parallel, const CollapsePlan* groupPlan = nullptr) {
    auto rankOrder = [](const VectorEngineHit& a, const VectorEngineHit& b) {
      return a.segOrd != b.segOrd ? a.segOrd < b.segOrd : a.valueRank < b.valueRank;
    };
    std::sort(candidates.begin(), candidates.end(), rankOrder);
    UnitKeyReader<Groups> keyReader(groupPlan);
    for (size_t begin = 0; begin < candidates.size();) {
      int32_t segOrd = candidates[begin].segOrd;
      size_t end = begin + 1;
      while (end < candidates.size() && candidates[end].segOrd == segOrd) end++;
      bool exact = !segApprox[(size_t)segOrd];
      std::optional<VectorReader> column;
      std::optional<VectorReader::Iterator> docs;
      if constexpr (Groups) if (!exact) {
        column.emplace(reader.segments()[(size_t)segOrd].postingsReader(), *segInfos.at((size_t)segOrd));
        if (column->isMultiValued()) docs.emplace(column->strColReader());
      }
      int32_t lastDoc = -1;
      for (size_t i = begin; i < end; i++) {
        auto hit = candidates[i];
        int32_t doc = v2dPerSeg[(size_t)segOrd].resolve(hit.valueRank);
        const auto* key = keyReader.read(segOrd, doc);
        if (key == nullptr) continue;
        if (docs) {
          if (lastDoc == doc) continue;
          lastDoc = doc;
          if (docs->docId() < doc) docs->advance(doc);
          auto [first, last] = docs->valueRange();
          for (int64_t rank = first; rank < last; rank++) {
            collector.mandatory({hit.score, segOrd, rank}, *key, false);
          }
        } else collector.mandatory(hit, *key, exact);
      }
      begin = end;
    }

    auto& records = collector.retained();
    std::sort(records.begin(), records.end(), [&](const auto& a, const auto& b) {
      if (rankOrder(a.hit, b.hit)) return true;
      if (rankOrder(b.hit, a.hit)) return false;
      return a.exact > b.exact;
    });
    records.erase(std::unique(records.begin(), records.end(), [](const auto& a, const auto& b) {
      return a.hit.segOrd == b.hit.segOrd && a.hit.valueRank == b.hit.valueRank;
    }), records.end());
    for (auto& record : records) record.unit->seed = KnnIntervalCollector<Groups>::NO_SLOT;
    for (size_t i = 0; i < records.size(); i++) {
      auto& record = records[i];
      size_t& best = record.unit->seed;
      if (best == KnnIntervalCollector<Groups>::NO_SLOT || betterHit(record.hit, records[best].hit)) best = i;
      record.selected = record.mandatory || record.exact;
    }
    auto* resource = collector.memoryResource();
    std::pmr::vector<size_t> seeds(resource);
    auto betterSeed = [&](size_t a, size_t b) { return betterHit(records[a].hit, records[b].hit); };
    for (size_t i = 0; i < records.size(); i++) {
      if (records[i].unit->seed != i) continue;
      if ((int64_t)seeds.size() < kDocs) { seeds.push_back(i); std::push_heap(seeds.begin(), seeds.end(), betterSeed); }
      else if (betterSeed(i, seeds.front())) {
        std::pop_heap(seeds.begin(), seeds.end(), betterSeed);
        seeds.back() = i;
        std::push_heap(seeds.begin(), seeds.end(), betterSeed);
      }
    }
    for (size_t i : seeds) records[i].selected = true;
    float pessimisticFloor = collector.floorScore();

    auto readStage = [&](bool seed, float floor) {
      std::pmr::vector<size_t> selected(resource);
      for (size_t i = 0; i < records.size(); i++) {
        auto& record = records[i];
        if (record.exact) continue;
        if (seed ? record.selected : record.upper >= std::max(floor, record.unit->exact.load(std::memory_order_relaxed))) {
          selected.push_back(i);
        }
      }
      std::pmr::vector<std::pair<size_t, size_t>> buckets(resource);
      for (size_t b = 0; b < selected.size();) {
        size_t e = b + 1;
        while (e < selected.size() && records[selected[e]].hit.segOrd == records[selected[b]].hit.segOrd) e++;
        buckets.emplace_back(b, e);
        b = e;
      }
      auto readBucket = [&](size_t bucket) {
        return [&, bucket]() {
          auto [b, e] = buckets[bucket];
          int32_t segOrd = records[selected[b]].hit.segOrd;
          VectorReader column(reader.segments()[(size_t)segOrd].postingsReader(), *segInfos.at((size_t)segOrd));
          RowReadAhead::Gate gate;
          RowReadAhead ahead(gate, column.strColReader());
          auto rankAt = [&](int64_t i) { return records[selected[b + (size_t)i]].hit.valueRank; };
          int64_t count = (int64_t)(e - b), reads = 0;
          ahead.probe({rankAt(count / 4), rankAt(count / 2 + count / 4)});
          for (size_t i = b; i < e; i++) {
            auto& record = records[selected[i]];
            // Other values of this unit, including values in another segment,
            // may already have established a better exact member.
            if (!seed && record.upper < record.unit->exact.load(std::memory_order_relaxed)) continue;
            ahead.sparseAt((int64_t)(i - b), count, [&](int64_t pos) { auto rank = rankAt(pos); ahead.queueRows(rank, rank + 1); });
            float score = exactScore(queryPtr, column.vectorAtRank(record.hit.valueRank), dims,
                metric, normalizeColumnOnCosineRescore, record.hit.score);
            record.hit.score = score;
            record.exact = true;
            float best = record.unit->exact.load(std::memory_order_relaxed);
            while (score > best && !record.unit->exact.compare_exchange_weak(best, score, std::memory_order_relaxed)) {}
            reads++;
          }
          if (statsForTests) {
            (seed ? statsForTests->seedReads : statsForTests->remainingReads) += reads;
            statsForTests->vectors += reads;
            statsForTests->bytes += reads * dims * (int64_t)sizeof(float);
          }
        };
      };
      std::pmr::vector<size_t> tasks(resource), local(resource);
      for (size_t i = 0; i < buckets.size(); i++) {
        if (parallel && ((int64_t)(buckets[i].second - buckets[i].first) >= rescoreTaskGrainCandidates
                         || i + 1 == buckets.size())) tasks.push_back(i);
        else local.push_back(i);
      }
      TaskGroupRunner runner;
      task_group_fanout(tasks.size() > 1 ? runner.group() : nullptr, tasks.size(),
                       [&](size_t i) { return readBucket(tasks[i]); });
      for (size_t i : local) readBucket(i)();
      runner.join();
    };
    readStage(true, KnnIntervalCollector<Groups>::NO_FLOOR);
    std::pmr::vector<float> exactWitnesses(resource);
    for (size_t i = 0; i < records.size(); i++) {
      if (records[i].unit->seed != i) continue;
      float score = records[i].unit->exact.load(std::memory_order_relaxed);
      if (score == KnnIntervalCollector<Groups>::NO_FLOOR) continue;
      if ((int64_t)exactWitnesses.size() < kDocs) {
        exactWitnesses.push_back(score);
        std::push_heap(exactWitnesses.begin(), exactWitnesses.end(), std::greater<float>{});
      } else if (score > exactWitnesses.front()) {
        std::pop_heap(exactWitnesses.begin(), exactWitnesses.end(), std::greater<float>{});
        exactWitnesses.back() = score;
        std::push_heap(exactWitnesses.begin(), exactWitnesses.end(), std::greater<float>{});
      }
    }
    float exactFloor = (int64_t)exactWitnesses.size() == kDocs ? exactWitnesses.front() : KnnIntervalCollector<Groups>::NO_FLOOR;
    readStage(false, std::max(pessimisticFloor, exactFloor));

    auto docHit = [&](size_t i) {
      const auto& hit = records[i].hit;
      return DocHit{hit.segOrd, v2dPerSeg[(size_t)hit.segOrd].resolve(hit.valueRank), hit.score};
    };
    for (auto& record : records) record.unit->seed = KnnIntervalCollector<Groups>::NO_SLOT;
    for (size_t i = 0; i < records.size(); i++) if (records[i].exact) {
      size_t& best = records[i].unit->seed;
      if (best == KnnIntervalCollector<Groups>::NO_SLOT || betterDoc(docHit(i), docHit(best))) best = i;
    }
    docHits.clear();
    for (size_t i = 0; i < records.size(); i++) {
      if (records[i].unit->seed == i) offerBounded(docHits, docHit(i), kDocs, betterDoc);
    }
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

  static std::pair<float, float> scoreInterval(float distance, double error, int32_t metric) {
    float lo = std::nextafter((float)((double)distance - error), -INFINITY);
    float hi = std::nextafter((float)((double)distance + error), INFINITY);
    bool l2 = metric == (int32_t)api::VectorMetric::L2;
    if (l2) {
      lo = std::max(0.0f, lo);
      hi = std::max(0.0f, hi);
      std::swap(lo, hi);
    }
    float upper = std::nextafter(scoreFromDist(hi, metric), INFINITY);
    return {std::nextafter(scoreFromDist(lo, metric), -INFINITY), l2 ? std::min(1.0f, upper) : upper};
  }

  // Invert a score floor outward, admitting equal scores and outward-rounded
  // interval endpoints. Only floor changes pay for these rounding operations.
  static float distanceThreshold(float floor, int32_t metric) {
    float score = std::nextafter(std::nextafter(floor, -INFINITY), -INFINITY);
    if (metric != (int32_t)api::VectorMetric::L2) return std::nextafter(score, -INFINITY);
    if (!(score > 0)) return std::numeric_limits<float>::infinity();
    // Inverting the rounded denominator needs its own outward step.
    float denominator = std::nextafter((float)(1.0 / (double)score), INFINITY);
    float distance = score >= 1.0f ? 1.0f - score : (float)((double)denominator - 1.0);
    return std::nextafter(std::nextafter(distance, INFINITY), INFINITY);
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

};

} // namespace luxir
