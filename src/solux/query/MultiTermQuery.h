#pragma once

#include <cstring>
#include <string_view>
#include <vector>

#include "Query.h"
#include "solux/reader/FilteredTermsEnum.h"
#include "solux/util/screaming.h"

namespace solux {

// Base for constant-score queries that union postings from multiple terms in
// one field. Subclasses provide the filtered term iterator.
class MultiTermQuery : public Query {
protected:
  std::string_view field;

public:
  explicit MultiTermQuery(std::string_view field) : field(field) {}

  std::string_view getField() const { return field; }
  ScoreProfile scoreProfile() const override {
    return ScoreProfile::automatic(1.0f);
  }

  // Build the per-segment filtered term iterator.
  virtual FilteredTermsEnum* createFilteredEnum(MemPool& pool, TermsEnum& te) = 0;

  Weight* createWeight(Context& context, int32_t flags,
                       float multiplier = 1.0f) override {
    return context.pool.make<MultiTermQuery::Weight>(context, *this, flags, multiplier);
  }

  // Iterates set bits of the membership bitset under a constant score.
  class Scorer final : public Query::ConstantScorer {
    FixedBitSet bits;
    int32_t maxDoc;
    int32_t docid = -1;

    // First set bit at or after `from`, or END when none remain.
    int32_t advanceTo(int32_t from) {
      if (from >= maxDoc) return docid = PostingsReader::END;
      return docid = bits.nextSetBit(from);  // MAX_INDEX == PostingsReader::END when none
    }

  public:
    Scorer(FixedBitSet bits, int32_t maxDoc, float constantScore)
      : Query::ConstantScorer(constantScore), bits(bits), maxDoc(maxDoc) {}

    int32_t next() override {
      if (docid == PostingsReader::END) return docid = PostingsReader::END;
      return advanceTo(docid + 1);
    }
    int32_t advance(int32_t target) override {
      assert(docid < target);  // strict advance
      return advanceTo(target);
    }
    int32_t docId() override { return docid; }

  protected:
    void exhaust() override { maxDoc = 0; }
  };

  // Builds one doc-id window at a time while retaining each term's docs-only
  // cursor. Windows and conjunction-driven advances are monotone, so a postings
  // block is decoded at most once.
  class LazyScorer final : public Query::ConstantScorer {
    static constexpr int32_t WINDOW_SIZE = DocsEnum::L1_DOCS;
    static constexpr int32_t WINDOW_WORDS = WINDOW_SIZE / 64;

    std::span<DocsEnum> docsEnums;
    std::span<uint64_t> windowBits;
    int32_t maxDoc;
    int32_t docid = -1;
    int32_t windowStart = -1;
    int32_t windowEnd = -1;
    int32_t nextWindowTarget = -1;

    int32_t findInWindow(int32_t target) const {
      if (target < windowStart || target >= windowEnd) {
        return PostingsReader::END;
      }
      int32_t index = target - windowStart;
      int32_t word = index >> 6;
      uint64_t bits = windowBits[(size_t) word] & (~0ULL << (index & 63));
      while (true) {
        if (bits != 0) {
          int32_t found = windowStart + (word << 6) + (int32_t) std::countr_zero(bits);
          return found < windowEnd ? found : PostingsReader::END;
        }
        word++;
        if (word >= WINDOW_WORDS || windowStart + (word << 6) >= windowEnd) {
          return PostingsReader::END;
        }
        bits = windowBits[(size_t) word];
      }
    }

    void fillWindow(int32_t target) {
      windowStart = target;
      windowEnd = (int32_t) std::min<int64_t>(maxDoc, (int64_t) target + WINDOW_SIZE);
      nextWindowTarget = windowEnd;
      memset(windowBits.data(), 0, windowBits.size_bytes());

      int32_t firstDocAfterWindow = PostingsReader::END;
      for (auto& docsEnum : docsEnums) {
        int32_t doc = docsEnum.docId();
        if (doc < windowStart) {
          doc = docsEnum.advanceDocOnly(windowStart);
        }
        if (doc >= windowEnd) {
          firstDocAfterWindow = std::min(firstDocAfterWindow, doc);
          continue;
        }
        docsEnum.intoBitSet(windowBits, windowStart, windowEnd);
      }

      bool empty = true;
      for (uint64_t bits : windowBits) {
        if (bits != 0) {
          empty = false;
          break;
        }
      }
      if (empty && firstDocAfterWindow != PostingsReader::END) {
        nextWindowTarget = firstDocAfterWindow;
      }
    }

    int32_t seek(int32_t target) {
      if (target >= maxDoc) {
        return docid = PostingsReader::END;
      }
      int32_t found = findInWindow(target);
      if (found == PostingsReader::END
          && target >= windowStart && target < windowEnd) {
        target = windowEnd;
      }
      if (target >= maxDoc) {
        return docid = PostingsReader::END;
      }
      while (found == PostingsReader::END) {
        fillWindow(target);
        found = findInWindow(target);
        if (found != PostingsReader::END) {
          break;
        }
        if (nextWindowTarget >= maxDoc) {
          return docid = PostingsReader::END;
        }
        target = nextWindowTarget;
      }
      return docid = found;
    }

  public:
    LazyScorer(std::span<DocsEnum> docsEnums,
               std::span<uint64_t> windowBits, int32_t maxDoc, float constantScore)
      : Query::ConstantScorer(constantScore), docsEnums(docsEnums),
        windowBits(windowBits), maxDoc(maxDoc) {
      static_assert((WINDOW_SIZE % 64) == 0);
      assert(windowBits.size() == WINDOW_WORDS);
    }

    int32_t next() override {
      if (docid == PostingsReader::END) {
        return docid = PostingsReader::END;
      }
      return seek(docid + 1);
    }
    int32_t advance(int32_t target) override {
      assert(docid < target);
      return seek(target);
    }
    int32_t docId() override { return docid; }

  protected:
    void exhaust() override { maxDoc = 0; }
  };

  class Weight final : public Query::Weight {
    MultiTermQuery& query;
    CachedFieldInfo* cachedFieldInfo = nullptr;
    float boost;
    bool canUseLazy;

  public:
    static inline bool disableLazyMultiTermForTests = false;
    // Bounds per-segment cursor memory for open ranges while keeping
    // ordinary prefix expansions on the windowed path.
    static constexpr size_t MAX_LAZY_TERMS = 4096;

    Weight(Context& context, MultiTermQuery& query, int32_t flags, float multiplier)
      : Query::Weight(context, flags), query(query),
        boost(constantWhenScored(flags, multiplier)),
        canUseLazy((flags & (NEED_SCORES | ALLOW_PRUNING))
                     == (NEED_SCORES | ALLOW_PRUNING)
                   && !disableLazyMultiTermForTests) {
      traits |= IS_CONSTANT_SCORING;  // every match scores the same
      cachedFieldInfo = context.getCachedFieldInfo(query.getField());
    }

  private:
    Query::Scorer* createScorerForMode(
        MemPool& targetPool, IndexReader::Segment& segment, bool useLazy) {
      if (cachedFieldInfo == nullptr) return nullptr;       // field absent everywhere
      auto* segFieldInfo = cachedFieldInfo->segInfos[segment.ord];
      if (segFieldInfo == nullptr) return nullptr;          // field absent in this segment

      // Fresh per-segment cursor because scorers can be built in parallel.
      auto* termsEnum = targetPool.make<TermsEnum>(targetPool, segment.postingsReader(), *segFieldInfo);
      FilteredTermsEnum* fenum = query.createFilteredEnum(targetPool, *termsEnum);

      int32_t maxDoc = segment.postingsReader().maxDoc();
      if (useLazy) {
        std::vector<TermsEnum::PostingsState> states;
        while (fenum->next()) {
          states.push_back(fenum->terms().postingsState());
          if (states.size() > MAX_LAZY_TERMS) {
            size_t nWords = FixedBitSet::sizeInWords(maxDoc);
            auto* words = (uint64_t*)targetPool.alloc(
                nWords * sizeof(uint64_t), alignof(uint64_t));
            memset(words, 0, nWords * sizeof(uint64_t));
            FixedBitSet bits(words, maxDoc);
            std::span<uint64_t> bitWords(words, nWords);

            for (const auto& state : states) {
              DocsEnum docsEnum(state);
              docsEnum.intoBitSet(bitWords, 0, maxDoc);
            }
            while (fenum->next()) {
              DocsEnum docsEnum(fenum->terms());
              docsEnum.intoBitSet(bitWords, 0, maxDoc);
            }
            return targetPool.make<MultiTermQuery::Scorer>(bits, maxDoc, boost);
          }
        }
        if (states.empty()) return nullptr;

        static_assert(std::is_trivially_destructible_v<DocsEnum>);
        auto* docsEnums = (DocsEnum*) targetPool.alloc(
            states.size() * sizeof(DocsEnum), alignof(DocsEnum));
        for (size_t i = 0; i < states.size(); i++) {
          new (&docsEnums[i]) DocsEnum(states[i]);
        }
        constexpr size_t windowWords = (size_t) DocsEnum::L1_DOCS / 64;
        auto windowBits = targetPool.make_span<uint64_t>(windowWords);
        return targetPool.make<MultiTermQuery::LazyScorer>(
            std::span(docsEnums, states.size()), windowBits, maxDoc, boost);
      }

      // Bitset words live in targetPool so the scorer stays trivially destructible.
      size_t nWords = FixedBitSet::sizeInWords(maxDoc);
      auto* words = (uint64_t*)targetPool.alloc(nWords * sizeof(uint64_t), alignof(uint64_t));
      memset(words, 0, nWords * sizeof(uint64_t));
      FixedBitSet bits(words, maxDoc);

      bool anyTerm = false;
      // Reclaim each term's DocsEnum allocations before scanning the next term.
      auto savepoint = targetPool.getSavePoint();
      while (fenum->next()) {
        anyTerm = true;
        {
          DocsEnum docsEnum(fenum->terms());
          for (int32_t doc = docsEnum.nextDoc(); doc != PostingsReader::END; doc = docsEnum.nextDoc()) {
            bits.set(doc);
          }
        }
        targetPool.rewind(savepoint);
      }

      if (!anyTerm) return nullptr;  // the field exists but no term matched
      return targetPool.make<MultiTermQuery::Scorer>(bits, maxDoc, boost);
    }

    class Supplier final : public Query::ScorerSupplier {
      Weight& weight;
      IndexReader::Segment& segment;

    public:
      Supplier(Weight& weight, IndexReader::Segment& segment)
        : weight(weight), segment(segment) {}

      int64_t cost() override { return segment.maxDoc(); }

      Query::Scorer* get(MemPool& targetPool, int64_t leadCost) override {
        bool selfDriven = leadCost == std::numeric_limits<int64_t>::max();
        return weight.createScorerForMode(
            targetPool, segment, weight.canUseLazy && selfDriven);
      }
    };

  public:
    Query::ScorerSupplier* scorerSupplier(
        MemPool& targetPool, IndexReader::Segment& segment) override {
      return targetPool.make<Supplier>(*this, segment);
    }

    Query::Scorer* createScorer(
        MemPool& targetPool, IndexReader::Segment& segment) override {
      return createScorerForMode(targetPool, segment, canUseLazy);
    }
  };
};

} // namespace solux
