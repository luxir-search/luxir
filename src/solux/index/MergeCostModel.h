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
  static constexpr int32_t POINTS_STREAMS = 1;
  static constexpr int32_t GEO_POINTS_STREAMS = 1;

  static constexpr int64_t POINTS_PER_LEAF = 512;
  static constexpr int64_t POINTS_WRITER_BYTES = 64 * 1024;
  static constexpr int64_t POINTS_RUN_BYTES = 1024;
  static constexpr int64_t POINTS_DECODE_BYTES = POINTS_PER_LEAF * (8 + 4 + 4);
  static constexpr int64_t SYNTHESIZED_POINT_BYTES = 16;
  // vector growth can retain nearly 2x the 16-byte triple payload.
  static constexpr int64_t GEO_POINT_BYTES = 32;
  // Three 8-byte directory arrays can each retain nearly 2x vector capacity.
  static constexpr int64_t POINTS_DIRECTORY_BYTES_PER_LEAF = 48;

  static int64_t storedFieldsBytes(int64_t maxChunkBytes) {
    return LIGHT_BYTES + maxChunkBytes * STORED_CHUNK_BUFFER_MULTIPLIER;
  }

  static int64_t pointsBytes(int64_t sourceRuns, int64_t totalValues,
                             int64_t synthesizedValues) {
    int64_t leaves = totalValues / POINTS_PER_LEAF
        + (totalValues % POINTS_PER_LEAF != 0);
    return POINTS_WRITER_BYTES
        + sourceRuns * (POINTS_RUN_BYTES + POINTS_DECODE_BYTES)
        + leaves * POINTS_DIRECTORY_BYTES_PER_LEAF
        + synthesizedValues * SYNTHESIZED_POINT_BYTES;
  }

  static int64_t geoPointsBytes(int64_t totalValues) {
    int64_t leaves = totalValues / POINTS_PER_LEAF
        + (totalValues % POINTS_PER_LEAF != 0);
    // The merged-column walk buffers every output triple. BKDWriter partitions
    // that array in place and retains the same directory shape as 1-D points.
    return POINTS_WRITER_BYTES
        + totalValues * GEO_POINT_BYTES
        + leaves * POINTS_DIRECTORY_BYTES_PER_LEAF;
  }
};

} // namespace solux
