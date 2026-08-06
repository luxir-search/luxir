#pragma once

#include "Query.h"

namespace solux {

// Bulk scorer over a materialized DocSet. Cache hits must retain a bulk
// implementation as well as pull/window membership; otherwise the same query
// would lose the dense COUNT route.
class DocSetBulkScorer final : public BulkScorer {
  static constexpr int32_t kWindowSize = DocsEnumMeta::L1_DOCS;
  static constexpr int32_t kWindowWords = (kWindowSize + 63) >> 6;

  DocSet* docs;
  int32_t maxDoc;
  std::span<const int32_t> arrDocs;
  int32_t sourceArrIdx = 0;
  std::span<int32_t> outDocs;
  std::span<float> outScores;
  std::span<uint64_t> windowBits;

  int32_t endFor(int32_t min, int32_t max) const {
    int32_t requested = min + kWindowSize;
    if (requested < min) requested = max;
    return std::min({requested, max, maxDoc});
  }

  std::span<const int32_t> arrayWindow(int32_t min, int32_t end) {
    const int32_t* base = arrDocs.data();
    const int32_t* limit = base + arrDocs.size();
    const int32_t* first = screaming::gallopLowerBound(
        base + sourceArrIdx, limit, min);
    if (first == limit || *first >= end) {
      sourceArrIdx = (int32_t) (first - base);
      return {first, (size_t) 0};
    }
    const int32_t* after = screaming::gallopLowerBound(
        first + 1, limit, end);
    sourceArrIdx = (int32_t) (after - base);
    return {first, (size_t) (after - first)};
  }

  int32_t fillSourceBits(int32_t min, int32_t end, bool computeCard) {
    assert(docs->type == DocSet::BITSET);
    skipCount(SkipStats::countBulkFillCalls);
    const FixedBitSet& source = ((BitDocSet*) docs)->bits();
    int32_t bitCount = end - min;
    int32_t words = (bitCount + 63) >> 6;
    int32_t sourceWord = min >> 6;
    int32_t shift = min & 63;
    int32_t sourceWords = (int32_t) FixedBitSet::sizeInWords(source.size());
    int32_t wordCard = 0;
    for (int32_t i = 0; i < words; i++) {
      uint64_t bits = source.words[sourceWord + i] >> shift;
      if (shift != 0 && sourceWord + i + 1 < sourceWords) {
        bits |= source.words[sourceWord + i + 1] << (64 - shift);
      }
      if (i + 1 == words && (bitCount & 63) != 0) {
        bits &= (1ULL << (bitCount & 63)) - 1ULL;
      }
      windowBits[(size_t) i] = bits;
      if (computeCard) {
        wordCard += (int32_t) std::popcount(bits);
      }
    }
    return wordCard;
  }

  int32_t intersectFilter(DocSet* filter, int32_t min, int32_t end,
                          bool computeCard) {
    assert(filter != nullptr);
    int32_t bitCount = end - min;
    int32_t words = (bitCount + 63) >> 6;
    int32_t wordCard = 0;
    if (filter->type == DocSet::BITSET) {
      const FixedBitSet& source = ((BitDocSet*) filter)->bits();
      intersectBitSetWindow(windowBits, min, end, source);
      for (int32_t i = 0; i < words; i++) {
        if (computeCard) {
          wordCard += (int32_t) std::popcount(windowBits[(size_t) i]);
        }
      }
      return wordCard;
    }

    for (int32_t wordIdx = 0; wordIdx < words; wordIdx++) {
      uint64_t bits = windowBits[(size_t) wordIdx];
      while (bits != 0) {
        int32_t bit = (int32_t) std::countr_zero(bits);
        int32_t doc = min + (wordIdx << 6) + bit;
        if (doc >= end) break;
        if (!filter->get(doc)) {
          windowBits[(size_t) wordIdx] &= ~(1ULL << bit);
        } else if (computeCard) {
          wordCard++;
        }
        bits &= bits - 1;
      }
    }
    return wordCard;
  }

public:
  DocSetBulkScorer(MemPool& pool, DocSet* docs, int32_t maxDoc)
      : docs(docs), maxDoc(maxDoc),
        outDocs(pool.make_span<int32_t>((size_t) kWindowSize)),
        outScores(pool.make_span<float>((size_t) kWindowSize)),
        windowBits(docs->type == DocSet::BITSET
                       ? pool.make_span<uint64_t>((size_t) kWindowWords)
                       : std::span<uint64_t>()) {
    if (docs->type == DocSet::ARRAY) {
      arrDocs = ((ArrDocSet*) docs)->docs();
    }
  }

  bool willCountDense() const override { return true; }

  int32_t scoreNextWindow(ScoreWindow& out, DocSet* filter,
                          int32_t min, int32_t max,
                          float minCompetitiveScore) override {
    int32_t end = endFor(min, max);
    out = {.min = min, .max = end};
    if (min >= end) return PostingsReader::END;
    if (docs->type == DocSet::ARRAY) {
      std::span<const int32_t> matches = arrayWindow(min, end);
      if (minCompetitiveScore <= 0.0f) {
        if (filter == nullptr) {
          std::copy(matches.begin(), matches.end(), outDocs.begin());
          out.size = (int32_t) matches.size();
        } else {
          for (int32_t doc : matches) {
            if (filter->get(doc)) {
              outDocs[(size_t) out.size++] = doc;
            }
          }
        }
        std::fill_n(outScores.begin(), out.size, 0.0f);
      }
      out.docs = outDocs.first((size_t) out.size);
      out.scores = outScores.first((size_t) out.size);
      return end >= max || end >= maxDoc ? PostingsReader::END : end;
    }

    fillSourceBits(min, end, false);
    if (filter != nullptr) {
      intersectFilter(filter, min, end, false);
    }
    if (minCompetitiveScore <= 0.0f) {
      int32_t words = (end - min + 63) >> 6;
      for (int32_t wordIdx = 0; wordIdx < words; wordIdx++) {
        uint64_t bits = windowBits[(size_t) wordIdx];
        while (bits != 0) {
          int32_t bit = (int32_t) std::countr_zero(bits);
          int32_t doc = min + (wordIdx << 6) + bit;
          if (doc >= end) break;
          outDocs[(size_t) out.size] = doc;
          outScores[(size_t) out.size] = 0.0f;
          out.size++;
          bits &= bits - 1;
        }
      }
    }
    out.docs = outDocs.first((size_t) out.size);
    out.scores = outScores.first((size_t) out.size);
    return end >= max || end >= maxDoc ? PostingsReader::END : end;
  }

  int32_t countNextWindow(int64_t& count, DocSetBuilder* domainOut,
                          DocSet* filter, int32_t min, int32_t max) override {
    max = std::min(max, maxDoc);
    if (min >= max) return PostingsReader::END;
    if (min == 0 && max == maxDoc && filter == nullptr && domainOut == nullptr) {
      count += docs->card();
      return PostingsReader::END;
    }

    int32_t end = endFor(min, max);
    if (docs->type == DocSet::ARRAY) {
      std::span<const int32_t> matches = arrayWindow(min, end);
      if (filter == nullptr) {
        count += (int32_t) matches.size();
        if (domainOut != nullptr && !matches.empty()) {
          skipCount(SkipStats::bulkDomainWindowsFed);
          domainOut->addSorted(matches);
        }
      } else {
        int32_t survivors = 0;
        for (int32_t doc : matches) {
          if (filter->get(doc)) {
            outDocs[(size_t) survivors++] = doc;
          }
        }
        count += survivors;
        if (domainOut != nullptr && survivors != 0) {
          skipCount(SkipStats::bulkDomainWindowsFed);
          domainOut->addSorted(outDocs.first((size_t) survivors));
        }
      }
      return end >= max ? PostingsReader::END : end;
    }

    int32_t wordCard = fillSourceBits(min, end, filter == nullptr);
    if (filter != nullptr) {
      wordCard = intersectFilter(filter, min, end, true);
    }
    if (wordCard != 0) {
      if (domainOut != nullptr) {
        skipCount(SkipStats::bulkDomainWindowsFed);
        domainOut->addWindowWords(windowBits.data(), min, end, wordCard);
      }
      count += wordCard;
    }
    return end >= max ? PostingsReader::END : end;
  }
};

} // namespace solux
