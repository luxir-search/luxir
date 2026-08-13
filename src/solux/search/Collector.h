#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>

#include "solux/query/Query.h"
#include "solux/search/DocSet.h"

namespace solux {

SOLUX_UNALIGNED_START
class segdoc {
  // docid must come first in little-endian for it to make up the low bytes of the int64_t
  int32_t docid;
  int32_t seg;
public:
  segdoc() {}
  segdoc(int32_t segment, int32_t docid) : docid(docid), seg(segment) {}

  int32_t segment() const {
    return seg;
  }

  int32_t docId() const {
    return docid;
  }

  auto operator<=>(const segdoc &other) const {
    return std::bit_cast<int64_t>(*this) <=> std::bit_cast<int64_t>(other);
  }

  bool operator==(const segdoc &other) const {
    return std::bit_cast<int64_t>(*this) == std::bit_cast<int64_t>(other);
  }

  // return negative if this < other, positive if this > other, 0 if equal
  int64_t compare(const segdoc &other) const {
    return std::bit_cast<int64_t>(*this) - std::bit_cast<int64_t>(other);
  }

} SOLUX_UNALIGNED_END;




class TopDocsCollector {
  public:

  // Having this packed helped both memory and CPU (presumably better cache hits?)
  SOLUX_UNALIGNED_START
  struct ScoreDoc {
    float score;
    segdoc doc;
  } SOLUX_UNALIGNED_END;


  int64_t hitCount = 0;

  float minCompetitiveVal = std::numeric_limits<float>::lowest();

  constexpr static auto scoreComp = [](const ScoreDoc &a, const ScoreDoc &b) {
    return a.score > b.score;
  };

  constexpr static auto scoreAndDocComp = [](const ScoreDoc &a, const ScoreDoc &b) {
    return a.score == b.score ? (a.doc < b.doc) : (a.score > b.score);
  };

  int64_t topCount;
  // Heap orders by (score, then doc) so ties at the k-th score are broken by (seg, docid):
  // the kept top-K is a deterministic total order, independent of collection/merge order
  // (and, once it exists, slicing).  Required for sliced==unsliced to hold at exact ties.
  // The storage expands on demand: topCount bounds the heap but is not allocated up
  // front, which matters for deep or unbounded limits (limit=-1 maps to maxDoc) and for
  // the per-slice collectors that never see topCount hits.
  ExpandingPQ<ScoreDoc, decltype(scoreAndDocComp)> pq;

  TopDocsCollector(int64_t topCount)
      : topCount(topCount), pq((size_t)std::max(topCount, (int64_t)0)) {
    // topCount == 0 is valid ("count/aggregate only, no docs", e.g. limit 0): the heap is
    // empty and collect() only counts.  Negative counts are a bug in the caller.
    assert(topCount >= 0);
  }

  void collect(int32_t segment, int32_t docid, float score) {
    hitCount++;

    // topCount == 0: keep no docs, just count hits (the heap has zero capacity, so any
    // insert/top() would be out of bounds).  hitCount above still yields an accurate
    // total, so get_number and sub-op domains are unaffected.
    if (topCount == 0) {
      return;
    }

    // Admit anything that can match OR beat the k-th best score (>=, not >): a doc whose
    // score ties the k-th score is still competitive if its (seg, docid) sorts ahead of the
    // current worst tied doc.  insertWithOverflow resolves that via the (score, then doc)
    // heap comparator, so a losing tie is a cheap no-op insert and a winning tie evicts the
    // worst.  This is what makes the kept top-K order-independent (collection, merge, slice).
    if (score >= minCompetitiveVal) {
      pq.insertWithOverflow({score, segdoc(segment, docid)});
      // Once the heap holds topCount docs, its root (a min-heap on score) is the k-th best
      // so far - the competitive threshold a later doc must beat to enter.  Publish it as
      // soon as the heap fills, not only when a doc evicts one: best-docs-first / index-
      // sorted / clustered input never evicts, so the old "update on overflow only" left the
      // threshold at lowest() forever and defeated impact pruning exactly when it helps most.
      if (pq.size() >= topCount) {
        minCompetitiveVal = pq.top().score;
      }
    }
  }

  // The heap floor stays inclusive because an equal-scoring doc with a
  // smaller total-order key can replace its root. For a monotone segment
  // stream, once its next key is past the root, equal scores cannot enter and
  // the scorer may use the next representable float as an exclusive floor.
  float minCompetitiveScoreForNextDoc(int32_t segment, int32_t docid) const {
    if (topCount == 0 || pq.size() < topCount) {
      return std::numeric_limits<float>::lowest();
    }
    const ScoreDoc& floor = pq.top();
    if (segdoc(segment, docid) > floor.doc) {
      return std::nextafter(floor.score, std::numeric_limits<float>::infinity());
    }
    return floor.score;
  }

  int64_t totalHits() const {
    return hitCount;
  }

  int64_t size() const {
    return pq.size();
  }

  // merge other into this.
  void merge(TopDocsCollector& other) {
    // Self-merge would iterate a span that collect() can reallocate out from under us.
    assert(this != &other);
    // In some scenarios, popping the top of the other heap until it's no longer competitive will be faster,
    // while in other scenarios just a linear scan of the other heap will be faster.
    // We'll just do a linear scan for now.
    auto newHitCount = hitCount + other.hitCount;
    std::span<ScoreDoc> otherDocs = other.pq.span();
    // Since min-heap has smallest element at position 0, it should be more efficient to start from the other end.
    // Future possible optimization: if we do a whole level of a min-tree without any insertions, we could stop early.
    for (int64_t i = (int64_t)otherDocs.size() - 1; i >= 0; i--) {
      collect(otherDocs[i].doc.segment(), otherDocs[i].doc.docId(), otherDocs[i].score);
    }
    hitCount = newHitCount;
  }

  // Since we use min-heap comparators in our priority queues, the list will be reverse-sorted (smallest last)
  // Repeated calls to pop() on the priority queue will also return the docs in order.
  std::span<ScoreDoc> sort() {
    // sort_heap must use the same comparator the heap was built with (scoreAndDocComp),
    // which also gives the stable response order: score desc, then (seg, docid) asc at ties.
    std::span<ScoreDoc> docs = pq.span();
    std::sort_heap(docs.begin(), docs.end(), scoreAndDocComp);
    return docs;
  }

  std::span<ScoreDoc> scoreDocs() {
    return pq.span();
  }
};


// A different way to get top-k results: Collect results in an array until we have 2k of them.
// The find the median (kth value), and drop all values less than the median. Repeat collection more results.
// This has the advantage of not having the bad worst-case behavior of a priority queue and collecting something
// that is already mostly sorted.  It is also more linear and doesn't degrade as much as k increases.
// The obvious downside is double the memory usage... we would want a different strategy if someone wants the
// whole index sorted for example.
// Also, since minCompetitiveVal is only updated every k docs, this is perhaps more compatible with
// multi-threaded collection (i.e. check/update a global minCompetitiveVal at the same time you calculate the
// new minCometitiveVal from the median.)  It also may just be less important to update this minCompetitiveVal
// in any case.
//
// I don't know who created this algorithm, but I learned about it from a blog post by Paul Masurel:
// https://quickwit.io/blog/top-k-complexity
//
// Performance: see the benchmarks in CollectorBM.cpp
//   For random order, this was slower across the board.  I think perhaps because there are more swaps.
//   For more complex sorts, we're going to need indirection anyway, so that may change things.
//   The tricky part of implementing indirection for this would be reusing the slots after you have
//   dropped half of them.  Seems worse for cache locality.
//   Update: I tried indirection (see TopScoreCollectorI) and it did improve performance.
class TopScoreCollector {
public:
  using ScoreDoc = TopDocsCollector::ScoreDoc;

  // use the same reversed comparator as the priority-queue based collector so larger values are first
  // and we can drop values less than the median by simply resizing the vector.
  constexpr static auto scoreComp = TopDocsCollector::scoreComp;


  int64_t hitCount = 0;
  int64_t k;  // number of top docs we want to find
  std::vector<ScoreDoc> topDocs;
  float minCompetitiveVal = std::numeric_limits<float>::lowest();

  TopScoreCollector(int64_t topCount) : k(topCount) {
    assert(topCount > 0);
    // topDocs.reserve(topCount*2);  // reserve or not?
  }

  void dropHalf() {
    // iterator at the kth element is at index k-1.
    std::nth_element(topDocs.begin(), topDocs.begin()+k-1, topDocs.end(), scoreComp);
    minCompetitiveVal = topDocs[k-1].score;  // kth value is at index k-1
    topDocs.resize(k);
  }

  void collect(int32_t segment, int32_t docid, float score) {
    hitCount++;
    if (score > minCompetitiveVal) {
      topDocs.push_back({score, segdoc(segment, docid)});
      if (topDocs.size() >= uint64_t(k<<1)) {
        dropHalf();
      }
    }
  }

  int64_t totalHits() const {
    return hitCount;
  }

  int64_t size() const {
    return topDocs.size();
  }

  void sort() {
    if (topDocs.size() > uint64_t(k)) {
      dropHalf();
    }
    std::sort(topDocs.begin(), topDocs.end(), scoreComp);
  }

  // merge other into this.
  void merge(TopDocsCollector& other) {
    auto newHitCount = hitCount + other.hitCount;
    // The generic way...  Could probably be optimized.
    // 1) if other.minCompetitiveVal is greater than our minCompetitiveVal, we can just append all of other's docs.
    // 2) we could skip the comparisons anyway if there is space?
    for (auto& sd: other.scoreDocs()) {
      collect(sd.doc.segment(), sd.doc.docId(), sd.score);
    }
    hitCount = newHitCount;
  }


};

class TopScoreCollectorI {  // I for "indirect"
public:
  using index_t = int16_t;
  using ScoreDoc = TopDocsCollector::ScoreDoc;

  int64_t hitCount = 0;
  int64_t k;  // number of top docs we want to find
  std::vector<ScoreDoc> topDocs;
  std::vector<index_t> indexes;
  int64_t held = 0;  // number of valid indexes currently held.  We don't downside indexes.
  float minCompetitiveVal = std::numeric_limits<float>::lowest();


  TopScoreCollectorI(int64_t topCount) : k(topCount) {
    assert(topCount > 0);
    // assign slot indexes to start off with, just to see what the performance would be.
    // downside is that this uses max memory to start, even if no hits.
    topDocs.resize(topCount*2);
    indexes.reserve(topCount*2);
    for (int i = 0; i < topCount*2; i++) {
      indexes.push_back(i);
    }
  }

  void dropHalf() {
    auto* arr = &topDocs[0];  // does this help not direct through this for every comparison?
    auto scoreCompI = [arr](index_t a, const index_t b) {
      return arr[a].score > arr[b].score;
    };
    // iterator at the kth element is at index k-1.
    std::nth_element(indexes.begin(), indexes.begin()+k-1, indexes.end(), scoreCompI);
    minCompetitiveVal = arr[indexes[k-1]].score;  // kth value is at index k-1
    held = k;  // drops half
  }

  void collect(int32_t segment, int32_t docid, float score) {
    hitCount++;
    if (score > minCompetitiveVal) {
      topDocs[indexes[held++]] = {score, segdoc(segment, docid)};
      if (held >= (k<<1)) {
        dropHalf();
      }
    }
  }

  int64_t totalHits() const {
    return hitCount;
  }

  int64_t size() const {
    return held;
  }

  void sort() {
    if (held > k) {
      dropHalf();
    }
    auto* arr = &topDocs[0];  // does this help not direct through this for every comparison?
    auto scoreCompI = [arr](index_t a, const index_t b) {
      return arr[a].score > arr[b].score;
    };
    std::sort(indexes.begin(), indexes.end(), scoreCompI);
  }

  // merge other into this.
  void merge(TopDocsCollector& other) {
    unused(other);
    /*
     * not implemented yet.
     */
  }


};

class MaxScoreAccumulator {
public:
  std::atomic<float> maxScore;

  MaxScoreAccumulator() : maxScore(std::numeric_limits<float>::lowest()) {
  }

  void accumulate(float score) {
    float cur = maxScore.load(std::memory_order_acquire);
    while (score > cur
           && !maxScore.compare_exchange_weak(
             cur, score, std::memory_order_acq_rel, std::memory_order_acquire)) {
    }
  }

  float get() const {
    return maxScore.load(std::memory_order_acquire);
  }
};

// Drive a per-segment Scorer through the optional `filter` domain, feeding (doc, score)
// into `collector`.  If `builder` is non-null, also records every matched doc for use as
// a sub-op domain (see TopDocsReq's per-segment subCalc dispatch).  Templated on Collector
// so both score-only and field-sort collectors can use the same loop, and so FusionOp can
// reuse it for per-source evaluation without dragging TopDocsReq's MergeableCollector along.
//
// Precondition: caller is responsible for any per-segment setup on `collector`.  In
// particular, FieldSortCollector requires `setSegment(segnum, &postingsReader)` to be
// called before this; TopDocsCollector has no per-segment setup.
// allowPruning: when false (e.g. the request asks for an exact total hit count via
// get_number), the rising threshold is NOT pushed to the scorer, so impact block
// skipping stays off and every matching doc is visited.  Dynamic pruning and an exact
// total count are mutually exclusive - skipping does not visit (cannot count) the docs
// it skips - so a query that needs the count must forgo pruning.
template <typename Collector>
void collectTopK(int32_t segnum, Query::Scorer* scorer, DocSet* filter,
                 DocSetBuilder* builder, Collector& collector, bool allowPruning = true,
                 MaxScoreAccumulator* accumulator = nullptr) {
  constexpr int32_t kAccumulatorPollPeriod = 1024;
  float lastPushedMinCompetitiveScore = std::numeric_limits<float>::lowest();
  int32_t accumulatorPollCount = 0;
  auto localMinCompetitiveScore = [&](int32_t nextDoc) {
    if constexpr (requires {
        collector.minCompetitiveScoreForNextDoc(segnum, nextDoc);
      }) {
      return collector.minCompetitiveScoreForNextDoc(segnum, nextDoc);
    } else if constexpr (requires { collector.minCompetitiveVal; }) {
      unused(nextDoc);
      return collector.minCompetitiveVal;
    } else {
      unused(nextDoc);
      return std::numeric_limits<float>::lowest();
    }
  };
  auto pushMinCompetitiveScore = [&](float localScore, bool localRise,
                                     bool periodicPoll) {
    if constexpr (requires { collector.minCompetitiveVal; }) {
      if (!allowPruning || builder != nullptr) {
        return;
      }
      float minCompetitiveScore = localScore;
      if (accumulator != nullptr) {
        if (localRise) {
          // Cross-segment publication stays inclusive: a sibling stream can
          // still contain an equal-scoring doc with a smaller total-order key.
          accumulator->accumulate(collector.minCompetitiveVal);
        }
        if (localRise || periodicPoll) {
          minCompetitiveScore = std::max(minCompetitiveScore, accumulator->get());
        }
      }
      if (minCompetitiveScore > lastPushedMinCompetitiveScore) {
        scorer->setMinCompetitiveScore(minCompetitiveScore);
        lastPushedMinCompetitiveScore = minCompetitiveScore;
      }
    } else {
      unused(localScore, localRise, periodicPoll, lastPushedMinCompetitiveScore,
             allowPruning, accumulator);
    }
  };
  auto collectOne = [&](int32_t doc, float score) {
    if constexpr (requires { collector.minCompetitiveVal; }) {
      float oldMinCompetitiveVal = collector.minCompetitiveVal;
      collector.collect(segnum, doc, score);
      bool localRise = collector.minCompetitiveVal > oldMinCompetitiveVal;
      bool periodicPoll = false;
      if (allowPruning && builder == nullptr && accumulator != nullptr) {
        accumulatorPollCount++;
        if (accumulatorPollCount >= kAccumulatorPollPeriod) {
          accumulatorPollCount = 0;
          periodicPoll = true;
        }
      }
      pushMinCompetitiveScore(localMinCompetitiveScore(doc + 1), localRise,
                              periodicPoll);
    } else {
      unused(lastPushedMinCompetitiveScore, accumulatorPollCount, accumulator, allowPruning);
      collector.collect(segnum, doc, score);
    }
  };

  // Seed from both the local heap and the shared inclusive threshold so a
  // reused collector or a sibling segment can prune from the first doc.
  if constexpr (requires { collector.minCompetitiveVal; }) {
    if (allowPruning && builder == nullptr) {
      float seed = localMinCompetitiveScore(0);
      if (accumulator != nullptr) {
        seed = std::max(seed, accumulator->get());
      }
      if (seed > lastPushedMinCompetitiveScore) {
        scorer->setMinCompetitiveScore(seed);
        lastPushedMinCompetitiveScore = seed;
      }
    }
  }

  // Count-only collection (topCount == 0) never reads a score back out of the
  // collector, and under a request without NEED_SCORES the scorers may not
  // even be able to produce one - so score() must not be called at all.
  bool needScores = true;
  if constexpr (requires { collector.topCount; }) {
    needScores = collector.topCount > 0;
  }
  if constexpr (requires { collector.needsScores; }) {
    needScores = needScores && collector.needsScores;
  }

  // Field-sort competitive pruning: jump the scorer over doc blocks whose sort
  // key bounds prove them noncompetitive. Skipped docs are uncounted, so the
  // caller must not need an exact hit count or domain when pruning.
  [[maybe_unused]] int32_t competitiveEnd = 0;
  constexpr bool hasSortRanges =
      requires { collector.nextCompetitiveRange(segnum, (int32_t)0); };
  [[maybe_unused]] bool sortPrune = false;
  if constexpr (hasSortRanges) {
    sortPrune = allowPruning && builder == nullptr;
  }

  if (filter == nullptr || filter->type == DocSet::BITSET) {
    BitDocSet* bitDocs = (BitDocSet*)filter;
    auto* domainBits = bitDocs ? &bitDocs->bits() : nullptr;
    for (;;) {
      auto doc = scorer->next();
      if (doc == PostingsReader::END) {
        break;
      }
      if constexpr (hasSortRanges) {
        while (sortPrune && doc >= competitiveEnd) {
          auto range = collector.nextCompetitiveRange(segnum, doc);
          competitiveEnd = range.end;
          if (range.begin <= doc) break;
          doc = scorer->advance(range.begin);
          if (doc == PostingsReader::END) break;
        }
        if (doc == PostingsReader::END) break;
      }
      if (domainBits && !domainBits->get(doc)) {
        continue;
      }
      if (builder) {
        builder->add(doc);
      }
      auto score = needScores ? scorer->score() : 0.0f;
      collectOne(doc, score);
    }
  } else {
    assert(filter->type == DocSet::ARRAY);
    ArrDocSet* arrDocs = (ArrDocSet*)filter;
    std::span<const int32_t> arr = arrDocs->docs();
    const int32_t* cur = arr.data();
    const int32_t* arrEnd = cur + arr.size();
    while (cur != arrEnd) {
      int32_t doc = *cur;
      if constexpr (hasSortRanges) {
        if (sortPrune && doc >= competitiveEnd) {
          auto range = collector.nextCompetitiveRange(segnum, doc);
          competitiveEnd = range.end;
          if (range.begin > doc) {
            cur = screaming::gallopLowerBound(cur, arrEnd, range.begin);
            continue;
          }
        }
      }
      cur++;
      if (scorer->docId() < doc) {
        scorer->advance(doc);
      }
      if (scorer->docId() != doc) {
        continue;
      }
      if (builder) {
        builder->add(doc);
      }
      auto score = needScores ? scorer->score() : 0.0f;
      collectOne(doc, score);
    }
  }
}

inline int64_t collectFirstKConstant(int32_t segnum, Query::Scorer* scorer,
                                     DocSet* filter, TopDocsCollector& collector,
                                     int64_t topCount) {
  assert(scorer != nullptr);
  assert(topCount > 0);
  int64_t collected = 0;
  auto collectOne = [&](int32_t doc) {
    collector.collect(segnum, doc, scorer->score());
    collected++;
  };

  if (filter == nullptr || filter->type == DocSet::BITSET) {
    BitDocSet* bitDocs = (BitDocSet*) filter;
    auto* domainBits = bitDocs == nullptr ? nullptr : &bitDocs->bits();
    while (collected < topCount) {
      int32_t doc = scorer->next();
      if (doc == PostingsReader::END) {
        break;
      }
      if (domainBits != nullptr && !domainBits->get(doc)) {
        continue;
      }
      collectOne(doc);
    }
  } else {
    for (int32_t doc : ((ArrDocSet*) filter)->docs()) {
      if (scorer->docId() < doc) {
        scorer->advance(doc);
      }
      if (scorer->docId() != doc) {
        continue;
      }
      collectOne(doc);
      if (collected >= topCount) {
        break;
      }
    }
  }
  return collected;
}

inline void collectConstantTopKAndDomain(
    int32_t segnum, Query::Scorer* scorer, DocSet* filter,
    DocSetBuilder& builder, TopDocsCollector& collector) {
  assert(scorer != nullptr);
  assert(collector.topCount > 0);
  skipCount(SkipStats::constantPullDomainCollections);
  int64_t collected = 0;
  auto collectOne = [&](int32_t doc) {
    builder.add(doc);
    if (collected < collector.topCount) {
      collector.collect(segnum, doc, scorer->score());
    } else {
      collector.hitCount++;
    }
    collected++;
  };

  if (filter == nullptr || filter->type == DocSet::BITSET) {
    BitDocSet* bitDocs = (BitDocSet*) filter;
    auto* domainBits = bitDocs == nullptr ? nullptr : &bitDocs->bits();
    for (;;) {
      int32_t doc = scorer->next();
      if (doc == PostingsReader::END) {
        break;
      }
      if (domainBits != nullptr && !domainBits->get(doc)) {
        continue;
      }
      collectOne(doc);
    }
  } else {
    assert(filter->type == DocSet::ARRAY);
    for (int32_t doc : ((ArrDocSet*) filter)->docs()) {
      if (scorer->docId() < doc) {
        scorer->advance(doc);
      }
      if (scorer->docId() != doc) {
        continue;
      }
      collectOne(doc);
    }
  }
}

// Exhaust the windowed bulk path, counting matches and optionally building the
// complete domain. No docs or scores are otherwise materialized.
inline int64_t countMatchesWindowed(BulkScorer* bulk, DocSet* filter,
                                    DocSetBuilder* builder, int32_t maxDoc) {
  assert(bulk != nullptr);
  int64_t count = 0;
  int32_t cursor = 0;
  while (cursor != PostingsReader::END && cursor < maxDoc) {
    int32_t next = bulk->countNextWindow(count, builder, filter, cursor, maxDoc);
    if (next == PostingsReader::END) {
      break;
    }
    assert(next > cursor);
    cursor = next;
  }
  assert(builder == nullptr || count == builder->card());
  return count;
}

inline void collectCountWindowed(BulkScorer* bulk, DocSet* filter,
                                 DocSetBuilder* builder,
                                 TopDocsCollector& collector, int32_t maxDoc) {
  assert(bulk != nullptr);
  assert(collector.topCount == 0);
  collector.hitCount += countMatchesWindowed(bulk, filter, builder, maxDoc);
}

enum class ConstantScoreDrain : uint8_t {
  LIMIT_ONLY,
  COMPLETE,
};

// Constant scores rank by doc order, so a segment's top K docs are its first
// K matches. LIMIT_ONLY stops as soon as K are captured. COMPLETE continues
// with count windows to satisfy an exact-count or domain consumer. One bulk
// arrangement then serves ranking, count, and domain; an independent capture
// scorer would rebuild every clause (a multiterm clause re-runs its dictionary
// scan per build). Score windows carry the weight's constant, so reported
// scores match what a pull scorer from the same weight returns.
inline void collectFirstKConstantWindowed(
    int32_t segnum, BulkScorer* bulk, DocSet* filter, DocSetBuilder* builder,
    TopDocsCollector& collector, int32_t maxDoc,
    ConstantScoreDrain drain) {
  assert(bulk != nullptr);
  assert(collector.topCount > 0);
  assert(drain == ConstantScoreDrain::COMPLETE || builder == nullptr);
  skipCount(SkipStats::constantWindowCaptures);
  bulk->setTopKDepth((int32_t) collector.topCount, false);
  int64_t collected = 0;
  int64_t overshoot = 0;
  int32_t cursor = 0;
  ScoreWindow window;
  while (cursor != PostingsReader::END && cursor < maxDoc
         && collected < collector.topCount) {
    int32_t next = bulk->scoreNextWindow(window, filter, cursor, maxDoc,
                                         std::numeric_limits<float>::lowest());
    if (builder != nullptr) {
      skipCount(SkipStats::bulkDomainWindowsFed);
    }
    for (int32_t i = 0; i < window.size; i++) {
      int32_t doc = window.docs[(size_t) i];
      if (builder != nullptr) {
        builder->add(doc);
      }
      if (collected < collector.topCount) {
        collector.collect(segnum, doc, window.scores[(size_t) i]);
        collected++;
      } else if (drain == ConstantScoreDrain::COMPLETE) {
        // The capture window ran past K; these are count-only.
        collector.hitCount++;
        overshoot++;
      } else {
        break;
      }
    }
    if (next == PostingsReader::END) {
      cursor = next;
      break;
    }
    assert(next > cursor);
    cursor = next;
  }
  if (drain == ConstantScoreDrain::LIMIT_ONLY) {
    return;
  }
  int64_t count = 0;
  while (cursor != PostingsReader::END && cursor < maxDoc) {
    int32_t next = bulk->countNextWindow(count, builder, filter, cursor, maxDoc);
    if (next == PostingsReader::END) {
      break;
    }
    assert(next > cursor);
    cursor = next;
  }
  collector.hitCount += count;
  assert(builder == nullptr
         || builder->card() == collected + overshoot + count);
  unused(overshoot);
}

// Consume every match window of [begin, end): produce, feed the domain
// builder and the collector, stop at the bound. END from a bounded request
// only means the bound was reached (BulkScorer::matchNextWindow contract),
// so callers may keep issuing later ranges afterwards. Returns the number
// of docs emitted to the collector.
template <typename Collector>
int64_t feedMatchWindows(int32_t segnum, BulkScorer* bulk, DocSet* filter,
                         DocSetBuilder* builder, Collector& collector,
                         int32_t begin, int32_t end) {
  ScoreWindow window;
  int32_t cursor = begin;
  int64_t emitted = 0;
  while (cursor < end) {
    int32_t next = bulk->matchNextWindow(window, filter, cursor, end);
    emitted += window.size;
    if (builder != nullptr) {
      for (int32_t i = 0; i < window.size; i++) {
        builder->add(window.docs[(size_t) i]);
      }
    }
    if constexpr (requires(Collector& c, std::span<const int32_t> docs) {
        c.collectWindow(int32_t{}, docs);
      }) {
      collector.collectWindow(
          segnum, window.docs.first((size_t)window.size));
    } else {
      for (int32_t i = 0; i < window.size; i++) {
        collector.collect(segnum, window.docs[(size_t)i], 0.0f);
      }
    }
    if (next == PostingsReader::END) {
      break;
    }
    assert(next > cursor);
    cursor = next;
  }
  return emitted;
}

// Collect match-only windows in doc order. Scores are intentionally absent
// from this path; collectors receive the unscored sentinel literal.
// allowPruning: when true and no domain builder is attached, a collector
// exposing nextCompetitiveRange() has noncompetitive doc blocks jumped over
// before window production, and every window request is bounded by the
// competitive range end - a one-doc candidate range costs one doc, not a
// whole overshooting window of key-rejected neighbors. Skipped docs are
// uncounted. visitedBlocks (with its block geometry) marks key blocks an
// earlier seeded pass already enumerated: the walk jumps them, and window
// production never crosses a block boundary without re-consulting the mask.
template <typename Collector>
void collectTopKMatchWindowed(int32_t segnum, BulkScorer* bulk, DocSet* filter,
                              DocSetBuilder* builder, Collector& collector,
                              int32_t maxDoc, bool allowPruning = false,
                              std::span<const uint64_t> visitedBlocks = {},
                              int32_t visitedBlockSize = 0) {
  assert(bulk != nullptr);
  assert(visitedBlocks.empty() || visitedBlockSize > 0);
  int32_t cursor = 0;
  constexpr bool hasSortRanges =
      requires { collector.nextCompetitiveRange(segnum, (int32_t)0); };
  [[maybe_unused]] bool sortPrune = false;
  if constexpr (hasSortRanges) {
    sortPrune = allowPruning && builder == nullptr;
  }
  while (cursor < maxDoc) {
    int32_t rangeEnd = maxDoc;
    if constexpr (hasSortRanges) {
      if (sortPrune) {
        auto range = collector.nextCompetitiveRange(segnum, cursor);
        if (range.begin == PostingsReader::END) {
          break;
        }
        cursor = std::max(cursor, range.begin);
        rangeEnd = std::min(rangeEnd, range.end);
      }
    }
    if (!visitedBlocks.empty()) {
      int64_t block = (int64_t)cursor / visitedBlockSize;
      int32_t blockLimit = (int32_t)std::min<int64_t>(
          (block + 1) * (int64_t)visitedBlockSize, (int64_t)maxDoc);
      if ((visitedBlocks[(size_t)(block >> 6)] >> (block & 63)) & 1) {
        skipCount(SkipStats::fieldSortSeedPass2Skips);
        cursor = blockLimit;
        continue;
      }
      rangeEnd = std::min(rangeEnd, blockLimit);
    }
    feedMatchWindows(segnum, bulk, filter, builder, collector, cursor,
                     rangeEnd);
    cursor = rangeEnd;
  }
}

// Best-first exact-domain field-sort driver. The whole domain is known up
// front - materialized docs, or every doc for match-all with no deletes -
// so no scorer runs: bounds are visited in ascending order through a mixed
// lazy frontier. Only the coarse blocks are heapified up front (setup cost
// stays independent of leaf count); popping a competitive coarse node
// replaces it with its leaves in the same (bound, firstDoc, level) heap,
// and only popped-competitive LEAVES gather. Coarse bounds lower-bound
// their leaves, so a SKIP_STRICT head still proves global termination, and
// a tie-skipped coarse node discards its whole subtree soundly (every
// child's bound and first unseen doc are at least as bad against the same
// bottom). workCap bounds leaf gather attempts on pathological tie
// plateaus; on cap, unvisited leaves are swept forward in doc order under
// the same classification. Every in-domain doc is gathered at most once,
// so hitCount keeps the pruned-path semantics (counts gathered docs;
// pruning is off when exact counts are required). The domain
// representation lives in `gather`, which feeds one leaf's in-domain
// (docs, keys) to admitGathered; the bitset and array entry points below
// supply it.
// Mixed coarse/leaf bound frontier shared by the bound-ordered drivers: a
// (bound, firstDoc, coarse-before-leaf) min-heap seeded with only the
// coarse nodes; a popped-competitive coarse node is replaced by its leaves
// via expand(). Peak occupancy never exceeds leafCount (each unexpanded
// coarse node stands in for at least one of its own leaves), and the
// backing span is UNINITIALIZED - the reservation touches only what the
// frontier actually holds, so a near-ceiling segment costs pages
// proportional to the visited frontier, not the leaf directory.
template <typename KeyBlockPlan>
class LeafBoundFrontier {
public:
  struct Node {
    int64_t bound;
    int32_t firstDoc;
    bool isLeaf;  // a coarse node orders before its equal-bound first leaf
    int64_t id;   // coarse block id or global leaf id
    bool operator>(const Node& o) const {
      if (bound != o.bound) return bound > o.bound;
      if (firstDoc != o.firstDoc) return firstDoc > o.firstDoc;
      return isLeaf && !o.isLeaf;
    }
  };

  LeafBoundFrontier(const KeyBlockPlan& plan, MemPool& pool)
      : plan(plan), leavesPerBlock(plan.blockSize / plan.leafSize),
        heap(pool.make_span_uninit<Node>((size_t)plan.leafCount)) {
    for (int64_t b = 0; b < plan.blockCount; b++) {
      heap[(size_t)b] = {plan.batch->blockBestKey(b),
                         (int32_t)(b * (int64_t)plan.blockSize), false, b};
    }
    size = (size_t)plan.blockCount;
    std::make_heap(heap.begin(), heap.begin() + size, cmp);
  }

  bool empty() const { return size == 0; }

  // Pop the best node. pop_heap parks it at heap[size], so
  // remainingWithHead() covers the head plus the still-live frontier until
  // the next expand() overwrites the parked slot.
  Node pop() {
    Node top = heap[0];
    std::pop_heap(heap.begin(), heap.begin() + size, cmp);
    size--;
    return top;
  }

  void expand(const Node& coarse) {
    assert(!coarse.isLeaf);
    int64_t firstLeaf = coarse.id * leavesPerBlock;
    int64_t leafLimit = std::min(firstLeaf + leavesPerBlock, plan.leafCount);
    for (int64_t leaf = firstLeaf; leaf < leafLimit; leaf++) {
      heap[size] = {plan.batch->leafBestKey(leaf),
                    (int32_t)(leaf * (int64_t)plan.leafSize), true, leaf};
      size++;
      std::push_heap(heap.begin(), heap.begin() + size, cmp);
    }
  }

  std::span<const Node> remainingWithHead() const {
    return heap.first(size + 1);
  }

private:
  KeyBlockPlan plan;
  int64_t leavesPerBlock;
  std::greater<Node> cmp;  // min-heap
  std::span<Node> heap;
  size_t size = 0;
};

template <typename Collector, typename GatherLeaf>
void collectTopKBestFirst(int32_t segnum, Collector& collector, MemPool& pool,
                          const typename Collector::KeyBlockPlan& plan,
                          int64_t workCap, GatherLeaf&& gather) {
  assert(plan.batch != nullptr && plan.leafSize != 0);
  using BC = typename Collector::BlockClass;
  using Frontier = LeafBoundFrontier<typename Collector::KeyBlockPlan>;
  skipCount(SkipStats::fieldSortBestFirstActivations);

  Frontier frontier(plan, pool);
  std::span<uint64_t> visited =
      pool.make_span<uint64_t>((size_t)((plan.leafCount + 63) >> 6));
  std::fill(visited.begin(), visited.end(), 0);

  auto gatherLeaf = [&](int64_t leaf) {
    visited[(size_t)(leaf >> 6)] |= 1ULL << (leaf & 63);
    skipCount(SkipStats::fieldSortBestFirstLeaves);
    gather(leaf);
  };

  int64_t leafGathers = 0;
  bool capped = false;
  while (!frontier.empty()) {
    auto top = frontier.pop();
    if (collector.heapFull()) {
      auto cls = collector.classifyBlock(segnum, top.firstDoc, top.bound,
                                         plan.batch);
      if (cls == BC::SKIP_STRICT) {
        // Heap order proves every remaining node is at least as bad; credit
        // the head and the whole remaining frontier as skipped, per level,
        // so cross-arm skip accounting stays comparable.
        skipCount(SkipStats::fieldSortBestFirstTerminations);
        if (SkipStats::enabled) {
          for (const auto& node : frontier.remainingWithHead()) {
            (node.isLeaf ? SkipStats::fieldSortLeavesSkipped
                         : SkipStats::fieldSortBlocksSkipped)++;
          }
        }
        return;
      }
      if (cls == BC::SKIP_TIE) {
        skipCount(top.isLeaf ? SkipStats::fieldSortLeavesSkipped
                             : SkipStats::fieldSortBlocksSkipped);
        continue;
      }
    }
    if (!top.isLeaf) {
      skipCount(SkipStats::fieldSortBestFirstExpansions);
      frontier.expand(top);
      continue;
    }
    gatherLeaf(top.id);
    if (++leafGathers >= workCap) {
      capped = true;
      break;
    }
  }
  if (!capped || frontier.empty()) return;

  // Work cap hit (tie plateau or adversarial bound layout): finish with one
  // forward doc-order sweep over unvisited leaves, classifying each.
  skipCount(SkipStats::fieldSortBestFirstFallbacks);
  for (int64_t leaf = 0; leaf < plan.leafCount; leaf++) {
    if ((visited[(size_t)(leaf >> 6)] >> (leaf & 63)) & 1) continue;
    if (collector.heapFull()) {
      auto cls = collector.classifyBlock(
          segnum, (int32_t)(leaf * (int64_t)plan.leafSize),
          plan.batch->leafBestKey(leaf), plan.batch);
      if (cls != BC::COLLECT) {
        skipCount(SkipStats::fieldSortLeavesSkipped);
        continue;
      }
    }
    gatherLeaf(leaf);
  }
}

// Bitset-domain entry: domainWords is a whole-segment word span (cached
// filter or liveDocs), or empty meaning every doc (match-all, no deletes).
// Leaves gather through the comparator's fused masked gather.
template <typename Collector>
void collectTopKBitSetBestFirst(int32_t segnum,
                                std::span<const uint64_t> domainWords,
                                Collector& collector, MemPool& pool,
                                int64_t workCap) {
  auto plan = collector.maskedKeyBlockPlan();
  assert(plan.batch != nullptr);
  std::span<int32_t> outDocs = pool.make_span<int32_t>((size_t)plan.leafSize);
  std::span<int64_t> outKeys = pool.make_span<int64_t>((size_t)plan.leafSize);
  collectTopKBestFirst(
      segnum, collector, pool, plan, workCap, [&](int64_t leaf) {
        int32_t n = plan.batch->gatherLeafMasked(leaf, domainWords, outDocs,
                                                 outKeys);
        if (n > 0) {
          collector.admitGathered(segnum, outDocs.first((size_t)n),
                                  outKeys.first((size_t)n), plan.batch);
        }
      });
}

// Array-domain entry: domainDocs is the whole sorted materialized set (an
// ARRAY DocSet, below the bitset promotion threshold). Each visited leaf
// gallops to its doc sub-span and gathers keys through the comparator's
// order-independent key gather - the docs themselves need no copy.
template <typename Collector>
void collectTopKArrayBestFirst(int32_t segnum,
                               std::span<const int32_t> domainDocs,
                               Collector& collector, MemPool& pool,
                               int64_t workCap) {
  auto plan = collector.maskedKeyBlockPlan();
  assert(plan.batch != nullptr);
  std::span<int64_t> outKeys = pool.make_span<int64_t>((size_t)plan.leafSize);
  const int32_t* domainEnd = domainDocs.data() + domainDocs.size();
  collectTopKBestFirst(
      segnum, collector, pool, plan, workCap, [&](int64_t leaf) {
        int32_t begin = (int32_t)std::min<int64_t>(
            leaf * (int64_t)plan.leafSize,
            (int64_t)std::numeric_limits<int32_t>::max());
        int32_t end = (int32_t)std::min<int64_t>(
            (leaf + 1) * (int64_t)plan.leafSize,
            (int64_t)std::numeric_limits<int32_t>::max());
        const int32_t* lo =
            screaming::gallopLowerBound(domainDocs.data(), domainEnd, begin);
        const int32_t* hi = screaming::gallopLowerBound(lo, domainEnd, end);
        if (lo == hi) return;
        std::span<const int32_t> docs(lo, hi);
        plan.batch->gatherKeys(docs, outKeys.first(docs.size()));
        collector.admitGathered(segnum, docs, outKeys.first(docs.size()),
                                plan.batch);
      });
}

// Seeded two-pass query-driven field-sort driver. The domain is reachable
// only through forward scorers (streaming postings, no rewind), so
// bound-ordered visiting is reformulated as two monotone passes: select the
// seedCount best-bounded LEAVES, walk them in doc order with one bulk
// scorer so the heap bottom matures to near-final, then sweep from doc 0
// with a second, independent bulk scorer whose competitive-range walk now
// skips essentially everything. Both products must exist before pass 1 runs:
// a pass-2 scorer cannot be recovered after pass 1 advanced a shared one.
// Leaves pass 1 actually enumerated are recorded in a visited mask the sweep
// jumps over, so every match reaches the collector at most once and hitCount
// keeps pruned-path semantics. Once the heap fills, each remaining seed is
// classified against the maturing bottom before any postings work. Two abort
// rules bound an anti-correlated query (its matches only in high-key leaves,
// so seed probes come back empty): a heap still underfull after
// fillAbortBudget enumerated seeds abandons the schedule, and so does a
// consecutive run of fillAbortBudget matchless enumerated seeds - the
// latter also fires when a warm heap (filled by earlier segments) lets
// low-bound leaves classify competitive without the underfull test ever
// engaging. Either way the sweep completes correctness, inheriting whatever
// bottom the seeds bought.
template <typename Collector>
void collectTopKMatchWindowedSeeded(
    int32_t segnum, BulkScorer* seedBulk, BulkScorer* sweepBulk,
    DocSet* filter, Collector& collector, MemPool& pool, int32_t maxDoc,
    int64_t seedCount, int64_t fillAbortBudget) {
  auto plan = collector.maskedKeyBlockPlan();
  assert(plan.batch != nullptr && plan.leafSize != 0);
  assert(seedBulk != nullptr && sweepBulk != nullptr);
  using BC = typename Collector::BlockClass;
  skipCount(SkipStats::fieldSortSeededActivations);

  // Select the seedCount globally best-bounded leaves through the same lazy
  // frontier the best-first driver uses - only the coarse level is
  // heapified, and a popped coarse node is replaced by its leaves - then
  // visit the selection in ascending doc order so the seed scorer only
  // moves forward. Selection is metadata-only: no classification, no
  // postings work (an undiscovered leaf cannot beat its parent's bound, so
  // the first seedCount leaf pops ARE the global best).
  LeafBoundFrontier<typename Collector::KeyBlockPlan> frontier(plan, pool);
  seedCount = std::min<int64_t>(seedCount, plan.leafCount);
  std::span<int64_t> seeds = pool.make_span<int64_t>((size_t)seedCount);
  int64_t selected = 0;
  while (!frontier.empty() && selected < seedCount) {
    auto top = frontier.pop();
    if (top.isLeaf) {
      seeds[(size_t)selected++] = top.id;
    } else {
      frontier.expand(top);
    }
  }
  std::span<int64_t> selectedSeeds = seeds.first((size_t)selected);
  std::sort(selectedSeeds.begin(), selectedSeeds.end());

  std::span<uint64_t> visited =
      pool.make_span<uint64_t>((size_t)((plan.leafCount + 63) >> 6));
  std::fill(visited.begin(), visited.end(), 0);

  int64_t enumerated = 0;
  int64_t consecutiveEmpty = 0;
  for (int64_t leaf : selectedSeeds) {
    if (collector.heapFull()) {
      if (collector.classifyBlock(
              segnum, (int32_t)(leaf * (int64_t)plan.leafSize),
              plan.batch->leafBestKey(leaf), plan.batch) != BC::COLLECT) {
        skipCount(SkipStats::fieldSortSeedClassifiedOut);
        continue;
      }
    } else if (enumerated >= fillAbortBudget) {
      skipCount(SkipStats::fieldSortSeedFillAborts);
      break;
    }
    int32_t begin = (int32_t)(leaf * (int64_t)plan.leafSize);
    int32_t end = (int32_t)std::min<int64_t>(
        (leaf + 1) * (int64_t)plan.leafSize, (int64_t)maxDoc);
    int64_t emitted = feedMatchWindows(segnum, seedBulk, filter, nullptr,
                                       collector, begin, end);
    visited[(size_t)(leaf >> 6)] |= 1ULL << (leaf & 63);
    enumerated++;
    skipCount(SkipStats::fieldSortSeedLeaves);
    if (emitted != 0) {
      consecutiveEmpty = 0;
    } else if (++consecutiveEmpty >= fillAbortBudget) {
      skipCount(SkipStats::fieldSortSeedFillAborts);
      break;
    }
  }

  collectTopKMatchWindowed(segnum, sweepBulk, filter, nullptr, collector,
                           maxDoc, true, visited, plan.leafSize);
}

// Rank score windows into a top-k collector. This is intentionally only a
// ranking primitive; callers that need an exact count or materialized domain
// compose it with countMatchesWindowed using an independent scorer supplier.
// allowPruning=false pins theta at lowest so every match is scored.
template <typename Collector>
void collectTopKWindowed(int32_t segnum, BulkScorer* bulk, DocSet* filter,
                         Collector& collector, MaxScoreAccumulator* accumulator,
                         int32_t maxDoc, bool allowPruning = true,
                         BulkScorer* exactScorer = nullptr) {
  static_assert(requires(Collector& c) { c.minCompetitiveVal; },
                "collectTopKWindowed is only for score top-k collectors");

  assert(bulk != nullptr);
  bulk->setTopKDepth(collector.topCount, allowPruning);
  int32_t cursor = 0;
  ScoreWindow window;
  while (cursor != PostingsReader::END && cursor < maxDoc) {
    float localTheta;
    if constexpr (requires {
        collector.minCompetitiveScoreForNextDoc(segnum, cursor);
      }) {
      localTheta = collector.minCompetitiveScoreForNextDoc(segnum, cursor);
    } else {
      localTheta = collector.minCompetitiveVal;
    }
    float theta = !allowPruning
      ? std::numeric_limits<float>::lowest()
      : accumulator != nullptr
        ? std::max(localTheta, accumulator->get())
        : localTheta;
    int32_t next = bulk->scoreNextWindow(window, filter, cursor, maxDoc, theta);

    if (exactScorer != nullptr && window.size > 0) {
      assert(exactScorer->supportsExactCandidateScoring());
      exactScorer->scoreCandidatesExact(
          window.docs.first((size_t) window.size),
          window.scores.first((size_t) window.size));
    }
    for (int32_t i = 0; i < window.size; i++) {
      int32_t doc = window.docs[(size_t) i];
      float score = window.scores[(size_t) i];
      float oldMinCompetitiveVal = collector.minCompetitiveVal;
      collector.collect(segnum, doc, score);
      if (accumulator != nullptr && collector.minCompetitiveVal > oldMinCompetitiveVal) {
        accumulator->accumulate(collector.minCompetitiveVal);
      }
    }

    if (next == PostingsReader::END) {
      break;
    }
    assert(next > cursor);
    cursor = next;
  }
}

} // namespace solux
