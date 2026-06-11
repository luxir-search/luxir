#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace solux {

/// One round of the host's widen loop.  The host re-enters search() along two
/// axes, growing exactly one per round:
///   - candidates (DEPTH): more results from the same recall effort.  On
///     round 1 (no `eligible` bitmaps) this is a plain pool-depth request.
///     From the first deepen round on, the request carries per-segment
///     `eligible` bitmaps with already-pooled ranks cleared and candidates
///     becomes a PER-ROUND FRESH BUDGET - it may repeat or shrink across
///     rounds - because every engine must exclude cleared ranks and so
///     spends the whole result heap on fresh hits (a poor-man's resumable
///     cursor).  The host plans round sizes and termination around that
///     fresh-only property; an engine that re-returned pooled hits would
///     stall the widen loop, not just waste work.
///   - breadth: more recall effort (for IVF, more lists; engine-defined for
///     other index kinds).  Units are merge-stable "reference index" lists:
///     the lists a single IVF index with nlist=sqrt(live vectors) would
///     probe.  0 means the engine picks its adaptive default.
struct VectorSearchRequest {
  const float* query = nullptr;
  int32_t dims = 0;
  int64_t candidates = 0;
  // Domain filtering is supplied by engine construction today.  Flat FAISS
  // receives it through SearchParameters::sel instead of per request state.
  int32_t breadth = 0;
  // Floor on the fraction of live vectors the engine examines, in [0,1];
  // raises the effective breadth when breadth maps below it.  1.0 forces a
  // full scan.  Exact engines satisfy it trivially.
  float minScanFraction = 0.0f;
  // Per-segment exclusion bitmaps (LSB-first bytes, bit r = vector rank r),
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
};

/// Candidate from the vector engine seam.  score is exact for flat engines;
/// quantized engines may return an approximate score and must be rescored by
/// the host before doc collapse.  valueRank is segment-relative: it is the
/// vector's ordinal within segOrd's vector column, not a shard-level FAISS id.
struct VectorEngineHit {
  float score = 0.0f;
  int32_t segOrd = -1;
  int64_t valueRank = -1;
};
static_assert(sizeof(VectorEngineHit) == 16);

/// The exhaustion flags drive the host's deepen policy, tried in this order:
///   !poolExhausted                      -> grow candidates (depth) and retry.
///   poolExhausted && !breadthExhausted  -> grow breadth and retry.
///   poolExhausted && breadthExhausted   -> nothing left; stop.
/// poolExhausted means "this effort level cannot yield more hits" (the heap
/// was not filled, or everything eligible at this breadth was returned).
/// breadthExhausted means there is no more effort to add (exact engines
/// always set it; IVF sets it when every list everywhere has been probed).
struct VectorSearchResult {
  std::vector<VectorEngineHit> hits;
  // False means hit.score is only an engine approximation; the host must
  // rescore from full-precision column vectors before sorting / doc collapse.
  bool scoresAreExact = false;
  // Refinement when scoresAreExact is false: segments whose hits in this
  // result carry exact scores anyway (a multi-engine merge where only some
  // segments are approximate).  The host's terminal rescore keeps those
  // hits' scores instead of re-reading their column vectors.  An engine that
  // cannot attribute exactness per segment leaves this empty - every hit is
  // then treated as approximate, which is always safe.
  std::vector<int32_t> exactSegOrds;
  bool poolExhausted = false;
  bool breadthExhausted = true;
  // Engine's hint for the next useful breadth value (the smallest one that
  // adds work).  0 = no hint; the host then steps breadth by 1.
  int32_t nextBreadth = 0;
};

/// Seam between the host widen/refine loop (KnnQuery::Weight::prepare) and a
/// vector index implementation.  Engines return raw vector candidates in
/// segment-local identities and report the taxonomy above; everything
/// cross-cutting stays in the host: candidate pooling and dedup across
/// rounds, the deepen policy, exact rescore, multi-valued doc collapse, and
/// the K-distinct-docs guarantee.  An engine therefore never needs to know
/// about docs, only vectors.
class VectorEngine {
public:
  virtual ~VectorEngine() = default;

  virtual VectorSearchResult search(const VectorSearchRequest& request) = 0;
};

} // namespace solux
