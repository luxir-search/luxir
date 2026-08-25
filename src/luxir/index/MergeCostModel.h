#pragma once

#include <cstdint>
#include <limits>

#include "luxir/reader/PostingsReader.h"

namespace luxir {

// Admission prices for one field-merge batch: a conservative peak-RAM estimate
// (computable up front from per-segment SegFieldInfo stats - merge memory is
// predictable in a way query memory is not) and the peak number of concurrently
// held output streams.  MAX_STREAMS caps the sum of in-flight stream counts per
// merge; since concurrently held streams cannot share a file, this bounds the
// merged segment's file count and is the effective parallelism dial.
struct MergeCostModel {
  static constexpr int64_t LIGHT_BYTES = 1024 * 1024;
  // Provisional file-parallelism cap.  Eight partitioned text ranges consume
  // sixteen streams before counting their shared terms stream or other fields.
  // This should become size-scaled with stream-buffer memory rather than remain
  // a topology-only constant.
  static constexpr int32_t MAX_STREAMS = 32;
  static constexpr int64_t TERM_RANGE_SOURCE_BYTES = 64 * 1024;
  static constexpr int64_t STORED_CHUNK_BUFFER_MULTIPLIER = 5;
  static constexpr int64_t MIN_TERM_RANGE_BYTES = 64LL * 1024 * 1024;
  static constexpr int64_t MIN_TERM_PARTITION_BYTES = 2 * MIN_TERM_RANGE_BYTES;
  static constexpr int32_t MAX_TERM_RANGES = 8;
  static constexpr int64_t MAX_MERGED_ORD_TERMS =
      (int64_t)std::numeric_limits<int32_t>::max() - (1 << 20);

  static bool ordTermsFit(int64_t currentTerms, int64_t sourceTerms) {
    return currentTerms >= 0 && sourceTerms >= 0 &&
           sourceTerms <= MAX_MERGED_ORD_TERMS - currentTerms;
  }

  // Doc ids must stay below PostingsReader::MAX_SEGMENT_DOCS (see the
  // arithmetic-headroom contract there). Merges are the only path to large
  // segments, so admission gates on the summed live docs of the sources.
  static bool docsFit(int64_t currentDocs, int64_t sourceDocs) {
    return currentDocs >= 0 && sourceDocs >= 0 &&
           sourceDocs <= (int64_t) PostingsReader::MAX_SEGMENT_DOCS
                             - currentDocs;
  }

  // Peak checked-out OutputStreams for each writer path.
  static constexpr int32_t TEXT_STREAMS = 3;      // TextWriter owns term/doc/pos streams together.
  static constexpr int32_t TERM_RANGE_STREAMS = 2;  // Range-local docs and positions.
  static constexpr int32_t TERM_COORDINATOR_STREAMS = 1;  // Shared terms stream.
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

  // TextWriter block buffers dominate the fixed part.  Each source also owns
  // a TermsEnum and its decode state for the duration of the range merge.
  static int64_t termRangeWorkingBytes(int64_t sources) {
    return LIGHT_BYTES + sources * TERM_RANGE_SOURCE_BYTES;
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

} // namespace luxir
