#pragma once

#include <algorithm>
#include <bit>
#include <cassert>
#include <cstdint>
#include <span>
#include <type_traits>

#include "solux/query/Query.h"

namespace solux {

struct PointsMaterialize {
  struct Materialized {
    std::span<int32_t> docs;
    uint64_t* words = nullptr;
  };

  static uint64_t arrayLimit(int32_t maxDoc) {
    return ((uint64_t)(uint32_t)maxDoc + 31) >> 5;
  }

  static bool useBitset(uint64_t expected, int32_t maxDoc) {
    return expected > arrayLimit(maxDoc);
  }

  static void setRun(FixedBitSet& bits, int32_t begin, int32_t end) {
    if (begin >= end) return;
    int32_t firstWord = begin >> 6;
    int32_t lastWord = (end - 1) >> 6;
    uint64_t firstMask = ~0ULL << (begin & 63);
    uint64_t lastMask = (end & 63) == 0
        ? ~0ULL : (1ULL << (end & 63)) - 1ULL;
    if (firstWord == lastWord) {
      bits.words[firstWord] |= firstMask & lastMask;
      return;
    }
    bits.words[firstWord] |= firstMask;
    for (int32_t word = firstWord + 1; word < lastWord; word++) {
      bits.words[word] = ~0ULL;
    }
    bits.words[lastWord] |= lastMask;
  }

  static void clearRun(FixedBitSet& bits, int32_t begin, int32_t end) {
    if (begin >= end) return;
    int32_t firstWord = begin >> 6;
    int32_t lastWord = (end - 1) >> 6;
    uint64_t firstMask = ~0ULL << (begin & 63);
    uint64_t lastMask = (end & 63) == 0
        ? ~0ULL : (1ULL << (end & 63)) - 1ULL;
    if (firstWord == lastWord) {
      bits.words[firstWord] &= ~(firstMask & lastMask);
      return;
    }
    bits.words[firstWord] &= ~firstMask;
    for (int32_t word = firstWord + 1; word < lastWord; word++) {
      bits.words[word] = 0;
    }
    bits.words[lastWord] &= ~lastMask;
  }

  // Materialized doc sets are exact and source-agnostic, so both scorer
  // representations can serve a WindowFilter regardless of which query
  // produced the points traversal.
  class PointsArrayScorer final : public Query::ConstantScorer {
    std::span<const int32_t> docs;
    int64_t index = -1;
    int32_t docid = -1;

  public:
    PointsArrayScorer(std::span<const int32_t> docs, float constantScore)
      : Query::ConstantScorer(constantScore), docs(docs) {}

    int32_t next() override {
      assert(docid != PostingsReader::END);
      index++;
      return docid = index < (int64_t)docs.size()
          ? docs[(size_t)index] : PostingsReader::END;
    }

    int32_t advance(int32_t target) override {
      assert(docid < target);
      auto begin = docs.begin() + std::min<int64_t>(index + 1, docs.size());
      auto found = std::lower_bound(begin, docs.end(), target);
      index = found - docs.begin();
      return docid = found == docs.end() ? PostingsReader::END : *found;
    }

    int32_t docId() override { return docid; }
    bool supportsWindowFilter() const override { return true; }

  protected:
    void exhaust() override { index = (int64_t)docs.size(); }
  };

  class PointsBitScorer final : public Query::ConstantScorer {
    FixedBitSet bits;
    int32_t limit;
    int32_t docid = -1;

    int32_t seek(int32_t target) {
      if (target >= limit) return docid = PostingsReader::END;
      int32_t found = bits.nextSetBit(target);
      return docid = found == FixedBitSet::MAX_INDEX
          ? PostingsReader::END : found;
    }

  public:
    PointsBitScorer(FixedBitSet bits, float constantScore)
      : Query::ConstantScorer(constantScore), bits(bits), limit(bits.size()) {}

    int32_t next() override {
      assert(docid != PostingsReader::END);
      return seek(docid + 1);
    }
    int32_t advance(int32_t target) override {
      assert(docid < target);
      return seek(target);
    }
    int32_t docId() override { return docid; }
    bool supportsWindowFilter() const override { return true; }
  };

  class PointsArrayBulkScorer final : public BulkScorer {
    std::span<const int32_t> docs;
    std::span<int32_t> outDocs;
    std::span<float> outScores;
    float constantScore;

    bool accepted(DocSet* filter, int32_t doc) const {
      return filter == nullptr || filter->get(doc);
    }

  public:
    PointsArrayBulkScorer(MemPool& pool, std::span<const int32_t> docs,
                          float constantScore)
        : docs(docs), outDocs(pool.make_span<int32_t>(docs.size())),
          outScores(pool.make_span<float>(docs.size())),
          constantScore(constantScore) {}

    int32_t countNextWindow(int64_t& count, DocSetBuilder* domainOut,
                            DocSet* filter, int32_t min, int32_t max) override {
      if (min >= max || (filter != nullptr && filter->card() == 0)) {
        return PostingsReader::END;
      }
      auto begin = std::lower_bound(docs.begin(), docs.end(), min);
      auto end = std::lower_bound(begin, docs.end(), max);
      for (auto it = begin; it != end; ++it) {
        if (!accepted(filter, *it)) continue;
        if (domainOut != nullptr) domainOut->add(*it);
        count++;
      }
      if (domainOut != nullptr) skipCount(SkipStats::bulkDomainWindowsFed);
      return PostingsReader::END;
    }

    int32_t scoreNextWindow(ScoreWindow& out, DocSet* filter, int32_t min,
                            int32_t max, float minCompetitiveScore) override {
      out.min = min;
      out.max = max;
      out.size = 0;
      out.docs = outDocs;
      out.scores = outScores;
      if (min >= max || minCompetitiveScore > constantScore
          || (filter != nullptr && filter->card() == 0)) {
        return PostingsReader::END;
      }
      auto begin = std::lower_bound(docs.begin(), docs.end(), min);
      auto end = std::lower_bound(begin, docs.end(), max);
      for (auto it = begin; it != end; ++it) {
        if (!accepted(filter, *it)) continue;
        outDocs[(size_t)out.size] = *it;
        outScores[(size_t)out.size] = constantScore;
        out.size++;
      }
      return PostingsReader::END;
    }
  };

  class PointsBitBulkScorer final : public BulkScorer {
    static constexpr int32_t WINDOW_SIZE = 4096;
    static constexpr int32_t WINDOW_WORDS = WINDOW_SIZE / 64;

    FixedBitSet bits;
    std::span<uint64_t> windowBits;
    std::span<int32_t> outDocs;
    std::span<float> outScores;
    int32_t windowStart = 0;
    int32_t windowEnd = 0;
    float constantScore;

    static uint64_t validMask(int32_t remaining) {
      if (remaining >= 64) return ~0ULL;
      if (remaining <= 0) return 0;
      return (1ULL << remaining) - 1ULL;
    }

    void loadSource(int32_t min, int32_t max) {
      windowStart = min;
      windowEnd = (int32_t)std::min<int64_t>(
          std::min(bits.size(), max), (int64_t)min + WINDOW_SIZE);
      int32_t sourceWords = (int32_t)FixedBitSet::sizeInWords(bits.size());
      for (int32_t w = 0; w < WINDOW_WORDS; w++) {
        int32_t firstDoc = windowStart + (w << 6);
        int32_t remaining = windowEnd - firstDoc;
        if (remaining <= 0) {
          windowBits[(size_t)w] = 0;
          continue;
        }
        int32_t sourceWord = firstDoc >> 6;
        int32_t shift = firstDoc & 63;
        uint64_t source = sourceWord < sourceWords
            ? bits.words[sourceWord] >> shift : 0;
        if (shift != 0 && sourceWord + 1 < sourceWords) {
          source |= bits.words[sourceWord + 1] << (64 - shift);
        }
        windowBits[(size_t)w] = source & validMask(remaining);
      }
    }

    void applyFilter(DocSet* filter) {
      if (filter == nullptr) return;
      if (filter->type == DocSet::BITSET) {
        const auto& filterBits = ((BitDocSet*)filter)->bits();
        int32_t sourceWords = (int32_t)FixedBitSet::sizeInWords(filterBits.size());
        for (int32_t w = 0; w < WINDOW_WORDS; w++) {
          int32_t firstDoc = windowStart + (w << 6);
          int32_t remaining = windowEnd - firstDoc;
          if (remaining <= 0) continue;
          int32_t sourceWord = firstDoc >> 6;
          int32_t shift = firstDoc & 63;
          uint64_t source = sourceWord < sourceWords
              ? filterBits.words[sourceWord] >> shift : 0;
          if (shift != 0 && sourceWord + 1 < sourceWords) {
            source |= filterBits.words[sourceWord + 1] << (64 - shift);
          }
          windowBits[(size_t)w] &= source & validMask(remaining);
        }
        return;
      }
      int32_t nbits = windowEnd - windowStart;
      for (int32_t w = 0; w < WINDOW_WORDS; w++) {
        uint64_t word = windowBits[(size_t)w];
        while (word != 0) {
          int32_t bit = (int32_t)std::countr_zero(word);
          int32_t index = (w << 6) + bit;
          if (index >= nbits) break;
          if (!filter->get(windowStart + index)) {
            windowBits[(size_t)w] &= ~(1ULL << bit);
          }
          word &= word - 1;
        }
      }
    }

    int32_t cardinality() const {
      int32_t count = 0;
      for (uint64_t word : windowBits) count += (int32_t)std::popcount(word);
      return count;
    }

    void fillWindow(DocSet* filter, int32_t min, int32_t max) {
      loadSource(min, max);
      applyFilter(filter);
    }

  public:
    PointsBitBulkScorer(MemPool& pool, FixedBitSet bits, float constantScore)
        : bits(bits),
          windowBits(pool.make_span<uint64_t>(WINDOW_WORDS)),
          outDocs(pool.make_span<int32_t>(WINDOW_SIZE)),
          outScores(pool.make_span<float>(WINDOW_SIZE)),
          constantScore(constantScore) {}

    int32_t countNextWindow(int64_t& count, DocSetBuilder* domainOut,
                            DocSet* filter, int32_t min, int32_t max) override {
      max = std::min(max, bits.size());
      if (min >= max || (filter != nullptr && filter->card() == 0)) {
        return PostingsReader::END;
      }
      fillWindow(filter, min, max);
      int32_t wordCard = cardinality();
      if (domainOut != nullptr && wordCard != 0) {
        skipCount(SkipStats::bulkDomainWindowsFed);
        domainOut->addWindowWords(
            windowBits.data(), windowStart, windowEnd, wordCard);
      }
      count += wordCard;
      return windowEnd >= max ? PostingsReader::END : windowEnd;
    }

    int32_t scoreNextWindow(ScoreWindow& out, DocSet* filter, int32_t min,
                            int32_t max, float minCompetitiveScore) override {
      max = std::min(max, bits.size());
      out.min = min;
      out.max = min;
      out.size = 0;
      out.docs = outDocs;
      out.scores = outScores;
      if (min >= max || (filter != nullptr && filter->card() == 0)) {
        return PostingsReader::END;
      }
      fillWindow(filter, min, max);
      out.min = windowStart;
      out.max = windowEnd;
      if (minCompetitiveScore <= constantScore) {
        int32_t nbits = windowEnd - windowStart;
        for (int32_t w = 0; w < WINDOW_WORDS; w++) {
          uint64_t word = windowBits[(size_t)w];
          while (word != 0) {
            int32_t bit = (int32_t)std::countr_zero(word);
            int32_t index = (w << 6) + bit;
            if (index >= nbits) break;
            outDocs[(size_t)out.size] = windowStart + index;
            outScores[(size_t)out.size] = constantScore;
            out.size++;
            word &= word - 1;
          }
        }
      }
      return windowEnd >= max ? PostingsReader::END : windowEnd;
    }
  };

  static Query::Scorer* scorerFor(MemPool& pool,
                                  const Materialized& result,
                                  int32_t maxDoc, float constantScore) {
    if (result.words != nullptr) {
      return pool.make<PointsBitScorer>(FixedBitSet(result.words, maxDoc),
                                        constantScore);
    }
    return pool.make<PointsArrayScorer>(result.docs, constantScore);
  }

  static BulkScorer* bulkFor(MemPool& pool, const Materialized& result,
                             int32_t maxDoc, float constantScore) {
    if (result.words != nullptr) {
      return pool.make<PointsBitBulkScorer>(
          pool, FixedBitSet(result.words, maxDoc), constantScore);
    }
    return pool.make<PointsArrayBulkScorer>(pool, result.docs, constantScore);
  }
};

static_assert(std::is_trivially_destructible_v<PointsMaterialize::PointsArrayScorer>);
static_assert(std::is_trivially_destructible_v<PointsMaterialize::PointsBitScorer>);
static_assert(std::is_trivially_destructible_v<PointsMaterialize::PointsArrayBulkScorer>);
static_assert(std::is_trivially_destructible_v<PointsMaterialize::PointsBitBulkScorer>);

} // namespace solux
