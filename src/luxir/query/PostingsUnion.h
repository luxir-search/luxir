#pragma once

#include <algorithm>
#include <cstring>
#include <functional>
#include <span>

#include "Query.h"
#include "luxir/reader/DocsEnum.h"
#include "luxir/reader/TermsEnum.h"

namespace luxir {

// Windowed docs-only unions over a set of terms' postings, shared by
// multi-term query expansion (MultiTermQuery) and string-sort competitive
// candidate generation (FieldSortCollector).

// Shared skeleton of the windowed constant scorers: set-bit production out
// of one L1-sized doc window, refilled forward-only by the derived class.
// fillWindow(target) populates windowBits/windowStart/windowEnd and leaves
// nextWindowTarget at the next docid worth filling a window at (>= windowEnd,
// or >= maxDoc once the postings are exhausted).
template <class Derived>
class UnionWindowScorer : public Query::ConstantScorer {
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

  UnionWindowScorer(std::span<uint64_t> windowBits, int32_t maxDoc,
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
class UnionLazyScorer final : public UnionWindowScorer<UnionLazyScorer> {
  friend class UnionWindowScorer<UnionLazyScorer>;

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
  UnionLazyScorer(std::span<DocsOnlyEnum> docsEnums,
                  std::span<uint64_t> windowBits, int32_t maxDoc,
                  float constantScore)
    : UnionWindowScorer<UnionLazyScorer>(windowBits, maxDoc, constantScore),
      docsEnums(docsEnums) {}
};

// Heap-gated windowed union for large expansions: terms wait on a min-heap
// keyed by a lower bound on their next doc and are consulted only once the
// window frontier reaches them. Docs cursors are built on first activation,
// so a term whose postings start beyond the consumed docid prefix never
// constructs an enum or decodes a block, and a pulsed single-doc term never
// needs one at all.
//
// narrow() shrinks the set of live terms by ordinal without touching the
// heap: entries whose term ordinal has left the allowed interval are dropped
// when they reach the frontier. Bounds may only narrow.
class UnionHeapScorer final : public UnionWindowScorer<UnionHeapScorer> {
  friend class UnionWindowScorer<UnionHeapScorer>;

  std::span<const TermsEnum::PostingsState> states;
  std::span<DocsOnlyEnum*> enums;  // per-term cursor, built on activation
  std::span<uint64_t> heap;        // (next doc << 32) | state index
  size_t heapSize = 0;
  MemPool& enumPool;
  int64_t minAllowedOrd = std::numeric_limits<int64_t>::min();
  int64_t maxAllowedOrd = std::numeric_limits<int64_t>::max();

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
      if (state.termOrdinal < minAllowedOrd
          || state.termOrdinal > maxAllowedOrd) {
        // The term left the allowed interval; its entry dies at the frontier.
        continue;
      }
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
  UnionHeapScorer(std::span<const TermsEnum::PostingsState> states,
                  std::span<DocsOnlyEnum*> enums, std::span<uint64_t> heap,
                  std::span<uint64_t> windowBits, MemPool& enumPool,
                  int32_t maxDoc, float constantScore)
    : UnionWindowScorer<UnionHeapScorer>(windowBits, maxDoc, constantScore),
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

  // Bounds may only narrow; entries outside them are dropped lazily at the
  // window frontier. Returns false (and changes nothing) on a widening
  // attempt, which callers treat as "disable and fall back".
  bool narrow(int64_t minOrd, int64_t maxOrd) {
    assert(minOrd >= minAllowedOrd && maxOrd <= maxAllowedOrd);
    if (minOrd < minAllowedOrd || maxOrd > maxAllowedOrd) return false;
    minAllowedOrd = minOrd;
    maxAllowedOrd = maxOrd;
    return true;
  }

  // First candidate doc at or after target without consuming it: the union's
  // cursor is positioned there, so a following next()/advance() resumes
  // normally. END when no candidates remain.
  int32_t peekAdvance(int32_t target) {
    if (docid >= target) return docid;
    return seek(target);
  }

  // Membership test for a doc at or ahead of the cursor, consuming the
  // stream up to it. Docs must be probed in ascending order.
  bool contains(int32_t doc) {
    if (docid == PostingsReader::END) return false;
    if (docid < doc) seek(doc);
    return docid == doc;
  }
};

} // end namespace
