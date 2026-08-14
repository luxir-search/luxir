#pragma once

#include <numeric>

#include "Query.h"

namespace luxir {

// Bulk scorer over a set-shaped source: a materialized DocSet, or - when docs
// is null - the implicit all-docs set over [0, maxDoc). Cache hits must retain
// a bulk implementation as well as pull/window membership; otherwise the same
// query would lose the dense COUNT route. The null-source form backs match-all
// queries: all-docs intersected with the per-call filter IS the filter, so the
// filter is consumed as the source and the emitted docs need no second check.
class DocSetBulkScorer final : public BulkScorer {
  static constexpr int32_t kWindowSize = DocsEnumMeta::L1_DOCS;
  static constexpr int32_t kWindowWords = (kWindowSize + 63) >> 6;

  DocSet* docs;
  int32_t maxDoc;
  float score;
  std::span<const int32_t> arrDocs;
  int32_t sourceArrIdx = 0;
  std::span<int32_t> outDocs;
  std::span<float> outScores;
  std::span<uint64_t> windowBits;

  int32_t endFor(int32_t min, int32_t max) const {
    int32_t bound = std::min(max, maxDoc);
    // Length-first arithmetic: min + kWindowSize would overflow signed int32
    // when min approaches INT_MAX (END-adjacent doc ids).
    return bound - min > kWindowSize ? min + kWindowSize : bound;
  }

  // The array cursor tracks one underlying array across monotone window
  // requests; a null-source scorer meets its array only through the per-call
  // filter, so rebind (and restart the cursor) if a different set shows up.
  void bindArray(DocSet* source) {
    std::span<const int32_t> d = ((ArrDocSet*) source)->docs();
    if (d.data() != arrDocs.data()) {
      arrDocs = d;
      sourceArrIdx = 0;
    }
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

  int32_t fillSourceBits(DocSet* source, int32_t min, int32_t end,
                         bool computeCard) {
    assert(source->type == DocSet::BITSET);
    skipCount(SkipStats::countBulkFillCalls);
    const FixedBitSet& sourceBits = ((BitDocSet*) source)->bits();
    int32_t bitCount = end - min;
    int32_t words = (bitCount + 63) >> 6;
    int32_t sourceWord = min >> 6;
    int32_t shift = min & 63;
    int32_t sourceWords = (int32_t) FixedBitSet::sizeInWords(sourceBits.size());
    int32_t wordCard = 0;
    for (int32_t i = 0; i < words; i++) {
      uint64_t bits = sourceBits.words[sourceWord + i] >> shift;
      if (shift != 0 && sourceWord + i + 1 < sourceWords) {
        bits |= sourceBits.words[sourceWord + i + 1] << (64 - shift);
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

  // Shared window production. emit=false advances source cursors without
  // materializing docs (a score window whose constant score is noncompetitive).
  template <bool kScores>
  int32_t emitNextWindow(ScoreWindow& out, DocSet* filter,
                         int32_t min, int32_t max, bool emit) {
    int32_t end = endFor(min, max);
    out = {.min = min, .max = end};
    if (min >= end) return PostingsReader::END;
    DocSet* source = docs != nullptr ? docs : filter;
    DocSet* extra = docs != nullptr ? filter : nullptr;

    if (source == nullptr) {
      if (emit) {
        out.size = end - min;
        std::iota(outDocs.begin(), outDocs.begin() + out.size, min);
      }
    } else if (source->type == DocSet::ARRAY) {
      bindArray(source);
      std::span<const int32_t> matches = arrayWindow(min, end);
      if (emit) {
        if (extra == nullptr) {
          std::copy(matches.begin(), matches.end(), outDocs.begin());
          out.size = (int32_t) matches.size();
        } else {
          for (int32_t doc : matches) {
            if (extra->get(doc)) {
              outDocs[(size_t) out.size++] = doc;
            }
          }
        }
      }
    } else if (emit) {
      fillSourceBits(source, min, end, false);
      if (extra != nullptr) {
        intersectFilter(extra, min, end, false);
      }
      int32_t words = (end - min + 63) >> 6;
      for (int32_t wordIdx = 0; wordIdx < words; wordIdx++) {
        uint64_t bits = windowBits[(size_t) wordIdx];
        while (bits != 0) {
          int32_t bit = (int32_t) std::countr_zero(bits);
          int32_t doc = min + (wordIdx << 6) + bit;
          if (doc >= end) break;
          outDocs[(size_t) out.size] = doc;
          out.size++;
          bits &= bits - 1;
        }
      }
    }
    if (kScores) {
      std::fill_n(outScores.begin(), out.size, score);
    }
    out.docs = outDocs.first((size_t) out.size);
    out.scores = outScores.first((size_t) out.size);
    return end >= max || end >= maxDoc ? PostingsReader::END : end;
  }

public:
  DocSetBulkScorer(MemPool& pool, DocSet* docs, int32_t maxDoc,
                   float score = 0.0f)
      : docs(docs), maxDoc(maxDoc), score(score),
        outDocs(pool.make_span<int32_t>((size_t) kWindowSize)),
        outScores(pool.make_span<float>((size_t) kWindowSize)),
        windowBits(docs == nullptr || docs->type == DocSet::BITSET
                       ? pool.make_span<uint64_t>((size_t) kWindowWords)
                       : std::span<uint64_t>()) {
    if (docs != nullptr && docs->type == DocSet::ARRAY) {
      arrDocs = ((ArrDocSet*) docs)->docs();
    }
  }

  bool willCountDense() const override { return true; }

  bool supportsMatchWindows() const override { return true; }

  int32_t scoreNextWindow(ScoreWindow& out, DocSet* filter,
                          int32_t min, int32_t max,
                          float minCompetitiveScore) override {
    return emitNextWindow<true>(out, filter, min, max,
                                minCompetitiveScore <= score);
  }

  int32_t matchNextWindow(ScoreWindow& out, DocSet* filter,
                          int32_t min, int32_t max) override {
    return emitNextWindow<false>(out, filter, min, max, true);
  }

  int32_t countNextWindow(int64_t& count, DocSetBuilder* domainOut,
                          DocSet* filter, int32_t min, int32_t max) override {
    max = std::min(max, maxDoc);
    if (min >= max) return PostingsReader::END;
    DocSet* source = docs != nullptr ? docs : filter;
    DocSet* extra = docs != nullptr ? filter : nullptr;

    if (source == nullptr) {
      if (domainOut == nullptr) {
        count += max - min;
        return PostingsReader::END;
      }
      int32_t end = endFor(min, max);
      int32_t n = end - min;
      std::iota(outDocs.begin(), outDocs.begin() + n, min);
      skipCount(SkipStats::bulkDomainWindowsFed);
      domainOut->addSorted(outDocs.first((size_t) n));
      count += n;
      return end >= max ? PostingsReader::END : end;
    }
    if (min == 0 && max == maxDoc && extra == nullptr && domainOut == nullptr) {
      count += source->card();
      return PostingsReader::END;
    }

    int32_t end = endFor(min, max);
    if (source->type == DocSet::ARRAY) {
      bindArray(source);
      std::span<const int32_t> matches = arrayWindow(min, end);
      if (extra == nullptr) {
        count += (int32_t) matches.size();
        if (domainOut != nullptr && !matches.empty()) {
          skipCount(SkipStats::bulkDomainWindowsFed);
          domainOut->addSorted(matches);
        }
      } else {
        int32_t survivors = 0;
        for (int32_t doc : matches) {
          if (extra->get(doc)) {
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

    int32_t wordCard = fillSourceBits(source, min, end, extra == nullptr);
    if (extra != nullptr) {
      wordCard = intersectFilter(extra, min, end, true);
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

} // namespace luxir
