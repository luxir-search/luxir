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
  // DocsEnum::advance() invocations (leapfrog / impact-skip driver calls).
  static inline int64_t advanceCalls = 0;

  static void reset() {
    docBlocksDecoded = 0;
    l0HeaderSteps = 0;
    l1GroupSteps = 0;
    advanceCalls = 0;
  }
};

inline void skipCount(int64_t& counter) {
  if (SkipStats::enabled) {
    counter++;
  }
}

} // namespace solux
