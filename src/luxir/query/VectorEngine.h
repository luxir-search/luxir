// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace luxir {

/// One round of WIDEN. Bounded collectors receive a fresh candidate budget
/// and exclude already returned ranks. Interval collectors persist witnesses
/// and retained records for the query: DEPTH never rescans them; BREADTH scans
/// only newly selected lists. Exact slots supply distinct-unit top-k witnesses.
struct VectorSearchRequest {
  const float* query = nullptr;
  int32_t dims = 0;
  // Bounded-channel budget; group bounded cuts count documents.
  // IVF applies one shared bounded cut across its bounded segments and chunks
  // for this round. A fresh-only depth request starts a new cut/floor.
  int64_t candidates = 0;
  // Domain filtering is supplied by engine construction. IVF scanners
  // apply it through task-local selectors instead of per-request state.
  int32_t breadth = 0;
  // Floor on the fraction of live vectors the engine examines, in [0,1];
  // raises the effective breadth when breadth maps below it.  1.0 forces a
  // full scan.  Exact engines satisfy it trivially.
  float minScanFraction = 0.0f;
  // Bounded-channel exclusion bitmaps (LSB-first bytes, bit r = vector rank r),
  // indexed by segOrd; empty (or an empty per-segment span) until the host's
  // first deepen round, when the host folds its pooled-hit set into them.
  // Contract:
  //   - A CLEARED bit means the rank is already in the host pool and MUST
  //     NOT be returned.  This is a return filter only: whether cleared
  //     ranks are visited or scored during traversal is engine business.
  //     An engine that cannot push the filter down may drop cleared ranks
  //     from its returned hits ONLY if the result heap is then refilled
  //     with non-cleared ranks (scan past the pooled prefix) or the drops
  //     are folded into its exhaustion answer - post-filtering a
  //     fixed-depth scan under-returns fresh hits while still reporting an
  //     unexhausted pool, which starves the host's depth axis (and never
  //     trips the stale-hit counter, because the pooled hits never arrive).
  //   - A SET bit promises NOTHING: not liveness, not domain membership.
  //     The host seeds these bitmaps from per-query copies of the liveness
  //     bitmap only where one exists; an engine's own domain/liveness
  //     filtering still applies in full.
  //   - The spans are valid only for the duration of this search() call.
  //     The host mutates the backing bytes between rounds; never cache them.
  std::span<const std::span<const uint8_t>> eligible;
  // Bounded group policy only; interval collectors have no member cap.
  int64_t perKey = 1;
};

/// Candidate from the vector engine seam. score is exact for flat engines;
/// quantized engines may return an internal ordering score, possibly above
/// the public L2 range [0, 1] for negative distance estimates. The host must
/// rescore before the final cut and similarity filtering. valueRank is
/// segment-relative: the vector's ordinal within segOrd's vector column,
/// not a shard-level FAISS id.
struct VectorEngineHit {
  float score = 0.0f;
  int32_t segOrd = -1;
  int64_t valueRank = -1;
};
static_assert(sizeof(VectorEngineHit) == 16);

enum class VectorSelection { COMPLETE, MORE_DEPTH };

/// Completion and breadth are independent. MORE_DEPTH reports unreturned
/// bounded candidates at this breadth. COMPLETE means no bounded depth remains
/// (including queries with no bounded channel). The host also stops a bounded
/// channel once its distinct-unit/count obligation is met. Interval selection
/// completes when the selected lists have been scanned, regardless of retained
/// size; a small interval set can request BREADTH, never DEPTH.
/// Resource exhaustion throws through the request-memory breaker, never marks
/// an incomplete interval selection complete.
struct VectorSearchResult {
  std::vector<VectorEngineHit> hits;
  // False means hit.score is only an engine approximation; the host must
  // rescore from full-precision column vectors before the final cut and
  // similarity filtering.
  bool scoresAreExact = false;
  // Refinement when scoresAreExact is false: segments whose hits in this
  // result carry exact scores anyway (a multi-engine merge where only some
  // segments are approximate).  The host's terminal rescore keeps those
  // hits' scores instead of re-reading their column vectors.  An engine that
  // cannot attribute exactness per segment leaves this empty - every hit is
  // then treated as approximate, which is always safe.
  std::vector<int32_t> exactSegOrds;
  VectorSelection boundedSelection = VectorSelection::COMPLETE;
  bool breadthExhausted = true;
  // Engine's hint for the next useful breadth value (the smallest one that
  // adds work).  0 = no hint; the host then steps breadth by 1.
  int32_t nextBreadth = 0;
};

/// Engines return fresh bounded and exact candidates here. Query-owned interval
/// state bypasses this result cut and survives every round. Exact scores and
/// interval lower endpoints may witness the true k-th unit; bounded estimates
/// never do. The host owns bounded pooling, depth obligations and terminal
/// exact scoring. Group identities come from the enclosing CollapsePlan.
class VectorEngine {
public:
  virtual ~VectorEngine() = default;

  virtual VectorSearchResult search(const VectorSearchRequest& request) = 0;
};

} // namespace luxir
