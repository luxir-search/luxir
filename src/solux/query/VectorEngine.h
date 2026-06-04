#pragma once

#include <cstdint>
#include <vector>

namespace solux {

struct VectorSearchRequest {
  const float* query = nullptr;
  int32_t dims = 0;
  int64_t candidates = 0;
  // Domain filtering is supplied by engine construction today.  Flat FAISS
  // receives it through SearchParameters::sel instead of per request state.
  int32_t breadth = 0;
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

struct VectorSearchResult {
  std::vector<VectorEngineHit> hits;
  bool poolExhausted = false;
  bool breadthExhausted = true;
  int32_t nextBreadth = 0;
};

class VectorEngine {
public:
  virtual ~VectorEngine() = default;

  virtual VectorSearchResult search(const VectorSearchRequest& request) = 0;
};

} // namespace solux
