#pragma once

#include <algorithm>
#include <vector>

#include "solux/reader/DocsEnum.h"
#include "solux/search/Similarity.h"
#include "solux/util/MemPool.h"

namespace solux {

// Per-doc-block score upper bounds for one term's postings, built from the
// impact data stored in the docs-stream block headers (DocsEnum::readBlockMaxTf).
// Scorers consult it to skip doc blocks that cannot beat the collector's
// min-competitive score.
//
// The simScorer defines what "score" means.  A term scorer passes its own BM25
// scorer.  PhraseQuery passes the PHRASE's scorer over each member term's
// frontier: phraseFreq <= that term's freq in every doc, so each term's bound
// is a valid (if loose) upper bound on the phrase's score, and the min across
// the terms' indexes bounds any doc range.
class ImpactsIndex {
  int32_t count = 0;
  int32_t* lastDocs = nullptr;  // last doc id of each block
  float* impacts = nullptr;     // per-block max possible score
  float* suffixMax = nullptr;   // max of impacts[block..]

public:
  // T2 frontier bound: evaluate each block's impact at all real Pareto frontier
  // points.  The old T1 corner bound (blockMaxTf x blockMinNorm) remains
  // available for A/B measurement.
  void build(MemPool& pool, DocsEnum& docsEnum, Similarity::BM25Scorer& simScorer,
             float boost, bool useFrontierBound = true) {
    std::vector<int32_t> blockMaxTf;
    std::vector<int32_t> blockLastDoc;
    std::vector<int32_t> blockMinNorm;
    DocsEnum::ImpactFrontiers impactFrontiers;
    docsEnum.readBlockMaxTf(blockMaxTf, nullptr, &blockLastDoc, &blockMinNorm,
                            nullptr, useFrontierBound ? &impactFrontiers : nullptr);
    if (blockMaxTf.empty()) {
      return;
    }
    assert(blockMaxTf.size() == blockLastDoc.size());
    assert(blockMaxTf.size() == blockMinNorm.size());
    assert(!useFrontierBound
           || impactFrontiers.offsets.size() == blockMaxTf.size() + 1);

    count = (int32_t) blockMaxTf.size();
    lastDocs = pool.make_arr<int32_t>((size_t) count);
    impacts = pool.make_arr<float>((size_t) count);
    suffixMax = pool.make_arr<float>((size_t) count);

    for (int32_t i = 0; i < count; i++) {
      lastDocs[i] = blockLastDoc[i];
      if (useFrontierBound) {
        int32_t start = impactFrontiers.offsets[(size_t) i];
        int32_t end = impactFrontiers.offsets[(size_t) i + 1];
        if (start < end) {
          float maxImpact = 0.0f;
          for (int32_t j = start; j < end; j++) {
            maxImpact = std::max(
                maxImpact,
                boost * simScorer.score((float) impactFrontiers.tfs[(size_t) j],
                                        (int64_t) impactFrontiers.norms[(size_t) j]));
          }
          impacts[i] = maxImpact;
          continue;
        }
      }
      impacts[i] = boost * simScorer.score((float) blockMaxTf[i], (int64_t) blockMinNorm[i]);
    }
    float running = 0.0f;
    for (int32_t i = count - 1; i >= 0; i--) {
      running = std::max(running, impacts[i]);
      suffixMax[i] = running;
    }
  }

  bool empty() const {
    return count == 0;
  }

  int32_t blockCount() const {
    return count;
  }

  int32_t lastDoc(int32_t block) const {
    assert(block >= 0 && block < count);
    return lastDocs[block];
  }

  float impact(int32_t block) const {
    assert(block >= 0 && block < count);
    return impacts[block];
  }

  float maxImpactFrom(int32_t block) const {
    assert(block >= 0 && block < count);
    return suffixMax[block];
  }

  // Index of the block containing target (the first block whose lastDoc >=
  // target); blockCount() when target is past the last block, or when the
  // index is empty.
  int32_t blockContaining(int32_t target) const {
    const int32_t* begin = lastDocs;
    const int32_t* end = lastDocs + count;
    const int32_t* it = std::lower_bound(begin, end, target);
    return (int32_t) (it - begin);
  }

  // blockContaining with a resume hint for monotone callers (block-max hops,
  // window walks): gallop forward from `from` instead of re-searching the
  // whole array.  `from` may be -1 or stale-behind; a target behind the hinted
  // block falls back to the full search.
  int32_t blockContainingFrom(int32_t from, int32_t target) const {
    if (from < 0) {
      return blockContaining(target);
    }
    if (from >= count) {
      return count;  // cursor already past the last block; targets only grow
    }
    if (from > 0 && target <= lastDocs[from - 1]) {
      return blockContaining(target);  // moved backward; rare
    }
    // gallop: probe from+1, from+2, from+4, ... then binary search the bracket
    int32_t lo = from;
    int32_t step = 1;
    while (lo < count && lastDocs[lo] < target) {
      lo += step;
      step <<= 1;
    }
    if (lo >= count) {
      lo = count;
    }
    int32_t bracketLo = std::max(from, lo - (step >> 1));
    const int32_t* begin = lastDocs + bracketLo;
    const int32_t* end = lastDocs + std::min(lo + 1, count);
    const int32_t* it = std::lower_bound(begin, end, target);
    return (int32_t) (it - lastDocs);
  }
};

} // namespace solux
