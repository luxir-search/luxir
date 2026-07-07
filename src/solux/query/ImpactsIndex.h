#pragma once

#include <algorithm>
#include <vector>

#include "solux/reader/DocsEnum.h"
#include "solux/search/Similarity.h"
#include "solux/util/MemPool.h"

namespace solux {

// Per-doc-block score upper bounds for one term's postings, built from the
// impact data stored in the docs-stream block headers.  Scorers consult it to
// skip doc blocks that cannot beat the collector's min-competitive score.
//
// The build is GROUP-FIRST AND LAZY: construction walks only the L1 group
// headers (~df/4096 steps, hopping group bodies), scoring each group's stored
// corner span (maxTf, minNorm).  A group's 32 per-block frontiers are parsed
// and BM25-scored only the first time a query resolves a block inside it.
// Never walk the whole postings up front - a "the"-sized term has ~35K block
// headers and queries touch a handful.
//
// The simScorer defines what "score" means.  A term scorer passes its own BM25
// scorer.  PhraseQuery passes the PHRASE's scorer over each member term's
// data: phraseFreq <= that term's freq in every doc, so each term's bound is
// a valid (if loose) upper bound on the phrase's score, and the min across
// the terms' indexes bounds any doc range.
class ImpactsIndex {
  struct Chunk {
    int32_t blockCount = 0;
    int32_t* lastDocs = nullptr;
    float* impacts = nullptr;
    float* suffixWithin = nullptr;  // max of impacts[i..] within the chunk
  };

  MemPool* pool = nullptr;
  const DocsEnum* docsEnum = nullptr;
  Similarity::BM25Scorer* simScorer = nullptr;
  float boost = 1.0f;
  bool useFrontierBound = true;

  int32_t count = 0;       // total blocks
  int32_t groupCount = 0;
  int32_t* groupLastDocs = nullptr;
  int32_t* groupBaseLastDocs = nullptr;
  int64_t* groupBodyOffs = nullptr;
  float* groupUpper = nullptr;       // corner bound per group
  float* groupSuffixUpper = nullptr; // max of groupUpper[g..]
  mutable Chunk** chunks = nullptr;  // lazily parsed per group

  static constexpr int32_t GROUP = DocsEnum::L1_PERIOD;

  const Chunk& ensureGroup(int32_t g) const {
    assert(g >= 0 && g < groupCount);
    if (chunks[g] != nullptr) {
      return *chunks[g];
    }
    // scratch is transient; chunk arrays live in the pool
    std::vector<int32_t> lastDocs;
    std::vector<int32_t> maxTf;
    std::vector<int32_t> minNorm;
    DocsEnum::ImpactFrontiers frontiers;
    docsEnum->readGroupBlockImpacts(g, groupBodyOffs[g], groupBaseLastDocs[g], lastDocs,
                                    maxTf, minNorm, useFrontierBound ? &frontiers : nullptr);
    auto* chunk = pool->make<Chunk>();
    chunk->blockCount = (int32_t) lastDocs.size();
    chunk->lastDocs = pool->make_arr<int32_t>(lastDocs.size());
    chunk->impacts = pool->make_arr<float>(lastDocs.size());
    chunk->suffixWithin = pool->make_arr<float>(lastDocs.size());
    for (size_t i = 0; i < lastDocs.size(); i++) {
      chunk->lastDocs[i] = lastDocs[i];
      if (useFrontierBound) {
        int32_t start = frontiers.offsets[i];
        int32_t end = frontiers.offsets[i + 1];
        if (start < end) {
          float maxImpact = 0.0f;
          for (int32_t j = start; j < end; j++) {
            maxImpact = std::max(
                maxImpact, boost * simScorer->score((float) frontiers.tfs[(size_t) j],
                                                    (int64_t) frontiers.norms[(size_t) j]));
          }
          chunk->impacts[i] = maxImpact;
          continue;
        }
      }
      chunk->impacts[i] = boost * simScorer->score((float) maxTf[i], (int64_t) minNorm[i]);
    }
    float running = 0.0f;
    for (int32_t i = chunk->blockCount - 1; i >= 0; i--) {
      running = std::max(running, chunk->impacts[i]);
      chunk->suffixWithin[i] = running;
    }
    chunks[g] = chunk;
    return *chunk;
  }

public:
  void build(MemPool& pool_, DocsEnum& docsEnum_, Similarity::BM25Scorer& simScorer_,
             float boost_, bool useFrontierBound_ = true) {
    DocsEnum::GroupImpacts groups;
    docsEnum_.readGroupImpacts(groups);
    if (groups.lastDocs.empty()) {
      return;
    }
    pool = &pool_;
    docsEnum = &docsEnum_;
    simScorer = &simScorer_;
    boost = boost_;
    useFrontierBound = useFrontierBound_;

    groupCount = (int32_t) groups.lastDocs.size();
    count = docsEnum_.numImpactBlocks();
    groupLastDocs = pool->make_arr<int32_t>((size_t) groupCount);
    groupBaseLastDocs = pool->make_arr<int32_t>((size_t) groupCount);
    groupBodyOffs = pool->make_arr<int64_t>((size_t) groupCount);
    groupUpper = pool->make_arr<float>((size_t) groupCount);
    groupSuffixUpper = pool->make_arr<float>((size_t) groupCount);
    chunks = pool->make_arr<Chunk*>((size_t) groupCount);
    for (int32_t g = 0; g < groupCount; g++) {
      groupLastDocs[g] = groups.lastDocs[(size_t) g];
      groupBaseLastDocs[g] = groups.baseLastDocs[(size_t) g];
      groupBodyOffs[g] = groups.bodyOffsets[(size_t) g];
      groupUpper[g] = boost * simScorer->score((float) groups.spanMaxTfs[(size_t) g],
                                               (int64_t) groups.spanMinNorms[(size_t) g]);
      chunks[g] = nullptr;
    }
    float running = 0.0f;
    for (int32_t g = groupCount - 1; g >= 0; g--) {
      running = std::max(running, groupUpper[g]);
      groupSuffixUpper[g] = running;
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
    const Chunk& chunk = ensureGroup(block / GROUP);
    return chunk.lastDocs[block % GROUP];
  }

  float impact(int32_t block) const {
    assert(block >= 0 && block < count);
    const Chunk& chunk = ensureGroup(block / GROUP);
    return chunk.impacts[block % GROUP];
  }

  float maxImpactFrom(int32_t block) const {
    assert(block >= 0 && block < count);
    int32_t g = block / GROUP;
    const Chunk& chunk = ensureGroup(g);
    float bound = chunk.suffixWithin[block % GROUP];
    if (g + 1 < groupCount) {
      bound = std::max(bound, groupSuffixUpper[g + 1]);
    }
    return bound;
  }

  // Max impact over blocks [fromBlock, toBlock], using parse-free group corner
  // bounds for fully covered middle groups (only the edge groups pay a parse).
  float maxImpactInRange(int32_t fromBlock, int32_t toBlock) const {
    assert(fromBlock >= 0 && fromBlock <= toBlock && toBlock < count);
    int32_t gFrom = fromBlock / GROUP;
    int32_t gTo = toBlock / GROUP;
    if (gFrom == gTo) {
      const Chunk& chunk = ensureGroup(gFrom);
      float bound = 0.0f;
      for (int32_t i = fromBlock % GROUP; i <= toBlock % GROUP; i++) {
        bound = std::max(bound, chunk.impacts[i]);
      }
      return bound;
    }
    const Chunk& head = ensureGroup(gFrom);
    float bound = head.suffixWithin[fromBlock % GROUP];
    for (int32_t g = gFrom + 1; g < gTo; g++) {
      bound = std::max(bound, groupUpper[g]);
    }
    const Chunk& tail = ensureGroup(gTo);
    for (int32_t i = 0; i <= toBlock % GROUP; i++) {
      bound = std::max(bound, tail.impacts[i]);
    }
    return bound;
  }

  // First doc at or after `doc` that lies in a block whose bound reaches
  // minScore: hops non-competitive GROUPS on their corner bounds without
  // parsing them, so a term-wide skip costs a group-table scan, not a
  // per-block walk (Lucene ImpactsDISI's getSkipUpTo shape).  Returns `doc`
  // itself when its own block competes (or lies past the impact data), and
  // DocsEnum::END when nothing later can compete.
  int32_t firstCompetitiveTarget(int32_t doc, float minScore, int64_t& skippedBlocks) const {
    const int32_t* it = std::lower_bound(groupLastDocs, groupLastDocs + groupCount, doc);
    int32_t g = (int32_t) (it - groupLastDocs);
    if (g >= groupCount) {
      return doc;  // past the impact data; the enum will run out naturally
    }
    for (;;) {
      if (groupSuffixUpper[g] < minScore) {
        skippedBlocks += count - g * GROUP;
        return DocsEnum::END;  // no group from here on can compete
      }
      if (groupUpper[g] >= minScore) {
        const Chunk& chunk = ensureGroup(g);
        const int32_t* bit =
            std::lower_bound(chunk.lastDocs, chunk.lastDocs + chunk.blockCount, doc);
        int32_t i = (int32_t) (bit - chunk.lastDocs);
        for (; i < chunk.blockCount; i++) {
          if (chunk.impacts[i] >= minScore) {
            int32_t blockStart = i == 0 ? (g == 0 ? 0 : groupLastDocs[g - 1] + 1)
                                        : chunk.lastDocs[i - 1] + 1;
            return std::max(doc, blockStart);
          }
          skippedBlocks++;
        }
      } else {
        skippedBlocks += GROUP;
      }
      if (g + 1 >= groupCount) {
        return DocsEnum::END;  // scanned to the end without a competitive block
      }
      doc = groupLastDocs[g] + 1;
      g++;
    }
  }

  // Index of the block containing target (the first block whose lastDoc >=
  // target); blockCount() when target is past the last block, or when the
  // index is empty.
  int32_t blockContaining(int32_t target) const {
    const int32_t* it = std::lower_bound(groupLastDocs, groupLastDocs + groupCount, target);
    int32_t g = (int32_t) (it - groupLastDocs);
    if (g >= groupCount) {
      return count;
    }
    const Chunk& chunk = ensureGroup(g);
    const int32_t* bit = std::lower_bound(chunk.lastDocs, chunk.lastDocs + chunk.blockCount, target);
    return g * GROUP + (int32_t) (bit - chunk.lastDocs);
  }

  // blockContaining with a resume hint for monotone callers (block-max hops,
  // window walks): gallop forward over the group table from `from`'s group.
  // A target behind the hinted block falls back to the full search.
  int32_t blockContainingFrom(int32_t from, int32_t target) const {
    if (from < 0 || from >= count) {
      return from >= count && from >= 0 ? count : blockContaining(target);
    }
    int32_t g = from / GROUP;
    if (g > 0 && target <= groupLastDocs[g - 1]) {
      return blockContaining(target);  // moved backward; rare
    }
    // gallop over groups
    int32_t lo = g;
    int32_t step = 1;
    while (lo < groupCount && groupLastDocs[lo] < target) {
      lo += step;
      step <<= 1;
    }
    if (lo >= groupCount) {
      lo = groupCount;
    }
    int32_t bracketLo = std::max(g, lo - (step >> 1));
    const int32_t* begin = groupLastDocs + bracketLo;
    const int32_t* end = groupLastDocs + std::min(lo + 1, groupCount);
    const int32_t* it = std::lower_bound(begin, end, target);
    int32_t hitGroup = (int32_t) (it - groupLastDocs);
    if (hitGroup >= groupCount) {
      return count;
    }
    const Chunk& chunk = ensureGroup(hitGroup);
    const int32_t* bit =
        std::lower_bound(chunk.lastDocs, chunk.lastDocs + chunk.blockCount, target);
    return hitGroup * GROUP + (int32_t) (bit - chunk.lastDocs);
  }
};

} // namespace solux
