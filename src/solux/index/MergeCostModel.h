#pragma once

#include <cstdint>

namespace solux {

// Admission prices for one field-merge batch: a conservative peak-RAM estimate
// (computable up front from per-segment SegFieldInfo stats - merge memory is
// predictable in a way query memory is not) and the peak number of concurrently
// held output streams.  MAX_STREAMS caps the sum of in-flight stream counts per
// merge; since concurrently held streams cannot share a file, this bounds the
// merged segment's file count and is the effective parallelism dial.
struct MergeCostModel {
  static constexpr int64_t LIGHT_BYTES = 1024 * 1024;
  static constexpr int32_t MAX_STREAMS = 16;
  static constexpr int64_t STORED_CHUNK_BUFFER_MULTIPLIER = 5;

  // Peak checked-out OutputStreams for each writer path.
  static constexpr int32_t TEXT_STREAMS = 3;      // TextWriter owns term/doc/pos streams together.
  static constexpr int32_t ORD_COL_STREAMS = 3;   // TextWriter phase dominates OrdColWriter's peak of 2.
  static constexpr int32_t INT_COL_STREAMS = 3;   // values + optional endValueRank + optional docsWithVal.
  static constexpr int32_t STR_COL_STREAMS = 5;   // values + endValueRank + valDoc + endOffset + docsWithVal.
  static constexpr int32_t STORED_STREAMS = 1;    // chunk, mono, and mono2 streams are opened sequentially.

  static int64_t storedFieldsBytes(int64_t maxChunkBytes) {
    return LIGHT_BYTES + maxChunkBytes * STORED_CHUNK_BUFFER_MULTIPLIER;
  }
};

} // namespace solux
