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

// Constant scores rank by doc order, so a segment's top K docs are its first
// K matches: capture them straight off the counting bulk's emitted windows,
// then count the remainder without materializing docs. One bulk arrangement
// serves ranking, count, and domain; an independent capture scorer would
// rebuild every clause (a multiterm clause re-runs its dictionary scan per
// build). Score windows carry the weight's constant, so reported scores
// match what a pull scorer from the same weight returns.
inline void collectFirstKConstantWindowed(
    int32_t segnum, BulkScorer* bulk, DocSet* filter, DocSetBuilder* builder,
    TopDocsCollector& collector, int32_t maxDoc) {
  assert(bulk != nullptr);
  assert(collector.topCount > 0);
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
      } else {
        // The capture window ran past K; these are count-only.
        collector.hitCount++;
        overshoot++;
      }
    }
    if (next == PostingsReader::END) {
      cursor = next;
      break;
    }
    assert(next > cursor);
    cursor = next;
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

// Collect match-only windows in doc order. Scores are intentionally absent
// from this path; collectors receive the unscored sentinel literal.
// allowPruning: when true and no domain builder is attached, a collector
// exposing nextCompetitiveRange() has noncompetitive doc blocks jumped over
// before window production, and every window request is bounded by the
// competitive range end - a one-doc candidate range costs one doc, not a
// whole overshooting window of key-rejected neighbors. END from a bounded
// request means the bound was reached; only an unbounded request's END is
// query exhaustion. Skipped docs are uncounted.
template <typename Collector>
void collectTopKMatchWindowed(int32_t segnum, BulkScorer* bulk, DocSet* filter,
                              DocSetBuilder* builder, Collector& collector,
                              int32_t maxDoc, bool allowPruning = false) {
  assert(bulk != nullptr);
  int32_t cursor = 0;
  [[maybe_unused]] int32_t competitiveEnd = 0;
  constexpr bool hasSortRanges =
      requires { collector.nextCompetitiveRange(segnum, (int32_t)0); };
  [[maybe_unused]] bool sortPrune = false;
  if constexpr (hasSortRanges) {
    sortPrune = allowPruning && builder == nullptr;
  }
  ScoreWindow window;
  while (cursor != PostingsReader::END && cursor < maxDoc) {
    int32_t windowMax = maxDoc;
    if constexpr (hasSortRanges) {
      if (sortPrune && cursor >= competitiveEnd) {
        auto range = collector.nextCompetitiveRange(segnum, cursor);
        competitiveEnd = range.end;
        if (range.begin > cursor) {
          cursor = range.begin;
          continue;
        }
      }
      if (sortPrune) {
        windowMax = std::min(windowMax, competitiveEnd);
      }
    }
    int32_t next = bulk->matchNextWindow(window, filter, cursor, windowMax);
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
      if (windowMax >= maxDoc) {
        break;
      }
      cursor = windowMax;
      continue;
    }
    assert(next > cursor);
    cursor = next;
  }
}

// Best-first exact-domain field-sort driver. The whole domain is known up
// front - a bitset (cached filter or liveDocs), or every doc when
// domainWords is empty (match-all, no deletes) - so no scorer runs: key
// blocks are visited in ascending bound order, each block's in-domain docs
// admitted through the comparator's fused masked gather. Bound order makes
// termination a proof - once the heap head classifies SKIP_STRICT against
// the current bottom, no remaining block can contribute. Equal-bound heads
// are popped individually through the tie/segdoc rules. workCap bounds
// pathological tie plateaus; on cap, unvisited blocks are swept forward in
// doc order under the same classification. Every in-domain doc is gathered
// at most once, so hitCount keeps the pruned-path semantics (counts
// gathered docs; pruning is off when exact counts are required).
template <typename Collector>
void collectTopKBitSetBestFirst(int32_t segnum,
                                std::span<const uint64_t> domainWords,
                                Collector& collector, MemPool& pool,
                                int64_t workCap) {
  auto plan = collector.maskedKeyBlockPlan();
  assert(plan.batch != nullptr);
  using BC = typename Collector::BlockClass;
  skipCount(SkipStats::fieldSortBestFirstActivations);

  struct BoundBlock {
    int64_t bound;
    int64_t block;
    bool operator>(const BoundBlock& o) const {
      return bound != o.bound ? bound > o.bound : block > o.block;
    }
  };
  std::span<BoundBlock> heap = pool.make_span<BoundBlock>(
      (size_t)plan.blockCount);
  for (int64_t b = 0; b < plan.blockCount; b++) {
    heap[(size_t)b] = {plan.batch->blockBestKey(b), b};
  }
  auto cmp = std::greater<BoundBlock>();  // min-heap by (bound, block)
  std::make_heap(heap.begin(), heap.end(), cmp);

  std::span<uint64_t> visited =
      pool.make_span<uint64_t>((size_t)((plan.blockCount + 63) >> 6));
  std::fill(visited.begin(), visited.end(), 0);
  std::span<int32_t> outDocs = pool.make_span<int32_t>((size_t)plan.blockSize);
  std::span<int64_t> outKeys = pool.make_span<int64_t>((size_t)plan.blockSize);

  auto gatherBlock = [&](int64_t block) {
    visited[(size_t)(block >> 6)] |= 1ULL << (block & 63);
    int32_t n = plan.batch->gatherBlockMasked(block, domainWords, outDocs,
                                              outKeys);
    skipCount(SkipStats::fieldSortBestFirstBlocks);
    if (n > 0) {
      collector.admitGathered(segnum, outDocs.first((size_t)n),
                              outKeys.first((size_t)n), plan.batch);
    }
  };

  int64_t visitedCount = 0;
  size_t heapSize = heap.size();
  while (heapSize > 0) {
    BoundBlock top = heap[0];
    std::pop_heap(heap.begin(), heap.begin() + heapSize, cmp);
    heapSize--;
    if (collector.heapFull()) {
      auto cls = collector.classifyBlock(
          segnum, (int32_t)(top.block * (int64_t)plan.blockSize), top.bound,
          plan.batch);
      if (cls == BC::SKIP_STRICT) {
        // Heap order proves every remaining block is at least as bad; credit
        // the head and the whole remaining heap as skipped so cross-arm
        // skip accounting stays comparable.
        skipCount(SkipStats::fieldSortBestFirstTerminations);
        if (SkipStats::enabled) {
          SkipStats::fieldSortBlocksSkipped += (int64_t)heapSize + 1;
        }
        return;
      }
      if (cls == BC::SKIP_TIE) {
        skipCount(SkipStats::fieldSortBlocksSkipped);
        continue;
      }
    }
    gatherBlock(top.block);
    if (++visitedCount >= workCap) break;
  }
  if (heapSize == 0) return;

  // Work cap hit (tie plateau or adversarial bound layout): finish with one
  // forward doc-order sweep over unvisited blocks, classifying each.
  skipCount(SkipStats::fieldSortBestFirstFallbacks);
  for (int64_t block = 0; block < plan.blockCount; block++) {
    if ((visited[(size_t)(block >> 6)] >> (block & 63)) & 1) continue;
    if (collector.heapFull()) {
      auto cls = collector.classifyBlock(
          segnum, (int32_t)(block * (int64_t)plan.blockSize),
          plan.batch->blockBestKey(block), plan.batch);
      if (cls != BC::COLLECT) {
        skipCount(SkipStats::fieldSortBlocksSkipped);
        continue;
      }
    }
    gatherBlock(block);
  }
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
