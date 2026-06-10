#pragma once

#include <cstdint>
#include <vector>

namespace solux {

/// One round of the host's widen loop.  The host re-enters search() with a
/// monotonically growing request along two axes:
///   - candidates (DEPTH): more results from the same recall effort.  Engines
///     without resumable iteration re-return prior hits at the larger count;
///     the host dedups, so that is correct (just not free).
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
