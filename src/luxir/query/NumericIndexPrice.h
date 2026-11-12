// Copyright 2020-2026 Yonik Seeley and Luxir contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "luxir/query/PointsMaterialize.h"
#include "luxir/reader/PointsReader.h"

namespace luxir {

// Numeric indexed producers, in point emissions. Geo retains the count-based
// PointsMaterialize price. Fits use the pinned 1M-document forced-arm board
// (2026-11-12), at about 0.65 ns/point; verification remains 20 points.
struct NumericIndexPrice {
  // Boolean d10/d50 l1 residuals put destination work at 2.7-3.6 points/word.
  // Their alone delta gives about 12 points/additional leaf. Shuffled date
  // r10/r50 give about 1.1 points/FOR value at 20 bits: decode plus scatter.
  static constexpr double DESTINATION_WORD_POINTS = 3;
  static constexpr double LEAF_POINTS = 12;
  static constexpr double DECODE_BIT_POINTS = 0.01;
  static constexpr double SCATTER_POINTS = 0.9;
  static constexpr double SOURCE_WORD_POINTS = 1;
  static constexpr double RUN_WORD_POINTS = 1;
  static constexpr double SELECT_POINTS = 64;

  struct PostingsWork {
    uint64_t terms = 0;
    uint64_t docs = 0;
    uint64_t packedDocs = 0;
    uint64_t wordBlocks = 0;
    double spanWords = 0;

    void add(uint64_t docFreq, uint64_t packedBlocks, int32_t maxDoc) {
      constexpr uint64_t BLOCK = Postings::DOCS_BLOCK_SIZE;
      assert(docFreq != 0 && packedBlocks <= docFreq / BLOCK);
      terms++;
      docs += docFreq;
      packedDocs += packedBlocks * BLOCK + docFreq % BLOCK;
      uint64_t blocks = docFreq / BLOCK - packedBlocks;
      wordBlocks += blocks;
      // A 128-doc block spans about 128/density docids, or 2/density words.
      // Sum per term: a union's aggregate density would lose mixed encodings.
      if (blocks != 0) spanWords += (double)blocks * (2.0 * maxDoc / docFreq);
    }
  };

  static constexpr double TERM_POINTS = 64;
  static constexpr double PACKED_DOC_POINTS = 2;
  // d50/d90 l1 cover the same span: about 117 us at 0.65 ns/point on
  // 16384 words, including the window term. No per-block surcharge is fitted.
  static constexpr double SPAN_WORD_POINTS = 10;
  static constexpr double WINDOW_WORD_POINTS = 1;

  // Single-valued range docs equal their summed docFreq. All of the
  // field's occurrences beyond one per term may belong to the range.
  static int64_t postingsTermLowerBound(int64_t inside, int64_t sumDocFreq, int64_t terms) {
    assert(sumDocFreq >= terms);
    return std::max<int64_t>(0, inside - (sumDocFreq - terms));
  }

  // At most one distinct numeric term fits each sortable integer encoding
  // outside the inclusive range. This remains useful when every leaf crosses.
  static int64_t postingsDomainLowerBound(int64_t terms, int64_t min, int64_t max,
                                        int64_t lo, int64_t hi) {
    __int128 outside = std::max<__int128>(0, (__int128)lo - min)
        + std::max<__int128>(0, (__int128)max - hi);
    return (int64_t)std::max<__int128>(0, (__int128)terms - outside);
  }

  static double postings(const PostingsWork& work, int32_t maxDoc) {
    // Bounded window setup; pulsed/short lists do not visit every word.
    return TERM_POINTS * work.terms + PACKED_DOC_POINTS * work.packedDocs
        + SPAN_WORD_POINTS * work.spanWords
        + WINDOW_WORD_POINTS * (double)std::min<uint64_t>(FixedBitSet::sizeInWords(maxDoc), work.docs);
  }

  static double postingsUnion(uint64_t docs, int32_t maxDoc) {
    // Traversal already pays for emitting documents/words. Add the union's
    // destination initialization, or its array collection and sorting.
    return PointsMaterialize::useBitset(docs, maxDoc)
        ? DESTINATION_WORD_POINTS * FixedBitSet::sizeInWords(maxDoc)
        : PointsMaterialize::ARRAY_POINTS * (double)docs;
  }

  // Directory arithmetic only: no leaf entries or payloads are touched.
  // Count invocations, including repeated boundary leaves of disjoint ranges.
  static double pointsLowerBound(std::span<const PointsReader::OrdinalRange> ranges,
                                 uint64_t pointCount, uint32_t leafSize, uint64_t emitted,
                                 int32_t maxDoc, bool complement = false, uint64_t baseDocs = 0) {
    if (!complement && !PointsMaterialize::useBitset(emitted, maxDoc)) {
      return PointsMaterialize::price(emitted, maxDoc);
    }
    uint64_t leaves = 0, cursor = 0;
    auto add = [&](uint64_t begin, uint64_t end) {
      if (begin < end) leaves += (end - 1) / leafSize - begin / leafSize + 1;
    };
    for (const auto& range : ranges) {
      if (complement) add(cursor, range.begin);
      else add(range.begin, range.end);
      cursor = range.end;
    }
    if (complement) add(cursor, pointCount);
    return points(PointsReader::EmissionWork{leaves}, emitted, maxDoc, complement, baseDocs);
  }

  static double points(const PointsReader::EmissionWork& work, uint64_t emitted,
                       int32_t maxDoc, bool complement = false, uint64_t baseDocs = 0) {
    // The calibrated array price includes traversal, expansion and sorting.
    if (!complement && !PointsMaterialize::useBitset(emitted, maxDoc)) {
      return PointsMaterialize::price(emitted, maxDoc);
    }
    return (DESTINATION_WORD_POINTS + (complement ? PointsMaterialize::WORD_POINTS : 0))
            * FixedBitSet::sizeInWords(maxDoc)
        + LEAF_POINTS * work.leaves + DECODE_BIT_POINTS * work.decodedBits
        + SCATTER_POINTS * (work.scattered + baseDocs)
        + SOURCE_WORD_POINTS * work.bitsetWords + RUN_WORD_POINTS * work.runWords
        + SELECT_POINTS * work.selects;
  }
};

} // namespace luxir
