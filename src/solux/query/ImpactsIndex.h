#pragma once

#include <algorithm>
#include <limits>
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
// impact frontier.  A group's 32 per-block frontiers are parsed and
// BM25-scored only the first time a query resolves a block inside it.
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
  float* groupUpper = nullptr;       // frontier bound per group
  float* groupSuffixUpper = nullptr; // max of groupUpper[g..]
  mutable Chunk** chunks = nullptr;  // lazily parsed per group

  static constexpr int32_t GROUP = DocsEnum::L1_PERIOD;

  int32_t blockCountForGroup(int32_t g) const {
    return std::min(GROUP, count - g * GROUP);
  }

  float scoreImpact(int32_t tf, int32_t norm) const {
    return boost * simScorer->score((float) tf, (int64_t) norm);
  }

  const Chunk& ensureGroup(int32_t g) const {
    assert(g >= 0 && g < groupCount);
    if (chunks[g] != nullptr) {
      return *chunks[g];
    }
    skipCount(SkipStats::impactL0GroupParses);
    auto* chunk = pool->make<Chunk>();
    chunk->blockCount = blockCountForGroup(g);
    chunk->lastDocs = pool->make_arr<int32_t>((size_t) chunk->blockCount);
    chunk->impacts = pool->make_arr<float>((size_t) chunk->blockCount);
    chunk->suffixWithin = pool->make_arr<float>((size_t) chunk->blockCount);

    DocsEnum::GroupBlockImpactScratch scratch;
    docsEnum->visitGroupBlockImpacts(
        g, groupBodyOffs[g], groupBaseLastDocs[g], scratch,
        [&](int32_t block, int32_t lastDoc, int32_t maxTf, int32_t minNorm,
            std::span<const int32_t> tfs, std::span<const int32_t> norms,
            bool frontierSpilled) {
          assert(block >= 0 && block < chunk->blockCount);
          chunk->lastDocs[block] = lastDoc;
          if (useFrontierBound && !frontierSpilled && !tfs.empty()) {
            float maxImpact = 0.0f;
            for (size_t j = 0; j < tfs.size(); j++) {
              maxImpact = std::max(maxImpact, scoreImpact(tfs[j], norms[j]));
            }
            chunk->impacts[block] = maxImpact;
          } else {
            chunk->impacts[block] = scoreImpact(maxTf, minNorm);
          }
        }
    );
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
    static thread_local DocsEnum::GroupImpacts groups;  // reused; reset by readGroupImpacts
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
      int32_t fStart = groups.frontiers.offsets[(size_t) g];
      int32_t fEnd = groups.frontiers.offsets[(size_t) g + 1];
      if (useFrontierBound && fStart < fEnd) {
        // Stored group frontier: real (norm, tf) pairs, so the bound is what
        // some doc in the group can actually score - no cross-doc corner slack.
        float maxImpact = 0.0f;
        for (int32_t j = fStart; j < fEnd; j++) {
          maxImpact = std::max(
              maxImpact, scoreImpact(groups.frontiers.tfs[(size_t) j],
                                     groups.frontiers.norms[(size_t) j]));
        }
        groupUpper[g] = maxImpact;
      } else {
        groupUpper[g] = scoreImpact(groups.spanMaxTfs[(size_t) g],
                                    groups.spanMinNorms[(size_t) g]);
      }
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

  int32_t numGroups() const {
    return groupCount;
  }

  int32_t groupContainingFrom(int32_t fromGroup, int32_t target) const {
    if (groupCount == 0) {
      return 0;
    }
    if (fromGroup < 0 || fromGroup >= groupCount) {
      if (fromGroup >= groupCount && fromGroup >= 0) {
        return groupCount;
      }
      const int32_t* it = std::lower_bound(groupLastDocs, groupLastDocs + groupCount, target);
      return (int32_t) (it - groupLastDocs);
    }
    if (fromGroup > 0 && target <= groupLastDocs[fromGroup - 1]) {
      const int32_t* it = std::lower_bound(groupLastDocs, groupLastDocs + groupCount, target);
      return (int32_t) (it - groupLastDocs);
    }
    int32_t lo = fromGroup;
    int32_t step = 1;
    while (lo < groupCount && groupLastDocs[lo] < target) {
      lo += step;
      step <<= 1;
    }
    if (lo >= groupCount) {
      lo = groupCount;
    }
    int32_t bracketLo = std::max(fromGroup, lo - (step >> 1));
    const int32_t* begin = groupLastDocs + bracketLo;
    const int32_t* end = groupLastDocs + std::min(lo + 1, groupCount);
    const int32_t* it = std::lower_bound(begin, end, target);
    return (int32_t) (it - groupLastDocs);
  }

  int32_t groupLastDoc(int32_t group) const {
    assert(group >= 0 && group < groupCount);
    return groupLastDocs[group];
  }

  bool groupParsed(int32_t group) const {
    assert(group >= 0 && group < groupCount);
    return chunks[group] != nullptr;
  }

  int32_t groupFirstBlock(int32_t group) const {
    assert(group >= 0 && group < groupCount);
    return group * GROUP;
  }

  int32_t groupLastBlock(int32_t group) const {
    assert(group >= 0 && group < groupCount);
    return group * GROUP + blockCountForGroup(group) - 1;
  }

  int32_t blockContainingInParsedGroup(int32_t group, int32_t target) const {
    assert(group >= 0 && group < groupCount);
    assert(chunks[group] != nullptr);
    const Chunk& chunk = *chunks[group];
    const int32_t* it = std::lower_bound(chunk.lastDocs, chunk.lastDocs + chunk.blockCount,
                                         target);
    return group * GROUP + (int32_t) (it - chunk.lastDocs);
  }

  int32_t parsedBlockLastDoc(int32_t block) const {
    assert(block >= 0 && block < count);
    int32_t group = block / GROUP;
    assert(chunks[group] != nullptr);
    return chunks[group]->lastDocs[block % GROUP];
  }

  float parsedBlockImpact(int32_t block) const {
    assert(block >= 0 && block < count);
    int32_t group = block / GROUP;
    assert(chunks[group] != nullptr);
    return chunks[group]->impacts[block % GROUP];
  }

  float maxGroupImpactFrom(int32_t group) const {
    assert(group >= 0 && group < groupCount);
    skipCount(SkipStats::impactGroupBoundCalls);
    skipCount(SkipStats::impactGroupBoundNoL0);
    return groupSuffixUpper[group];
  }

  float maxGroupImpactInRange(int32_t fromGroup, int32_t toGroup) const {
    assert(fromGroup >= 0 && fromGroup <= toGroup && toGroup < groupCount);
    skipCount(SkipStats::impactGroupBoundCalls);
    skipCount(SkipStats::impactGroupBoundNoL0);
    if (toGroup == groupCount - 1) {
      return groupSuffixUpper[fromGroup];
    }
    float bound = 0.0f;
    for (int32_t g = fromGroup; g <= toGroup; g++) {
      bound = std::max(bound, groupUpper[g]);
    }
    return bound;
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

  // Max impact over blocks [fromBlock, toBlock], using parse-free group
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

  // Max impact over blocks [fromBlock, toBlock] without materializing any
  // additional group. Parsed groups use their block impacts; unparsed groups
  // fall back to their group frontier bound.
  float maxImpactInRangeNoParse(int32_t fromBlock, int32_t toBlock) const {
    assert(fromBlock >= 0 && fromBlock <= toBlock && toBlock < count);
    int32_t gFrom = fromBlock / GROUP;
    int32_t gTo = toBlock / GROUP;
    float bound = 0.0f;
    bool usedGroupBound = false;
    for (int32_t g = gFrom; g <= gTo; g++) {
      int32_t first = std::max(fromBlock, groupFirstBlock(g));
      int32_t last = std::min(toBlock, groupLastBlock(g));
      if (chunks[g] == nullptr) {
        bound = std::max(bound, groupUpper[g]);
        usedGroupBound = true;
        continue;
      }
      const Chunk& chunk = *chunks[g];
      for (int32_t block = first; block <= last; block++) {
        bound = std::max(bound, chunk.impacts[block % GROUP]);
      }
    }
    if (usedGroupBound) {
      skipCount(SkipStats::impactGroupBoundCalls);
      skipCount(SkipStats::impactGroupBoundNoL0);
    }
    return bound;
  }

  // Tight block-granular bound over [fromBlock, toBlock]. This is the explicit
  // refinement path: every covering group may be parsed.
  float maxImpactInRangeParsed(int32_t fromBlock, int32_t toBlock) const {
    assert(fromBlock >= 0 && fromBlock <= toBlock && toBlock < count);
    int32_t gFrom = fromBlock / GROUP;
    int32_t gTo = toBlock / GROUP;
    float bound = 0.0f;
    for (int32_t g = gFrom; g <= gTo; g++) {
      const Chunk& chunk = ensureGroup(g);
      int32_t first = std::max(fromBlock, groupFirstBlock(g));
      int32_t last = std::min(toBlock, groupLastBlock(g));
      for (int32_t block = first; block <= last; block++) {
        bound = std::max(bound, chunk.impacts[block % GROUP]);
      }
    }
    return bound;
  }

  // First doc at or after `doc` that lies in a block whose bound reaches
  // minScore: hops non-competitive GROUPS on their frontier bounds without
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
