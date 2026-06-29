#pragma once

#include <cstdint>
#include <cmath>
#include <array>
#include <cassert>
#include <bit>
#include <span>
#include "solux/reader/Postings.h"
#include "solux/util/solux_util.h"

namespace solux {

//
// Adapted directly from Lucene. See Lucene javadoc for more in-depth info.
//
class SmallFloat {
  static const std::array<float,256> byteToLength;
public:
  /// Field length is stored as a byte, this converts it back to a float
  /// This uses an array lookup as opposed to calculating the value.
  /// For length 40 and below, this is exact.
  static constexpr float decodeLengthByte(uint8_t code) {
    return byteToLength[code];
  }


  /// Float-like encoding for positive longs that preserves ordering and 4 significant bits.
  static constexpr int32_t longToInt4(int64_t i) {
    assert(i >= 0);
    int numBits = 64 - std::countl_zero((uint64_t)i);
    if (numBits < 4) {
      // subnormal value
      return (int32_t)i;
    } else {
      // normal value
      int shift = numBits - 4;
      // only keep the 5 most significant bits
      auto encoded =  (int32_t)(((uint64_t)i) >> shift);
      // clear the most significant bit, which is implicit
      encoded &= 0x07;
      // encode the shift, adding 1 because 0 is reserved for subnormal values
      encoded |= (shift + 1) << 3;
      return encoded;
    }
  }

 static constexpr int64_t int4ToLong(int32_t i) {
   uint8_t bits = i & 0x07;
   int shift = (i >> 3) - 1;
   int64_t decoded;
    if (shift == -1) {
      // subnormal value
      decoded = bits;
    } else {
      // normal value
      decoded = (bits | int64_t(0x08)) << shift;
    }
    return decoded;
  }

  // static constexpr int MAX_INT4 = longToInt4((int64_t)std::numeric_limits<int32_t>::max());
  static constexpr int32_t MAX_INT4 = 231; // help out the compiler
  static constexpr int32_t NUM_FREE_VALUES = 255 - MAX_INT4;  // should be 24


  static constexpr uint8_t intToByte4(int32_t i) {
    assert(i >= 0);
    if (i < NUM_FREE_VALUES) {
      return (uint8_t)i;
    } else {
      return (uint8_t)(NUM_FREE_VALUES + longToInt4(i - NUM_FREE_VALUES));
    }
  }

  static constexpr int32_t byte4ToInt(uint8_t b) {
    if (b < NUM_FREE_VALUES) {
      return b;
    } else {
      uint64_t decoded = NUM_FREE_VALUES + int4ToLong(b - NUM_FREE_VALUES);
      return (int32_t)decoded;
    }
  }

};



//
// The same tradeoffs, quantization, and order-of-operations were used as Lucene here to make scores compatible.
// See the Lucene javadoc for BM25Similarity for more info.
//
class Similarity {
public:
  const float k1;
  const float b;

  Similarity(float k1=1.2f, float b=0.75f)
  : k1(k1), b(b) {
  }

  /// Stats for the field, used in scoring. Represents the stats for a field across the entire collection.
  /// That includes all segments in the current IndexReader, as well as any other
  /// shards or remote indexes (if global scoring is desired)
  class FieldStats {
  public:
    int64_t maxDoc = 0;
    int64_t docsWithField = 0;
    int64_t sumTotalTermFreq = 0;
    int64_t sumDocFreq = 0;

    /// add another FieldStats to this one
    void add(const FieldStats& other) {
      maxDoc += other.maxDoc;
      docsWithField += other.docsWithField;
      sumTotalTermFreq += other.sumTotalTermFreq;
      sumDocFreq += other.sumDocFreq;
    }
  };

  /// Stats for a term in a specific field, used in scoring.  Represents the stats for a term in a field
  /// across the entire collection. That includes all segments in the current IndexReader, as well as any other
  /// shards or remote indexes (if global scoring is desired)
  class TermStats {
  public:
    int64_t docFreq = 0;
    int64_t totalTermFreq = 0;

    /// add another TermStats to this one
    void add(const TermStats& other) {
      docFreq += other.docFreq;
      totalTermFreq += other.totalTermFreq;
    }
  };


  /// Term-specific BM25 scorer for this Similarity.
  /// Because this scorer partially precomputes scores, it needs to be different for each term.
  ///
  /// NOTE: This is a big object because of the norm cache (~1KB RAM)
  class BM25Scorer {
    // a cache of byte-encoded-field-length to inverse norm
    std::array<float,256> invNorm{};
    const float weight;
    const float boost;
    const float k1;
    const float b;
    const float idf;
    const float avgdl;

  public:
    BM25Scorer(float boost, float k1, float b, float idf, float avgdl);

    float score(float termFreq, int64_t encodedNorm) {
      solux::unused(boost,k1,b,idf,avgdl); // already folded in
      // Adapted from lucene, see BM25Similarity.java for more details.
      auto normInverse = invNorm[ (uint8_t)encodedNorm ];
      return weight - weight / (1.0f + termFreq * normInverse);
    }

    // PERF-CRITICAL, RELIES ON AUTO-VECTORIZATION. Block BM25 for the dense
    // disjunction hot path (bulk_dense, via fillScoresFromSpans). It is split on
    // purpose: a scalar invNorm[] LUT-gather loop, then a math-ONLY loop that gcc
    // auto-vectorizes to AVX (vdivps / vfmadd132ps / vmulps / vsubps over ymm,
    // 8-wide). Two rules if you touch this:
    //   1. Do NOT merge the two loops - the gather (invNorm[norms[i]]) inside the
    //      math loop blocks vectorization.
    //   2. Do NOT algebraically rewrite loop 2 (no precomputing boost*weight, no
    //      reassociation). Scores must stay BIT-IDENTICAL to score() above, and the
    //      vector FMA only matches the scalar path because the expression is verbatim.
    // After any change, objdump TermQuery::Scorer::fillScoresFromSpans (this inlines
    // there) and confirm vdivps/ymm survive, then re-run the byte-identical score
    // guards + the bulk_dense gcc-release A/B. Auto-vec confirmed on g++ (Ubuntu)
    // 16.0.1 20260322 (trunk r16-8246); a future compiler may need a re-check.
    void scoreBlock(const int32_t* tf, const uint8_t* norms, float boost, float* out,
                    int32_t count) {
      assert(count >= 0 && count <= Postings::DOCS_BLOCK_SIZE);
      float factor[Postings::DOCS_BLOCK_SIZE];
      for (int32_t i = 0; i < count; i++) {
        factor[i] = invNorm[norms[i]];
      }
      for (int32_t i = 0; i < count; i++) {
        out[i] = boost * (weight - weight / (1.0f + (float)tf[i] * factor[i]));
      }
    }
  };


  // Pass in field stats and term stats to get a scorer for this Similarity
  BM25Scorer getScorer(float boost, const FieldStats& fieldStats, const TermStats& termStats) {
    auto idf_ = idf(fieldStats, termStats);
    auto avgdl = avgFieldLength(fieldStats);
    return BM25Scorer(boost, k1, b, idf_, avgdl);
  }

  BM25Scorer getScorer(float boost, const FieldStats& fieldStats, float idf_) {
    auto avgdl = avgFieldLength(fieldStats);
    return BM25Scorer(boost, k1, b, idf_, avgdl);
  }

  BM25Scorer getScorer(float boost, const FieldStats& fieldStats, std::span<TermStats*> termStatsList) {
    double idf_ = 0.0;
    for (auto termStats : termStatsList) {
      idf_ += idf(fieldStats, *termStats);
    }
    auto avgdl = avgFieldLength(fieldStats);
    return BM25Scorer(boost, k1, b, idf_, avgdl);
  }

  float idf(int64_t docFreq, int64_t docCount) {
    return (float) std::log1p( (docCount - docFreq + 0.5L) / (docFreq + 0.5L) );
  }

  float idf(const FieldStats& fieldStats, const TermStats& termStats) {
    return idf(termStats.docFreq, fieldStats.docsWithField);
  }

  float avgFieldLength(const FieldStats& fieldStats) {
    return (float) ((double)fieldStats.sumTotalTermFreq / (double)fieldStats.docsWithField);
  }

};




}
