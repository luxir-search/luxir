#pragma once

#include <cstdint>

namespace solux {

// Skip-effectiveness counters for the internal validation harness (the go/no-go
// for the skip-vs-skip top-k benchmark, and the level2 decision). Deliberately
// non-atomic and single-thread: drive the harness on one thread (force-merged
// single segment is also what the measurement wants) so per-query block
// accounting is deterministic. Concurrent decoders would race these, producing
// approximate totals -- do not read them under intra-query parallelism.
//
// Zero cost in production: every bump is behind the `enabled` gate (default
// false), a branch predicted-not-taken, and the counted events are per-block /
// per-skip, not per-doc.
struct SkipStats {
  static inline bool enabled = false;

  // Docs-block body decodes (full 128-doc blocks + the StreamVByte tail). This is
  // the real decode work; blocks_decoded(pruned) / blocks_decoded(exhaustive) is
  // the "% of blocks decoded vs total" headline.
  static inline int64_t docBlocksDecoded = 0;
  // Per-block L0 headers walked in skipToBlock (within an L1 group). Bounded by
  // ~L1_PERIOD per advance; the within-group skip cost.
  static inline int64_t l0HeaderSteps = 0;
  // L1 group headers walked in skipToBlock. The level2 decision metric: if this
  // dominates on long-list queries, a coarser third level pays off.
  static inline int64_t l1GroupSteps = 0;
  // DocsEnum::advance() and advance-or-begin probe invocations (leapfrog /
  // impact-skip driver calls).
  static inline int64_t advanceCalls = 0;
  // Position-stream accounting: O(1) seeks off the L0 posByteOff anchors
  // (skipToBlock), whole position blocks hopped without unpacking
  // (startPositions), and position block body decodes.
  static inline int64_t posSeeks = 0;
  // Block-max conjunction: bound evaluations vs ranges actually skipped.
  static inline int64_t conjRangeEvals = 0;
  static inline int64_t conjRangeSkips = 0;
  static inline int64_t posBlocksSkipped = 0;
  static inline int64_t posBlocksDecoded = 0;
  // Phrase per-candidate outcomes: docs rejected by the pre-position score
  // bound vs docs that paid a position verification.
  static inline int64_t phraseBoundRejects = 0;
  static inline int64_t phraseVerifies = 0;
  // Shallow-advance outcomes: targets answered from the cached shallow block
  // vs calls that had to move the block cursor (search + possible group parse).
  static inline int64_t shallowCacheHits = 0;
  static inline int64_t shallowCursorMoves = 0;
  static inline int64_t impactGroupBoundCalls = 0;
  static inline int64_t impactGroupBoundNoL0 = 0;
  static inline int64_t impactGroupShallowAnswers = 0;
  static inline int64_t impactRefinesTriggered = 0;
  static inline int64_t impactL0GroupParses = 0;
  static inline int64_t impactL0GroupParseScratchSpills = 0;
  static inline int64_t maxScoreOuterWindows = 0;
  static inline int64_t maxScoreInnerWindows = 0;
  static inline int64_t maxScoreOuterWindowRefines = 0;
  static inline int64_t maxScoreSetupFallbackBlockBounds = 0;
  static inline int64_t maxScoreSweepWindows = 0;
  static inline int64_t maxScoreRequiredSweeps = 0;
  static inline int64_t maxScoreBufferCompactions = 0;
  static inline int64_t maxScoreDirectFills = 0;
  static inline int64_t mandOptWindowSkips = 0;
  static inline int64_t mandOptConjunctionWindows = 0;
  static inline int64_t mandOptWindowEvals = 0;
  static inline int64_t mandOptBulkWindows = 0;
  static inline int64_t mandOptBulkWindowSkips = 0;
  static inline int64_t mandOptBulkSweeps = 0;
  static inline int64_t mandOptBulkCompactions = 0;
  static inline int64_t mandOptBulkOptDrivenWindows = 0;
  static inline int64_t countBulkFillCalls = 0;
  static inline int64_t countBulkFillBlocks = 0;
  static inline int64_t countBulkFillDocs = 0;
  static inline int64_t countBulkFillContiguousBlocks = 0;
  // Whole word-encoded (bitset/contiguous) doc blocks OR'd into count windows
  // straight from the stream, without decoding to the doc buffer.
  static inline int64_t countBulkFillWordBlocks = 0;
  static inline int64_t conjDenseCountWindows = 0;
  static inline int64_t conjCountFallbacks = 0;
  static inline int64_t bulkDomainWindowsFed = 0;
  static inline int64_t docsOnlyFreqBlocksSkipped = 0;
  static inline int64_t docsOnlyWordProbeAdvances = 0;
  static inline int64_t scoredWordProbeAdvances = 0;

  static void reset() {
    docBlocksDecoded = 0;
    l0HeaderSteps = 0;
    l1GroupSteps = 0;
    advanceCalls = 0;
    posSeeks = 0;
    conjRangeEvals = 0;
    conjRangeSkips = 0;
    posBlocksSkipped = 0;
    posBlocksDecoded = 0;
    phraseBoundRejects = 0;
    phraseVerifies = 0;
    shallowCacheHits = 0;
    shallowCursorMoves = 0;
    impactGroupBoundCalls = 0;
    impactGroupBoundNoL0 = 0;
    impactGroupShallowAnswers = 0;
    impactRefinesTriggered = 0;
    impactL0GroupParses = 0;
    impactL0GroupParseScratchSpills = 0;
    maxScoreOuterWindows = 0;
    maxScoreInnerWindows = 0;
    maxScoreOuterWindowRefines = 0;
    maxScoreSetupFallbackBlockBounds = 0;
    maxScoreSweepWindows = 0;
    maxScoreRequiredSweeps = 0;
    maxScoreBufferCompactions = 0;
    maxScoreDirectFills = 0;
    mandOptWindowSkips = 0;
    mandOptConjunctionWindows = 0;
    mandOptWindowEvals = 0;
    mandOptBulkWindows = 0;
    mandOptBulkWindowSkips = 0;
    mandOptBulkSweeps = 0;
    mandOptBulkCompactions = 0;
    mandOptBulkOptDrivenWindows = 0;
    countBulkFillCalls = 0;
    countBulkFillBlocks = 0;
    countBulkFillDocs = 0;
    countBulkFillContiguousBlocks = 0;
    countBulkFillWordBlocks = 0;
    conjDenseCountWindows = 0;
    conjCountFallbacks = 0;
    bulkDomainWindowsFed = 0;
    docsOnlyFreqBlocksSkipped = 0;
    docsOnlyWordProbeAdvances = 0;
    scoredWordProbeAdvances = 0;
  }
};

inline void skipCount(int64_t& counter) {
  if (SkipStats::enabled) {
    counter++;
  }
}

} // namespace solux
