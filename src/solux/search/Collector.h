#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>

#include "solux/query/Query.h"
#include "solux/search/DocSet.h"

namespace solux {

SOLUX_PACKED_START
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

} SOLUX_PACKED_END;




class TopDocsCollector {
  public:

  // Having this packed helped both memory and CPU (presumably better cache hits?)
  SOLUX_PACKED_START
  struct ScoreDoc {
    float score;
    segdoc doc;
  } SOLUX_PACKED_END;


  int64_t hitCount = 0;

  float minCompetitiveVal = std::numeric_limits<float>::lowest();

  constexpr static auto scoreComp = [](const ScoreDoc &a, const ScoreDoc &b) {
    return a.score > b.score;
  };

  constexpr static auto scoreAndDocComp = [](const ScoreDoc &a, const ScoreDoc &b) {
    return a.score == b.score ? (a.doc < b.doc) : (a.score > b.score);
  };

  int64_t topCount;
  std::vector<ScoreDoc> topDocs;  // TODO: create an expanding PQ backed by a vector so we don't have to allocate a vector of size topCount
  // Heap orders by (score, then doc) so ties at the k-th score are broken by (seg, docid):
  // the kept top-K is a deterministic total order, independent of collection/merge order
  // (and, once it exists, slicing).  Required for sliced==unsliced to hold at exact ties.
  DirectPQ<ScoreDoc, decltype(scoreAndDocComp)> pq;

  TopDocsCollector(int64_t topCount) : topCount(topCount), topDocs(topCount), pq(topDocs) {
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
    // In some scenarios, popping the top of the other heap until it's no longer competitive will be faster,
    // while in other scenarios just a linear scan of the other heap will be faster.
    // We'll just do a linear scan for now.
    auto newHitCount = hitCount + other.hitCount;
    // Since min-heap has smallest element at position 0, it should be more efficient to start from the other end.
    // Future possible optimization: if we do a whole level of a min-tree without any insertions, we could stop early.
    for (int i = other.size() - 1; i >= 0; i--) {
      collect(other.topDocs[i].doc.segment(), other.topDocs[i].doc.docId(), other.topDocs[i].score);
    }
    hitCount = newHitCount;
  }

  // Since we use min-heap comparators in our priority queues, the list will be reverse-sorted (smallest last)
  // Repeated calls to pop() on the priority queue will also return the docs in order.
  std::span<ScoreDoc> sort() {
    // sort_heap must use the same comparator the heap was built with (scoreAndDocComp),
    // which also gives the stable response order: score desc, then (seg, docid) asc at ties.
    std::sort_heap(topDocs.begin(), topDocs.begin() + pq.size(), scoreAndDocComp);
    return {topDocs.data(), pq.size()};
  }

  std::span<ScoreDoc> scoreDocs() {
    return {topDocs.data(), pq.size()};
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
    for (auto& sd: other.topDocs) {
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

  if (filter == nullptr || filter->type == DocSet::BITSET) {
    BitDocSet* bitDocs = (BitDocSet*)filter;
    auto* domainBits = bitDocs ? &bitDocs->bits() : nullptr;
    for (;;) {
      auto doc = scorer->next();
      if (doc == PostingsReader::END) {
        break;
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
    for (auto doc : arrDocs->docs()) {
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

// Collect exhaustive match-only windows in doc order. Scores are intentionally
// absent from this path; collectors receive the unscored sentinel literal.
template <typename Collector>
void collectTopKMatchWindowed(int32_t segnum, BulkScorer* bulk, DocSet* filter,
                              DocSetBuilder* builder, Collector& collector,
                              int32_t maxDoc) {
  assert(bulk != nullptr);
  int32_t cursor = 0;
  ScoreWindow window;
  while (cursor != PostingsReader::END && cursor < maxDoc) {
    int32_t next = bulk->matchNextWindow(window, filter, cursor, maxDoc);
    for (int32_t i = 0; i < window.size; i++) {
      int32_t doc = window.docs[(size_t) i];
      if (builder != nullptr) {
        builder->add(doc);
      }
      collector.collect(segnum, doc, 0.0f);
    }
    if (next == PostingsReader::END) {
      break;
    }
    assert(next > cursor);
    cursor = next;
  }
}

// Rank score windows into a top-k collector. This is intentionally only a
// ranking primitive; callers that need an exact count or materialized domain
// compose it with countMatchesWindowed using an independent scorer supplier.
// allowPruning=false pins theta at lowest so every match is scored.
template <typename Collector>
void collectTopKWindowed(int32_t segnum, BulkScorer* bulk, DocSet* filter,
                         Collector& collector, MaxScoreAccumulator* accumulator,
                         int32_t maxDoc, bool allowPruning = true) {
  static_assert(requires(Collector& c) { c.minCompetitiveVal; },
                "collectTopKWindowed is only for score top-k collectors");

  assert(bulk != nullptr);
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

    for (int32_t i = 0; i < window.size; i++) {
      float oldMinCompetitiveVal = collector.minCompetitiveVal;
      collector.collect(segnum, window.docs[(size_t) i], window.scores[(size_t) i]);
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
