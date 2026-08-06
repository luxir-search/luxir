#pragma once

#include <algorithm>
#include <cstring>
#include <functional>
#include <limits>
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

  // Shared skeleton of the windowed constant scorers: set-bit production out
  // of one L1-sized doc window, refilled forward-only by the derived class.
  // fillWindow(target) populates windowBits/windowStart/windowEnd and leaves
  // nextWindowTarget at the next docid worth filling a window at (>= windowEnd,
  // or >= maxDoc once the postings are exhausted).
  template <class Derived>
  class WindowScorer : public Query::ConstantScorer {
  public:
    static constexpr int32_t WINDOW_SIZE = DocsEnumMeta::L1_DOCS;
    static constexpr int32_t WINDOW_WORDS = WINDOW_SIZE / 64;

  protected:
    std::span<uint64_t> windowBits;
    int32_t maxDoc;
    int32_t docid = -1;
    int32_t windowStart = -1;
    int32_t windowEnd = -1;
    int32_t nextWindowTarget = -1;

    WindowScorer(std::span<uint64_t> windowBits, int32_t maxDoc,
                 float constantScore)
      : Query::ConstantScorer(constantScore), windowBits(windowBits),
        maxDoc(maxDoc) {
      static_assert((WINDOW_SIZE % 64) == 0);
      assert(windowBits.size() == WINDOW_WORDS);
    }

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

    int32_t seek(int32_t target) {
      if (target >= maxDoc) {
        return docid = PostingsReader::END;
      }
      int32_t found = findInWindow(target);
      if (found == PostingsReader::END
          && target >= windowStart && target < windowEnd) {
        // The consumer can only hold a doc from a filled window, and every
        // fill leaves nextWindowTarget valid.
        target = nextWindowTarget;
      }
      if (target >= maxDoc) {
        return docid = PostingsReader::END;
      }
      while (found == PostingsReader::END) {
        static_cast<Derived*>(this)->fillWindow(target);
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

  // Retains a live docs cursor for every term and consults all of them each
  // window. Windows and conjunction-driven advances are monotone, so a postings
  // block is decoded at most once.
  class LazyScorer final : public WindowScorer<LazyScorer> {
    friend class WindowScorer<LazyScorer>;

    std::span<DocsOnlyEnum> docsEnums;

    void fillWindow(int32_t target) {
      windowStart = target;
      windowEnd = (int32_t) std::min<int64_t>(maxDoc, (int64_t) target + WINDOW_SIZE);
      nextWindowTarget = windowEnd;
      memset(windowBits.data(), 0, windowBits.size_bytes());

      int32_t firstDocAfterWindow = PostingsReader::END;
      for (auto& docsEnum : docsEnums) {
        int32_t doc = docsEnum.docId();
        if (doc < windowStart) {
          doc = docsEnum.advance(windowStart);
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

  public:
    LazyScorer(std::span<DocsOnlyEnum> docsEnums,
               std::span<uint64_t> windowBits, int32_t maxDoc, float constantScore)
      : WindowScorer<LazyScorer>(windowBits, maxDoc, constantScore),
        docsEnums(docsEnums) {}
  };

  // Heap-gated windowed union for large expansions: terms wait on a min-heap
  // keyed by a lower bound on their next doc and are consulted only once the
  // window frontier reaches them. Docs cursors are built on first activation,
  // so a term whose postings start beyond the consumed docid prefix never
  // constructs an enum or decodes a block, and a pulsed single-doc term never
  // needs one at all.
  class HeapScorer final : public WindowScorer<HeapScorer> {
    friend class WindowScorer<HeapScorer>;

    std::span<const TermsEnum::PostingsState> states;
    std::span<DocsOnlyEnum*> enums;  // per-term cursor, built on activation
    std::span<uint64_t> heap;        // (next doc << 32) | state index
    size_t heapSize = 0;
    MemPool& enumPool;

    static uint64_t entry(int32_t doc, size_t idx) {
      assert(doc >= 0);
      return ((uint64_t) (uint32_t) doc << 32) | (uint32_t) idx;
    }
    static int32_t entryDoc(uint64_t e) { return (int32_t) (e >> 32); }
    static size_t entryIdx(uint64_t e) { return (uint32_t) e; }

    void push(uint64_t e) {
      heap[heapSize++] = e;
      std::push_heap(heap.begin(), heap.begin() + (ptrdiff_t) heapSize,
                     std::greater<uint64_t>());
    }

    uint64_t popTop() {
      std::pop_heap(heap.begin(), heap.begin() + (ptrdiff_t) heapSize,
                    std::greater<uint64_t>());
      return heap[--heapSize];
    }

    void setWindowBit(int32_t doc) {
      assert(doc >= windowStart && doc < windowEnd);
      const uint32_t index = (uint32_t) (doc - windowStart);
      windowBits[index >> 6] |= 1ULL << (index & 63);
    }

    void fillWindow(int32_t target) {
      windowStart = target;
      windowEnd = (int32_t) std::min<int64_t>(maxDoc, (int64_t) target + WINDOW_SIZE);
      memset(windowBits.data(), 0, windowBits.size_bytes());

      while (heapSize != 0 && entryDoc(heap[0]) < windowEnd) {
        const uint64_t top = popTop();
        const size_t idx = entryIdx(top);
        const auto& state = states[idx];
        if (state.docsEnd == state.docsStart) {
          // Pulsed single-doc term: the key is the doc. A key behind the
          // window was strided past by an advance and is dead.
          const int32_t doc = state.pulsedDoc;
          if (doc >= windowStart) {
            setWindowBit(doc);
          }
          continue;
        }
        DocsOnlyEnum* docsEnum = enums[idx];
        int32_t doc;
        if (docsEnum == nullptr) {
          docsEnum = enums[idx] = enumPool.make<DocsOnlyEnum>(state);
          doc = docsEnum->nextDoc();
          assert(doc >= entryDoc(top));  // the key is a lower bound
        } else {
          doc = docsEnum->docId();       // parked exactly on its next doc
        }
        if (doc < windowStart) {
          doc = docsEnum->advance(windowStart);
          if (doc == PostingsReader::END) continue;
        }
        if (doc >= windowEnd) {
          push(entry(doc, idx));
          continue;
        }
        docsEnum->intoBitSet(windowBits, windowStart, windowEnd);
        // intoBitSet leaves the cursor on its last doc below windowEnd with
        // the successor already buffered (or exhausted); park on the
        // successor so the next pop resumes with the current doc unemitted.
        if (docsEnum->docId() == PostingsReader::END) continue;
        doc = docsEnum->nextDoc();
        if (doc == PostingsReader::END) continue;
        push(entry(doc, idx));
      }

      assert(heapSize == 0 || entryDoc(heap[0]) >= windowEnd);
      nextWindowTarget = heapSize == 0 ? maxDoc : entryDoc(heap[0]);
    }

  public:
    HeapScorer(std::span<const TermsEnum::PostingsState> states,
               std::span<DocsOnlyEnum*> enums, std::span<uint64_t> heap,
               std::span<uint64_t> windowBits, MemPool& enumPool,
               int32_t maxDoc, float constantScore)
      : WindowScorer<HeapScorer>(windowBits, maxDoc, constantScore),
        states(states), enums(enums), heap(heap), enumPool(enumPool) {
      assert(!states.empty());
      assert(states.size() == enums.size());
      assert(states.size() == heap.size());
      heapSize = states.size();
      for (size_t i = 0; i < states.size(); i++) {
        heap[i] = entry(DocsOnlyEnum::firstDocLowerBound(states[i]), i);
      }
      std::make_heap(heap.begin(), heap.end(), std::greater<uint64_t>());
    }
  };

  class Weight final : public Query::Weight {
    MultiTermQuery& query;
    CachedFieldInfo* cachedFieldInfo = nullptr;
    float boost;
    bool canUseLazy;

  public:
    // Test/bench force switch over the constant-score union scorers. AUTO is
    // the production policy; the forced modes bind only where the lazy
    // preconditions hold (scored + pruning + self-driven), everything else
    // stays eager.
    enum class ScorerMode : uint8_t { AUTO, FORCE_EAGER, FORCE_WINDOWED, FORCE_HEAP };
    static inline ScorerMode scorerModeForTests = ScorerMode::AUTO;

    // Past this many terms AUTO abandons the all-live-cursors windowed scorer
    // for the heap scorer, whose per-term cost is one retained state instead
    // of a full cursor.
    static constexpr size_t MAX_LAZY_TERMS = 4096;
    // Retained-state budget for the heap scorer; expansions past it (term
    // ranges approaching the whole dictionary) fall back to the eager union.
    // Initial value, not yet measured against a real budget tradeoff.
    // Non-const so tests can pin the spill path without a quarter-million
    // term index.
    static inline size_t maxLazyStateBytes = 32u << 20;

    Weight(Context& context, MultiTermQuery& query, int32_t flags, float multiplier)
      : Query::Weight(context, flags), query(query),
        boost(constantWhenScored(flags, multiplier)),
        canUseLazy((flags & (NEED_SCORES | ALLOW_PRUNING))
                     == (NEED_SCORES | ALLOW_PRUNING)
                   && scorerModeForTests != ScorerMode::FORCE_EAGER) {
      traits |= IS_CONSTANT_SCORING;  // every match scores the same
      cachedFieldInfo = context.getCachedFieldInfo(query.getField());
    }

  private:
    // Retained-state budget exceeded: union the collected states plus the
    // rest of the term stream into a full bitset.
    Query::Scorer* eagerRemainder(
        MemPool& targetPool, FilteredTermsEnum& fenum,
        const std::vector<TermsEnum::PostingsState>& states, int32_t maxDoc) {
      size_t nWords = FixedBitSet::sizeInWords(maxDoc);
      auto* words = (uint64_t*)targetPool.alloc(
          nWords * sizeof(uint64_t), alignof(uint64_t));
      memset(words, 0, nWords * sizeof(uint64_t));
      FixedBitSet bits(words, maxDoc);
      std::span<uint64_t> bitWords(words, nWords);

      for (const auto& state : states) {
        DocsOnlyEnum docsEnum(state);
        docsEnum.intoBitSet(bitWords, 0, maxDoc);
      }
      while (fenum.next()) {
        DocsOnlyEnum docsEnum(fenum.terms());
        docsEnum.intoBitSet(bitWords, 0, maxDoc);
      }
      return targetPool.make<MultiTermQuery::Scorer>(bits, maxDoc, boost);
    }

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
        const ScorerMode mode = scorerModeForTests;
        // AUTO bounds retained states by bytes; the forced modes are test
        // hooks and unbounded so the A/B matrix can probe past the budget.
        const size_t maxStates = mode == ScorerMode::AUTO
            ? maxLazyStateBytes / sizeof(TermsEnum::PostingsState)
            : std::numeric_limits<size_t>::max();
        std::vector<TermsEnum::PostingsState> states;
        while (fenum->next()) {
          states.push_back(fenum->terms().postingsState());
          if (states.size() > maxStates) {
            return eagerRemainder(targetPool, *fenum, states, maxDoc);
          }
        }
        if (states.empty()) return nullptr;

        if (mode == ScorerMode::FORCE_HEAP
            || (mode == ScorerMode::AUTO && states.size() > MAX_LAZY_TERMS)) {
          auto statesArr = targetPool.make_span<TermsEnum::PostingsState>(states.size());
          std::copy(states.begin(), states.end(), statesArr.begin());
          auto enums = targetPool.make_span<DocsOnlyEnum*>(states.size());
          std::fill(enums.begin(), enums.end(), nullptr);
          auto heap = targetPool.make_span<uint64_t>(states.size());
          auto windowBits =
              targetPool.make_span<uint64_t>((size_t) HeapScorer::WINDOW_WORDS);
          return targetPool.make<MultiTermQuery::HeapScorer>(
              std::span<const TermsEnum::PostingsState>(statesArr), enums, heap,
              windowBits, targetPool, maxDoc, boost);
        }

        static_assert(std::is_trivially_destructible_v<DocsOnlyEnum>);
        auto* docsEnums = (DocsOnlyEnum*) targetPool.alloc(
            states.size() * sizeof(DocsOnlyEnum), alignof(DocsOnlyEnum));
        for (size_t i = 0; i < states.size(); i++) {
          new (&docsEnums[i]) DocsOnlyEnum(states[i]);
        }
        auto windowBits =
            targetPool.make_span<uint64_t>((size_t) LazyScorer::WINDOW_WORDS);
        return targetPool.make<MultiTermQuery::LazyScorer>(
            std::span(docsEnums, states.size()), windowBits, maxDoc, boost);
      }

      // Bitset words live in targetPool so the scorer stays trivially destructible.
      size_t nWords = FixedBitSet::sizeInWords(maxDoc);
      auto* words = (uint64_t*)targetPool.alloc(nWords * sizeof(uint64_t), alignof(uint64_t));
      memset(words, 0, nWords * sizeof(uint64_t));
      FixedBitSet bits(words, maxDoc);

      bool anyTerm = false;
      // Reclaim each term's postings-enum allocations before scanning the next term.
      auto savepoint = targetPool.getSavePoint();
      while (fenum->next()) {
        anyTerm = true;
        {
          DocsOnlyEnum docsEnum(fenum->terms());
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
