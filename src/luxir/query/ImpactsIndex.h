#pragma once

#include <algorithm>
#include <limits>
#include <span>
#include <vector>

#include "luxir/reader/DocsEnum.h"
#include "luxir/search/Similarity.h"
#include "luxir/util/MemPool.h"

namespace luxir {

// Per-doc-block score upper bounds for one term's postings, built from the
// impact data stored in the docs-stream block headers.  Scorers consult it to
// skip doc blocks that cannot beat the collector's min-competitive score.
//
// Production build is term-frontier first: construction scores only the
// dictionary's whole-term frontier for the global max. L1 group headers are
// parsed into a prefix on first touch, and a group's 32 per-block frontiers are
// BM25-scored only the first time a query resolves a block inside it.
// Never walk the whole postings up front - a "the"-sized term has ~35K block
// headers and queries touch a handful. The forced eager path is kept as the
// test oracle and for fields without dictionary term frontiers.
//
// The simScorer defines what "score" means. A term scorer passes its own BM25
// scorer. Exact phrases also use scored impacts because exact phrase frequency
// is bounded by every slot's term frequency. Sloppy phrases instead consume
// the raw tf/norm range metadata and score one combined envelope; their phrase
// frequency can exceed an individual term frequency.
class ImpactsIndex {
public:
  struct CompetitiveTarget {
    int32_t doc;
    int32_t lastDoc;
    float impact;
    // Landing group, for monotone callers to feed back as the next call's
    // fromGroup hint (numGroups() on the terminal results).
    int32_t group;
  };

  struct RawRange {
    int32_t maxTf = 0;
    int32_t minNorm = std::numeric_limits<int32_t>::max();
  };

private:
  struct Chunk {
    int32_t blockCount = 0;
    int32_t* lastDocs = nullptr;
    float* impacts = nullptr;
    float* suffixWithin = nullptr;  // max of impacts[i..] within the chunk
    int32_t* maxTfs = nullptr;
    int32_t* minNorms = nullptr;
  };

  MemPool* pool = nullptr;
  const DocsEnumMeta* docsEnum = nullptr;
  Similarity::BM25Scorer* simScorer = nullptr;
  float boost = 1.0f;
  bool useFrontierBound = true;
  bool lazyGroupHeaders = false;
  float globalMax = 0.0f;

  int32_t count = 0;       // total blocks
  int32_t groupCount = 0;
  mutable int32_t groupHeadersParsed = 0;
  int32_t* groupLastDocs = nullptr;
  int32_t* groupBaseLastDocs = nullptr;
  int64_t* groupBodyOffs = nullptr;
  float* groupUpper = nullptr;       // frontier bound per group
  float* groupSuffixUpper = nullptr; // eager path: max of groupUpper[g..]
  int32_t* groupMaxTfs = nullptr;
  int32_t* groupMinNorms = nullptr;
  mutable Chunk** chunks = nullptr;  // lazily parsed per group
  mutable DocsEnumMeta::GroupImpactCursor groupCursor;

  static constexpr int32_t GROUP = DocsEnumMeta::L1_PERIOD;

  int32_t blockCountForGroup(int32_t g) const {
    return std::min(GROUP, count - g * GROUP);
  }

  float scoreImpact(int32_t tf, int32_t norm) const {
    return boost * simScorer->score((float) tf, (int64_t) norm);
  }

  float scoreFrontier(const DocsEnumMeta::GroupImpactHeader& header) const {
    if (useFrontierBound && !header.frontierNorms.empty()) {
      return simScorer->scoreFrontier(header.frontierNorms, header.frontierTfBytes,
                                      header.frontierTfWidth, boost);
    }
    return scoreImpact(header.spanMaxTf, header.spanMinNorm);
  }

  void ensureGroupHeadersThrough(int32_t g) const {
    if (!lazyGroupHeaders || groupCount == 0) {
      return;
    }
    if (g < 0) {
      return;
    }
    if (g >= groupCount) {
      g = groupCount - 1;
    }
    if (g < groupHeadersParsed) {
      return;
    }
    docsEnum->readGroupImpactHeadersThrough(
        groupCursor, g, true,
        [&](const DocsEnumMeta::GroupImpactHeader& header) {
          int32_t idx = groupHeadersParsed;
          assert(header.group == idx);
          groupLastDocs[idx] = header.lastDoc;
          groupBaseLastDocs[idx] = header.baseLastDoc;
          groupBodyOffs[idx] = header.bodyOffset;
          groupUpper[idx] = scoreFrontier(header);
          groupMaxTfs[idx] = header.spanMaxTf;
          groupMinNorms[idx] = header.spanMinNorm;
          chunks[idx] = nullptr;
          groupHeadersParsed++;
        });
    assert(g < groupHeadersParsed);
  }

  void ensureGroupHeadersCoverDoc(int32_t target) const {
    if (!lazyGroupHeaders || groupCount == 0) {
      return;
    }
    while (groupHeadersParsed < groupCount) {
      if (groupHeadersParsed > 0 && target <= groupLastDocs[groupHeadersParsed - 1]) {
        return;
      }
      ensureGroupHeadersThrough(groupHeadersParsed);
    }
  }

  // Forward gallop over groupLastDocs[fromGroup, limit) for a monotone
  // caller; fromGroup must already be a valid lower bracket for target.
  int32_t gallopGroupContaining(int32_t fromGroup, int32_t target,
                                int32_t limit) const {
    int32_t lo = fromGroup;
    int32_t step = 1;
    while (lo < limit && groupLastDocs[lo] < target) {
      lo += step;
      step <<= 1;
    }
    if (lo >= limit) {
      lo = limit;
    }
    int32_t bracketLo = std::max(fromGroup, lo - (step >> 1));
    const int32_t* begin = groupLastDocs + bracketLo;
    const int32_t* end = groupLastDocs + std::min(lo + 1, limit);
    const int32_t* it = std::lower_bound(begin, end, target);
    return (int32_t) (it - groupLastDocs);
  }

  float maxParsedGroupUpper(int32_t fromGroup, int32_t toGroup) const {
    assert(fromGroup >= 0 && fromGroup <= toGroup);
    assert(toGroup < groupHeadersParsed);
    float bound = 0.0f;
    for (int32_t g = fromGroup; g <= toGroup; g++) {
      bound = std::max(bound, groupUpper[g]);
    }
    return bound;
  }

  const Chunk& ensureGroup(int32_t g) const {
    assert(g >= 0 && g < groupCount);
    ensureGroupHeadersThrough(g);
    if (chunks[g] != nullptr) {
      return *chunks[g];
    }
    skipCount(SkipStats::impactL0GroupParses);
    auto* chunk = pool->make<Chunk>();
    chunk->blockCount = blockCountForGroup(g);
    chunk->lastDocs = pool->make_arr<int32_t>((size_t) chunk->blockCount);
    chunk->impacts = pool->make_arr<float>((size_t) chunk->blockCount);
    chunk->suffixWithin = pool->make_arr<float>((size_t) chunk->blockCount);
    chunk->maxTfs = pool->make_arr<int32_t>((size_t) chunk->blockCount);
    chunk->minNorms = pool->make_arr<int32_t>((size_t) chunk->blockCount);

    DocsEnumMeta::GroupBlockImpactScratch scratch;
    docsEnum->visitGroupBlockImpacts(
        g, groupBodyOffs[g], groupBaseLastDocs[g], scratch,
        [&](int32_t block, int32_t lastDoc, int32_t maxTf, int32_t minNorm,
            std::span<const int32_t> tfs, std::span<const int32_t> norms,
            bool frontierSpilled) {
          assert(block >= 0 && block < chunk->blockCount);
          chunk->lastDocs[block] = lastDoc;
          chunk->maxTfs[block] = maxTf;
          chunk->minNorms[block] = minNorm;
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
  static inline bool forceEagerForTests = false;

  void build(MemPool& pool_, DocsEnumMeta& docsEnum_, Similarity::BM25Scorer& simScorer_,
             float boost_, bool useFrontierBound_ = true) {
    pool = &pool_;
    docsEnum = &docsEnum_;
    simScorer = &simScorer_;
    boost = boost_;
    useFrontierBound = useFrontierBound_;
    lazyGroupHeaders = false;
    globalMax = 0.0f;
    count = docsEnum_.numImpactBlocks();
    groupCount = docsEnum_.numImpactGroups();
    groupHeadersParsed = 0;
    groupCursor = {};

    if (useFrontierBound && docsEnum_.hasTermImpacts() && !forceEagerForTests) {
      docsEnum_.visitTermImpactFrontier([&](int32_t tf, int32_t norm) {
        globalMax = std::max(globalMax, scoreImpact(tf, norm));
      });
      if (groupCount == 0) {
        return;
      }
      groupLastDocs = pool->make_arr<int32_t>((size_t) groupCount);
      groupBaseLastDocs = pool->make_arr<int32_t>((size_t) groupCount);
      groupBodyOffs = pool->make_arr<int64_t>((size_t) groupCount);
      groupUpper = pool->make_arr<float>((size_t) groupCount);
      groupSuffixUpper = nullptr;
      groupMaxTfs = pool->make_arr<int32_t>((size_t) groupCount);
      groupMinNorms = pool->make_arr<int32_t>((size_t) groupCount);
      chunks = pool->make_arr<Chunk*>((size_t) groupCount);
      for (int32_t g = 0; g < groupCount; g++) {
        chunks[g] = nullptr;
      }
      lazyGroupHeaders = true;
      return;
    }

    static thread_local DocsEnumMeta::GroupImpacts groups;  // reused; reset by readGroupImpacts
    docsEnum_.readGroupImpacts(groups);
    if (groups.lastDocs.empty()) {
      return;
    }

    groupCount = (int32_t) groups.lastDocs.size();
    groupLastDocs = pool->make_arr<int32_t>((size_t) groupCount);
    groupBaseLastDocs = pool->make_arr<int32_t>((size_t) groupCount);
    groupBodyOffs = pool->make_arr<int64_t>((size_t) groupCount);
    groupUpper = pool->make_arr<float>((size_t) groupCount);
    groupSuffixUpper = pool->make_arr<float>((size_t) groupCount);
    groupMaxTfs = pool->make_arr<int32_t>((size_t) groupCount);
    groupMinNorms = pool->make_arr<int32_t>((size_t) groupCount);
    chunks = pool->make_arr<Chunk*>((size_t) groupCount);
    for (int32_t g = 0; g < groupCount; g++) {
      groupLastDocs[g] = groups.lastDocs[(size_t) g];
      groupBaseLastDocs[g] = groups.baseLastDocs[(size_t) g];
      groupBodyOffs[g] = groups.bodyOffsets[(size_t) g];
      groupMaxTfs[g] = groups.spanMaxTfs[(size_t) g];
      groupMinNorms[g] = groups.spanMinNorms[(size_t) g];
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
    globalMax = groupSuffixUpper[0];
    groupHeadersParsed = groupCount;
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

  float globalMaxImpact() const {
    return globalMax;
  }

  int32_t groupContainingFrom(int32_t fromGroup, int32_t target) const {
    if (groupCount == 0) {
      return 0;
    }
    int32_t limit = groupCount;
    if (lazyGroupHeaders) {
      ensureGroupHeadersCoverDoc(target);
      limit = groupHeadersParsed;
    }
    if (fromGroup < 0 || fromGroup >= limit) {
      if (fromGroup >= limit && fromGroup >= 0 && !lazyGroupHeaders) {
        return groupCount;
      }
      const int32_t* it = std::lower_bound(groupLastDocs, groupLastDocs + limit, target);
      return (int32_t) (it - groupLastDocs);
    }
    if (fromGroup > 0 && target <= groupLastDocs[fromGroup - 1]) {
      const int32_t* it = std::lower_bound(groupLastDocs, groupLastDocs + limit, target);
      return (int32_t) (it - groupLastDocs);
    }
    return gallopGroupContaining(fromGroup, target, limit);
  }

  int32_t groupLastDoc(int32_t group) const {
    assert(group >= 0 && group < groupCount);
    ensureGroupHeadersThrough(group);
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
    ensureGroupHeadersThrough(group);
    if (lazyGroupHeaders) {
      float bound = maxParsedGroupUpper(group, groupHeadersParsed - 1);
      if (groupHeadersParsed < groupCount) {
        bound = std::max(bound, globalMax);
      }
      return bound;
    }
    return groupSuffixUpper[group];
  }

  float maxGroupImpactInRange(int32_t fromGroup, int32_t toGroup) const {
    assert(fromGroup >= 0 && fromGroup <= toGroup && toGroup < groupCount);
    skipCount(SkipStats::impactGroupBoundCalls);
    skipCount(SkipStats::impactGroupBoundNoL0);
    ensureGroupHeadersThrough(toGroup);
    if (lazyGroupHeaders) {
      return maxParsedGroupUpper(fromGroup, toGroup);
    }
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
      if (lazyGroupHeaders) {
        if (groupHeadersParsed > g + 1) {
          bound = std::max(bound, maxParsedGroupUpper(g + 1, groupHeadersParsed - 1));
        }
        if (groupHeadersParsed < groupCount) {
          bound = std::max(bound, globalMax);
        }
      } else {
        bound = std::max(bound, groupSuffixUpper[g + 1]);
      }
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
    ensureGroupHeadersThrough(gTo);
    for (int32_t g = gFrom + 1; g < gTo; g++) {
      bound = std::max(bound, groupUpper[g]);
    }
    const Chunk& tail = ensureGroup(gTo);
    for (int32_t i = 0; i <= toBlock % GROUP; i++) {
      bound = std::max(bound, tail.impacts[i]);
    }
    return bound;
  }

  // Raw score-independent envelope over blocks [fromBlock, toBlock]. Fully
  // covered middle groups use the L1 span header; edge groups use their
  // decoded L0 block corners. Sloppy phrases combine these once at the common
  // phrase scorer, avoiding unsafe sums of already-rounded float impacts.
  RawRange rawRangeInRange(int32_t fromBlock, int32_t toBlock) const {
    assert(fromBlock >= 0 && fromBlock <= toBlock && toBlock < count);
    int32_t gFrom = fromBlock / GROUP;
    int32_t gTo = toBlock / GROUP;
    ensureGroupHeadersThrough(gTo);
    RawRange range;
    auto addBlocks = [&](int32_t group, int32_t first, int32_t last) {
      const Chunk& chunk = ensureGroup(group);
      for (int32_t block = first; block <= last; block++) {
        int32_t i = block % GROUP;
        range.maxTf = std::max(range.maxTf, chunk.maxTfs[i]);
        range.minNorm = std::min(range.minNorm, chunk.minNorms[i]);
      }
    };
    if (gFrom == gTo) {
      addBlocks(gFrom, fromBlock, toBlock);
      return range;
    }
    addBlocks(gFrom, fromBlock, groupLastBlock(gFrom));
    for (int32_t g = gFrom + 1; g < gTo; g++) {
      range.maxTf = std::max(range.maxTf, groupMaxTfs[g]);
      range.minNorm = std::min(range.minNorm, groupMinNorms[g]);
    }
    addBlocks(gTo, groupFirstBlock(gTo), toBlock);
    return range;
  }

  // Max impact over blocks [fromBlock, toBlock] without materializing any
  // additional group. Parsed groups use their block impacts; unparsed groups
  // fall back to their group frontier bound.
  float maxImpactInRangeNoParse(int32_t fromBlock, int32_t toBlock) const {
    assert(fromBlock >= 0 && fromBlock <= toBlock && toBlock < count);
    int32_t gFrom = fromBlock / GROUP;
    int32_t gTo = toBlock / GROUP;
    ensureGroupHeadersThrough(gTo);
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
    ensureGroupHeadersThrough(gTo);
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
  // DocsEnumMeta::END when nothing later can compete. lastDoc and impact certify
  // the competitive landing block; past the impact data they permanently
  // certify the remainder of the posting list. Monotone callers pass their
  // previous landing group as fromGroup to gallop instead of re-searching the
  // group table from the start.
  CompetitiveTarget firstCompetitiveTarget(int32_t doc, float minScore,
                                            int64_t& skippedBlocks,
                                            int32_t fromGroup = -1) const {
    if (globalMax < minScore) {
      skippedBlocks += count;
      return {DocsEnumMeta::END, DocsEnumMeta::END,
              std::numeric_limits<float>::infinity(), groupCount};
    }
    int32_t g = groupContainingFrom(fromGroup, doc);
    if (g >= groupCount) {
      return {doc, DocsEnumMeta::END, std::numeric_limits<float>::infinity(),
              groupCount};
    }
    for (;;) {
      ensureGroupHeadersThrough(g);
      if (groupUpper[g] >= minScore) {
        const Chunk& chunk = ensureGroup(g);
        const int32_t* bit =
            std::lower_bound(chunk.lastDocs, chunk.lastDocs + chunk.blockCount, doc);
        int32_t i = (int32_t) (bit - chunk.lastDocs);
        for (; i < chunk.blockCount; i++) {
          if (chunk.impacts[i] >= minScore) {
            int32_t blockStart = i == 0 ? (g == 0 ? 0 : groupLastDocs[g - 1] + 1)
                                        : chunk.lastDocs[i - 1] + 1;
            return {std::max(doc, blockStart), chunk.lastDocs[i],
                    chunk.impacts[i], g};
          }
          skippedBlocks++;
        }
      } else {
        skippedBlocks += blockCountForGroup(g);
      }
      if (g + 1 >= groupCount) {
        return {DocsEnumMeta::END, DocsEnumMeta::END,
                std::numeric_limits<float>::infinity(), groupCount};
      }
      doc = groupLastDocs[g] + 1;
      g++;
    }
  }

  // Index of the block containing target (the first block whose lastDoc >=
  // target); blockCount() when target is past the last block, or when the
  // index is empty.
  int32_t blockContaining(int32_t target) const {
    int32_t g = groupContainingFrom(-1, target);
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
    int32_t hitGroup = groupContainingFrom(from / GROUP, target);
    if (hitGroup >= groupCount) {
      return count;
    }
    const Chunk& chunk = ensureGroup(hitGroup);
    const int32_t* bit =
        std::lower_bound(chunk.lastDocs, chunk.lastDocs + chunk.blockCount, target);
    return hitGroup * GROUP + (int32_t) (bit - chunk.lastDocs);
  }
};

} // namespace luxir
