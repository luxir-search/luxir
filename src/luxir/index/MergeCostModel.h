// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

#include "luxir/index/BKDWriter.h"
#include "luxir/reader/PostingsReader.h"

namespace luxir {

// Admission prices for one field-merge batch: a conservative peak-RAM estimate
// (computable up front from per-segment DecodedFieldRecord stats - merge memory is
// predictable in a way query memory is not) and the peak number of concurrently
// held output streams.  The stream budget caps the sum of in-flight stream
// counts per merge; since concurrently held streams cannot share a file, this
// bounds the merged segment's file count and is the effective parallelism dial.
struct MergeCostModel {
  static constexpr int64_t LIGHT_BYTES = 1024 * 1024;
  static constexpr int32_t STREAM_BUDGET_FLOOR = 32;
  static constexpr uint64_t TARGET_FILE_BYTES = 256ULL * 1024 * 1024;
  static constexpr int32_t STREAM_BUDGET_CAP = 128;
  // A range holds one TermsEnum (about 1 KiB with its term buffer) per source;
  // postings decode state lives on the stack.
  static constexpr int64_t TERM_RANGE_SOURCE_BYTES = 4 * 1024;
  // Range term blocks buffer in RAM until stitch and are charged as they grow,
  // in steps of this size.  Debug builds resync often so the suite exercises
  // in-range growth.
#ifdef NDEBUG
  static constexpr int64_t TERM_RANGE_RESYNC_BYTES = 1024 * 1024;
#else
  static constexpr int64_t TERM_RANGE_RESYNC_BYTES = 4 * 1024;
#endif
  // Split planning materializes one estimate (first term plus byte counts) per
  // sampled source term block; the term's heap bytes are charged separately.
  static constexpr int64_t TERM_PLAN_BLOCK_BYTES = 96;
  // Cuts only balance range work, so planning samples every k-th block of each
  // source, keeping a field's plan to about this many estimates.
  static constexpr int64_t MAX_TERM_PLAN_SAMPLES = 256 * 1024;
  static constexpr int64_t STORED_CHUNK_BUFFER_MULTIPLIER = 5;
  static constexpr int64_t MIN_TERM_RANGE_BYTES = 64LL * 1024 * 1024;
  static constexpr int64_t MIN_TERM_PARTITION_BYTES = 2 * MIN_TERM_RANGE_BYTES;
  static constexpr int32_t DERIVED_TERM_RANGES = 0;
  static constexpr int64_t MAX_MERGED_ORD_TERMS =
      (int64_t)std::numeric_limits<int32_t>::max() - (1 << 20);

  static bool ordTermsFit(int64_t currentTerms, int64_t sourceTerms) {
    return currentTerms >= 0 && sourceTerms >= 0 &&
           sourceTerms <= MAX_MERGED_ORD_TERMS - currentTerms;
  }

  // File setup/metadata cost is fixed per file, so a larger merged output can
  // tolerate proportionally more files.  At scale this keeps files per shard
  // near indexBytes / TARGET_FILE_BYTES regardless of segment layout; the floor
  // preserves the existing admission schedule for small merges.
  static constexpr int32_t streamBudgetForBytes(uint64_t estimatedOutputBytes) {
    uint64_t scaled = estimatedOutputBytes / TARGET_FILE_BYTES;
    return (int32_t) std::clamp<uint64_t>(
        scaled, (uint64_t) STREAM_BUDGET_FLOOR,
        (uint64_t) STREAM_BUDGET_CAP);
  }

  // Allow more ranges than can run at once. Queuing largest first evens out
  // the skew left by cuts on block-leading terms. The stream budget controls
  // simultaneous checkouts, while the writer assigns spill files as needed
  // and can reuse them when a checkout finishes.
  static constexpr int32_t RANGE_QUEUE_FACTOR = 2;
  static constexpr int32_t MAX_TERM_PARTITIONS = 64;  // Scheduler limit for one field.

  static constexpr int32_t maxTermRangesForStreams(int32_t streamBudget) {
    return std::min(MAX_TERM_PARTITIONS,
                    RANGE_QUEUE_FACTOR * streamBudget / TERM_RANGE_STREAMS);
  }

  // Doc ids must stay below PostingsReader::MAX_SEGMENT_DOCS (see the
  // arithmetic-headroom contract there). Merges are the only path to large
  // segments, so admission gates on the summed live docs of the sources.
  static bool docsFit(int64_t currentDocs, int64_t sourceDocs) {
    return currentDocs >= 0 && sourceDocs >= 0 &&
           sourceDocs <= (int64_t) PostingsReader::MAX_SEGMENT_DOCS
                             - currentDocs;
  }

  // IVF overlays store list ids (a field's segment-local value ranks) as
  // uint32, so one vector field may hold at most this many values per segment.
  // Multi-valued fields are not bounded by the doc cap.  A flushed segment
  // cannot reach it: every value appends at least one byte (its size) to the
  // inverter pool, which addresses at most 4 GiB, and the per-inverter RAM cap
  // flushes far sooner.  Merge admission is the only gate.
  static constexpr int64_t MAX_SEGMENT_VECTORS = std::numeric_limits<uint32_t>::max();

  static bool vectorsFit(int64_t currentVectors, int64_t sourceVectors) {
    return currentVectors >= 0 && sourceVectors >= 0 &&
           sourceVectors <= MAX_SEGMENT_VECTORS - currentVectors;
  }

  // Peak checked-out OutputStreams for each writer path.
  static constexpr int32_t TEXT_STREAMS = 3;      // TextWriter owns term/doc/pos streams together.
  static constexpr int32_t TERM_RANGE_STREAMS = 2;  // Range-local docs and positions.
  static constexpr int32_t TERM_COORDINATOR_STREAMS = 1;  // Shared dictionary, then norms.
  static constexpr int32_t ORD_COL_STREAMS = 3;   // TextWriter phase dominates OrdColWriter's peak of 2.
  static constexpr int32_t INT_COL_STREAMS = 3;   // values + optional endValueRank + optional docs.
  static constexpr int32_t STR_COL_STREAMS = 5;   // values + endValueRank + owners + endOffset + docs.
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
  // The same for a range field's 24-byte (min, max, docid) point.
  static constexpr int64_t RANGE_POINT_BYTES = 48;
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

  static int64_t termPlanBytes(int64_t sourceBlocks) {
    return sourceBlocks * TERM_PLAN_BLOCK_BYTES;
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

  // Index sort reorder (SegmentMerger::indexSortOrder). The plan holds a
  // 16-byte keyed entry and an 8-byte newToOld slot per merged doc; newToOld
  // stays for the whole merge. The absolute int32 remap per source doc is
  // charged with the row maps.
  static constexpr int64_t SORT_KEY_ENTRY_BYTES = 16;
  static int64_t indexSortPlanBytes(int64_t mergedDocs) {
    return mergedDocs * (SORT_KEY_ENTRY_BYTES + 8);
  }
  // Per-field reorder scratch indexed by merged doc: a source value rank
  // for columns, a norm for text.
  static constexpr int64_t REORDER_RANK_BYTES = 4;
  static constexpr int64_t REORDER_NORM_BYTES = 2;
  // One term's postings wait in RAM to be re-sorted: doc, tf, position
  // start, sort key and its radix buffer per doc, reserved from the term's
  // document frequency, which docsWithField bounds. Positions go in windows
  // of at most REORDER_POSITIONS_WINDOW_BYTES (a single document's positions
  // can exceed it), fewer when the field's total term frequency is smaller.
  static constexpr int64_t REORDER_POSTING_BYTES = 4 + 4 + 8 + 8 + 8;
  static constexpr int64_t REORDER_POSITIONS_WINDOW_BYTES = 16LL << 20;
  static int64_t reorderTermBytes(int64_t docsWithField, int64_t totalTermFreq, bool positions,
                                  int64_t windowBytes = REORDER_POSITIONS_WINDOW_BYTES) {
    int64_t bytes = docsWithField * REORDER_POSTING_BYTES;
    if (positions) bytes += std::min(totalTermFreq * 4, windowBytes);
    return bytes;
  }
  // Reordered stored fields: sources whose docs land out of order decode
  // their docs once into scratch (ScratchBytes), held in RAM up to
  // REORDER_STORED_RAM_BYTES and then spilled to a temp file behind a write
  // buffer (the RAM is released first, so the larger of the two is held),
  // found through an 8-byte offset per merged doc. A directory without a
  // scratch path keeps every decoded byte in RAM; the merge charges that
  // growth as it happens.
  static constexpr int64_t REORDER_STORED_RAM_BYTES = 32LL << 20;
  static constexpr int64_t REORDER_STORED_WRITE_BUFFER_BYTES = 1LL << 20;
  static int64_t reorderStoredBytes(int64_t mergedDocs, int64_t ramBytes = REORDER_STORED_RAM_BYTES) {
    return std::max(ramBytes, REORDER_STORED_WRITE_BUFFER_BYTES) + mergedDocs * 8;
  }

  static int64_t geoPointsBytes(int64_t totalValues) {
    return totalValues * GEO_POINT_BYTES + (int64_t)BKDWriter::workspaceBytes(totalValues);
  }

  // A range column holds two values per point.
  static int64_t rangePointsBytes(int64_t totalValues) {
    int64_t points = totalValues / 2;
    return points * RANGE_POINT_BYTES
        + (int64_t)BasicBKDWriter<BKDInt64Traits>::workspaceBytes(points);
  }
};

} // namespace luxir
